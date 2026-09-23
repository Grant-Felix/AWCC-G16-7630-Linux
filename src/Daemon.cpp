#include "Daemon.h"
#include "EffectController.h"
#include "KeyBinder.h"
#include "Thermals.h"
#include "LightFX.h"
#include "helper.h"
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <loguru.hpp>
#include <pwd.h>
#include <regex>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

#define BUF_SIZE 1024

namespace {
Daemon *s_instance = nullptr;

constexpr uid_t EXPECTED_UID = 1000;

void daemon_signal_handler(int /*unused*/) {
    if (s_instance != nullptr)
        s_instance->stop();
    exit(0);
}
} // namespace

Daemon::Daemon(EffectController &effectsController, std::string socket_path)
    : m_running(false), m_server_fd(-1), m_socket_path(std::move(socket_path)),
      m_effectsController(effectsController) {
    s_instance = this;
}

// Daemon::~Daemon() { stop(); }

Daemon::~Daemon() { LOG_S(INFO) << "Daemon Module deinitialized"; }
bool Daemon::isDaemonRunning() {
    return (access(m_socket_path.c_str(), F_OK) == 0 &&
            std::filesystem::exists(m_socket_path));
}

void Daemon::m_StopBinder() {
    if (m_binder != nullptr) {
        m_binder->stop();
        if (m_keybinderThread.joinable())
            m_keybinderThread.join();
        // Delete the binder
        delete m_binder;
        m_binder = nullptr;
    }
}

namespace {
/// 模式名 → ThermalModes；未知返回 false。名字与 KeyBinds / GUI 里的动作参数一致。
bool ModeFromName(const std::string &name, ThermalModes &out) {
    if (name == "battery") {
        out = ThermalModes::BatterySaver;
    } else if (name == "cool") {
        out = ThermalModes::Cool;
    } else if (name == "quiet") {
        out = ThermalModes::Quiet;
    } else if (name == "balanced") {
        out = ThermalModes::Balanced;
    } else if (name == "performance") {
        out = ThermalModes::Performance;
    } else if (name == "gmode") {
        out = ThermalModes::Gmode;
    } else {
        return false;
    }
    return true;
}
} // namespace

void Daemon::stop() {
    if (!m_running)
        return;
    if (m_server_fd != -1)
        close(m_server_fd);
    unlink(m_socket_path.c_str());
    LOG_S(INFO) << "Cleaned up socket file. Daemon stopped.";
    LOG_S(INFO) << "Stopping KeyBinder Module";
    m_StopBinder();
}

void Daemon::m_onGmodeKey() {
    if (m_onGmodeKeyCallback) {
        m_onGmodeKeyCallback();
    } else
        LOG_S(ERROR) << "GMode Callback Not set";
}

void Daemon::m_onLightKey() {
    const std::string path = "/etc/awcc/brightness";

    if (std::filesystem::exists(path)) {
        std::ifstream in(path);
        int value;
        if (in >> value) {
            if (value == 0 || value == 50 || value == 100) {
                m_brightness = value;
            }
        }
    }

    switch (m_brightness) {
    case 0:
        m_brightness = 50;
        break;
    case 50:
        m_brightness = 100;
        break;
    case 100:
        m_brightness = 0;
        break;
    default:
        m_brightness = 0;
        break;
    }

    m_effectsController.Brightness(m_brightness);

    std::ofstream out(path);
    if (out) {
        out << m_brightness;
    }
}

// TODO: Make it only allow a certain type of commands
void Daemon::init() {
    if (isDaemonRunning()) {
        LOG_S(ERROR)
            << "Socket file exists. Another daemon may be running. Exiting.";
        exit(1);
    }

    // Desktop systems do not expose the internal laptop keyboard used for
    // Alienware hotkeys. Keep the daemon usable and only skip those hotkeys.
    // 绑定表：文件不存在就用出厂默认（G 模式键切 G 模式、灯键循环亮度）
    m_keyBinds = KeyBinds::Load(m_keyBindPath);

    // 状态的初值：此刻在 daemon 里（root），读 ACPI 不会弹授权框；
    // 亮度沿用上游那个 /etc/awcc/brightness（记忆功能本身还没做，见 TODO）
    if (m_thermals != nullptr) {
        const ThermalModes current = m_thermals->getCurrentMode();
        const char *name = (current == ThermalModes::BatterySaver) ? "battery"
                           : (current == ThermalModes::Cool)        ? "cool"
                           : (current == ThermalModes::Quiet)       ? "quiet"
                           : (current == ThermalModes::Performance) ? "performance"
                           : (current == ThermalModes::Gmode)       ? "gmode"
                                                                    : "balanced";
        m_state.mode = name;
    }
    if (std::filesystem::exists("/etc/awcc/brightness")) {
        std::ifstream in("/etc/awcc/brightness");
        int value = 0;
        if (in >> value && value >= 0 && value <= 100) {
            m_state.brightness = value;
        }
    }
    LOG_S(INFO) << "初始状态：" << m_StateSnapshot();

    m_binder = new KeyBinder("AT Translated Set 2 keyboard");
    if (m_binder->isAvailable()) {
        m_binder->setBinds(&m_keyBinds);
        m_binder->setOnScan([this](int scan) { this->m_performKeyAction(scan); });
        m_keybinderThread = std::thread([this]() { this->m_binder->run(); });
    } else {
        LOG_S(WARNING) << "Internal keyboard hotkeys unavailable; continuing "
                          "without KeyBinder";
        delete m_binder;
        m_binder = nullptr;
    }

    signal(SIGINT, daemon_signal_handler);
    signal(SIGTERM, daemon_signal_handler);

    // std::atexit([]() {
    //     if (s_instance)
    //         s_instance->stop();
    // });
    //
    m_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_server_fd < 0) {
        LOG_S(ERROR) << "Failed to create socket: " << strerror(errno);
        exit(1);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(m_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_S(ERROR) << "Failed to bind socket: " << strerror(errno);
        exit(1);
    }

    // Security: set permissions and ownership
    // If running as root, chown to your app user and chmod to 0600
    chown(m_socket_path.c_str(), EXPECTED_UID, EXPECTED_UID);
    chmod(m_socket_path.c_str(), 0666);

    listen(m_server_fd, 5);
    m_running = true;
    LOG_S(INFO) << "Daemon listening on " << m_socket_path;

    while (m_running) {
        if (m_binder != nullptr && m_binder->isAvailable() &&
            !m_keybinderThread.joinable()) {
            LOG_S(ERROR) << "KeyBinder thread has exited!";
            m_StopBinder();
            break;
        }

        int client_fd = accept(m_server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            LOG_S(ERROR) << "Accept failed: " << strerror(errno);
            continue;
        }

        // SO_PEERCRED security check (Linux only)
        // struct ucred cred;
        // socklen_t len = sizeof(cred);
        // if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
        // {
        //     LOG_S(INFO) << "fixme: Client UID " << cred.uid;
        //     // if (cred.uid != EXPECTED_UID) {
        //     //     LOG_S(ERROR)
        //     //         << "Rejected unauthorized client (UID=" << cred.uid <<
        //     //         ")";
        //     //     close(client_fd);
        //     //     continue;
        //     // }
        // } else {
        //     LOG_S(ERROR) << "Failed to get peer credentials: "
        //                  << strerror(errno);
        //     close(client_fd);
        //     continue;
        // }

        std::array<char, BUF_SIZE> buf{};
        int n = read(client_fd, buf.data(), buf.size() - 1);
        if (n > 0) {
            buf[n] = '\0';
            // Sanitize input for command check
            std::string cmd(buf.data());
            cmd.erase(cmd.find_last_not_of(" \n\r\t") + 1);
            if (cmd == "stop") {
                LOG_S(INFO) << "Received stop command, shutting down daemon.";
                write(client_fd, "Daemon stopped", 15);
                close(client_fd);
                stop();  // cleanup
                exit(0); // kill process
            } else if (cmd == "subscribe") {
                // 长连接：交给专门线程，主循环不 close 这个 fd
                {
                    std::lock_guard<std::mutex> lock(m_subscribersMutex);
                    m_subscribers.push_back(client_fd);
                }
                LOG_S(INFO) << "新的订阅者，共 " << m_subscribers.size() << " 个";
                std::thread([this, client_fd] { this->m_ServeSubscriber(client_fd); })
                    .detach();
                continue;
            } else if (cmd.rfind("mode-set ", 0) == 0) {
                const std::string name = cmd.substr(std::string("mode-set ").size());
                const std::string out = m_SetMode(name) ? "ok\n" : "error: 未知模式\n";
                write(client_fd, out.c_str(), out.size());
            } else if (cmd.rfind("brightness-set ", 0) == 0) {
                int value = 0;
                std::istringstream ss(cmd.substr(std::string("brightness-set ").size()));
                const bool ok = static_cast<bool>(ss >> value) && m_SetBrightness(value);
                const std::string out = ok ? "ok\n" : "error: 亮度需为 0-100\n";
                write(client_fd, out.c_str(), out.size());
            } else if (cmd.rfind("keybind-", 0) == 0) {
                const std::string output = m_HandleKeyBindCommand(cmd);
                write(client_fd, output.c_str(), output.size());
            } else {
                std::string output = executeFromDaemon(cmd.c_str());
                write(client_fd, output.c_str(), output.size());
            }
        }
        close(client_fd);
    }
}
std::string Daemon::m_StateSnapshot() const {
    std::lock_guard<std::mutex> lock(m_stateMutex);
    std::string out = "mode " + m_state.mode + "\n";
    out += "brightness " + std::to_string(m_state.brightness) + "\n";
    out += "keybinds " + std::to_string(m_state.keybindsVersion) + "\n";
    return out;
}

void Daemon::m_Broadcast(const std::string &line) {
    std::lock_guard<std::mutex> lock(m_subscribersMutex);
    for (auto it = m_subscribers.begin(); it != m_subscribers.end();) {
        // 客户端可能已经退出：写失败就把它摘掉，别让一个死连接拖住所有广播
        if (write(*it, line.c_str(), line.size()) < 0) {
            close(*it);
            it = m_subscribers.erase(it);
        } else {
            ++it;
        }
    }
}

void Daemon::m_ServeSubscriber(int fd) {
    const std::string snapshot = m_StateSnapshot();
    if (write(fd, snapshot.c_str(), snapshot.size()) < 0) {
        close(fd);
        return;
    }
    // 之后就不再主动读客户端：阻塞在这里等对方断开（读到 0 或出错），
    // 推送由 m_Broadcast 从别的线程写同一个 fd（读写互不相干）。
    std::array<char, 64> buf{};
    while (read(fd, buf.data(), buf.size()) > 0) {
    }
    {
        std::lock_guard<std::mutex> lock(m_subscribersMutex);
        for (auto it = m_subscribers.begin(); it != m_subscribers.end(); ++it) {
            if (*it == fd) {
                m_subscribers.erase(it);
                break;
            }
        }
    }
    close(fd);
    LOG_S(INFO) << "订阅者断开，剩余 " << m_subscribers.size() << " 个";
}

bool Daemon::m_SetMode(const std::string &name) {
    ThermalModes mode;
    if (!ModeFromName(name, mode)) {
        return false;
    }
    std::string actual = name;
    if (m_thermals != nullptr) {
        m_thermals->setThermalMode(mode);
        // 回读真实值：ACPI 写可能静默失败（例如之前 pkexec 那条路），状态不能因此说谎
        const ThermalModes readBack = m_thermals->getCurrentMode();
        if (readBack != mode) {
            LOG_S(WARNING) << "模式没生效：请求 " << name << "，硬件回读仍是 "
                           << static_cast<int>(readBack);
        }
        for (const auto &[m, n] :
             std::initializer_list<std::pair<ThermalModes, const char *>>{
                 {ThermalModes::BatterySaver, "battery"}, {ThermalModes::Cool, "cool"},
                 {ThermalModes::Quiet, "quiet"}, {ThermalModes::Balanced, "balanced"},
                 {ThermalModes::Performance, "performance"}, {ThermalModes::Gmode, "gmode"}}) {
            if (m == readBack) {
                actual = n;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.mode = actual;
    }
    m_Broadcast("mode " + actual + "\n");
    return true;
}

bool Daemon::m_SetBrightness(int value) {
    if (value < 0 || value > 100) {
        return false;
    }
    m_effectsController.Brightness(static_cast<short>(value));
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state.brightness = value;
    }
    m_Broadcast("brightness " + std::to_string(value) + "\n");
    return true;
}

void Daemon::m_performKeyAction(int scan) {
    const std::string action = m_keyBinds.ActionFor(scan);
    const char *label = KeyBinds::KeyLabel(scan);
    LOG_S(INFO) << "按键触发：" << (label != nullptr ? label : "未知键") << "(" << scan
                << ") -> " << action;
    if (action.empty() || action == "none") {
        return;
    }
    if (action == "gmode-toggle") {
        m_onGmodeKey();
        // G 模式切换会改模式，广播后的真实值由下一次 set 或 GUI 读取校正
        m_Broadcast("mode " + std::string("gmode") + "\n");
        return;
    }
    if (action == "brightness-cycle") {
        m_onLightKey();
        return;
    }
    const std::string prefix = "mode:";
    if (action.rfind(prefix, 0) == 0) {
        if (m_thermals == nullptr) {
            LOG_S(WARNING) << "没有装配 Thermals，模式类动作不可用";
            return;
        }
        if (!m_SetMode(action.substr(prefix.size()))) {
            LOG_S(WARNING) << "未知模式：" << action;
        }
        return;
    }
    LOG_S(WARNING) << "未知动作：" << action;
}

// keybind-list / keybind-set 走这里，不经过白名单 + popen 那条通用路径：
// 它们是我们自己解析的结构化命令，没有理由让它们能拼出任意 shell。
std::string Daemon::m_HandleKeyBindCommand(const std::string &cmd) {
    if (cmd == "keybind-list") {
        std::string out;
        for (const auto &[scan, bind] : m_keyBinds.Items()) {
            const char *label = KeyBinds::KeyLabel(scan);
            out += std::to_string(scan) + " " + bind.action;
            if (label != nullptr) {
                out += " # " + std::string(label);
            }
            out += "\n";
        }
        return out;
    }
    if (cmd.rfind("keybind-set ", 0) == 0) {
        std::istringstream ss(cmd.substr(std::string("keybind-set ").size()));
        int scan = 0;
        std::string action;
        if (!(ss >> scan >> action)) {
            return "error: 用法 keybind-set <扫描码> <动作>\n";
        }
        if (!KeyBinds::ValidAction(action)) {
            return "error: 未知动作 " + action + "\n";
        }
        if (scan <= 0) {
            return "error: 扫描码必须是正整数\n";
        }
        if (!m_keyBinds.Set(scan, action)) {
            return "error: 设置失败\n";
        }
        if (!m_keyBinds.Save(m_keyBindPath)) {
            return "error: 写入 " + m_keyBindPath + " 失败\n";
        }
        if (m_binder != nullptr) {
            m_binder->setBinds(&m_keyBinds); // 立即生效，不必重启 daemon
        }
        size_t version = 0;
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            version = ++m_state.keybindsVersion; // 客户端据此重读绑定表
        }
        m_Broadcast("keybinds " + std::to_string(version) + "\n");
        return "ok\n";
    }
    return "error: 未知命令\n";
}

bool Daemon::m_CommandAllowed(const std::string &cmd) {
    // Allowed command patterns (expand as needed)
    if (cmd == "stop")
        return true;
    static const std::vector<std::regex> allowed_patterns{
        // pkexec ACPI call
        std::regex{
            R"(pkexec sh -c 'echo "\\_SB\..*WMAX 0 0x[0-9a-fA-F]+ \{(?:\s*0x[0-9a-fA-F]+,?)+\s*\}" > /proc/acpi/call && cat /proc/acpi/call')"},
        // Intel turbo
        std::regex{
            R"(echo [01] \| sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo)"},
        // AMD turbo
        std::regex{
            R"(echo [01] \| sudo tee /sys/devices/system/cpu/cpufreq/boost)"}};

    for (const auto &pat : allowed_patterns) {
        if (std::regex_match(cmd, pat))
            return true;
    }
    return false;
}

std::string Daemon::executeFromDaemon(const char *command) {
    if (m_running) {
        if (!m_CommandAllowed(command)) {
            LOG_S(ERROR) << "Rejected command: " << command;
            return "Rejected command";
        }
        // 以 root 运行的 daemon 不需要 pkexec：实测在 daemon 里 pkexec 会失败
        // （Error checking for authorization org.freedesktop.policykit.exec:
        //   org.freedesktop.PolicyKit1.Error.Failed: Process not found），
        // 于是热模式 / 风扇 boost / 睿频这些 ACPI 写全部无声失效。
        // 白名单已经按**原命令**校验过，这里只是把 pkexec 外壳剥掉、以 root 直接执行。
        std::string toRun = command;
        if (geteuid() == 0 && toRun.rfind("pkexec ", 0) == 0) {
            toRun = toRun.substr(std::string("pkexec ").size());
            LOG_S(INFO) << "以 root 直接执行（已去掉 pkexec）：" << toRun;
        } else {
            LOG_S(INFO) << "Executing command: " << toRun;
        }
        FILE *fp = popen(toRun.c_str(), "r");
        std::string result;
        if (fp == nullptr) {
            result = "Failed to execute command";
            LOG_S(ERROR) << result;
        } else {
            std::array<char, BUF_SIZE> outbuf{};
            while (fgets(outbuf.data(), outbuf.size(), fp) != nullptr) {
                result += outbuf.data();
            }
            pclose(fp);
        }
        return result;
    } else {
        int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            LOG_S(ERROR) << "Socket error: " << strerror(errno);
            return "Socket error";
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, m_socket_path.c_str(),
                sizeof(addr.sun_path) - 1);

        if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOG_S(ERROR) << "Connect error: " << strerror(errno);
            close(sock_fd);
            return "Connect error";
        }

        if (write(sock_fd, command, strlen(command)) < 0) {
            LOG_S(ERROR) << "Write error: " << strerror(errno);
            close(sock_fd);
            return "Write error";
        }

        std::array<char, BUF_SIZE> buf{};
        std::string output;
        int n;
        while ((n = read(sock_fd, buf.data(), buf.size() - 1)) > 0) {
            buf[n] = '\0';
            output += buf.data();
        }
        close(sock_fd);
        return output;
    }
}
