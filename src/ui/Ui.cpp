#include "Ui.h"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <loguru.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
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
// 缩放规则：以内容区短边为基准取比例，夹在 16–40px。只设 GtkImage 的 pixel-size（正方形），
// 不设 width/height，因此长宽比固定不变。

// ── 页面清单 ────────────────────────────────────────────────────────────────
// 源码里的串一律用**英文**做 msgid：gettext 查不到翻译时回落到 msgid，于是中文环境显示
// po/zh_CN.po 的译文、其他环境显示英文——这就是「中英文自动切换」。见 DESIGN.md 第二节。
// 最新中文对照见 po/zh_CN.po。

struct SubPage {
    const char *name;       // GtkStack 子页名
    const char *shortLabel; // 子页切换按钮上的字（msgid）
    const char *title;      // (msgid)
    const char *note;       // (msgid) 缺什么后端
};

struct NavEntry {
    const char *name; // 也是 GtkStack 子页名，与 .ui 里 nav_<name> 对应
    const char *title;
    const char *note;
    const SubPage *subs;
    size_t subCount;
};

constexpr SubPage kPerformanceSubs[] = {
    {"overview", N_("Overview"), N_("Performance · Overview"),
     N_("CPU / memory / disk are readable (M1 telemetry); GPU is unavailable while the "
        "dGPU is runtime-suspended")},
    {"thermal", N_("Thermal"), N_("Performance · Thermal"),
     N_("CPU / GPU temperatures and fan RPM are readable (M1 telemetry)")},
};

constexpr SubPage kAlienfxSubs[] = {
    {"lighting", N_("Lighting"), N_("ALIENFX™ · Lighting"),
     N_("The seven effects and brightness have working backends (wired up in M3); the "
        "color picker becomes GtkColorDialogButton")},
    {"keybinds", N_("Key bindings"), N_("ALIENFX™ · Key bindings"),
     N_("Can only listen to the G key / light key; remapping needs EC support "
        "(unconfirmed)")},
};

constexpr SubPage kSettingsSubs[] = {
    {"about", N_("About"), N_("Settings · About"),
     N_("Version comes from the VERSION macro; doable")},
    {"appearance", N_("Appearance"), N_("Settings · Appearance"),
     N_("Only the dark theme this round; light / follow-system is undecided")},
    {"overlay", N_("Overlay"), N_("Settings · Overlay"),
     N_("There is no in-game overlay on Linux → marked as not applicable")},
    {"onboarding", N_("Onboarding"), N_("Settings · Onboarding"),
     N_("Onboarding flow is undecided")},
    {"presets", N_("Presets"), N_("Settings · Global presets"),
     N_("Local preferences are doable; per-game switching has no game detection")},
    {"performance", N_("Performance"), N_("Settings · Performance"),
     N_("Overclocking is limited by the EC (capability unknown); Windows power saving "
        "is not applicable")},
};

constexpr NavEntry kNav[] = {
    {"home", N_("Home · Active"),
     N_("Power modes, lighting and brightness have working backends; the four ring "
        "gauges wait for the M1 telemetry layer"),
     nullptr, 0},
    {"performance", N_("Performance"), N_("Overview and Thermal are sub-pages"),
     kPerformanceSubs, std::size(kPerformanceSubs)},
    {"alienfx", N_("ALIENFX™"), N_("Lighting and Key bindings are sub-pages"), kAlienfxSubs,
     std::size(kAlienfxSubs)},
    {"macro", N_("Macros"), N_("Not present upstream, so no backend exists"), nullptr, 0},
    {"library", N_("Library"),
     N_("No game scanning source on Linux; manual add or placeholder"), nullptr, 0},
    {"settings", N_("Settings"), N_("Six sub-pages, per item in M4"), kSettingsSubs,
     std::size(kSettingsSubs)},
    {"help", N_("Help"), N_("Content and links are undecided"), nullptr, 0},
};

// 运行期上下文
struct AppContext {
    GtkWidget *window = nullptr;
    GtkWidget *stack = nullptr;
    GtkWidget *crumbMain = nullptr;
    GtkWidget *crumbSub = nullptr;
    std::vector<GtkWidget *> railButtons; // 图标随窗口缩放的按钮
    int iconPx = 0;                       // 上次算出的图标边长，避免重复设置
    int observedWidth = 0;                // 尺寸感知容器实际分配到的尺寸（自检用）
    int observedHeight = 0;
    GApplication *app = nullptr;
    bool selftest = false;
    bool error = false; // .ui 加载不全等情况：让 --ui-selftest 以非零码退出
    std::vector<std::string> pageLog; // --ui-selftest 的输出
};

// 图标随窗口缩放：以窗口短边为基准取比例，再夹到合理区间。只喂 pixel-size（正方形），
// 不改 width/height，所以长宽比固定。
constexpr double kIconRatio = 0.030; // 1000x680 的窗口 → 20px
constexpr int kIconMinPx = 16;
constexpr int kIconMaxPx = 40;

// 图标边长：以内容区短边为基准取比例，再夹到区间内。纯计算，抽出来便于自检打印样本。
int iconPxFor(int width, int height) {
    const int base = std::min(width, height);
    return std::clamp(static_cast<int>(std::lround(base * kIconRatio)), kIconMinPx,
                      kIconMaxPx);
}

void applyIconScale(AppContext &ctx, int width, int height) {
    ctx.observedWidth = width;
    ctx.observedHeight = height;
    const int px = iconPxFor(width, height);
    if (px == ctx.iconPx) {
        return; // 尺寸没变就别动控件，避免每帧重建样式
    }
    ctx.iconPx = px;
    for (GtkWidget *button : ctx.railButtons) {
        GtkWidget *child = gtk_button_get_child(GTK_BUTTON(button));
        if (child != nullptr && GTK_IS_IMAGE(child)) {
            gtk_image_set_pixel_size(GTK_IMAGE(child), px);
        }
        // 按钮也给成正方形，边距按图标等比放大，整体比例不变
        const int side = px + static_cast<int>(std::lround(px * 0.9)) + 8;
        gtk_widget_set_size_request(button, side, side);
    }
}

// 占位页：标题 + 「施工中」徽标 + 一句「缺什么」。M2 起由真页面逐个替换。
GtkWidget *makePlaceholderPage(const char *title, const char *note) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *badge = gtk_label_new(_("Under construction"));
    gtk_widget_add_css_class(badge, "badge-construction");
    gtk_widget_set_halign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), badge);

    GtkWidget *titleLabel = gtk_label_new(_(title));
    gtk_widget_add_css_class(titleLabel, "page-title");
    gtk_box_append(GTK_BOX(box), titleLabel);

    if (note != nullptr && *note != '\0') {
        GtkWidget *noteLabel = gtk_label_new(_(note));
        gtk_widget_add_css_class(noteLabel, "page-note");
        gtk_label_set_wrap(GTK_LABEL(noteLabel), TRUE);
        gtk_label_set_justify(GTK_LABEL(noteLabel), GTK_JUSTIFY_CENTER);
        gtk_widget_set_size_request(noteLabel, 420, -1);
        gtk_box_append(GTK_BOX(box), noteLabel);
    }
    return box;
}

void setCrumb(AppContext &ctx, const char *page, const char *sub) {
    gtk_label_set_text(GTK_LABEL(ctx.crumbMain), _(page));
    gtk_label_set_text(GTK_LABEL(ctx.crumbSub),
                       sub != nullptr ? _(sub) : _("System"));
}

// 帧时钟回调：每帧读一次内容区尺寸。尺寸没变时 applyIconScale 第一件事就返回，开销可忽略。
gboolean onContentTick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock;
    auto *ctx = static_cast<AppContext *>(data);
    applyIconScale(*ctx, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
    return G_SOURCE_CONTINUE;
}

void onSubToggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) {
        return;
    }
    auto *stack = GTK_WIDGET(data);
    gtk_stack_set_visible_child_name(GTK_STACK(stack),
                                     gtk_widget_get_name(GTK_WIDGET(btn)));
}

// 带子页的页面：顶部一排子页切换按钮 + 内层 GtkStack。
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
        gtk_stack_add_named(GTK_STACK(inner), makePlaceholderPage(sub.title, sub.note),
                            sub.name);
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

// 以下两个只服务 --ui-selftest。窗口真正 map 之后才有分配尺寸，所以延时取一次真实观测，
// 证明帧时钟钩子确实拿到内容区尺寸并算出图标边长；再打一组边界样本证明算式与夹取。
// （不试「map 之后再改窗口尺寸」——GTK4 的 default-size 只在首次显示时生效，实测改不动。）
gboolean selftestReportSize(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    g_print("observed: %dx%d -> icon %dpx\n", ctx->observedWidth,
            ctx->observedHeight, ctx->iconPx);
    g_print("scale samples: 800x500 -> %dpx, 1000x634 -> %dpx, 1400x900 -> %dpx, "
            "2000x1200 -> %dpx\n",
            iconPxFor(800, 500), iconPxFor(1000, 634), iconPxFor(1400, 900),
            iconPxFor(2000, 1200));
    return G_SOURCE_REMOVE;
}

gboolean selftestFinish(gpointer data) {
    auto *ctx = static_cast<AppContext *>(data);
    selftestReportSize(data);
    g_application_quit(ctx->app);
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
    // .ui 里的 translatable="yes" 串走同一个 gettext 域（见 po/zh_CN.po）
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

    // 页面：M0 全是占位（M0.4 的说明见 TODO.md）。带子页的在页内再分一层。
    GtkToggleButton *navLeader = nullptr;
    for (const NavEntry &entry : kNav) {
        // 先记父页，再建页面——否则子页会先于父页进入自检输出，看着像层级颠倒了
        // 自检日志打译文（跟界面一致），否则看不出语言有没有切过去
        ctx->pageLog.push_back(std::string("├─ ") + entry.name + "  " + _(entry.title));
        GtkWidget *pageWidget = entry.subCount > 0
                                    ? makeSectionPage(entry, *ctx)
                                    : makePlaceholderPage(entry.title, entry.note);
        gtk_stack_add_named(GTK_STACK(ctx->stack), pageWidget, entry.name);

        const std::string btnId = std::string("nav_") + entry.name;
        auto *btn = GTK_TOGGLE_BUTTON(gtk_builder_get_object(builder, btnId.c_str()));
        if (btn == nullptr) {
            LOG_S(ERROR) << "导航按钮缺失：" << btnId;
            continue;
        }
        if (navLeader == nullptr) {
            navLeader = btn;
        } else {
            gtk_toggle_button_set_group(btn, navLeader);
        }
        g_signal_connect(btn, "toggled", G_CALLBACK(onNavToggled), ctx);
        ctx->railButtons.push_back(GTK_WIDGET(btn));
    }

    // 内容根节点（.ui 里的 content_root）挂帧时钟回调，把内容区尺寸喂给图标缩放逻辑。
    // 注意必须在 g_object_unref(builder) 之前查，否则 builder 已失效（gtk_builder_get_object
    // 会断言失败并返回 NULL）。
    GtkWidget *contentRoot =
        GTK_WIDGET(gtk_builder_get_object(builder, "content_root"));
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
        // 无人值守自检：页面树 + 两阶段尺寸观测，然后退出（见 TODO.md 的验证方式）。
        g_print("page tree (--ui-selftest):\n");
        for (const std::string &line : ctx->pageLog) {
            g_print("%s\n", line.c_str());
        }
        g_print("%zu page nodes\n", ctx->pageLog.size());
        g_timeout_add(900, selftestFinish, ctx);
    }
}

} // namespace

int Ui::Run(int argc, char **argv, const Services &services) {
    (void)services; // M2 接页面时使用

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
