#include "Ui.h"

#include <adwaita.h>
#include <loguru.hpp>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

// ── 页面清单 ────────────────────────────────────────────────────────────────
// 一级页面 = 左栏一个图标；性能 / ALIENFX / 设置再分「概况 / 散热」这类子页。
// 参考图索引在 ADAPTATION.md 第九节，页面与数据源映射在 DESIGN.md 第四、五节。
// note 写「缺什么后端」，就是界面上那句「施工中」的说明。

struct SubPage {
    const char *name;       // GtkStack 子页名
    const char *shortLabel; // 子页切换按钮上的字
    const char *title;
    const char *note;
};

struct NavEntry {
    const char *name; // 也是 GtkStack 子页名，与 .ui 里 nav_<name> 对应
    const char *title;
    const char *note;
    const SubPage *subs;
    size_t subCount;
};

constexpr SubPage kPerformanceSubs[] = {
    {"overview", "概况", "性能 · 概况",
     "CPU / 内存 / 磁盘可读（M1 遥测层）；GPU 因 dGPU 处于 runtime suspend 暂不可用"},
    {"thermal", "散热", "性能 · 散热", "CPU / GPU 温度与风扇转速可读（M1 遥测层）"},
};

constexpr SubPage kAlienfxSubs[] = {
    {"lighting", "灯效", "ALIENFX™ · 灯效",
     "7 种灯效与亮度是现成后端，M3 接入；颜色选择器改用 GtkColorDialogButton"},
    {"keybinds", "按键绑定", "ALIENFX™ · 按键绑定",
     "只能监听 G 键 / 灯键，改键映射需 EC 支持（未确认）"},
};

constexpr SubPage kSettingsSubs[] = {
    {"about", "关于", "设置 · 关于", "版本取 VERSION 宏，可做"},
    {"appearance", "外观", "设置 · 外观", "本期只实现深色，浅色 / 跟随系统待定"},
    {"overlay", "覆盖", "设置 · 覆盖", "Linux 没有游戏内叠加层 → 标记「不适用」"},
    {"onboarding", "新手入门", "设置 · 新手入门", "引导流程未定"},
    {"presets", "全局预设", "设置 · 全局预设", "本地偏好可做；「按游戏自动切换」无游戏检测"},
    {"performance", "性能", "设置 · 性能",
     "超频受 EC 限制（能力未知）；Windows 节能模式不适用"},
};

constexpr NavEntry kNav[] = {
    {"home", "主页 · 当前生效", "模式、灯效与亮度是现成后端；四个环形仪表要等 M1 遥测层",
     nullptr, 0},
    {"performance", "性能", "分概况与散热两个子页", kPerformanceSubs,
     std::size(kPerformanceSubs)},
    {"alienfx", "ALIENFX™", "分灯效与按键绑定两个子页", kAlienfxSubs,
     std::size(kAlienfxSubs)},
    {"macro", "宏", "上游没有这个功能，后端不存在", nullptr, 0},
    {"library", "库", "Linux 上无游戏扫描来源，只能手动添加或占位", nullptr, 0},
    {"settings", "设置", "六个子页，逐项见 M4", kSettingsSubs, std::size(kSettingsSubs)},
    {"help", "帮助", "内容与链接待定", nullptr, 0},
};

// 运行期上下文：M0 只需要窗口、内容栈、面包屑与自检开关。
struct AppContext {
    GtkWidget *window = nullptr;
    GtkWidget *stack = nullptr;
    GtkWidget *crumbMain = nullptr;
    GtkWidget *crumbSub = nullptr;
    bool selftest = false;
    bool error = false; // .ui 加载不全等情况：让 --ui-selftest 以非零码退出
    std::vector<std::string> pageLog; // --ui-selftest 的输出
};

// 占位页：标题 + 「施工中」徽标 + 一句「缺什么」。M2 起由真页面逐个替换。
GtkWidget *makePlaceholderPage(const char *title, const char *note) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *badge = gtk_label_new("施工中");
    gtk_widget_add_css_class(badge, "badge-construction");
    gtk_widget_set_halign(badge, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), badge);

    GtkWidget *titleLabel = gtk_label_new(title);
    gtk_widget_add_css_class(titleLabel, "page-title");
    gtk_box_append(GTK_BOX(box), titleLabel);

    if (note != nullptr && *note != '\0') {
        GtkWidget *noteLabel = gtk_label_new(note);
        gtk_widget_add_css_class(noteLabel, "page-note");
        gtk_label_set_wrap(GTK_LABEL(noteLabel), TRUE);
        gtk_label_set_justify(GTK_LABEL(noteLabel), GTK_JUSTIFY_CENTER);
        gtk_widget_set_size_request(noteLabel, 420, -1);
        gtk_box_append(GTK_BOX(box), noteLabel);
    }
    return box;
}

void setCrumb(AppContext &ctx, const char *page, const char *sub) {
    gtk_label_set_text(GTK_LABEL(ctx.crumbMain), page);
    gtk_label_set_text(GTK_LABEL(ctx.crumbSub), sub != nullptr ? sub : "系统");
}

// 子页切换按钮被点中：切内层栈，并更新面包屑第二段。
void onSubToggled(GtkToggleButton *btn, gpointer data) {
    if (!gtk_toggle_button_get_active(btn)) {
        return;
    }
    auto *stack = GTK_WIDGET(data);
    const char *name = gtk_widget_get_name(GTK_WIDGET(btn));
    gtk_stack_set_visible_child_name(GTK_STACK(stack), name);
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
        GtkWidget *btn = gtk_toggle_button_new_with_label(sub.shortLabel);
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
        ctx.pageLog.push_back(std::string("  └─ ") + sub.name + "  " + sub.title);
    }
    if (leader != nullptr) {
        gtk_toggle_button_set_active(leader, TRUE);
    }
    return page;
}

// 左栏按钮被点中：切内容栈并更新面包屑。
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
        ctx->pageLog.push_back(std::string("├─ ") + entry.name + "  " + entry.title);
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
    }

    g_object_unref(builder);

    if (navLeader != nullptr) {
        gtk_toggle_button_set_active(navLeader, TRUE); // 默认停在第一个页面
    }
    gtk_window_present(GTK_WINDOW(ctx->window));

    if (ctx->selftest) {
        // 无人值守自检：把页面树打到 stdout 就退出，不等人关窗口（值见 TODO.md 的验证方式）。
        g_print("页面树（--ui-selftest）：\n");
        for (const std::string &line : ctx->pageLog) {
            g_print("%s\n", line.c_str());
        }
        g_print("共 %zu 个页面节点\n", ctx->pageLog.size());
        g_application_quit(G_APPLICATION(app));
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
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), &ctx);

    LOG_S(INFO) << "Starting GTK4 frontend";
    const int status = g_application_run(G_APPLICATION(app),
                                         static_cast<int>(args.size()),
                                         args.data());
    g_object_unref(app);
    return ctx.error ? 1 : status;
}
