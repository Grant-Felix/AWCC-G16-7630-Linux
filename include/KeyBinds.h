#pragma once
// 按键绑定表（议题 #2）。
//
// 为什么以「MSC 扫描码」为主键：本机实测，G 模式键与 F2～F6 这六个键的 EV_MSC 扫描码各不相同
// （104/146/147/148/149/150），而其中 F4、F6 根本不发 EV_KEY——按 keycode 绑定会漏掉它们。
// 实测数据见 Forgejo 议题 #2 的讨论与 scripts/probe-keys.py。
//
// 文件位置定在 /etc/awcc/keybinds.conf：读键盘的是以 root 运行的 daemon，若读用户目录会有
// 多用户歧义；GUI 侧通过 daemon 的套接字写入，所以 GUI 自己不需要 root。
//
// 行格式（# 开头是注释）：
//   <扫描码> <动作> [参数]
// 动作取值见 ActionFromString()；未知动作会被拒绝，写回时不会保留。

#include <map>
#include <optional>
#include <string>
#include <vector>

class KeyBinds {
  public:
    struct Bind {
        int scan;         // EV_MSC 扫描码
        std::string action; // 见 ActionFromString()
    };

    /// 解析一行内容，失败时返回 nullopt（调用方可据此跳过并告警）。
    static std::optional<Bind> ParseLine(const std::string &line);

    /// 动作字符串是否合法（含参数校验）。
    static bool ValidAction(const std::string &action);

    /// 界面下拉里可选的动作（顺序即展示顺序）；界面负责把它们翻成中文（动作键本身
    /// 是配置里的稳定标识，不能随语言变）。
    static const std::vector<std::string> &AvailableActions();

    /// 扫描码对应的键名（语言无关，如 "F9"、"F2"），未知返回 nullptr。
    static const char *KeyLabel(int scan);

    /// 出厂默认：G 模式键（104）切 G 模式、灯键（105，本机没有）循环亮度，
    /// 其余五个实测扫描码先设为 none——不猜用户想绑什么。
    static KeyBinds Defaults();

    /// 读文件；文件不存在或为空时返回 Defaults()（daemon 首次启动即得到可用状态）。
    static KeyBinds Load(const std::string &path);

    /// 写文件（原子替换：先写临时文件再 rename，避免写一半被读到）。
    bool Save(const std::string &path) const;

    [[nodiscard]] const std::map<int, Bind> &Items() const { return m_binds; }
    /// 设置某个扫描码的绑定；action 非法时返回 false 且不改动。
    bool Set(int scan, const std::string &action);
    /// 该扫描码是否有绑定（未绑定的键 daemon 直接忽略）。
    [[nodiscard]] bool Bound(int scan) const { return m_binds.count(scan) != 0; }
    /// 该扫描码的动作，未绑定返回空串。
    [[nodiscard]] const std::string &ActionFor(int scan) const;

  private:
    std::map<int, Bind> m_binds;
};
