#pragma once
#include "EffectController.h"
#include "KeyBinder.h"
#include "KeyBinds.h"
#include <mutex>
#include <string>
#include <functional>
#include <loguru.hpp>
#include <thread>

class Thermals;

class Daemon {
  private:
    bool m_running;
    int m_server_fd;
    std::string m_socket_path;
    std::thread m_keybinderThread;
    void m_StopBinder();
    KeyBinder *m_binder = nullptr;
    EffectController &m_effectsController;
    // Thermals &thermals;

    short m_brightness{};
    void m_onGmodeKey();
    void m_onLightKey();
    // ── 按键绑定（议题 #2）──
    KeyBinds m_keyBinds;
    std::string m_keyBindPath = "/etc/awcc/keybinds.conf";
    Thermals *m_thermals = nullptr; // 可选：绑到「切模式」的动作要用它
    /// 执行某个扫描码绑定的动作。
    void m_performKeyAction(int scan);
    /// 处理 keybind-list / keybind-set 这两条自定义命令（在 popen 之前分支掉）。
    std::string m_HandleKeyBindCommand(const std::string &cmd);

    // ── 状态与订阅（见 DESIGN.md 第九节）────────────────────────────────
    // 状态的所有权在 daemon：GUI 只保存快照、只发命令；任何改动都广播出去，
    // 这样「按 F9 改模式」与「GUI 里点模式」看到的是同一份值。
    struct State {
        std::string mode = "balanced"; // battery/cool/quiet/balanced/performance/gmode
        int brightness = 50;           // 0-100
        size_t keybindsVersion = 1;    // 绑定表改一次涨一，客户端据此重读
    };
    State m_state;
    mutable std::mutex m_stateMutex;   // 保护 m_state
    std::vector<int> m_subscribers;    // 订阅者的 fd
    std::mutex m_subscribersMutex;     // 保护 m_subscribers
    /// 当前状态的快照文本（每行 `<属性> <值>`），订阅时先推这个。
    std::string m_StateSnapshot() const;
    /// 给所有订阅者推一行；写失败的（客户端已退出）顺手摘掉并关闭。
    void m_Broadcast(const std::string &line);
    /// 订阅者线程主体：推快照，然后阻塞在 read 上等对方断开。
    void m_ServeSubscriber(int fd);
    /// 改模式：真正改硬件 + 更新状态 + 广播（按键动作与 GUI 命令都走这里）。
    bool m_SetMode(const std::string &name);
    /// 改亮度：同上。
    bool m_SetBrightness(int value);
    std::function<void()> m_onGmodeKeyCallback;
    static bool m_CommandAllowed(const std::string &cmd);

  public:
    Daemon(EffectController &effectsController,
           std::string socket_path = "/tmp/awcc.sock");
    ~Daemon();
    void setOnGmodeKeyCallback(std::function<void()> cb) {
        m_onGmodeKeyCallback = std::move(cb);
    }
    /// 传入 Thermals 后，「mode:<名字>」这类动作才可用（main.cpp 里装配）。
    void setThermals(Thermals *thermals) { m_thermals = thermals; }
    bool isDaemonRunning();
    void init();
    std::string executeFromDaemon(const char *command);
    void stop();
    [[nodiscard]] bool isServerMode() const { return m_running; }
    // NOTE: This is not needed as handled manually
    // ~Daemon();
};
