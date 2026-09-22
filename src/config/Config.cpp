#include "Config.h"

#include <glib.h>
#include <loguru.hpp>

namespace {

constexpr const char *kGroup = "ui";
constexpr const char *kModeKey = "background_mode";
constexpr const char *kColorKey = "background_color";
constexpr const char *kImageKey = "background_image";

} // namespace

namespace Config {

std::string configPath() {
    // g_get_user_config_dir() 会尊重 $XDG_CONFIG_HOME，没有就回落到 ~/.config
    return std::string(g_get_user_config_dir()) + "/awcc/config.ini";
}

const char *modeKey(BackgroundMode mode) {
    switch (mode) {
    case BackgroundMode::Black:
        return "black";
    case BackgroundMode::Theme:
        return "theme";
    case BackgroundMode::Color:
        return "color";
    case BackgroundMode::Image:
        return "image";
    case BackgroundMode::Glow:
        return "glow";
    }
    return "black";
}

BackgroundMode modeFromKey(const std::string &key) {
    if (key == "theme") {
        return BackgroundMode::Theme;
    }
    if (key == "color") {
        return BackgroundMode::Color;
    }
    if (key == "image") {
        return BackgroundMode::Image;
    }
    if (key == "glow") {
        return BackgroundMode::Glow;
    }
    return BackgroundMode::Black;
}

Ui load() {
    Ui ui;
    GKeyFile *file = g_key_file_new();
    const std::string path = configPath();
    if (g_key_file_load_from_file(file, path.c_str(), G_KEY_FILE_NONE, nullptr) != FALSE) {
        if (gchar *mode = g_key_file_get_string(file, kGroup, kModeKey, nullptr);
            mode != nullptr) {
            ui.background.mode = modeFromKey(mode);
            g_free(mode);
        }
        if (gchar *color = g_key_file_get_string(file, kGroup, kColorKey, nullptr);
            color != nullptr) {
            ui.background.color = color;
            g_free(color);
        }
        if (gchar *image = g_key_file_get_string(file, kGroup, kImageKey, nullptr);
            image != nullptr) {
            ui.background.image = image;
            g_free(image);
        }
    }
    g_key_file_unref(file);
    return ui;
}

bool save(const Ui &ui) {
    GKeyFile *file = g_key_file_new();
    g_key_file_set_string(file, kGroup, kModeKey, modeKey(ui.background.mode));
    g_key_file_set_string(file, kGroup, kColorKey, ui.background.color.c_str());
    g_key_file_set_string(file, kGroup, kImageKey, ui.background.image.c_str());

    const std::string path = configPath();
    gchar *dir = g_path_get_dirname(path.c_str());
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    const gboolean ok = g_key_file_save_to_file(file, path.c_str(), nullptr);
    if (ok == FALSE) {
        LOG_S(ERROR) << "写配置失败：" << path;
    }
    g_key_file_unref(file);
    return ok != FALSE;
}

std::string backgroundCss(const Ui &ui) {
    switch (ui.background.mode) {
    case BackgroundMode::Black:
        // style.css 的默认值就是纯黑，不需要额外覆盖
        return {};
    case BackgroundMode::Theme:
        // 亮色时的覆盖写在 style.css 的 window.light.* 规则里（代码负责切 light 类）
        return {};
    case BackgroundMode::Glow:
        // 官方截图里那层满宽蓝色辉光，作为可选项保留
        return "window.background { background-color: #11151d; background-image: "
               "radial-gradient(ellipse 95% 42% at 50% 5%, #3430a4 0%, #26286f 42%, "
               "#11151d 78%); }";
    case BackgroundMode::Color:
        return "window.background { background-color: " + ui.background.color +
               "; background-image: none; }";
    case BackgroundMode::Image:
        // 图片铺满，卡片改成半透明好让图片透出来（.image-bg 见 style.css）
        return "window.background { background-color: #000000; background-image: "
               "url(\"file://" +
               ui.background.image +
               "\"); background-size: cover; background-position: center; }";
    }
    return {};
}

} // namespace Config
