#pragma once
#include "EffectController.h"
#include "KeyBinder.h"
#include "KeyBinds.h"
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
