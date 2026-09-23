#include "Ui.h"

#include "AcpiUtils.h"
#include "Config.h"
#include "KeyBinds.h"
#include "EffectController.h"
#include "Thermals.h"
#include "database.h"
#include "helper.h"

#include <adwaita.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <glib/gi18n.h>
#include <loguru.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// 这两条在「与 daemon 的状态同步」那段里定义（在本文件靠后），前面的回调要用，先声明
std::string stateCommand(const std::string &cmd);
std::string modeKey(ThermalModes mode);

// ── 图标随窗口缩放 ──────────────────────────────────────────────────────────
// 需求：图标大小随窗口大小自动调整、长宽比不变。
// 为什么不用 size_allocate vfunc 拿窗口尺寸：GTK4 的 GtkWidgetClass.size_allocate 文档写明
// 「if the widget does not have a layout manager」才调用——GtkBox 自带 GtkBoxLayout，实测它的
// size_allocate 一次都不会被调用（加探针验证过）。为一个缩放钩子去写自定义容器 + GtkBuildable
// 不值得，改用帧时钟回调：每帧读一次内容区尺寸，尺寸没变就直接返回，开销是几次整数比较。
//
// 只有 GtkImage 的 pixel-size（正方形）会被改，不碰 width/height，因此长宽比固定不变。
struct IconGroup {
    // 每个图标带一个倍率：Adwaita 各图标的墨迹占比不同（键盘是扁的、点阵是稀疏的），
    // 只统一 pixel-size 出来的视觉大小并不一致，所以按量到的墨迹给每个图标补一个倍率。
    std::vector<std::pair<GtkImage *, double>> images;
    std::vector<int> inkHeights; // 图标墨迹高度（量一次用来算补偿倍率）
    std::vector<GtkWidget *> squares; // 跟着一起放大的按钮（给成正方形）
    double ratio;                     // 相对内容区短边的比例
    int minPx;
    int maxPx;
    int lastPx = 0;
};

// ── 页面清单 ────────────────────────────────────────────────────────────────
// 源码里的串一律用**英文**做 msgid：gettext 查不到翻译时回落到 msgid，于是中文环境显示
// po/zh_CN.po 的译文、其他环境显示英文——这就是「中英文自动切换」。见 DESIGN.md 第二节。
// 表格里的串用 N_() 标记，否则 xgettext 提取不到（见 po/awcc.pot）。

struct AppContext;

struct SubPage {
    const char *name;       // GtkStack 子页名
    const char *shortLabel; // 子页切换按钮上的字（msgid）
    const char *title;      // (msgid)
    const char *note;       // (msgid) 缺什么后端
    GtkWidget *(*build)(AppContext &); // 非空则建真页面，空则出占位页
};

struct NavEntry {
    const char *name; // 也是 GtkStack 子页名，与 .ui 里 nav_<name> 对应
    const char *title;
    const char *note;
    const SubPage *subs;
    size_t subCount;
    GtkWidget *(*build)(AppContext &);
};

// 运行期上下文
struct AppContext {
    // 外壳
    GtkWidget *window = nullptr;
    GtkWidget *stack = nullptr;
    GtkWidget *crumbMain = nullptr;
    GtkWidget *crumbSub = nullptr;
    // 用户配置（背景等）与运行期注入的背景 CSS
    Config::Ui config;
    GtkCssProvider *backgroundProvider = nullptr;
    GtkWidget *bgColorRow = nullptr; // 「外观」页里两行的可用性随背景模式变
    GtkWidget *bgImageRow = nullptr;
    GtkWidget *bgImageLabel = nullptr;
    // 服务层（main.cpp 传进来，不持有所有权）
    Thermals *thermals = nullptr;
    AcpiUtils *acpi = nullptr;
    EffectController *effects = nullptr;
    bool daemonRunning = false;
    // 性能模式的唯一状态源：主页 / 性能·概况 / 性能·散热 三页各有一组模式按钮，状态统一放这里。
    // 之前每页各建一组、各自记着建页那一刻的旧值，于是「主页改了、性能页不跟着变」（议题 #1）。
    ThermalModes currentMode = ThermalModes::Balanced;
    std::vector<std::pair<ThermalModes, GtkToggleButton *>> modeButtons;
    std::vector<GtkWidget *> modeLabels; // 显示「当前模式」的文字，改模式后一起刷新
    bool updatingModes = false;          // 刷新按钮状态期间别再回头触发一次设置
    size_t modeRowCount = 0;             // 建了几组模式按钮（自检报告用）
    // 与 daemon 的状态订阅（DESIGN.md 第九节）：这里只保存「最近一次从后端收到的快照」，
    // 控件从它渲染；set 只发命令、不做乐观更新，等广播回来再画。
    int stateFd = -1;                    // 订阅连接
    guint stateWatch = 0;
    guint stateRetry = 0;
    std::string stateBuffer;
    GtkWidget *brightnessScale = nullptr;   // 订阅推来新亮度时要回填它
    GtkWidget *brightnessLabel = nullptr;
    bool updatingBrightness = false;        // 回填期间别把值再发回去
    bool updatingKeybinds = false;          // 刷新下拉期间同上
    std::function<void()> keybindRefresh;   // 绑定表变了就重读一次
    // 图标缩放
    // 用 deque 而不是 vector：buildModeCard 里会再 addIconGroup，vector 扩容会让先取的引用悬空
    std::deque<IconGroup> iconGroups;
    // 宽度按内容区比例走的卡片（官方版式：如主页底部「仪表卡 2/3 + 游戏库卡 1/3」）
    std::vector<std::pair<GtkWidget *, double>> widthRatios;
    int observedWidth = 0;
    int observedHeight = 0;
    // 界面状态
    uint32_t color = 0x00ff00; // #RRGGBB，默认绿（与 CLI 示例一致）
    int brightness = 50;
    uint16_t duration = 1000; // Spectrum / Rainbow 用，毫秒
    // 滑块的防抖：拖动时先攒住，停一下再下发，免得把 USB / ACPI 打爆
    guint brightnessTimer = 0;
    int brightnessPending = 0;
    guint cpuBoostTimer = 0;
    int cpuBoostPending = 0;
    guint gpuBoostTimer = 0;
    int gpuBoostPending = 0;
    // 自检
    GApplication *app = nullptr;
    bool selftest = false;
    std::string snapshotPath;      // --ui-snapshot=<路径>：把窗口渲染成 PNG（见下）
    std::string initialPage;       // --ui-page=<name>：启动时停在哪一页（截图/自检用）
    bool forceLight = false;       // --ui-force-light：强制按亮色渲染（验证亮色配色用）
    std::string initialSubPage;    // --ui-subpage=<name>：连子页一起指定（截图用）
    bool selftestReported = false; // 首帧已经处理过一次（等真实分配，不用固定延时）
    bool error = false;
    std::vector<std::string> pageLog;
};

gboolean selftestFinish(gpointer data);
gboolean finishFirstFrame(gpointer data);

// 把图标单独渲染出来，量它的「墨迹」高度/宽度。
// 为什么要量：Adwaita 各图标在 16×16 画布里的留白差别很大（键盘是扁的、还有稀疏点阵的），
// 只把 pixel-size 统一，视觉大小并不一致——量出来再逐个补偿才真的齐。
constexpr int kMeasurePx = 32;

int measureIconInk(const char *iconName, int *outWidth) {
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    GtkIconPaintable *icon = gtk_icon_theme_lookup_icon(
        theme, iconName, nullptr, kMeasurePx, 1, GTK_TEXT_DIR_NONE,
        static_cast<GtkIconLookupFlags>(0));
    if (icon == nullptr) {
        return 0;
    }
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(icon), snapshot, kMeasurePx, kMeasurePx);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, kMeasurePx, kMeasurePx);
    cairo_t *cr = cairo_create(surface);
    if (node != nullptr) {
        gsk_render_node_draw(node, cr);
        gsk_render_node_unref(node);
    }
    cairo_surface_flush(surface);
    const unsigned char *data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    int minX = kMeasurePx;
    int maxX = -1;
    int minY = kMeasurePx;
    int maxY = -1;
    for (int y = 0; y < kMeasurePx; ++y) {
        for (int x = 0; x < kMeasurePx; ++x) {
            if (data[y * stride + x * 4 + 3] > 16) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
        }
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    g_object_unref(icon);
    if (maxY < 0) {
        return 0;
    }
    if (outWidth != nullptr) {
        *outWidth = maxX - minX + 1;
    }
    return maxY - minY + 1;
}

int iconPxFor(int base, const IconGroup &group) {
    return std::clamp(static_cast<int>(std::lround(base * group.ratio)), group.minPx,
                      group.maxPx);
}

void applyIconScale(AppContext &ctx, int width, int height) {
    ctx.observedWidth = width;
    ctx.observedHeight = height;
    const int base = std::min(width, height);
    if (base <= 0) {
        return; // 还没 map，别按 0 算出一堆最小值去改控件
    }
    // 等到第一次真实分配再处理（自检报告 / 渲染快照），比固定 sleep 可靠（窗口 map 时间不定）
    if ((ctx.selftest || !ctx.snapshotPath.empty()) && !ctx.selftestReported) {
        ctx.selftestReported = true;
        g_timeout_add(200, finishFirstFrame, &ctx);
    }
    // 比例宽度的可用宽度 = 窗口宽 - 左栏(48) - 页面左右边距(16+16) - 卡片间距(12)；
    // 不扣这些的话算出来的卡片会比可用空间宽，把窗口越撑越大（实测被撑到 1405）。
    constexpr int kChromeWidth = 48 + 16 + 16 + 12;
    const int usable = width - kChromeWidth;
    for (auto &[widget, ratio] : ctx.widthRatios) {
        const int wanted = static_cast<int>(std::lround(usable * ratio));
        if (wanted > 0 && usable > 0) {
            gtk_widget_set_size_request(widget, wanted, -1);
        }
    }
    for (IconGroup &group : ctx.iconGroups) {
        const int px = iconPxFor(base, group);
        if (px == group.lastPx) {
            continue;
        }
        group.lastPx = px;
        for (auto &[image, scale] : group.images) {
            gtk_image_set_pixel_size(
                image, std::max(1, static_cast<int>(std::lround(px * scale))));
        }
        const int side = px + static_cast<int>(std::lround(px * 0.9)) + 8;
        for (GtkWidget *square : group.squares) {
            gtk_widget_set_size_request(square, side, side);
        }
    }
}

// 帧时钟回调：每帧读一次内容区尺寸。尺寸没变时上面第一件事就返回，开销可忽略。
gboolean onContentTick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock;
    auto *ctx = static_cast<AppContext *>(data);
    applyIconScale(*ctx, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
    return G_SOURCE_CONTINUE;
}

IconGroup &addIconGroup(AppContext &ctx, double ratio, int minPx, int maxPx) {
    ctx.iconGroups.push_back(IconGroup{{}, {}, {}, ratio, minPx, maxPx});
    return ctx.iconGroups.back();
}

// ── 小工具 ──────────────────────────────────────────────────────────────────

// 注意这里对传入串做 gettext 查表：调用方可以直接写 N_("Color")。对已经翻好的串再查一次
// 是无害的（查不到就原样返回），动态串（如配置路径）同理。
GtkWidget *makeLabel(const char *text, const char *cssClass) {
    GtkWidget *label = gtk_label_new(_(text));
    if (cssClass != nullptr) {
        gtk_widget_add_css_class(label, cssClass);
    }
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

GtkWidget *makeBadge(const char *text, const char *cssClass) {
    GtkWidget *badge = gtk_label_new(_(text));
    gtk_widget_add_css_class(badge, cssClass);
    gtk_widget_set_halign(badge, GTK_ALIGN_START);
    return badge;
}

// 卡片：标题 + 可选说明 + 内容区。返回卡片本身，内容用 gtk_box_append(card, ...) 加。
GtkWidget *makeCard(const char *title, const char *note) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_add_css_class(card, "card");
    gtk_box_append(GTK_BOX(card), makeLabel(_(title), "card-title"));
    if (note != nullptr && *note != '\0') {
        GtkWidget *noteLabel = makeLabel(_(note), "card-note");
        gtk_label_set_wrap(GTK_LABEL(noteLabel), TRUE);
        gtk_box_append(GTK_BOX(card), noteLabel);
    }
    return card;
}

// 数值 + 单位的横排（滑块右边跟着显示当前值）
GtkWidget *makeValueLabel() {
    GtkWidget *label = gtk_label_new("");
    gtk_widget_add_css_class(label, "value-label");
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    return label;
}

GtkWidget *makeSliderRow(const char *rowLabel, GtkWidget *scale, GtkWidget *valueLabel) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *label = makeLabel(_(rowLabel), "row-label");
    gtk_widget_set_size_request(label, 96, -1);
    gtk_box_append(GTK_BOX(row), label);
    gtk_widget_set_hexpand(scale, true);
    gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row), scale);
    gtk_box_append(GTK_BOX(row), valueLabel);
    return row;
}

// ── 防抖下发 ────────────────────────────────────────────────────────────────
// 滑块拖动会连发 value-changed；等 150ms 没有新值了再真正下发，避免把 USB / ACPI 打爆。

gboolean flushBrightness(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (ctx->daemonRunning) {
        // 交给 daemon：它才是这份状态的持有者，改完会广播回来（DESIGN.md 第九节）
        stateCommand("brightness-set " + std::to_string(ctx->brightnessPending));
    } else if (ctx->effects != nullptr) {
        ctx->effects->Brightness(static_cast<uint8_t>(ctx->brightnessPending));
    }
    ctx->brightnessTimer = 0;
    return G_SOURCE_REMOVE;
}

gboolean flushCpuBoost(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (ctx->thermals != nullptr) {
        ctx->thermals->setCpuBoost(ctx->cpuBoostPending);
    }
    ctx->cpuBoostTimer = 0;
    return G_SOURCE_REMOVE;
}

gboolean flushGpuBoost(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (ctx->thermals != nullptr) {
        ctx->thermals->setGpuBoost(ctx->gpuBoostPending);
    }
    ctx->gpuBoostTimer = 0;
    return G_SOURCE_REMOVE;
}

void onBrightnessChanged(GtkRange *range, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (ctx->updatingBrightness) {
        return; // 这次变化是订阅回填引起的，不要再发回给 daemon
    }
    ctx->brightness = static_cast<int>(gtk_range_get_value(range));
    ctx->brightnessPending = ctx->brightness;
    if (ctx->brightnessTimer == 0) {
        ctx->brightnessTimer = g_timeout_add(150, flushBrightness, ctx);
    }
}

void onCpuBoostChanged(GtkRange *range, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    ctx->cpuBoostPending = static_cast<int>(gtk_range_get_value(range));
    if (ctx->cpuBoostTimer == 0) {
        ctx->cpuBoostTimer = g_timeout_add(150, flushCpuBoost, ctx);
    }
}

void onGpuBoostChanged(GtkRange *range, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    ctx->gpuBoostPending = static_cast<int>(gtk_range_get_value(range));
    if (ctx->gpuBoostTimer == 0) {
        ctx->gpuBoostTimer = g_timeout_add(150, flushGpuBoost, ctx);
    }
}

// ── 电源模式 ────────────────────────────────────────────────────────────────

struct ModeSpec {
    ThermalModes mode;
    ThermalModeSet set;
    const char *label; // (msgid)
};

constexpr ModeSpec kModes[] = {
    {ThermalModes::BatterySaver, ThermalModeSet::BatterySaver, N_("Battery saver")},
    {ThermalModes::Cool, ThermalModeSet::Cool, N_("Cool")},
    {ThermalModes::Quiet, ThermalModeSet::Quiet, N_("Quiet")},
    {ThermalModes::Balanced, ThermalModeSet::Balanced, N_("Balanced")},
    {ThermalModes::Performance, ThermalModeSet::Performance, N_("Performance")},
    {ThermalModes::Gmode, ThermalModeSet::GMode, N_("G mode")},
};
// 三页共用的模式状态：任何一处改动 → 刷新所有按钮与「当前模式」文字
const char *modeLabel(ThermalModes mode) {
    for (const ModeSpec &spec : kModes) {
        if (spec.mode == mode) {
            return _(spec.label);
        }
    }
    return "—"; // 机型上报的模式不在官方这六档里（例如 Manual）
}

void syncModeButtons(AppContext &ctx) {
    // 期间 set_active 也会发 toggled，用标志挡掉，免得回头又去写一次模式
    ctx.updatingModes = true;
    for (const auto &[mode, btn] : ctx.modeButtons) {
        gtk_toggle_button_set_active(btn, mode == ctx.currentMode);
    }
    for (GtkWidget *label : ctx.modeLabels) {
        gtk_label_set_text(GTK_LABEL(label), modeLabel(ctx.currentMode));
    }
    ctx.updatingModes = false;
}

void applyMode(AppContext &ctx, ThermalModes mode) {
    // 真相在 daemon：只把命令发出去，等它的广播回来再重画（DESIGN.md 第九节）。
    // 不做乐观更新，就不会出现「界面显示 A、硬件实际是 B」。
    if (ctx.daemonRunning && !ctx.selftest) {
        const std::string out = stateCommand("mode-set " + modeKey(mode));
        if (out.rfind("ok", 0) == 0) {
            return;
        }
        LOG_S(WARNING) << "mode-set 失败：" << out << "（退回本地路径）";
    }
    // 自检（--ui-selftest）里一概不碰硬件：不写 ACPI 也不回读，避免弹授权框，也保证自检无副作用
    const bool touchHardware = !ctx.selftest && ctx.thermals != nullptr;
    if (touchHardware) {
        // 注意：daemon 没在跑时这里会走 pkexec，弹授权框期间主循环被挡住（见 DESIGN.md 第三节）
        ctx.thermals->setThermalMode(mode);
    }
    // 只有 daemon 在跑时才回读硬件：没有它读 ACPI 同样会弹授权框（DESIGN.md 第三节）
    ctx.currentMode = (touchHardware && ctx.daemonRunning) ? ctx.thermals->getCurrentMode() : mode;
    syncModeButtons(ctx);
}

void onModeToggled(GtkToggleButton *btn, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (ctx->updatingModes || !gtk_toggle_button_get_active(btn)) {
        return;
    }
    applyMode(*ctx, static_cast<ThermalModes>(
                        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "awcc-mode"))));
}

// 官方样式里这排电源模式按钮是「无卡片、整行居中」浮在顶部辉光上的
GtkWidget *buildModeRow(AppContext &ctx) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);

    GtkToggleButton *leader = nullptr;
    // 当前模式取自 ctx（onActivate 里定：只有 daemon 在跑才读硬件），不再各自去读一次 ACPI
    const ThermalModes current = ctx.currentMode;

    for (const ModeSpec &spec : kModes) {
        if (ctx.acpi != nullptr && !ctx.acpi->hasThermalMode(spec.set)) {
            continue; // 本机型不支持的模式就不摆出来
        }
        // 官方样式里模式按钮是纯文字小黑块（没有图标），选中变红
        GtkWidget *btn = gtk_toggle_button_new_with_label(_(spec.label));
        gtk_widget_add_css_class(btn, "mode-button");
        g_object_set_data(G_OBJECT(btn), "awcc-mode", GINT_TO_POINTER(spec.mode));

        if (leader == nullptr) {
            leader = GTK_TOGGLE_BUTTON(btn);
        } else {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), leader);
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(onModeToggled), &ctx);
        // 登记到 ctx：这一组与另外两页的那两组从此共享同一个状态
        ctx.modeButtons.emplace_back(spec.mode, GTK_TOGGLE_BUTTON(btn));
        gtk_box_append(GTK_BOX(row), btn);
    }
    // 建页时统一设一次选中状态（syncModeButtons 里是静默设置的，不会反过来去写模式）
    ++ctx.modeRowCount;
    syncModeButtons(ctx);
    return row;
}

// ── 与 daemon 的状态同步（DESIGN.md 第九节）─────────────────────────────────
//
// 值的所有权在 daemon。这里只做两件事：把用户操作转成命令发出去；把后端推来的变更落到
// store 并重画。GUI 自己不发乐观更新——否则会出现「界面显示 A、硬件实际是 B」。

// 给 daemon 发一条命令并读回回复（短连接；工程里既有的套接字调用也是这条路）
std::string stateCommand(const std::string &cmd) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "/tmp/awcc.sock");
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return {};
    }
    if (write(fd, cmd.c_str(), cmd.size()) < 0) {
        close(fd);
        return {};
    }
    std::string out;
    char buf[512];
    ssize_t n = 0;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

std::string modeKey(ThermalModes mode) {
    switch (mode) {
    case ThermalModes::BatterySaver:
        return "battery";
    case ThermalModes::Cool:
        return "cool";
    case ThermalModes::Quiet:
        return "quiet";
    case ThermalModes::Performance:
        return "performance";
    case ThermalModes::Gmode:
        return "gmode";
    default:
        return "balanced";
    }
}

ThermalModes modeFromKey(const std::string &name) {
    if (name == "battery") return ThermalModes::BatterySaver;
    if (name == "cool") return ThermalModes::Cool;
    if (name == "quiet") return ThermalModes::Quiet;
    if (name == "performance") return ThermalModes::Performance;
    if (name == "gmode") return ThermalModes::Gmode;
    return ThermalModes::Balanced;
}

// 动作键 → 界面文字。模式类复用模式自己的标签，省一批 msgid。
const char *actionLabel(const std::string &action) {
    if (action == "none") {
        return _("Do nothing");
    }
    if (action == "gmode-toggle") {
        return _("Toggle G mode");
    }
    if (action == "brightness-cycle") {
        return _("Cycle brightness");
    }
    const std::string prefix = "mode:";
    if (action.rfind(prefix, 0) == 0) {
        const ThermalModes mode = modeFromKey(action.substr(prefix.size()));
        for (const ModeSpec &spec : kModes) {
            if (spec.mode == mode) {
                return _(spec.label);
            }
        }
    }
    return action.c_str();
}

// 把后端推来的一行落到 store 并重画（幂等：同样的值再推一次也无害）
void applyStateLine(AppContext &ctx, const std::string &key, const std::string &value) {
    if (key == "mode") {
        ctx.currentMode = modeFromKey(value);
        syncModeButtons(ctx);
        return;
    }
    if (key == "brightness") {
        int value100 = 50;
        try {
            value100 = std::stoi(value);
        } catch (...) {
            return;
        }
        ctx.brightness = std::clamp(value100, 0, 100);
        if (ctx.brightnessScale != nullptr) {
            // 回填期间把改动挡掉，免得又发一条 brightness-set 回去
            ctx.updatingBrightness = true;
            gtk_range_set_value(GTK_RANGE(ctx.brightnessScale), ctx.brightness);
            ctx.updatingBrightness = false;
        }
        if (ctx.brightnessLabel != nullptr) {
            gchar *text = g_strdup_printf("%d%%", ctx.brightness);
            gtk_label_set_text(GTK_LABEL(ctx.brightnessLabel), text);
            g_free(text);
        }
        return;
    }
    if (key == "keybinds") {
        if (ctx.keybindRefresh) {
            ctx.keybindRefresh();
        }
        return;
    }
}

void stateClientStart(AppContext &ctx);

gboolean onStateReadable(gint fd, GIOCondition cond, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if ((cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
        if (ctx->stateWatch != 0) {
            g_source_remove(ctx->stateWatch);
            ctx->stateWatch = 0;
        }
        close(fd);
        ctx->stateFd = -1;
        // daemon 可能刚重启：断线后 2 秒重连一次（这不是轮询，只在断了之后连）
        ctx->stateRetry = g_timeout_add(
            2000,
            [](gpointer d) -> gboolean {
                auto *c = static_cast<AppContext *>(d);
                c->stateRetry = 0;
                stateClientStart(*c);
                return G_SOURCE_REMOVE;
            },
            ctx);
        return G_SOURCE_REMOVE;
    }
    char buf[512];
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
        return G_SOURCE_CONTINUE;
    }
    ctx->stateBuffer.append(buf, static_cast<size_t>(n));
    size_t pos = 0;
    while ((pos = ctx->stateBuffer.find('\n')) != std::string::npos) {
        const std::string line = ctx->stateBuffer.substr(0, pos);
        ctx->stateBuffer.erase(0, pos + 1);
        const size_t space = line.find(' ');
        if (space == std::string::npos) {
            continue;
        }
        applyStateLine(*ctx, line.substr(0, space), line.substr(space + 1));
    }
    return G_SOURCE_CONTINUE;
}

void stateClientStart(AppContext &ctx) {
    if (!ctx.daemonRunning || ctx.selftest || ctx.stateFd >= 0) {
        return;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "/tmp/awcc.sock");
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    const std::string cmd = "subscribe";
    if (write(fd, cmd.c_str(), cmd.size()) < 0) {
        close(fd);
        return;
    }
    ctx.stateFd = fd;
    ctx.stateWatch = g_unix_fd_add(
        fd, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), onStateReadable, &ctx);
    LOG_S(INFO) << "已订阅 daemon 状态（DESIGN 第九节）";
}

// ── 灯效 ────────────────────────────────────────────────────────────────────

struct EffectSpec {
    const char *label; // (msgid)
    void (*apply)(EffectController &, AppContext &, uint32_t);
};

void applyStatic(EffectController &fx, AppContext &, uint32_t color) {
    fx.StaticColor(color);
}
void applyBreathe(EffectController &fx, AppContext &, uint32_t color) {
    fx.Breathe(color);
}
void applySpectrum(EffectController &fx, AppContext &ctx, uint32_t) {
    fx.Spectrum(ctx.duration);
}
void applyWave(EffectController &fx, AppContext &, uint32_t color) {
    fx.Wave(color);
}
void applyRainbow(EffectController &fx, AppContext &ctx, uint32_t) {
    fx.Rainbow(ctx.duration);
}
void applyBackAndForth(EffectController &fx, AppContext &, uint32_t color) {
    fx.BackAndForth(color);
}
void applyDefaultBlue(EffectController &fx, AppContext &, uint32_t) {
    fx.DefaultBlue();
}

constexpr EffectSpec kEffects[] = {
    {N_("Static"), applyStatic},   {N_("Breathe"), applyBreathe},
    {N_("Spectrum"), applySpectrum}, {N_("Wave"), applyWave},
    {N_("Rainbow"), applyRainbow}, {N_("Back and forth"), applyBackAndForth},
    {N_("Default blue"), applyDefaultBlue},
};

void onEffectClicked(GtkButton *btn, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    const auto index =
        static_cast<size_t>(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "awcc-effect")));
    if (ctx->effects != nullptr && index < std::size(kEffects)) {
        kEffects[index].apply(*ctx->effects, *ctx, ctx->color);
        LOG_S(INFO) << "Applied effect: " << kEffects[index].label;
    }
}

void onColorChanged(GObject *object, GParamSpec *, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
    if (rgba == nullptr) {
        return;
    }
    const auto red = static_cast<uint32_t>(std::lround(rgba->red * 255.0));
    const auto green = static_cast<uint32_t>(std::lround(rgba->green * 255.0));
    const auto blue = static_cast<uint32_t>(std::lround(rgba->blue * 255.0));
    ctx->color = (red << 16) | (green << 8) | blue;
}

GtkWidget *buildLightingCard(AppContext &ctx, bool withDuration) {
    GtkWidget *card = makeCard(N_("Keyboard lighting"), N_("Applies immediately"));

    GtkWidget *grid = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 7);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 3);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 8);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 8);
    for (size_t i = 0; i < std::size(kEffects); ++i) {
        GtkWidget *btn = gtk_button_new_with_label(_(kEffects[i].label));
        gtk_widget_add_css_class(btn, "effect-button");
        g_object_set_data(G_OBJECT(btn), "awcc-effect", GINT_TO_POINTER(i));
        g_signal_connect(btn, "clicked", G_CALLBACK(onEffectClicked), &ctx);
        gtk_flow_box_append(GTK_FLOW_BOX(grid), btn);
    }
    gtk_box_append(GTK_BOX(card), grid);

    // 颜色
    GtkWidget *colorRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(colorRow), makeLabel(N_("Color"), "row-label"));
    GtkWidget *colorBtn =
        gtk_color_dialog_button_new(gtk_color_dialog_new());
    GdkRGBA rgba{};
    rgba.red = ((ctx.color >> 16) & 0xff) / 255.0;
    rgba.green = ((ctx.color >> 8) & 0xff) / 255.0;
    rgba.blue = (ctx.color & 0xff) / 255.0;
    rgba.alpha = 1.0;
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorBtn), &rgba);
    g_signal_connect(colorBtn, "notify::rgba", G_CALLBACK(onColorChanged), &ctx);
    gtk_box_append(GTK_BOX(colorRow), colorBtn);
    gtk_box_append(GTK_BOX(card), colorRow);

    // 亮度
    GtkWidget *brightnessScale =
        gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_range_set_value(GTK_RANGE(brightnessScale), ctx.brightness);
    gtk_scale_set_draw_value(GTK_SCALE(brightnessScale), FALSE);
    GtkWidget *brightnessValue = makeValueLabel();
    // 订阅推来新亮度时要回填这两个控件（见 applyStateLine）
    ctx.brightnessScale = brightnessScale;
    ctx.brightnessLabel = brightnessValue;
    gchar *brightnessText = g_strdup_printf("%d%%", ctx.brightness);
    gtk_label_set_text(GTK_LABEL(brightnessValue), brightnessText);
    g_free(brightnessText);
    g_signal_connect(brightnessScale, "value-changed", G_CALLBACK(onBrightnessChanged), &ctx);
    // 值标签跟着滑块走
    g_signal_connect_data(
        brightnessScale, "value-changed",
        G_CALLBACK(+[](GtkRange *range, gpointer data) {
            GtkWidget *label = GTK_WIDGET(data);
            gchar *text = g_strdup_printf("%d%%", static_cast<int>(gtk_range_get_value(range)));
            gtk_label_set_text(GTK_LABEL(label), text);
            g_free(text);
        }),
        brightnessValue, nullptr, G_CONNECT_DEFAULT);
    gtk_box_append(GTK_BOX(card),
                   makeSliderRow(N_("Brightness"), brightnessScale, brightnessValue));

    if (withDuration) {
        GtkWidget *durationScale =
            gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 100, 5000, 100);
        gtk_range_set_value(GTK_RANGE(durationScale), ctx.duration);
        gtk_scale_set_draw_value(GTK_SCALE(durationScale), FALSE);
        GtkWidget *durationValue = makeValueLabel();
        gchar *durationText = g_strdup_printf("%u ms", ctx.duration);
        gtk_label_set_text(GTK_LABEL(durationValue), durationText);
        g_free(durationText);
        g_signal_connect_data(
            durationScale, "value-changed",
            G_CALLBACK(+[](GtkRange *range, gpointer data) {
                auto *context = static_cast<AppContext *>(
                    g_object_get_data(G_OBJECT(range), "awcc-ctx"));
                context->duration =
                    static_cast<uint16_t>(gtk_range_get_value(range));
                GtkWidget *label = GTK_WIDGET(data);
                gchar *text =
                    g_strdup_printf("%u ms", static_cast<unsigned>(context->duration));
                gtk_label_set_text(GTK_LABEL(label), text);
                g_free(text);
            }),
            durationValue, nullptr, G_CONNECT_DEFAULT);
        g_object_set_data(G_OBJECT(durationScale), "awcc-ctx", &ctx);
        gtk_box_append(GTK_BOX(card),
                       makeSliderRow(N_("Duration"), durationScale, durationValue));
    }
    return card;
}


// ── 风扇与睿频 ──────────────────────────────────────────────────────────────

gboolean onTurboStateSet(GtkSwitch *sw, gboolean state, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (ctx->acpi != nullptr) {
        ctx->acpi->setTurboBoost(state == TRUE);
    }
    (void)sw;
    return FALSE; // 让开关自己更新外观
}

GtkWidget *buildFanCard(AppContext &ctx) {
    GtkWidget *card = makeCard(N_("Fan and turbo"), N_("Applies immediately"));

    const bool hasFanBoost =
        ctx.acpi == nullptr || ctx.acpi->hasFeature(FeatureSet::FanBoost);
    if (hasFanBoost) {
        // CPU / GPU boost 的当前值要读 ACPI；只有 daemon 在跑时才读，否则会弹授权框
        int cpuBoost = 0;
        int gpuBoost = 0;
        if (ctx.daemonRunning && ctx.thermals != nullptr) {
            cpuBoost = std::clamp(ctx.thermals->getCpuBoost(), 0, 100);
            gpuBoost = std::clamp(ctx.thermals->getGpuBoost(), 0, 100);
        }

        GtkWidget *cpuScale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        gtk_range_set_value(GTK_RANGE(cpuScale), cpuBoost);
        gtk_scale_set_draw_value(GTK_SCALE(cpuScale), FALSE);
        GtkWidget *cpuValue = makeValueLabel();
        gchar *cpuText = g_strdup_printf("%d%%", cpuBoost);
        gtk_label_set_text(GTK_LABEL(cpuValue), cpuText);
        g_free(cpuText);
        g_signal_connect(cpuScale, "value-changed", G_CALLBACK(onCpuBoostChanged), &ctx);
        g_signal_connect_data(
            cpuScale, "value-changed",
            G_CALLBACK(+[](GtkRange *range, gpointer data) {
                GtkWidget *label = GTK_WIDGET(data);
                gchar *text =
                    g_strdup_printf("%d%%", static_cast<int>(gtk_range_get_value(range)));
                gtk_label_set_text(GTK_LABEL(label), text);
                g_free(text);
            }),
            cpuValue, nullptr, G_CONNECT_DEFAULT);
        gtk_box_append(GTK_BOX(card),
                       makeSliderRow(N_("CPU fan"), cpuScale, cpuValue));

        GtkWidget *gpuScale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        gtk_range_set_value(GTK_RANGE(gpuScale), gpuBoost);
        gtk_scale_set_draw_value(GTK_SCALE(gpuScale), FALSE);
        GtkWidget *gpuValue = makeValueLabel();
        gchar *gpuText = g_strdup_printf("%d%%", gpuBoost);
        gtk_label_set_text(GTK_LABEL(gpuValue), gpuText);
        g_free(gpuText);
        g_signal_connect(gpuScale, "value-changed", G_CALLBACK(onGpuBoostChanged), &ctx);
        g_signal_connect_data(
            gpuScale, "value-changed",
            G_CALLBACK(+[](GtkRange *range, gpointer data) {
                GtkWidget *label = GTK_WIDGET(data);
                gchar *text =
                    g_strdup_printf("%d%%", static_cast<int>(gtk_range_get_value(range)));
                gtk_label_set_text(GTK_LABEL(label), text);
                g_free(text);
            }),
            gpuValue, nullptr, G_CONNECT_DEFAULT);
        gtk_box_append(GTK_BOX(card),
                       makeSliderRow(N_("GPU fan"), gpuScale, gpuValue));
    } else {
        gtk_box_append(GTK_BOX(card), makeBadge(N_("Not available on this model"), "badge-na"));
    }

    if (ctx.acpi == nullptr || ctx.acpi->hasFeature(FeatureSet::AutoBoost)) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_box_append(GTK_BOX(row), makeLabel(N_("Turbo boost"), "row-label"));
        GtkWidget *sw = gtk_switch_new();
        gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);
        if (ctx.acpi != nullptr) {
            gtk_switch_set_active(GTK_SWITCH(sw), ctx.acpi->getTurboBoost());
        }
        g_signal_connect(sw, "state-set", G_CALLBACK(onTurboStateSet), &ctx);
        gtk_box_append(GTK_BOX(row), sw);
        gtk_box_append(GTK_BOX(card), row);
    }
    return card;
}


// ── 设备信息 ────────────────────────────────────────────────────────────────

struct FeatureSpec {
    FeatureSet feature;
    const char *label; // (msgid)
};

constexpr FeatureSpec kFeatures[] = {
    {FeatureSet::FanBoost, N_("Fan boost")},
    {FeatureSet::ThermalModes, N_("Thermal modes")},
    {FeatureSet::AutoBoost, N_("Turbo boost")},
    {FeatureSet::CpuTemp, N_("CPU temperature")},
    {FeatureSet::GpuTemp, N_("GPU temperature")},
    {FeatureSet::BrightnessControl, N_("Brightness control")},
    {FeatureSet::GModeToggle, N_("G mode toggle")},
};

GtkWidget *addInfoRow(GtkWidget *card, const char *label, const std::string &value) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "info-row");
    GtkWidget *key = makeLabel(_(label), "row-label");
    gtk_widget_set_size_request(key, 140, -1);
    gtk_box_append(GTK_BOX(row), key);
    GtkWidget *val = gtk_label_new(value.c_str());
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_widget_add_css_class(val, "info-value");
    // 官方表格是「左标签、右数值」，数值贴着列右边
    gtk_widget_set_hexpand(val, true);
    gtk_widget_set_halign(val, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(row), val);
    gtk_box_append(GTK_BOX(card), row);
    return val; // 调用方需要时可以把数值标签登记起来，改数据后刷新
}

void addInfoSeparator(GtkWidget *card) {
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_add_css_class(sep, "info-sep");
    gtk_box_append(GTK_BOX(card), sep);
}

GtkWidget *buildAboutCard(AppContext &ctx) {
    GtkWidget *card = makeCard(N_("Device"), nullptr);
    addInfoRow(card, N_("Model"), Helper::getDeviceName());
    addInfoSeparator(card);
    addInfoRow(card, N_("Version"), VERSION);
    addInfoSeparator(card);
    addInfoRow(card, N_("Keyboard zones"),
               std::to_string(ctx.acpi != nullptr ? ctx.acpi->getKeyboardZones().size() : 0));
    addInfoSeparator(card);
    // 这行要跟着模式变化刷新，所以把数值标签登记到 ctx；只有 daemon 在跑时才读 ACPI，
    // 否则读它同样会弹授权框（DESIGN.md 第三节）
    ctx.modeLabels.push_back(addInfoRow(
        card, N_("Current mode"),
        ctx.daemonRunning && ctx.thermals != nullptr ? ctx.thermals->getCurrentModeName()
                                                    : modeLabel(ctx.currentMode)));
    addInfoSeparator(card);
    addInfoRow(card, N_("Daemon"),
               ctx.daemonRunning ? _("running (socket)") : _("not running (pkexec fallback)"));
    addInfoSeparator(card);

    std::string features;
    if (ctx.acpi != nullptr) {
        for (const FeatureSpec &spec : kFeatures) {
            if (!ctx.acpi->hasFeature(spec.feature)) {
                continue;
            }
            if (!features.empty()) {
                features += " · ";
            }
            features += _(spec.label);
        }
    }
    addInfoRow(card, N_("Features"), features.empty() ? "-" : features);
    return card;
}

// ── 页面组装 ────────────────────────────────────────────────────────────────

// 页面内容盒与滚动容器分开：**不要**用 gtk_scrolled_window_get_child() 反查内容盒——
// GTK4 内部会插一层 GtkViewport，get_child() 拿到的是那个 viewport（实测），拿它当 GtkBox
// 用会一路 gtk_box_append 断言失败。所以内容盒由调用方自己持有，最后再交给 wrapScrolled()。
GtkWidget *makePageContent() {
    GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content, 16);
    gtk_widget_set_margin_end(content, 16);
    gtk_widget_set_margin_top(content, 16);
    gtk_widget_set_margin_bottom(content, 16);
    return content;
}

GtkWidget *wrapScrolled(GtkWidget *content) {
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), content);
    return scrolled;
}

// 占位页：标题 + 「施工中」徽标 + 一句「缺什么」。真页面接完就换成下面的 builder。
GtkWidget *makePlaceholderPage(const char *title, const char *note) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *badge = makeBadge(N_("Under construction"), "badge-construction");
    gtk_widget_set_halign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), badge);

    GtkWidget *titleLabel = makeLabel(_(title), "page-title");
    gtk_widget_set_halign(titleLabel, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), titleLabel);

    if (note != nullptr && *note != '\0') {
        GtkWidget *noteLabel = makeLabel(_(note), "page-note");
        gtk_label_set_wrap(GTK_LABEL(noteLabel), TRUE);
        gtk_label_set_justify(GTK_LABEL(noteLabel), GTK_JUSTIFY_CENTER);
        gtk_widget_set_size_request(noteLabel, 460, -1);
        gtk_widget_set_halign(noteLabel, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), noteLabel);
    }
    return box;
}

// ── 环形仪表（官方版式）──────────────────────────────────────────────────
// 灰轨道 + 从 12 点顺时针的红色弧 + 中间大号白字 + 单位，仪表下方是红色名称与灰色说明。
// 数据还没接（M1 遥测层）时 value < 0：只画灰轨道、中间显示「—」，绝不画成 0 骗人。
constexpr double kGaugeStroke = 8.0;  // 量自官方截图：描边 ≈8 逻辑像素
constexpr int kGaugeSize = 130;       // 量自官方截图：外径 ≈130 逻辑像素

enum class GaugeStyle { Smooth, Jagged }; // 锯齿环用于风扇转速（官方散热页）

struct GaugeData {
    double fraction = 0.0; // 0..1，未知时忽略
    bool unknown = true;
    GaugeStyle style = GaugeStyle::Smooth;
};

void drawGauge(GtkDrawingArea *area, cairo_t *cr, int width, int height,
               gpointer data) {
    auto *gauge = static_cast<GaugeData *>(data);
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    const double radius = std::min(cx, cy) - kGaugeStroke;
    const double start = -M_PI / 2.0; // 12 点方向起，顺时针
    const double full = 2.0 * M_PI - 0.35; // 留一点缺口，和官方一样

    cairo_set_line_width(cr, kGaugeStroke);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

    const bool filled = !gauge->unknown && gauge->fraction > 0.0;
    const double redEnd = start + full * gauge->fraction;

    if (gauge->style == GaugeStyle::Jagged) {
        // 锯齿环：沿圆周排一圈小径向刻度（官方风扇转速那种）
        constexpr int kTicks = 56;
        for (int i = 0; i < kTicks; ++i) {
            const double a = start + full * (static_cast<double>(i) / kTicks);
            const bool lit = filled && a <= redEnd;
            cairo_set_source_rgb(cr, lit ? 0xfd / 255.0 : 0x3a / 255.0,
                                 lit ? 0x56 / 255.0 : 0x3f / 255.0,
                                 lit ? 0x43 / 255.0 : 0x47 / 255.0);
            const double inner = radius - kGaugeStroke * 1.4;
            cairo_move_to(cr, cx + std::cos(a) * inner, cy + std::sin(a) * inner);
            cairo_line_to(cr, cx + std::cos(a) * (radius + kGaugeStroke / 2),
                          cy + std::sin(a) * (radius + kGaugeStroke / 2));
            cairo_stroke(cr);
        }
        return;
    }

    cairo_set_source_rgb(cr, 0x3a / 255.0, 0x3f / 255.0, 0x47 / 255.0);
    cairo_arc(cr, cx, cy, radius, start, start + full);
    cairo_stroke(cr);

    if (filled) {
        cairo_set_source_rgb(cr, 0xfd / 255.0, 0x56 / 255.0, 0x43 / 255.0);
        cairo_arc(cr, cx, cy, radius, start, redEnd);
        cairo_stroke(cr);
    }
}

GtkWidget *makeGauge(double fraction, bool unknown, const char *valueText,
                     const char *unit, const char *label, const char *sublabel,
                     GaugeStyle style = GaugeStyle::Smooth) {
    auto *gauge = new GaugeData{fraction, unknown, style};

    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, kGaugeSize, kGaugeSize);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), drawGauge, gauge,
                                   [](gpointer data) { delete static_cast<GaugeData *>(data); });

    GtkWidget *valueBox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_halign(valueBox, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(valueBox, GTK_ALIGN_CENTER);
    GtkWidget *value = gtk_label_new(valueText);
    gtk_widget_add_css_class(value, "gauge-value");
    gtk_box_append(GTK_BOX(valueBox), value);
    GtkWidget *unitLabel = gtk_label_new(unit);
    gtk_widget_add_css_class(unitLabel, "gauge-unit");
    gtk_box_append(GTK_BOX(valueBox), unitLabel);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), area);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), valueBox);

    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(column, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(column), overlay);
    GtkWidget *name = gtk_label_new(_(label));
    gtk_widget_add_css_class(name, "gauge-label");
    gtk_box_append(GTK_BOX(column), name);
    if (sublabel != nullptr) {
        GtkWidget *sub = gtk_label_new(_(sublabel));
        gtk_widget_add_css_class(sub, "gauge-sublabel");
        gtk_box_append(GTK_BOX(column), sub);
    }
    return column;
}

// 官方 性能·概况 页：四列，每列「标题行(小图标+灰字) + 环形表 + 参数表」。
// 参数表暂时用「—」占位（M1 遥测层接上后换成真值，见 TODO.md）。
GtkWidget *makeParamTable(const char *const *rows, size_t rowCount) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "param-table");
    for (size_t i = 0; i < rowCount; ++i) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(row, "info-row");
        GtkWidget *key = makeLabel(_(rows[i]), "row-label");
        gtk_widget_set_hexpand(key, true);
        gtk_box_append(GTK_BOX(row), key);
        GtkWidget *val = gtk_label_new("—");
        gtk_widget_add_css_class(val, "info-value");
        gtk_box_append(GTK_BOX(row), val);
        gtk_box_append(GTK_BOX(card), row);
        if (i + 1 < rowCount) {
            GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_add_css_class(sep, "info-sep");
            gtk_box_append(GTK_BOX(card), sep);
        }
    }
    return card;
}

// 官方里每列顶上那行：一个小图标 + 灰色标题（CPU 概况 / GPU 概况 …）
GtkWidget *makeColumnHeader(const char *iconName, const char *title) {
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(header, GTK_ALIGN_CENTER);
    GtkWidget *icon = gtk_image_new_from_icon_name(iconName);
    gtk_widget_add_css_class(icon, "column-icon");
    gtk_box_append(GTK_BOX(header), icon);
    GtkWidget *label = makeLabel(_(title), "column-title");
    gtk_box_append(GTK_BOX(header), label);
    return header;
}

// ── 背景（默认纯黑 / 跟随亮暗 / 自定义颜色 / 图片 / 官方辉光）──────────────
// 背景色与图片来自用户配置，用一份运行期 CSS 覆盖 style.css 的默认值；
// 「跟随系统」时给窗口挂 .light 类，颜色覆盖写在 style.css 的 window.light.* 里。
void applyBackground(AppContext &ctx) {
    if (ctx.backgroundProvider != nullptr) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(ctx.backgroundProvider));
        g_object_unref(ctx.backgroundProvider);
        ctx.backgroundProvider = nullptr;
    }
    const std::string css = Config::backgroundCss(ctx.config);
    if (!css.empty()) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(provider, css.c_str());
        // 优先级比 style.css 高一档，才能覆盖它的默认背景
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        ctx.backgroundProvider = provider;
    }

    // 「跟随系统」时必须把样式管理器的选择权交回系统，否则永远是暗色、跟不到亮色
    AdwStyleManager *styleManager = adw_style_manager_get_default();
    adw_style_manager_set_color_scheme(
        styleManager, ctx.config.background.mode == Config::BackgroundMode::Theme
                          ? ADW_COLOR_SCHEME_DEFAULT
                          : ADW_COLOR_SCHEME_PREFER_DARK);

    gtk_widget_remove_css_class(ctx.window, "light");
    gtk_widget_remove_css_class(ctx.window, "image-bg");
    if (ctx.config.background.mode == Config::BackgroundMode::Image) {
        gtk_widget_add_css_class(ctx.window, "image-bg");
    }
    const bool systemLight =
        ctx.forceLight ||
        (ctx.config.background.mode == Config::BackgroundMode::Theme &&
         adw_style_manager_get_dark(adw_style_manager_get_default()) == FALSE);
    if (systemLight) {
        gtk_widget_add_css_class(ctx.window, "light");
    }
}

void refreshBackgroundRows(AppContext &ctx) {
    if (ctx.bgColorRow != nullptr) {
        gtk_widget_set_sensitive(
            ctx.bgColorRow,
            ctx.config.background.mode == Config::BackgroundMode::Color);
    }
    if (ctx.bgImageRow != nullptr) {
        gtk_widget_set_sensitive(
            ctx.bgImageRow,
            ctx.config.background.mode == Config::BackgroundMode::Image);
    }
    if (ctx.bgImageLabel != nullptr) {
        gtk_label_set_text(GTK_LABEL(ctx.bgImageLabel),
                           ctx.config.background.image.empty()
                               ? _("(none)")
                               : ctx.config.background.image.c_str());
    }
}

void commitBackground(AppContext &ctx) {
    Config::save(ctx.config);
    applyBackground(ctx);
    refreshBackgroundRows(ctx);
}

struct BackgroundModeSpec {
    Config::BackgroundMode mode;
    const char *label; // (msgid)
    const char *note;  // (msgid) 可为空
};

constexpr BackgroundModeSpec kBackgroundModes[] = {
    {Config::BackgroundMode::Black, N_("Pure black"), N_("Default")},
    {Config::BackgroundMode::Theme, N_("Follow system"), nullptr},
    {Config::BackgroundMode::Glow, N_("Official glow"), nullptr},
    {Config::BackgroundMode::Color, N_("Custom color"), nullptr},
    {Config::BackgroundMode::Image, N_("Image"), nullptr},
};

void onBackgroundModeToggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) {
        return;
    }
    auto *ctx = static_cast<AppContext *>(data);
    ctx->config.background.mode = static_cast<Config::BackgroundMode>(
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "awcc-bg-mode")));
    commitBackground(*ctx);
}

void onBackgroundColorChanged(GObject *object, GParamSpec *, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    const GdkRGBA *rgba =
        gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(object));
    if (rgba == nullptr) {
        return;
    }
    gchar *hex = g_strdup_printf("#%02x%02x%02x",
                                 static_cast<unsigned>(lround(rgba->red * 255.0)),
                                 static_cast<unsigned>(lround(rgba->green * 255.0)),
                                 static_cast<unsigned>(lround(rgba->blue * 255.0)));
    ctx->config.background.color = hex;
    g_free(hex);
    if (ctx->config.background.mode == Config::BackgroundMode::Color) {
        commitBackground(*ctx);
    } else {
        Config::save(ctx->config); // 先记下，等切到该模式再用
    }
}

void onImageChosen(GObject *source, GAsyncResult *result, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    GError *error = nullptr;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    if (file != nullptr) {
        if (gchar *path = g_file_get_path(file); path != nullptr) {
            ctx->config.background.image = path;
            ctx->config.background.mode = Config::BackgroundMode::Image;
            g_free(path);
            commitBackground(*ctx);
        }
        g_object_unref(file);
    } else if (error != nullptr) {
        LOG_S(INFO) << "选择背景图片取消或失败：" << error->message;
        g_error_free(error); // 用户取消属正常
    }
    g_object_unref(source); // 与 open 前那次 ref 配对
}

void onChooseImageClicked(GtkButton *, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Choose background image"));
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("Images"));
    gtk_file_filter_add_mime_type(filter, "image/*");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dialog, GTK_WINDOW(ctx->window), nullptr, onImageChosen, ctx);
    g_object_unref(filters);
    g_object_unref(filter);
    // 异步期间要保持 dialog 活着，回调里 unref
    g_object_ref(dialog);
    g_object_unref(dialog);
}

void onResetBackgroundClicked(GtkButton *, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    ctx->config.background = Config::Background{};
    Config::save(ctx->config);
    applyBackground(*ctx);
    refreshBackgroundRows(*ctx);
}

// 「设置 · 外观」页：背景模式 + 颜色 + 图片 + 恢复默认 + 配置文件位置
GtkWidget *buildAppearancePage(AppContext &ctx) {
    GtkWidget *content = makePageContent();

    GtkWidget *card = makeCard(N_("Background"), N_("Saved to the user config file"));
    GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(modes, GTK_ALIGN_START);
    GtkToggleButton *leader = nullptr;
    for (const BackgroundModeSpec &spec : kBackgroundModes) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(_(spec.label));
        gtk_widget_add_css_class(btn, "mode-button");
        if (spec.note != nullptr) {
            gtk_widget_set_tooltip_text(btn, _(spec.note));
        }
        g_object_set_data(G_OBJECT(btn), "awcc-bg-mode", GINT_TO_POINTER(spec.mode));
        if (leader == nullptr) {
            leader = GTK_TOGGLE_BUTTON(btn);
        } else {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), leader);
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(onBackgroundModeToggled), &ctx);
        gtk_box_append(GTK_BOX(modes), btn);
        if (spec.mode == ctx.config.background.mode) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);
        }
    }
    gtk_box_append(GTK_BOX(card), modes);

    // 自定义颜色
    GtkWidget *colorRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(colorRow), makeLabel(N_("Color"), "row-label"));
    GtkWidget *colorBtn = gtk_color_dialog_button_new(gtk_color_dialog_new());
    GdkRGBA rgba{};
    if (!gdk_rgba_parse(&rgba, ctx.config.background.color.c_str())) {
        rgba = GdkRGBA{0.0, 0.0, 0.0, 1.0};
    }
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colorBtn), &rgba);
    g_signal_connect(colorBtn, "notify::rgba", G_CALLBACK(onBackgroundColorChanged), &ctx);
    gtk_box_append(GTK_BOX(colorRow), colorBtn);
    gtk_box_append(GTK_BOX(card), colorRow);
    ctx.bgColorRow = colorRow;

    // 自定义图片
    GtkWidget *imageRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(imageRow), makeLabel(N_("Image"), "row-label"));
    GtkWidget *choose = gtk_button_new_with_label(_("Choose image…"));
    g_signal_connect(choose, "clicked", G_CALLBACK(onChooseImageClicked), &ctx);
    gtk_box_append(GTK_BOX(imageRow), choose);
    GtkWidget *imageLabel = gtk_label_new("");
    gtk_widget_add_css_class(imageLabel, "card-note");
    gtk_label_set_ellipsize(GTK_LABEL(imageLabel), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(imageLabel, true);
    gtk_widget_set_halign(imageLabel, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(imageRow), imageLabel);
    gtk_box_append(GTK_BOX(card), imageRow);
    ctx.bgImageRow = imageRow;
    ctx.bgImageLabel = imageLabel;

    GtkWidget *reset = gtk_button_new_with_label(_("Reset to default"));
    gtk_widget_set_halign(reset, GTK_ALIGN_START);
    g_signal_connect(reset, "clicked", G_CALLBACK(onResetBackgroundClicked), &ctx);
    gtk_box_append(GTK_BOX(card), reset);

    gtk_box_append(GTK_BOX(content), card);

    GtkWidget *pathCard = makeCard(N_("Config file"), nullptr);
    gtk_box_append(GTK_BOX(pathCard),
                   makeLabel(Config::configPath().c_str(), "card-note"));
    gtk_box_append(GTK_BOX(content), pathCard);

    refreshBackgroundRows(ctx);
    return wrapScrolled(content);
}

// ── 官方版式：主页 / 性能·概况 / 性能·散热 ────────────────────────────────
// 构成读自官方截图（ADAPTATION.md 第九节），尺寸见 DESIGN.md 第三节。

// 主页（官方 _3_9）：模式行 → 整宽机身图大卡 → 底部一行两卡：
// 左 2/3 四个环形仪表（含「性能 / 散热」切换），右 1/3 游戏库卡。
GtkWidget *buildHomePage(AppContext &ctx) {
    GtkWidget *content = makePageContent();
    gtk_box_append(GTK_BOX(content), buildModeRow(ctx));

    GtkWidget *hero = makeCard(N_("Device"), nullptr);
    gtk_widget_set_size_request(hero, -1, 230);
    GtkWidget *heroInner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(heroInner, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(heroInner, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(heroInner, true);
    gtk_box_append(GTK_BOX(heroInner),
                   makeBadge(N_("Under construction"), "badge-construction"));
    gtk_box_append(GTK_BOX(heroInner),
                   makeLabel(N_("Official here is a laptop render; we have no such asset yet."),
                             "card-note"));
    gtk_box_append(GTK_BOX(hero), heroInner);
    gtk_box_append(GTK_BOX(content), hero);

    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    // 左：四个环形仪表（数据待 M1 遥测层，先画成「—」）
    GtkWidget *gauges = makeCard(N_("Overview"), nullptr);
    GtkWidget *gaugeRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(gaugeRow, GTK_ALIGN_CENTER);
    struct MiniGauge {
        const char *label;
        const char *sub;
        const char *unit;
    };
    const MiniGauge minis[] = {
        {N_("CPU"), N_("Utilization"), "%"},
        {N_("GPU"), N_("Utilization"), "%"},
        {N_("Memory"), N_("Usage"), "GB"},
        {N_("C: disk"), N_("Free space"), "GB"},
    };
    for (const MiniGauge &mini : minis) {
        gtk_box_append(GTK_BOX(gaugeRow),
                       makeGauge(0.0, true, "—", mini.unit, mini.label, mini.sub));
    }
    gtk_box_append(GTK_BOX(gauges), gaugeRow);
    // 官方左下角是「性能 / 散热」分段切换
    GtkWidget *perfToggle = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(perfToggle, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(perfToggle, "subnav");
    for (const char *label : {N_("Performance"), N_("Thermal")}) {
        GtkWidget *btn = gtk_toggle_button_new_with_label(_(label));
        gtk_box_append(GTK_BOX(perfToggle), btn);
    }
    gtk_box_append(GTK_BOX(gauges), perfToggle);
    gtk_box_append(GTK_BOX(bottom), gauges);
    ctx.widthRatios.emplace_back(gauges, 0.66);

    // 右：游戏库（官方有「新游戏 / 最近 / 收藏 / 最常玩的游戏」标签页；Linux 无扫描来源）
    GtkWidget *library = makeCard(N_("Game library"), nullptr);
    GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_halign(tabs, GTK_ALIGN_CENTER);
    for (const char *label :
         {N_("New"), N_("Recent"), N_("Favorites"), N_("Most played")}) {
        gtk_box_append(GTK_BOX(tabs), makeLabel(label, "card-note"));
    }
    gtk_box_append(GTK_BOX(library), tabs);
    GtkWidget *libInner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_valign(libInner, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(libInner, GTK_ALIGN_CENTER);
    gtk_widget_set_vexpand(libInner, true);
    gtk_box_append(GTK_BOX(libInner),
                   makeBadge(N_("Under construction"), "badge-construction"));
    gtk_box_append(GTK_BOX(libInner),
                   makeLabel(N_("No game scanning source on Linux yet."), "card-note"));
    gtk_box_append(GTK_BOX(library), libInner);
    gtk_box_append(GTK_BOX(bottom), library);
    ctx.widthRatios.emplace_back(library, 0.33);

    gtk_box_append(GTK_BOX(content), bottom);
    return wrapScrolled(content);
}

// 性能 · 概况（官方 _4_9）：模式行 → 整宽卡内四等列（小图标＋灰标题 / 环形表 / 参数表）。
GtkWidget *buildOverviewPage(AppContext &ctx) {
    GtkWidget *content = makePageContent();
    gtk_box_append(GTK_BOX(content), buildModeRow(ctx));

    const char *cpuRows[] = {N_("Frequency (GHz)"), N_("Temperature"), N_("Power (W)"),
                             N_("Voltage (V)")};
    const char *gpuRows[] = {N_("Frequency (MHz)"), N_("Temperature"), N_("VRAM (MHz)")};
    const char *memRows[] = {N_("Available (GB)"), N_("Frequency (MHz)"),
                             N_("Unpaged (GB)"), N_("Cached (GB)")};
    const char *diskRows[] = {N_("Available (GB)"), N_("Read (MB/s)"), N_("Write (MB/s)"),
                              N_("Active time (%)")};
    struct Column {
        const char *icon;
        const char *title;
        const char *gaugeLabel;
        const char *gaugeSub;
        const char *unit;
        const char *const *rows;
        size_t rowCount;
    };
    const Column columns[] = {
        {"utilities-system-monitor-symbolic", N_("CPU overview"), N_("CPU"),
         N_("Utilization"), "%", cpuRows, std::size(cpuRows)},
        {"video-display-symbolic", N_("GPU overview"), N_("GPU"), N_("Utilization"), "%",
         gpuRows, std::size(gpuRows)},
        {"media-flash-symbolic", N_("Memory overview"), N_("Memory"), N_("Usage"), "GB",
         memRows, std::size(memRows)},
        {"drive-harddisk-symbolic", N_("Disk overview"), N_("C: disk"), N_("Free space"),
         "GB", diskRows, std::size(diskRows)},
    };

    // 官方式样：四列同处一张卡内，列间距 32（量自截图）
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(card, "card");
    GtkWidget *grid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
    for (const Column &spec : columns) {
        GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
        gtk_widget_set_halign(column, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(column), makeColumnHeader(spec.icon, spec.title));
        gtk_box_append(GTK_BOX(column),
                       makeGauge(0.0, true, "—", spec.unit, spec.gaugeLabel, spec.gaugeSub));
        gtk_box_append(GTK_BOX(column), makeParamTable(spec.rows, spec.rowCount));
        gtk_box_append(GTK_BOX(grid), column);
    }
    gtk_box_append(GTK_BOX(card), grid);
    gtk_box_append(GTK_BOX(content), card);

    GtkWidget *note = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(note, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(note),
                   makeBadge(N_("Under construction"), "badge-construction"));
    gtk_box_append(GTK_BOX(note),
                   makeLabel(N_("Values arrive with the M1 telemetry layer."), "card-note"));
    gtk_box_append(GTK_BOX(content), note);
    return wrapScrolled(content);
}

// 性能 · 散热（官方 _6_9）：模式行 → 卡内 2 个温度环 + 2 个风扇转速锯齿环。
GtkWidget *buildThermalPage(AppContext &ctx) {
    GtkWidget *content = makePageContent();
    gtk_box_append(GTK_BOX(content), buildModeRow(ctx));

    GtkWidget *card = makeCard(N_("Temperatures and fan speed"), nullptr);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 40);
    gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row),
                   makeGauge(0.0, true, "—", "°C", N_("CPU area"), N_("Temperature")));
    gtk_box_append(GTK_BOX(row),
                   makeGauge(0.0, true, "—", "°C", N_("GPU area"), N_("Temperature")));
    gtk_box_append(GTK_BOX(row),
                   makeGauge(0.0, true, "—", "%", N_("CPU"), N_("Fan speed"),
                             GaugeStyle::Jagged));
    gtk_box_append(GTK_BOX(row),
                   makeGauge(0.0, true, "—", "%", N_("GPU"), N_("Fan speed"),
                             GaugeStyle::Jagged));
    gtk_box_append(GTK_BOX(card), row);
    gtk_box_append(GTK_BOX(content), card);

    GtkWidget *note = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(note, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(note),
                   makeBadge(N_("Under construction"), "badge-construction"));
    gtk_box_append(GTK_BOX(note),
                   makeLabel(N_("Values arrive with the M1 telemetry layer."), "card-note"));
    gtk_box_append(GTK_BOX(content), note);
    return wrapScrolled(content);
}

GtkWidget *buildLightingPage(AppContext &ctx) {
    GtkWidget *content = makePageContent();
    gtk_box_append(GTK_BOX(content), buildLightingCard(ctx, true));

    GtkWidget *perKey = makeCard(N_("Per-key editing"), nullptr);
    gtk_box_append(GTK_BOX(perKey),
                   makeBadge(N_("Under construction"), "badge-construction"));
    gtk_box_append(GTK_BOX(perKey),
                   makeLabel(N_("The keyboard layout view and per-key colors come later."),
                             "card-note"));
    gtk_box_append(GTK_BOX(content), perKey);
    return wrapScrolled(content);
}

GtkWidget *buildAboutPage(AppContext &ctx) {
    GtkWidget *content = makePageContent();
    gtk_box_append(GTK_BOX(content), buildAboutCard(ctx));
    return wrapScrolled(content);
}

void onKeybindSelected(GObject *drop, GParamSpec *, gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (ctx->updatingKeybinds) {
        return; // 建行或刷新时设的值，不要再发回去
    }
    const int scan = GPOINTER_TO_INT(g_object_get_data(drop, "awcc-scan"));
    const guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
    const auto &actions = KeyBinds::AvailableActions();
    if (selected >= actions.size()) {
        return;
    }
    const std::string out =
        stateCommand("keybind-set " + std::to_string(scan) + " " + actions[selected]);
    auto *status = static_cast<GtkWidget *>(g_object_get_data(drop, "awcc-status"));
    if (status != nullptr) {
        gtk_label_set_text(GTK_LABEL(status),
                           out.rfind("ok", 0) == 0 ? _("Saved") : _("Failed"));
    }
}

GtkWidget *buildKeybindsPage(AppContext &ctx) {
    GtkWidget *content = makePageContent();

    if (!ctx.daemonRunning) {
        // 绑定表住在 daemon（读键盘的是它），daemon 没跑就只能给说明
        GtkWidget *card = makeCard(N_("Hotkeys"), nullptr);
        gtk_box_append(GTK_BOX(card),
                       makeBadge(N_("Not available on this model"), "badge-na"));
        gtk_box_append(GTK_BOX(card),
                       makeLabel(N_("The daemon is not running, so bindings cannot be read or "
                                    "written. They live in /etc/awcc/keybinds.conf."),
                                 "card-note"));
        gtk_box_append(GTK_BOX(content), card);
        return wrapScrolled(content);
    }

    GtkWidget *card = makeCard(N_("Hotkeys"), nullptr);
    gtk_box_append(GTK_BOX(card),
                   makeLabel(N_("Keys are matched by their EV_MSC scan code; on this model F4 "
                                "and F6 send no keycode at all."),
                             "card-note"));
    gtk_box_append(GTK_BOX(card),
                   makeLabel(N_("F2 sends KEY_MEDIA and F5 sends KEY_CAMERA, so the desktop may "
                                "also react unless you unbind those shortcuts."),
                             "card-note"));

    auto rows = std::make_shared<std::vector<std::pair<int, GtkWidget *>>>();
    const auto &actions = KeyBinds::AvailableActions();

    std::vector<KeyBinds::Bind> binds;
    {
        std::istringstream ss(stateCommand("keybind-list"));
        std::string line;
        while (std::getline(ss, line)) {
            const auto parsed = KeyBinds::ParseLine(line);
            if (parsed) {
                binds.push_back(*parsed);
            }
        }
    }
    if (binds.empty()) {
        gtk_box_append(GTK_BOX(card),
                       makeLabel(N_("Could not read the binding table from the daemon."),
                                 "card-note"));
        gtk_box_append(GTK_BOX(content), card);
        return wrapScrolled(content);
    }

    for (const KeyBinds::Bind &bind : binds) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class(row, "info-row");

        const char *keyLabel = KeyBinds::KeyLabel(bind.scan);
        gchar *title = g_strdup_printf("%s (%d)", keyLabel != nullptr ? keyLabel : "?",
                                       bind.scan);
        GtkWidget *name = makeLabel(title, "row-label");
        g_free(title);
        gtk_widget_set_size_request(name, 160, -1);
        gtk_box_append(GTK_BOX(row), name);

        GtkStringList *items = gtk_string_list_new(nullptr);
        for (const std::string &action : actions) {
            gtk_string_list_append(items, actionLabel(action));
        }
        GtkWidget *drop = gtk_drop_down_new(G_LIST_MODEL(items), nullptr);
        gtk_widget_set_hexpand(drop, true);
        // 先设当前值再连信号：否则建行时就会触发一次「保存」
        ctx.updatingKeybinds = true;
        for (size_t i = 0; i < actions.size(); ++i) {
            if (actions[i] == bind.action) {
                gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), i);
                break;
            }
        }
        ctx.updatingKeybinds = false;
        // 注意不能用 makeLabel("")：它内部会走 _()，而 gettext("") 按约定返回的是
        // 整个 .po 的头部（Project-Id-Version 那一坨），会当场渲染到界面上
        GtkWidget *status = gtk_label_new("");
        gtk_widget_add_css_class(status, "card-note");
        g_object_set_data(G_OBJECT(drop), "awcc-scan", GINT_TO_POINTER(bind.scan));
        g_object_set_data(G_OBJECT(drop), "awcc-status", status);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(onKeybindSelected), &ctx);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(row), status);
        gtk_box_append(GTK_BOX(card), row);
        rows->emplace_back(bind.scan, drop);
    }

    // 后端广播 keybinds 变化时重读一遍（另一个 GUI 实例或命令行改过也会跟上）
    ctx.keybindRefresh = [&ctx, rows] {
        const std::string text = stateCommand("keybind-list");
        std::map<int, std::string> current;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line)) {
            const auto parsed = KeyBinds::ParseLine(line);
            if (parsed) {
                current[parsed->scan] = parsed->action;
            }
        }
        const auto &list = KeyBinds::AvailableActions();
        ctx.updatingKeybinds = true;
        for (const auto &[scan, drop] : *rows) {
            const auto it = current.find(scan);
            if (it == current.end()) {
                continue;
            }
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i] == it->second) {
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), i);
                    break;
                }
            }
        }
        ctx.updatingKeybinds = false;
    };

    gtk_box_append(GTK_BOX(content), card);
    return wrapScrolled(content);
}

// ── 页面清单（表在 AppContext 之后定义，builder 用函数指针）──────────────────

const SubPage kPerformanceSubs[] = {
    {"overview", N_("Overview"), N_("Performance · Overview"),
     N_("CPU / memory / disk are readable (M1 telemetry); GPU is unavailable while the "
        "dGPU is runtime-suspended"),
     buildOverviewPage},
    {"thermal", N_("Thermal"), N_("Performance · Thermal"),
     N_("CPU / GPU temperatures and fan RPM are readable (M1 telemetry)"),
     buildThermalPage},
};

const SubPage kAlienfxSubs[] = {
    {"lighting", N_("Lighting"), N_("ALIENFX™ · Lighting"),
     N_("The seven effects and brightness have working backends (wired up in M3); the "
        "color picker becomes GtkColorDialogButton"),
     buildLightingPage},
    {"keybinds", N_("Key bindings"), N_("ALIENFX™ · Key bindings"),
     N_("Can only listen to the G key / light key; remapping needs EC support "
        "(unconfirmed)"),
     buildKeybindsPage},
};

const SubPage kSettingsSubs[] = {
    {"about", N_("About"), N_("Settings · About"), N_("Version comes from the VERSION macro; doable"),
     buildAboutPage},
    {"appearance", N_("Appearance"), N_("Settings · Appearance"),
     N_("Background can be pure black, follow the system, a custom color or an image"),
     buildAppearancePage},
    {"overlay", N_("Overlay"), N_("Settings · Overlay"),
     N_("There is no in-game overlay on Linux → marked as not applicable"), nullptr},
    {"onboarding", N_("Onboarding"), N_("Settings · Onboarding"),
     N_("Onboarding flow is undecided"), nullptr},
    {"presets", N_("Presets"), N_("Settings · Global presets"),
     N_("Local preferences are doable; per-game switching has no game detection"),
     nullptr},
    {"performance", N_("Performance"), N_("Settings · Performance"),
     N_("Overclocking is limited by the EC (capability unknown); Windows power saving "
        "is not applicable"),
     nullptr},
};

const NavEntry kNav[] = {
    {"home", N_("Home · Active"),
     N_("Power modes, lighting and brightness have working backends; the four ring "
        "gauges wait for the M1 telemetry layer"),
     nullptr, 0, buildHomePage},
    {"performance", N_("Performance"), N_("Overview and Thermal are sub-pages"),
     kPerformanceSubs, std::size(kPerformanceSubs), nullptr},
    {"alienfx", N_("ALIENFX™"), N_("Lighting and Key bindings are sub-pages"), kAlienfxSubs,
     std::size(kAlienfxSubs), nullptr},
    {"macro", N_("Macros"), N_("Not present upstream, so no backend exists"), nullptr, 0,
     nullptr},
    {"library", N_("Library"),
     N_("No game scanning source on Linux; manual add or placeholder"), nullptr, 0, nullptr},
    {"settings", N_("Settings"), N_("Six sub-pages, per item in M4"), kSettingsSubs,
     std::size(kSettingsSubs), nullptr},
    {"help", N_("Help"), N_("Content and links are undecided"), nullptr, 0, nullptr},
};

// ── 外壳与导航 ──────────────────────────────────────────────────────────────

void setCrumb(AppContext &ctx, const char *page, const char *sub) {
    gtk_label_set_text(GTK_LABEL(ctx.crumbMain), _(page));
    gtk_label_set_text(GTK_LABEL(ctx.crumbSub), sub != nullptr ? _(sub) : _("System"));
}

void onSubToggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) {
        return;
    }
    gtk_stack_set_visible_child_name(GTK_STACK(data),
                                     gtk_widget_get_name(GTK_WIDGET(btn)));
}

// 带子页的页面：顶部一排子页切换按钮 + 内层 GtkStack（子页有自己的 builder 就建真页面）
GtkWidget *makeSectionPage(const NavEntry &entry, AppContext &ctx) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *inner = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(inner), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(inner, true);
    gtk_box_append(GTK_BOX(page), inner);

    // 官方把「概况 / 散热」这类分段控件放在底部居中，不是在顶部
    GtkWidget *bottomBar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bottomBar, "subnav-bar");
    GtkWidget *subnav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(subnav, "subnav");
    gtk_widget_set_halign(subnav, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(subnav, true);
    gtk_box_append(GTK_BOX(bottomBar), subnav);
    gtk_box_append(GTK_BOX(page), bottomBar);

    GtkToggleButton *leader = nullptr;
    for (size_t i = 0; i < entry.subCount; ++i) {
        const SubPage &sub = entry.subs[i];
        GtkWidget *btn = gtk_toggle_button_new_with_label(_(sub.shortLabel));
        gtk_widget_set_name(btn, sub.name);
        if (leader == nullptr) {
            leader = GTK_TOGGLE_BUTTON(btn);
        } else {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), leader);
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(onSubToggled), inner);
        gtk_box_append(GTK_BOX(subnav), btn);

        GtkWidget *child = sub.build != nullptr ? sub.build(ctx)
                                                : makePlaceholderPage(sub.title, sub.note);
        gtk_stack_add_named(GTK_STACK(inner), child, sub.name);
        ctx.pageLog.push_back(std::string("  └─ ") + sub.name + "  " + _(sub.title));
    }
    if (!ctx.initialSubPage.empty()) {
        // --ui-subpage：按名字选中子页（截图/自检用，只影响初始状态）
        for (GtkWidget *child = gtk_widget_get_first_child(subnav); child != nullptr;
             child = gtk_widget_get_next_sibling(child)) {
            if (GTK_IS_TOGGLE_BUTTON(child) &&
                ctx.initialSubPage == gtk_widget_get_name(child)) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(child), TRUE);
                return page;
            }
        }
    }
    if (leader != nullptr) {
        gtk_toggle_button_set_active(leader, TRUE);
    }
    return page;
}

void onNavToggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) {
        return;
    }
    auto *ctx = static_cast<AppContext *>(data);
    const char *name = gtk_widget_get_name(GTK_WIDGET(btn));
    gtk_stack_set_visible_child_name(GTK_STACK(ctx->stack), name);
    for (const NavEntry &entry : kNav) {
        if (std::strcmp(entry.name, name) == 0) {
            // 官方面包屑是「顶层页名 | 系统」，第二段固定，不随子页变化
            setCrumb(*ctx, entry.title, nullptr);
            return;
        }
    }
}

// ── 自检用的观测 ────────────────────────────────────────────────────────────

gboolean selftestReport(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    g_print("observed: %dx%d\n", ctx->observedWidth, ctx->observedHeight);
    for (const IconGroup &group : ctx->iconGroups) {
        g_print("icon group ratio=%.3f -> %dpx (%zu 个图标)\n", group.ratio, group.lastPx,
                group.images.size());
    }
    g_print("scale samples: 800x500 -> %dpx, 1000x634 -> %dpx, 1400x900 -> %dpx, "
            "2000x1200 -> %dpx\n",
            iconPxFor(std::min(800, 500), ctx->iconGroups.front()),
            iconPxFor(std::min(1000, 634), ctx->iconGroups.front()),
            iconPxFor(std::min(1400, 900), ctx->iconGroups.front()),
            iconPxFor(std::min(2000, 1200), ctx->iconGroups.front()));
    return G_SOURCE_REMOVE;
}

// --ui-snapshot=<路径>：把整个窗口渲染成 PNG。
// 为什么要这个：样式是纯观感，而这个环境里看不到屏幕（截图工具在 Wayland 下拿不到本进程窗口），
// 有了自渲染就能自己核对，也方便以后做视觉回归。走 GtkWidgetPaintable，所以窗口自身的 CSS 背景
// （顶部的蓝色辉光）也会被画进去；默认渲染器与 GL 无关，沙箱里也能出图。
void writeWindowSnapshot(AppContext &ctx, const char *path) {
    const int width = gtk_widget_get_width(ctx.window);
    const int height = gtk_widget_get_height(ctx.window);
    GdkPaintable *paintable = gtk_widget_paintable_new(ctx.window);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);

    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    if (node != nullptr) {
        gsk_render_node_draw(node, cr);
        gsk_render_node_unref(node);
    }
    const cairo_status_t status = cairo_surface_write_to_png(surface, path);
    if (status == CAIRO_STATUS_SUCCESS) {
        g_print("snapshot: %s (%dx%d)\n", path, width, height);
    } else {
        g_printerr("快照写文件失败：status=%d\n", static_cast<int>(status));
        ctx.error = true;
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    g_object_unref(paintable);
}

gboolean finishFirstFrame(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    if (!ctx->snapshotPath.empty()) {
        writeWindowSnapshot(*ctx, ctx->snapshotPath.c_str());
    }
    if (ctx->selftest) {
        selftestReport(data);
    }
    if (ctx->app != nullptr) {
        g_application_quit(ctx->app);
        ctx->app = nullptr; // 兜底超时与首次分配回调都可能触发，只退一次
    }
    return G_SOURCE_REMOVE;
}

gboolean selftestFinish(gpointer data) { return finishFirstFrame(data); }

void onActivate(GtkApplication *app, gpointer userData) {
    auto *ctx = static_cast<AppContext *>(userData);

    // 配色方案由 applyBackground() 按背景模式决定（「跟随系统」时要交出选择权）。
    // 注意必须在 GTK 初始化之后做——放到 g_application_run() 之前会触发
    // 「gdk_display_manager_get() was called before gtk_init()」并崩掉。

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(css, "/org/felix/awcc/style.css");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkBuilder *builder = gtk_builder_new_from_resource("/org/felix/awcc/awcc.ui");
    gtk_builder_set_translation_domain(builder, GETTEXT_PACKAGE);
    ctx->window = GTK_WIDGET(gtk_builder_get_object(builder, "main_window"));
    ctx->stack = GTK_WIDGET(gtk_builder_get_object(builder, "content_stack"));
    ctx->crumbMain = GTK_WIDGET(gtk_builder_get_object(builder, "crumb_main"));
    ctx->crumbSub = GTK_WIDGET(gtk_builder_get_object(builder, "crumb_sub"));

    // .ui 解析失败时上面几个会拿到 NULL。注意 gtk4-builder-tool validate 认不出 libadwaita
    // 的类型（它没加载 Adw 类型库），所以这条检查是 .ui 正确性的主要防线。
    if (ctx->window == nullptr || ctx->stack == nullptr || ctx->crumbMain == nullptr ||
        ctx->crumbSub == nullptr) {
        LOG_S(ERROR) << "awcc.ui 加载不全：窗口 / 内容栈 / 面包屑有缺失";
        g_printerr("awcc.ui 加载不全（见上）\n");
        ctx->error = true;
        g_object_unref(builder);
        g_application_quit(G_APPLICATION(app));
        return;
    }

    gtk_window_set_application(GTK_WINDOW(ctx->window), app);

    // 背景：按用户配置注入覆盖 CSS；「跟随系统」时还要跟着系统亮暗切换
    applyBackground(*ctx);
    g_signal_connect(adw_style_manager_get_default(), "notify::dark",
                     G_CALLBACK(+[](GObject *, GParamSpec *, gpointer data) {
                         auto *context = static_cast<AppContext *>(data);
                         if (context->config.background.mode ==
                             Config::BackgroundMode::Theme) {
                             applyBackground(*context);
                         }
                     }),
                     ctx);

    // 导航图标的缩放组（左栏）；模式按钮那组在 buildModeCard 里加
    IconGroup &rail = addIconGroup(*ctx, 0.030, 16, 40);

    GtkToggleButton *navLeader = nullptr;
    GtkToggleButton *wantedPage = nullptr;
    for (const NavEntry &entry : kNav) {
        // 先记父页，再建页面——否则子页会先于父页进入自检输出，看着像层级颠倒了
        ctx->pageLog.push_back(std::string("├─ ") + entry.name + "  " + _(entry.title));
        GtkWidget *pageWidget = entry.build != nullptr
                                    ? entry.build(*ctx)
                                    : (entry.subCount > 0
                                           ? makeSectionPage(entry, *ctx)
                                           : makePlaceholderPage(entry.title, entry.note));
        gtk_stack_add_named(GTK_STACK(ctx->stack), pageWidget, entry.name);

        const std::string btnId = std::string("nav_") + entry.name;
        auto *btn = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, btnId.c_str()));
        if (btn == nullptr) {
            LOG_S(ERROR) << "导航按钮缺失：" << btnId;
            continue;
        }
        GtkWidget *child = gtk_button_get_child(GTK_BUTTON(btn));
        if (child != nullptr && GTK_IS_IMAGE(child)) {
            const char *iconName = gtk_image_get_icon_name(GTK_IMAGE(child));
            rail.images.emplace_back(GTK_IMAGE(child), 1.0);
            rail.inkHeights.push_back(
                iconName != nullptr ? measureIconInk(iconName, nullptr) : 0);
        }
        if (navLeader == nullptr) {
            navLeader = btn;
        } else {
            gtk_toggle_button_set_group(btn, navLeader);
        }
        if (!ctx->initialPage.empty() && ctx->initialPage == entry.name) {
            wantedPage = btn;
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(onNavToggled), ctx);
    }

    // 左栏图标：把墨迹最小的补到与最大者同高，这样一排看起来才齐
    if (!rail.inkHeights.empty()) {
        const int target = *std::max_element(rail.inkHeights.begin(), rail.inkHeights.end());
        if (target > 0) {
            for (size_t i = 0; i < rail.inkHeights.size(); ++i) {
                const int ink = rail.inkHeights[i];
                rail.images[i].second =
                    ink > 0 ? std::clamp(static_cast<double>(target) / ink, 0.8, 1.6) : 1.0;
            }
        }
        rail.inkHeights.clear();
    }

    // 内容根节点（.ui 里的 content_root）挂帧时钟回调，把内容区尺寸喂给图标缩放逻辑。
    // 注意必须在 g_object_unref(builder) 之前查，否则 builder 已失效（gtk_builder_get_object
    // 会断言失败并返回 NULL）。
    // 官方标题栏里面包屑在左侧，而 AdwHeaderBar 的 title-widget 是居中的，所以用 pack_start
    if (auto *headerBar = GTK_WIDGET(gtk_builder_get_object(builder, "header_bar"));
        headerBar != nullptr) {
        if (auto *crumbBox = GTK_WIDGET(gtk_builder_get_object(builder, "crumb_box"));
            crumbBox != nullptr) {
            g_object_ref(crumbBox);
            gtk_widget_unparent(crumbBox);
            adw_header_bar_pack_start(ADW_HEADER_BAR(headerBar), crumbBox);
            g_object_unref(crumbBox);
        }
    }

    GtkWidget *contentRoot = GTK_WIDGET(gtk_builder_get_object(builder, "content_root"));
    if (contentRoot == nullptr || !GTK_IS_WIDGET(contentRoot)) {
        LOG_S(ERROR) << "awcc.ui 里找不到 content_root，图标缩放不会生效";
        ctx->error = true;
    } else {
        gtk_widget_add_tick_callback(contentRoot, onContentTick, ctx, nullptr);
    }

    g_object_unref(builder);

    if (wantedPage != nullptr) {
        gtk_toggle_button_set_active(wantedPage, TRUE); // --ui-page 指定的页
    } else if (navLeader != nullptr) {
        gtk_toggle_button_set_active(navLeader, TRUE); // 默认停在第一个页面
    }
    // 建完页面再订阅：这份快照落地时，模式按钮与亮度控件都已登记好
    stateClientStart(*ctx);
    gtk_window_present(GTK_WINDOW(ctx->window));

    if (ctx->selftest) {
        // 无人值守自检：页面树 + 真实尺寸观测 + 缩放样本，然后退出（见 TODO.md 的验证方式）。
        g_print("page tree (--ui-selftest):\n");
        for (const std::string &line : ctx->pageLog) {
            g_print("%s\n", line.c_str());
        }
        g_print("%zu page nodes\n", ctx->pageLog.size());

        // 模式按钮一致性（议题 #1）：主页 / 性能·概况 / 性能·散热 各有一组，状态必须共享。
        // 这里只改 ctx 里的状态再刷新，不碰硬件——自检不该写 ACPI、更不该弹授权框。
        if (!ctx->modeButtons.empty()) {
            const ThermalModes original = ctx->currentMode;
            bool consistent = true;
            for (const ModeSpec &spec : kModes) {
                ctx->currentMode = spec.mode;
                syncModeButtons(*ctx);
                for (const auto &[mode, btn] : ctx->modeButtons) {
                    if (gtk_toggle_button_get_active(btn) != (mode == spec.mode)) {
                        consistent = false;
                    }
                }
            }
            ctx->currentMode = original;
            syncModeButtons(*ctx);
            g_print("mode sync: %zu 个按钮 / %zu 组 -> %s\n", ctx->modeButtons.size(),
                    ctx->modeRowCount, consistent ? "所有组状态一致" : "不一致（议题 #1）");

            // 再模拟一次真实点击（走 onModeToggled → applyMode），验证「点一组、另外两组跟着变」。
            // 每组的按钮是连续登记的，所以按 组数 均分即可定位。
            const size_t groupSize = ctx->modeButtons.size() / ctx->modeRowCount;
            if (ctx->modeRowCount >= 2 && groupSize >= 2) {
                gtk_toggle_button_set_active(ctx->modeButtons[groupSize - 1].second, TRUE);
                bool followed = true;
                for (size_t g = 1; g < ctx->modeRowCount; ++g) {
                    if (!gtk_toggle_button_get_active(
                            ctx->modeButtons[g * groupSize + groupSize - 1].second)) {
                        followed = false;
                    }
                }
                g_print("mode click: 在第一组点「%s」-> 另外 %zu 组%s\n",
                        modeLabel(ctx->modeButtons[groupSize - 1].first), ctx->modeRowCount - 1,
                        followed ? "同步" : "未同步（议题 #1）");
            }
            ctx->currentMode = original;
            syncModeButtons(*ctx);
        }
        // 兜底：窗口万一没 map，也不能把自检挂死
        g_timeout_add(5000, selftestFinish, ctx);
    }
}

} // namespace

int Ui::Run(int argc, char **argv, const Services &services) {
    // GtkApplication 用自己的 GOptionContext 解析命令行，遇到不认识的参数会直接报错退出。
    // 本程序自己的开关已由 main.cpp 处理完，这里滤掉以免「Unknown option」；其余参数
    // （如 --display）留给 GTK。
    bool selftest = false;
    std::string snapshotPath;      // --ui-snapshot=<路径>：把窗口渲染成 PNG（见下）
    std::string initialPage;       // --ui-page=<name>：启动时停在哪一页（截图/自检用）
    bool forceLight = false;       // --ui-force-light：强制按亮色渲染（验证亮色配色用）
    std::string initialSubPage;    // --ui-subpage=<name>：连子页一起指定（截图用）
    std::vector<char *> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (i > 0 && (arg == "-g" || arg == "--gui")) {
            continue;
        }
        if (i > 0 && arg == "--ui-selftest") {
            selftest = true;
            continue;
        }
        if (i > 0 && arg.starts_with("--ui-subpage=")) {
            initialSubPage = std::string(arg.substr(std::strlen("--ui-subpage=")));
            continue;
        }
        if (i > 0 && arg == "--ui-force-light") {
            forceLight = true;
            continue;
        }
        if (i > 0 && arg.starts_with("--ui-page=")) {
            initialPage = std::string(arg.substr(std::strlen("--ui-page=")));
            continue;
        }
        if (i > 0 && arg.starts_with("--ui-snapshot=")) {
            snapshotPath = std::string(arg.substr(std::strlen("--ui-snapshot=")));
            continue;
        }
        args.push_back(argv[i]);
    }

    static AppContext ctx;
    ctx.selftest = selftest;
    ctx.snapshotPath = snapshotPath;
    ctx.initialPage = initialPage;
    ctx.forceLight = forceLight;
    ctx.initialSubPage = initialSubPage;
    ctx.thermals = services.thermals;
    ctx.acpi = services.acpi;
    ctx.effects = services.effects;
    ctx.daemonRunning = services.daemonRunning;
    // 模式状态：只有 daemon 在跑时才读硬件——没有它读 ACPI 会弹授权框（见 DESIGN.md 第三节）。
    // 三页的模式按钮建页时都取这个值，所以必须在建页之前定下来。
    ctx.currentMode = (ctx.daemonRunning && ctx.thermals != nullptr)
                          ? ctx.thermals->getCurrentMode()
                          : ThermalModes::Balanced;
    ctx.config = Config::load();
    if (ctx.effects != nullptr) {
        ctx.brightness = std::clamp(ctx.effects->getBrightness(), 0, 100);
    }

    AdwApplication *app =
        adw_application_new("org.felix.awcc", G_APPLICATION_DEFAULT_FLAGS);
    ctx.app = G_APPLICATION(app);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), &ctx);

    LOG_S(INFO) << "Starting GTK4 frontend";
    const int status = g_application_run(G_APPLICATION(app),
                                         static_cast<int>(args.size()),
                                         args.data());
    g_object_unref(app);
    return ctx.error ? 1 : status;
}
