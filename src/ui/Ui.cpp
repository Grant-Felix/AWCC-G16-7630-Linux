#include "Ui.h"

#include "AcpiUtils.h"
#include "EffectController.h"
#include "Thermals.h"
#include "database.h"
#include "helper.h"

#include <adwaita.h>
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

// ── 图标随窗口缩放 ──────────────────────────────────────────────────────────
// 需求：图标大小随窗口大小自动调整、长宽比不变。
// 为什么不用 size_allocate vfunc 拿窗口尺寸：GTK4 的 GtkWidgetClass.size_allocate 文档写明
// 「if the widget does not have a layout manager」才调用——GtkBox 自带 GtkBoxLayout，实测它的
// size_allocate 一次都不会被调用（加探针验证过）。为一个缩放钩子去写自定义容器 + GtkBuildable
// 不值得，改用帧时钟回调：每帧读一次内容区尺寸，尺寸没变就直接返回，开销是几次整数比较。
//
// 只有 GtkImage 的 pixel-size（正方形）会被改，不碰 width/height，因此长宽比固定不变。
struct IconGroup {
    std::vector<GtkImage *> images;
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
    // 服务层（main.cpp 传进来，不持有所有权）
    Thermals *thermals = nullptr;
    AcpiUtils *acpi = nullptr;
    EffectController *effects = nullptr;
    bool daemonRunning = false;
    // 图标缩放
    // 用 deque 而不是 vector：buildModeCard 里会再 addIconGroup，vector 扩容会让先取的引用悬空
    std::deque<IconGroup> iconGroups;
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
    bool selftestReported = false; // 自检已经报告过一次（等真实分配，不用固定延时）
    bool error = false;
    std::vector<std::string> pageLog;
};

gboolean selftestFinish(gpointer data);

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
    // 自检：等到第一次真实分配再报告，比固定 sleep 可靠（窗口 map 时间不定）
    if (ctx.selftest && !ctx.selftestReported) {
        ctx.selftestReported = true;
        g_timeout_add(200, selftestFinish, &ctx);
    }
    for (IconGroup &group : ctx.iconGroups) {
        const int px = iconPxFor(base, group);
        if (px == group.lastPx) {
            continue;
        }
        group.lastPx = px;
        for (GtkImage *image : group.images) {
            gtk_image_set_pixel_size(image, px);
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
    ctx.iconGroups.push_back(IconGroup{{}, {}, ratio, minPx, maxPx});
    return ctx.iconGroups.back();
}

// ── 小工具 ──────────────────────────────────────────────────────────────────

GtkWidget *makeLabel(const char *text, const char *cssClass) {
    GtkWidget *label = gtk_label_new(text);
    if (cssClass != nullptr) {
        gtk_widget_add_css_class(label, cssClass);
    }
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

GtkWidget *makeBadge(const char *text, const char *cssClass) {
    GtkWidget *badge = gtk_label_new(text);
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
    if (ctx->effects != nullptr) {
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
    const char *label;    // (msgid)
    const char *iconName; // resources/modes 下的 PNG
};

constexpr ModeSpec kModes[] = {
    {ThermalModes::BatterySaver, ThermalModeSet::BatterySaver, N_("Battery saver"),
     "batteryMode.png"},
    {ThermalModes::Cool, ThermalModeSet::Cool, N_("Cool"), "quiteMode.png"},
    {ThermalModes::Quiet, ThermalModeSet::Quiet, N_("Quiet"), "quiteMode.png"},
    {ThermalModes::Balanced, ThermalModeSet::Balanced, N_("Balanced"), "balancedMode.png"},
    {ThermalModes::Performance, ThermalModeSet::Performance, N_("Performance"),
     "performanceMode.png"},
    {ThermalModes::Gmode, ThermalModeSet::GMode, N_("G mode"), "gMode.png"},
};

void onModeToggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) {
        return;
    }
    auto *ctx = static_cast<AppContext *>(data);
    const auto mode = static_cast<ThermalModes>(GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(btn), "awcc-mode")));
    if (ctx->thermals != nullptr) {
        // 注意：daemon 没在跑时这里会走 pkexec，弹授权框期间主循环被挡住（见 DESIGN.md 第三节）
        ctx->thermals->setThermalMode(mode);
    }
}

GtkWidget *buildModeCard(AppContext &ctx) {
    GtkWidget *card = makeCard(N_("Power mode"), N_("Applies immediately"));
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(row, GTK_ALIGN_START);

    IconGroup &group = addIconGroup(ctx, 0.055, 28, 64);
    GtkToggleButton *leader = nullptr;
    const ThermalModes current =
        ctx.thermals != nullptr ? ctx.thermals->getCurrentMode() : ThermalModes::Balanced;

    for (const ModeSpec &spec : kModes) {
        if (ctx.acpi != nullptr && !ctx.acpi->hasThermalMode(spec.set)) {
            continue; // 本机型不支持的模式就不摆出来
        }
        GtkWidget *btn = gtk_toggle_button_new();
        gtk_widget_add_css_class(btn, "mode-button");
        g_object_set_data(G_OBJECT(btn), "awcc-mode", GINT_TO_POINTER(spec.mode));

        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        const std::string path = std::string("/org/felix/awcc/modes/") + spec.iconName;
        GtkWidget *icon = gtk_image_new_from_resource(path.c_str());
        gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
        group.images.push_back(GTK_IMAGE(icon));
        gtk_box_append(GTK_BOX(content), icon);
        GtkWidget *label = gtk_label_new(_(spec.label));
        gtk_box_append(GTK_BOX(content), label);
        gtk_button_set_child(GTK_BUTTON(btn), content);

        if (leader == nullptr) {
            leader = GTK_TOGGLE_BUTTON(btn);
        } else {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), leader);
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(onModeToggled), &ctx);
        gtk_box_append(GTK_BOX(row), btn);
        if (spec.mode == current) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);
        }
    }
    gtk_box_append(GTK_BOX(card), row);
    return card;
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

void addInfoRow(GtkWidget *card, const char *label, const std::string &value) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *key = makeLabel(_(label), "row-label");
    gtk_widget_set_size_request(key, 140, -1);
    gtk_box_append(GTK_BOX(row), key);
    GtkWidget *val = gtk_label_new(value.c_str());
    gtk_label_set_selectable(GTK_LABEL(val), TRUE);
    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), val);
    gtk_box_append(GTK_BOX(card), row);
}

GtkWidget *buildAboutCard(AppContext &ctx) {
    GtkWidget *card = makeCard(N_("Device"), nullptr);
    addInfoRow(card, N_("Model"), Helper::getDeviceName());
    addInfoRow(card, N_("Version"), VERSION);
    addInfoRow(card, N_("Keyboard zones"),
               std::to_string(ctx.acpi != nullptr ? ctx.acpi->getKeyboardZones().size() : 0));
    addInfoRow(card, N_("Current mode"),
               ctx.thermals != nullptr ? ctx.thermals->getCurrentModeName() : "-");
    addInfoRow(card, N_("Daemon"),
               ctx.daemonRunning ? _("running (socket)") : _("not running (pkexec fallback)"));

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

// 主页：模式 + 灯效 + 风扇（都是既有后端）；环形仪表还没接（M1 遥测层）
GtkWidget *buildHome(AppContext &ctx) {
    GtkWidget *content = makePageContent();

    if (!ctx.daemonRunning) {
        GtkWidget *strip = gtk_label_new(
            _("Daemon is not running: thermal and fan changes fall back to pkexec and will "
              "ask for authorization."));
        gtk_widget_add_css_class(strip, "warn-strip");
        gtk_label_set_wrap(GTK_LABEL(strip), TRUE);
        gtk_widget_set_halign(strip, GTK_ALIGN_FILL);
        gtk_box_append(GTK_BOX(content), strip);
    }

    gtk_box_append(GTK_BOX(content), buildModeCard(ctx));
    gtk_box_append(GTK_BOX(content), buildLightingCard(ctx, false));
    gtk_box_append(GTK_BOX(content), buildFanCard(ctx));

    GtkWidget *gauges = makeCard(N_("Ring gauges"), nullptr);
    gtk_box_append(GTK_BOX(gauges),
                   makeBadge(N_("Under construction"), "badge-construction"));
    gtk_box_append(GTK_BOX(gauges),
                   makeLabel(N_("Needs the M1 telemetry layer (CPU / memory / disk / GPU)."),
                             "card-note"));
    gtk_box_append(GTK_BOX(content), gauges);
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

GtkWidget *buildKeybindsPage(AppContext &ctx) {
    (void)ctx;
    GtkWidget *content = makePageContent();
    GtkWidget *card = makeCard(N_("Hotkeys"), nullptr);
    gtk_box_append(GTK_BOX(card),
                   makeBadge(N_("Under construction"), "badge-construction"));
    gtk_box_append(GTK_BOX(card),
                   makeLabel(N_("The daemon owns the G key / light key listener; showing and "
                                "editing bindings needs EC support (unconfirmed)."),
                              "card-note"));
    gtk_box_append(GTK_BOX(content), card);
    return wrapScrolled(content);
}

// ── 页面清单（表在 AppContext 之后定义，builder 用函数指针）──────────────────

const SubPage kPerformanceSubs[] = {
    {"overview", N_("Overview"), N_("Performance · Overview"),
     N_("CPU / memory / disk are readable (M1 telemetry); GPU is unavailable while the "
        "dGPU is runtime-suspended"),
     nullptr},
    {"thermal", N_("Thermal"), N_("Performance · Thermal"),
     N_("CPU / GPU temperatures and fan RPM are readable (M1 telemetry)"), nullptr},
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
     N_("Only the dark theme this round; light / follow-system is undecided"), nullptr},
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
     nullptr, 0, buildHome},
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

    GtkWidget *subnav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(subnav, "subnav");
    gtk_widget_set_halign(subnav, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(subnav, 12);
    gtk_box_append(GTK_BOX(page), subnav);

    GtkWidget *inner = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(inner), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand(inner, true);
    gtk_box_append(GTK_BOX(page), inner);

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
            setCrumb(*ctx, entry.title,
                     entry.subCount > 0 ? entry.subs[0].title : nullptr);
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

gboolean selftestFinish(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    selftestReport(data);
    if (ctx->app != nullptr) {
        g_application_quit(ctx->app);
        ctx->app = nullptr; // 兜底超时与首次分配回调都可能触发，只退一次
    }
    return G_SOURCE_REMOVE;
}

void onActivate(GtkApplication *app, gpointer userData) {
    auto *ctx = static_cast<AppContext *>(userData);

    // 深色打底（DESIGN.md 第六节）。必须在 GTK 初始化之后调用——放到 g_application_run()
    // 之前会触发「gdk_display_manager_get() was called before gtk_init()」并崩掉。
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                       ADW_COLOR_SCHEME_PREFER_DARK);

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

    // 导航图标的缩放组（左栏）；模式按钮那组在 buildModeCard 里加
    IconGroup &rail = addIconGroup(*ctx, 0.030, 16, 40);

    GtkToggleButton *navLeader = nullptr;
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
            rail.images.push_back(GTK_IMAGE(child));
        }
        if (navLeader == nullptr) {
            navLeader = btn;
        } else {
            gtk_toggle_button_set_group(btn, navLeader);
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(onNavToggled), ctx);
    }

    // 内容根节点（.ui 里的 content_root）挂帧时钟回调，把内容区尺寸喂给图标缩放逻辑。
    // 注意必须在 g_object_unref(builder) 之前查，否则 builder 已失效（gtk_builder_get_object
    // 会断言失败并返回 NULL）。
    GtkWidget *contentRoot = GTK_WIDGET(gtk_builder_get_object(builder, "content_root"));
    if (contentRoot == nullptr || !GTK_IS_WIDGET(contentRoot)) {
        LOG_S(ERROR) << "awcc.ui 里找不到 content_root，图标缩放不会生效";
        ctx->error = true;
    } else {
        gtk_widget_add_tick_callback(contentRoot, onContentTick, ctx, nullptr);
    }

    g_object_unref(builder);

    if (navLeader != nullptr) {
        gtk_toggle_button_set_active(navLeader, TRUE); // 默认停在第一个页面
    }
    gtk_window_present(GTK_WINDOW(ctx->window));

    if (ctx->selftest) {
        // 无人值守自检：页面树 + 真实尺寸观测 + 缩放样本，然后退出（见 TODO.md 的验证方式）。
        g_print("page tree (--ui-selftest):\n");
        for (const std::string &line : ctx->pageLog) {
            g_print("%s\n", line.c_str());
        }
        g_print("%zu page nodes\n", ctx->pageLog.size());
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
        args.push_back(argv[i]);
    }

    static AppContext ctx;
    ctx.selftest = selftest;
    ctx.thermals = services.thermals;
    ctx.acpi = services.acpi;
    ctx.effects = services.effects;
    ctx.daemonRunning = services.daemonRunning;
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
