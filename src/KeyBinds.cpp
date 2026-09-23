#include "KeyBinds.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <loguru.hpp>
#include <sstream>

namespace {
// 动作里的模式名 → 与 Thermals 的六档一致（见 src/ui/Ui.cpp 的 kModes）
const char *kModeNames[] = {"battery", "cool", "quiet", "balanced", "performance",
                            "gmode"};

std::string Trim(const std::string &s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}
} // namespace

std::optional<KeyBinds::Bind> KeyBinds::ParseLine(const std::string &line) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
        return std::nullopt;
    }
    std::istringstream ss(trimmed);
    std::string scanText;
    std::string action;
    if (!(ss >> scanText >> action)) {
        return std::nullopt;
    }
    int scan = 0;
    try {
        scan = std::stoi(scanText);
    } catch (...) {
        return std::nullopt;
    }
    if (scan <= 0 || !ValidAction(action)) {
        return std::nullopt;
    }
    return Bind{scan, action};
}

bool KeyBinds::ValidAction(const std::string &action) {
    if (action == "none" || action == "gmode-toggle" || action == "brightness-cycle" ||
        action == "effect-next") {
        return true;
    }
    const std::string prefix = "mode:";
    if (action.rfind(prefix, 0) == 0) {
        const std::string mode = action.substr(prefix.size());
        for (const char *name : kModeNames) {
            if (mode == name) {
                return true;
            }
        }
    }
    return false;
}

const std::vector<std::string> &KeyBinds::AvailableActions() {
    static const std::vector<std::string> actions{
        "none", "gmode-toggle", "brightness-cycle", "effect-next",
        "mode:battery", "mode:cool", "mode:quiet", "mode:balanced",
        "mode:performance", "mode:gmode"};
    return actions;
}

const char *KeyBinds::KeyLabel(int scan) {
    switch (scan) {
    case 104:
        return "F9";
    case 146:
        return "F2";
    case 147:
        return "F3";
    case 148:
        return "F4";
    case 149:
        return "F5";
    case 150:
        return "F6";
    case 105:
        return "Light";
    default:
        return nullptr;
    }
}

KeyBinds KeyBinds::Defaults() {
    KeyBinds binds;
    binds.m_binds[104] = Bind{104, "gmode-toggle"};
    binds.m_binds[105] = Bind{105, "brightness-cycle"};
    for (int scan : {146, 147, 148, 149, 150}) {
        binds.m_binds[scan] = Bind{scan, "none"};
    }
    return binds;
}

KeyBinds KeyBinds::Load(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        LOG_S(INFO) << "KeyBinds: " << path << " not found, using defaults";
        return Defaults();
    }
    KeyBinds binds;
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const auto parsed = ParseLine(line);
        if (!parsed) {
            if (!Trim(line).empty() && Trim(line)[0] != '#') {
                LOG_S(WARNING) << "KeyBinds: 跳过无效行 " << path << ":" << lineno
                               << " -> " << line;
            }
            continue;
        }
        binds.m_binds[parsed->scan] = *parsed;
    }
    if (binds.m_binds.empty()) {
        LOG_S(WARNING) << "KeyBinds: " << path << " 里没有有效绑定，回落到默认";
        return Defaults();
    }
    return binds;
}

bool KeyBinds::Save(const std::string &path) const {
    // 原子替换：daemon 随时可能重读，写一半被读到会得到半个表
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            LOG_S(ERROR) << "KeyBinds: 无法写入 " << tmp;
            return false;
        }
        out << "# 按键绑定表（议题 #2）。格式：<扫描码> <动作>\n";
        out << "# 扫描码是 EV_MSC 的扫描码，实测值见 scripts/probe-keys.py\n";
        out << "# 动作：" << "none / gmode-toggle / brightness-cycle / effect-next / "
            << "mode:{battery,cool,quiet,balanced,performance,gmode}\n";
        for (const auto &[scan, bind] : m_binds) {
            const char *label = KeyLabel(scan);
            out << scan << " " << bind.action;
            if (label != nullptr) {
                out << "   # " << label;
            }
            out << "\n";
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        LOG_S(ERROR) << "KeyBinds: rename 失败: " << ec.message();
        return false;
    }
    return true;
}

bool KeyBinds::Set(int scan, const std::string &action) {
    if (scan <= 0 || !ValidAction(action)) {
        return false;
    }
    m_binds[scan] = Bind{scan, action};
    return true;
}

const std::string &KeyBinds::ActionFor(int scan) const {
    static const std::string empty;
    const auto it = m_binds.find(scan);
    return it == m_binds.end() ? empty : it->second.action;
}
