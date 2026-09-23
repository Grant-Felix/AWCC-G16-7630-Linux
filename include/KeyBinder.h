#pragma once
#include "KeyBinds.h"
#include <chrono>
#include <functional>
#include <libevdev/libevdev.h>
#include <string>
#include <unordered_map>

class KeyBinder {
  public:
    using Callback = std::function<void()>;

    KeyBinder(const std::string &target_device_name, double timeout_sec = 0.3);
    ~KeyBinder();

    /// 绑定的绑定表（不持有所有权）。只对表里「已绑定且不是 none」的扫描码回调，
    /// 这样没绑定的键连唤醒 daemon 都不必（所有按键都会发 EV_MSC，不能来者必喧）。
    void setBinds(const KeyBinds *binds) { m_binds = binds; }
    /// 命中一次按键时回调，参数是 EV_MSC 扫描码。
    void setOnScan(std::function<void(int)> cb) { m_onScan = std::move(cb); }
    [[nodiscard]] bool isAvailable() const { return dev_ != nullptr; }
    void run();
    void stop();

  private:
    int fd_;
    libevdev *dev_;
    bool m_isRunning;
    double m_timeoutSec;
    std::unordered_map<int, std::chrono::steady_clock::time_point>
        m_lastTriggered;
    const KeyBinds *m_binds = nullptr; // 不持有所有权，daemon 保证活得更久
    std::function<void(int)> m_onScan;  // 参数是 EV_MSC 扫描码
};
