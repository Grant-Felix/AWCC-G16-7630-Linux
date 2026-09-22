#pragma once

#include <string>

// 用户配置：$XDG_CONFIG_HOME/awcc/config.ini（GKeyFile）。
//
// 为什么不用 GSettings（官方做法）：配置要同时被普通用户身份的 GUI 与 root 身份的 daemon 读，
// 而 GSettings 的后端 dconf 是 per-user 的，root 读用户的 dconf 很别扭。见 DESIGN.md 第二节。
//
// 为什么放用户目录而不是 /etc/awcc：/etc/awcc/brightness 归 root 所有，普通用户写不进去，
// 那正是「亮度每次打开都回到 50%」的根因。
namespace Config {

/// 背景模式。默认纯黑（需求），另有跟随系统亮暗 / 自定义颜色 / 自定义图片 / 官方蓝色辉光。
enum class BackgroundMode { Black, Theme, Color, Image, Glow };

struct Background {
    BackgroundMode mode = BackgroundMode::Black;
    std::string color = "#000000"; // Color 模式用，#RRGGBB
    std::string image;             // Image 模式用，绝对路径
};

/// 界面相关配置
struct Ui {
    Background background;
};

/// 配置文件的绝对路径（找不到 $XDG_CONFIG_HOME 时回落到 ~/.config）
[[nodiscard]] std::string configPath();

/// 读配置；文件不存在或读失败时返回默认值（不报错，首次运行就是这种情况）
[[nodiscard]] Ui load();

/// 写配置；目录不存在会创建。失败返回 false（调用方记日志即可，不该因此崩）
bool save(const Ui &ui);

/// 按背景配置生成覆盖用的 CSS 片段（只含 background 与配套的类选择器）。
/// 默认纯黑模式返回空串——style.css 里的默认值就是纯黑，不需要额外覆盖。
[[nodiscard]] std::string backgroundCss(const Ui &ui);

/// 供界面显示的模式的稳定标识（也是 ini 里的值）
[[nodiscard]] const char *modeKey(BackgroundMode mode);

/// 解析 ini 里的模式值，认不出就回落到默认（纯黑）
[[nodiscard]] BackgroundMode modeFromKey(const std::string &key);

} // namespace Config
