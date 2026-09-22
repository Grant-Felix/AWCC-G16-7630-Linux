#include "Ui.h"

#include <adwaita.h>
#include <loguru.hpp>
#include <string_view>
#include <vector>

namespace {

// GtkApplication 的 activate：首次启动与之后每次被激活（单实例转发）都会走到这里。
void onActivate(GtkApplication *app, gpointer userData) {
    auto *services = static_cast<Ui::Services *>(userData);
    (void)services; // M0 只有骨架窗口；服务层在 M2 接页面时使用

    // 深色打底：DESIGN.md 第六节要求界面自成一套深色风格，正式样式在 M0.5 用 CSS 覆盖。
    // 必须在 GTK 初始化之后调用——放在 g_application_run() 之前会触发
    // 「gdk_display_manager_get() was called before gtk_init()」并直接崩掉。
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                       ADW_COLOR_SCHEME_PREFER_DARK);

    GtkWidget *window = adw_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Alienware Command Center v" VERSION);
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 680);

    // M0 的占位内容：M0.4 换成「左导航 + 面包屑 + 12 个页面」的骨架。
    GtkWidget *placeholder = gtk_label_new("GTK4 前端骨架（M0）");
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), placeholder);

    gtk_window_present(GTK_WINDOW(window));
}

} // namespace

int Ui::Run(int argc, char **argv, const Services &services) {
    // GtkApplication 用自己的 GOptionContext 解析命令行，遇到不认识的参数会直接报错退出。
    // 本程序自己的开关（--gui 等）已由 main.cpp 处理完，这里滤掉以免「Unknown option」；
    // 其余参数（如 --display）留给 GTK。
    std::vector<char *> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (i > 0 && (arg == "-g" || arg == "--gui" || arg == "--test-mode")) {
            continue;
        }
        args.push_back(argv[i]);
    }

    AdwApplication *app =
        adw_application_new("org.felix.awcc", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate),
                     const_cast<Services *>(&services));

    LOG_S(INFO) << "Starting GTK4 frontend";
    const int status = g_application_run(G_APPLICATION(app),
                                         static_cast<int>(args.size()),
                                         args.data());
    g_object_unref(app);
    return status;
}
