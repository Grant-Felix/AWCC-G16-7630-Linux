#!/usr/bin/env bash
# AWCC-G16-7630-Linux 交互式安装脚本（TUI）
#
# 为什么用脚本而不是发行版包：这个 fork 只针对 Dell G16 7630 验证过，装机量小、迭代快，
# 维护 deb/rpm/AUR 三套配方的成本高于收益。本脚本在用户机器上按需构建并安装，同时提供卸载。
#
# 设计约定：
#   - 菜单用**数字输入**（不用方向键）：本机终端对方向键支持不佳（早前踩过）
#   - 安装到系统目录（/usr/bin 等），因为程序里的路径是写死的（配置在 /etc/awcc、
#     按键绑定在 /etc/awcc/keybinds.conf）；所以要 sudo
#   - 装完把**文件清单**记到 /var/lib/awcc/installed-files.list，卸载据此逐个删除，
#     不去猜「哪些文件可能是我们装的」
#   - AWCC_INSTALL_ROOT 可以把整套装到别处（测试/试用用，卸载同样认它）
#
# 用法：
#   ./install.sh                # 交互菜单
#   ./install.sh --check        # 只检查依赖
#   ./install.sh --build        # 只构建
#   ./install.sh --install      # 直接安装（不提问，仍会提示 sudo）
#   ./install.sh --uninstall    # 直接卸载
#   AWCC_INSTALL_ROOT=/tmp/awcc ./install.sh --install    # 装到别处（不动系统）
set -uo pipefail

readonly SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly BUILD_DIR="$SELF_DIR/build"
readonly STAGE_PREFIX="/usr"
readonly SERVICE="awccd"
readonly UDEV_RULE="70-awcc.rules"
readonly INSTALL_ROOT="${AWCC_INSTALL_ROOT:-}"
readonly MANIFEST="${INSTALL_ROOT}/var/lib/awcc/installed-files.list"
readonly CONFIG_DIR="${INSTALL_ROOT}/etc/awcc"

# ── 外观 ────────────────────────────────────────────────────────────────────
# 只在真正往终端写的时候上色：重定向到文件/管道时自动关掉，日志才干净
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    C_RESET=$'\033[0m'; C_DIM=$'\033[2m'; C_BOLD=$'\033[1m'
    C_TITLE=$'\033[1;36m'; C_OK=$'\033[1;32m'; C_WARN=$'\033[1;33m'
    C_ERR=$'\033[1;31m'; C_KEY=$'\033[1;35m'
else
    C_RESET=; C_DIM=; C_BOLD=; C_TITLE=; C_OK=; C_WARN=; C_ERR=; C_KEY=
fi

# 终端宽度，夹在 62~96 之间：太窄的窗口不至于折行，太宽也不至于空荡
ui_width() {
    local w
    w="$(tput cols 2>/dev/null || echo 80)"
    [ "$w" -lt 62 ] && w=62
    [ "$w" -gt 96 ] && w=96
    printf '%s' "$w"
}

# 分隔线：用终端宽度（locale 无关），夹在 56~88 之间
rule() {
    local w
    w="$(tput cols 2>/dev/null || echo 72)"
    [ "$w" -lt 48 ] && w=48
    [ "$w" -gt 88 ] && w=88
    printf '  %s%s%s\n' "$C_DIM" "$(printf '─%.0s' $(seq 1 "$((w - 4))"))" "$C_RESET"
}

# 说明：这里**不画右边框**——右边框要求精确算中文的显示宽度，而 ${#s} 与切片在不同 locale 下
# 是按字符还是按字节并不一致（实测 LC_ALL=C 时一个中文字被当成 3 个字符，框就歪了）。
# 只画横向分隔线就不需要任何宽度计算，任何终端/语言环境都不会歪。
info() { printf '%s  ⟶%s %s\n' "$C_TITLE" "$C_RESET" "$*"; }
ok()   { printf '%s  ✓%s %s\n' "$C_OK" "$C_RESET" "$*"; }
warn() { printf '%s  !%s %s\n' "$C_WARN" "$C_RESET" "$*"; }
err()  { printf '%s  ✗%s %s\n' "$C_ERR" "$C_RESET" "$*" >&2; }

# 数字菜单：只认数字 + 回车（本机终端对方向键支持不佳，早前踩过）
choose() {
    local question="$1" def="$2"; shift 2
    local -a items=("$@")
    printf '\n   %s%s%s\n\n' "$C_BOLD" "$question" "$C_RESET"
    local i=1
    for it in "${items[@]}"; do
        if [ "$i" -eq "$def" ]; then
            printf '    %s%s%s  %s   %s★ 默认%s\n' "$C_KEY" "$i" "$C_RESET" "$it" "$C_DIM" "$C_RESET"
        else
            printf '    %s%s%s  %s\n' "$C_KEY" "$i" "$C_RESET" "$it"
        fi
        i=$((i + 1))
    done
    printf '\n   %s数字 + 回车；直接回车取默认项%s\n' "$C_DIM" "$C_RESET"
    printf '   %s›%s ' "$C_TITLE" "$C_RESET"
    local answer=""
    read -r answer || true
    [ -z "$answer" ] && answer="$def"
    if ! [[ "$answer" =~ ^[0-9]+$ ]] || [ "$answer" -lt 1 ] || [ "$answer" -gt "${#items[@]}" ]; then
        warn "输入无效，按默认项 $def 处理"
        answer="$def"
    fi
    REPLY="$answer"
}

confirm() {
    printf '  %s?%s %s %s[y/N]%s ' "$C_WARN" "$C_RESET" "$1" "$C_DIM" "$C_RESET"
    local a=""
    read -r a || true
    case "$a" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# 检查结果放全局，供菜单里展示与安装前拦截
DEPS_MISSING=()
check_deps() {
    DEPS_MISSING=()
    info "检查构建依赖"
    local cmd
    for cmd in cmake ninja meson git pkg-config; do
        if command -v "$cmd" >/dev/null 2>&1; then
            ok "$cmd"
        else
            err "缺少命令：$cmd"
            DEPS_MISSING+=("$cmd")
        fi
    done
    # 头文件/库用 pkg-config 探（比猜包名可靠）
    local mod
    for mod in gtk4 libadwaita-1 libudev; do
        if pkg-config --exists "$mod" 2>/dev/null; then
            ok "pkg-config: $mod $(pkg-config --modversion "$mod" 2>/dev/null)"
        else
            err "缺少开发包：$mod"
            DEPS_MISSING+=("$mod")
        fi
    done
    if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1; then
        err "缺少 C++ 编译器（g++ 或 clang++）"
        DEPS_MISSING+=("c++")
    else
        ok "C++ 编译器"
    fi
    if [ "${#DEPS_MISSING[@]}" -gt 0 ]; then
        printf '\n缺这些，按发行版可以这样装：\n  %s\n' "$(dep_install_hint)"
        return 1
    fi
    ok "构建依赖齐全"
    check_runtime_deps || true
    DEPS_MISSING=()
    return 0
}

# ── 运行时依赖 ──────────────────────────────────────────────────────────────
# acpi_call 是热模式 / 风扇控制的前提（daemon 往 /proc/acpi/call 写 ACPI 命令）。
# 发行版包当年是靠 depends 声明让包管理器处理的，脚本安装得自己管——而且要分两种情况：
# 模块文件在不在（重启后还能不能用）、以及当前内核是否恰好还加载着它。
readonly DEP_MARKER="${INSTALL_ROOT}/var/lib/awcc/installed-deps"

acpi_module_present() { modinfo acpi_call >/dev/null 2>&1; }
acpi_currently_loaded() { [ -e /proc/acpi/call ]; }

acpi_install_hint() {
    case "$(detect_distro)" in
        arch)   echo "sudo pacman -S --needed acpi_call-dkms   # 多内核都用的话还需要各内核的 -headers；也可按内核装 acpi_call / acpi_call-lts" ;;
        debian) echo "sudo apt install acpi-call-dkms" ;;
        fedora) echo "acpi_call 在 RPM Fusion 里：sudo dnf install akmod-acpi_call（先启用 rpmfusion-free）" ;;
        *)      echo "请自行安装 acpi_call 内核模块（DKMS 包名多为 acpi-call-dkms）" ;;
    esac
}

check_runtime_deps() {
    info "检查运行时依赖"
    local missing=0
    if acpi_module_present; then
        ok "acpi_call 内核模块（热模式 / 风扇控制）"
    elif acpi_currently_loaded; then
        # 本机实测状态：模块文件已被移除（例如 pacman -Rns 连带卸掉了 acpi_call-dkms），
        # 但当前内核还加载着它，所以现在能用、重启后就没了
        warn "acpi_call 模块文件已不在，只是当前内核还加载着 /proc/acpi/call —— **重启后会失效**"
        printf '   装回来：%s\n' "$(acpi_install_hint)"
        missing=1
    else
        err "缺少 acpi_call 内核模块（热模式 / 风扇控制用不了）"
        printf '   装它：%s\n' "$(acpi_install_hint)"
        missing=1
    fi
    if [ "$missing" -eq 0 ]; then
        ok "运行时依赖齐全"
        return 0
    fi
    return 1
}

install_runtime_deps() {
    acpi_module_present && return 0
    warn "热模式与风扇控制依赖 acpi_call 内核模块，它现在不可用"
    if ! confirm "现在按上面的命令安装它吗？"; then
        warn "跳过：界面能用，但热模式 / 风扇控制会失败"
        return 0
    fi
    case "$(detect_distro)" in
        arch)
            if $SUDO pacman -S --needed --noconfirm acpi_call-dkms; then
                ok "已安装 acpi_call-dkms"
            else
                err "安装失败，请手动装（多内核注意装各内核的 headers）"
                return 1
            fi
            ;;
        debian)
            $SUDO apt install -y acpi-call-dkms || { err "安装失败"; return 1; }
            ;;
        fedora)
            $SUDO dnf install -y akmod-acpi_call || { err "安装失败（acpi_call 在 RPM Fusion 里）"; return 1; }
            ;;
        *)
            err "不认识这个发行版，请手动装 acpi_call，再重跑本脚本"
            return 1
            ;;
    esac
    # 记一笔：卸载时只删「我们装的」依赖，不动用户原本就有的
    if [ -n "$SUDO" ]; then
        $SUDO install -d "$(dirname "$DEP_MARKER")"
        echo "acpi_call-dkms" | $SUDO tee "$DEP_MARKER" >/dev/null
    else
        mkdir -p "$(dirname "$DEP_MARKER")"
        echo "acpi_call-dkms" >"$DEP_MARKER"
    fi
    ok "已记录（卸载时会问你是否一并移除）"
    return 0
}

# ── 构建 ────────────────────────────────────────────────────────────────────
do_build() {
    if ! check_deps; then
        err "依赖不全，无法构建"
        return 1
    fi
    info "配置并编译（首次会联网拉取 libusb / libevdev / loguru 等源码，需要几分钟）"
    if ! cmake -S "$SELF_DIR" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release; then
        err "cmake 配置失败"
        return 1
    fi
    if ! ninja -C "$BUILD_DIR"; then
        err "编译失败"
        return 1
    fi
    ok "编译完成：$BUILD_DIR/awcc"
    # 自检：不用显示器也能跑，能看到版本与页面树
    if "$BUILD_DIR/awcc" -h >/dev/null 2>&1; then
        ok "二进制自检通过：$("$BUILD_DIR/awcc" -h 2>/dev/null | head -1)"
    fi
    return 0
}

# ── 上游包冲突 ──────────────────────────────────────────────────────────────
pacman_owner() {
    command -v pacman >/dev/null 2>&1 || return 1
    pacman -Qo "$1" 2>/dev/null | awk '{print $NF}'
}

handle_upstream_conflict() {
    local owner
    owner="$(pacman_owner "/usr/bin/awcc" || true)"
    if [ -n "$owner" ]; then
        warn "检测到 /usr/bin/awcc 由包 $owner 提供（多半是上游的 awcc-bin / awcc-git）"
        warn "不先卸掉它，我们的文件会与包管理器互相覆盖，卸载时也会打架"
        if confirm "现在卸载 $owner 吗？"; then
            sudo pacman -R --noconfirm "$owner" || {
                err "卸载失败，请手动处理后重跑"
                return 1
            }
            ok "已卸载 $owner"
        else
            err "保留它会互相覆盖，安装中止"
            return 1
        fi
    fi
    # 手动装的那份（无包托管）会盖住我们装到 /usr/bin 的
    if [ -e /usr/local/bin/awcc ]; then
        warn "检测到 /usr/local/bin/awcc（无包托管的手动安装），它在 PATH 里更靠前会盖住我们这份"
        if confirm "把它改名为 /usr/local/bin/awcc.bak 吗？"; then
            sudo mv -v /usr/local/bin/awcc /usr/local/bin/awcc.bak && ok "已备份为 awcc.bak"
        fi
    fi
    return 0
}

# ── 安装 ────────────────────────────────────────────────────────────────────
do_install() {
    [ -x "$BUILD_DIR/awcc" ] || { err "还没构建，请先构建"; return 1; }
    # 装到系统时要 sudo；装到 AWCC_INSTALL_ROOT 时不用
    [ -n "$INSTALL_ROOT" ] && mkdir -p "$INSTALL_ROOT"
    local SUDO=""
    if [ -z "$INSTALL_ROOT" ]; then
        if ! sudo -n true 2>/dev/null; then
            info "安装到系统目录需要管理员权限，下面会提示输入密码"
        fi
        SUDO="sudo"
        handle_upstream_conflict || return 1
    fi

    install_runtime_deps || return 1

    # 先装到暂存目录：这样能得到**准确的文件清单**（卸载就靠它，不用去猜）
    local stage
    stage="$(mktemp -d)"
    info "暂存安装到 $stage"
    if ! DESTDIR="$stage" cmake --install "$BUILD_DIR" --component awcc >/dev/null; then
        err "cmake --install 失败"
        rm -rf "$stage"
        return 1
    fi
    local count
    count="$(cd "$stage" && find . -type f | wc -l)"
    ok "共 $count 个文件"

    # 覆盖已安装的二进制前必须先停服务：daemon 正在执行 /usr/bin/awcc 时，
    # 直接 cp 会 ETXTBSY（"文本文件忙"）而**静默**失败——实测装完看着成功、其实还是旧二进制
    local was_active=0
    if [ -z "$INSTALL_ROOT" ] && systemctl is-active --quiet "$SERVICE" 2>/dev/null; then
        was_active=1
        info "先停掉 $SERVICE（它正占用 /usr/bin/awcc，不停就换不掉）"
        sudo systemctl stop "$SERVICE" || warn "停止失败，继续尝试"
    fi
    if [ -z "$INSTALL_ROOT" ] && pgrep -x awcc >/dev/null 2>&1; then
        warn "还有 awcc 进程在跑（多半是界面窗口）；关掉它更稳"
    fi

    # 已有配置先留个备份：包管理器遇到改动过的配置会写 .pacnew，我们不覆盖用户的手改
    local db="${INSTALL_ROOT}/etc/awcc/database.json"
    if [ -e "$db" ]; then
        warn "已存在 $db，先备份为 database.json.bak"
        if [ -n "$SUDO" ]; then $SUDO cp -a "$db" "$db.bak"; else cp -a "$db" "$db.bak"; fi
    fi

    info "复制到 ${INSTALL_ROOT:-/}"
    # 老版本把图标散放在主题根目录下（Icon=awcc 解析不到，显示空图标），顺手清掉
    local legacy_icon="${INSTALL_ROOT}/usr/share/icons/awcc.png"
    if [ -e "$legacy_icon" ]; then
        [ -n "$SUDO" ] && $SUDO rm -f "$legacy_icon" || rm -f "$legacy_icon"
        warn "清掉了旧位置的空图标残留 $legacy_icon"
    fi

    # 二进制先删再放：运行中的进程持有旧 inode，删掉名字不影响它；而直接覆盖写会 ETXTBSY
    local bin_target="${INSTALL_ROOT}/usr/bin/awcc"
    [ -n "$SUDO" ] && $SUDO rm -f "$bin_target" || rm -f "$bin_target"
    local copied=0
    if [ -n "$SUDO" ]; then
        $SUDO cp -a "$stage"/. "${INSTALL_ROOT}/" && copied=1
    else
        cp -a "$stage"/. "${INSTALL_ROOT}/" && copied=1
    fi
    if [ "$copied" -eq 0 ]; then
        err "复制失败（看上面 cp 的报错；若是"文本文件忙"，说明还有进程在跑这个二进制）"
        [ "$was_active" -eq 1 ] && sudo systemctl start "$SERVICE" 2>/dev/null || true
        rm -rf "$stage"
        return 1
    fi
    ok "文件已就位"

    # 记清单（卸载据此删除）
    local manifest_dir
    manifest_dir="$(dirname "$MANIFEST")"
    if [ -n "$SUDO" ]; then
        $SUDO install -d "$manifest_dir"
        (cd "$stage" && find . -type f | sed 's|^\./||') | $SUDO tee "$MANIFEST" >/dev/null
    else
        mkdir -p "$manifest_dir"
        (cd "$stage" && find . -type f | sed 's|^\./||') >"$MANIFEST"
    fi
    ok "文件清单记到 $MANIFEST"

    # udev 规则与 systemd 单元
    if [ -z "$INSTALL_ROOT" ]; then
        info "刷新 udev 与 systemd"
        sudo udevadm control --reload || warn "udevadm reload 失败（不致命）"
        sudo systemctl daemon-reload || warn "daemon-reload 失败（不致命）"
        if confirm "现在启用并启动守护进程 $SERVICE 吗？（热模式/风扇/按键绑定需要它）"; then
            sudo systemctl enable --now "$SERVICE" && ok "$SERVICE 已启动"
        else
            warn "稍后可手动：sudo systemctl enable --now $SERVICE"
        fi
        # 这两条是 pacman 的钩子会自动做的，脚本安装得自己来，否则菜单里可能看不到图标
        # 图标缓存在主题目录里（hicolor），不是 /usr/share/icons 根目录
        command -v gtk-update-icon-cache >/dev/null 2>&1 && \
            sudo gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor 2>/dev/null || true
        command -v update-desktop-database >/dev/null 2>&1 && \
            sudo update-desktop-database -q /usr/share/applications 2>/dev/null || true
    else
        warn "装到了 $INSTALL_ROOT（测试用），跳过 systemd / udev 操作"
    fi
    # 刚才为覆盖二进制停过服务，这里恢复（若用户已选择启用启动，上面那条已经起来了）
    if [ "$was_active" -eq 1 ] && ! systemctl is-active --quiet "$SERVICE" 2>/dev/null; then
        sudo systemctl start "$SERVICE" && ok "$SERVICE 已重新启动"
    fi

    printf '\n安装完成。\n'
    printf '  启动界面：%s --gui\n' "${INSTALL_ROOT}/usr/bin/awcc"
    printf '  语言随系统；配置在 %s（背景）与 %s（按键绑定）\n' "$CONFIG_DIR/config.ini" "$CONFIG_DIR/keybinds.conf"
    return 0
}

# ── 卸载 ────────────────────────────────────────────────────────────────────
do_uninstall() {
    local manifest="$MANIFEST"
    if [ ! -r "$manifest" ]; then
        err "找不到安装清单 $manifest"
        warn "如果你是用旧办法（发行版包或手动复制）装的，这个脚本卸不了它"
        warn "手动安装的可以自己删：/usr/bin/awcc、/etc/systemd/system/awccd.service、"
        warn "  /etc/udev/rules.d/$UDEV_RULE、/etc/awcc/、/usr/share/applications/awcc.desktop"
        return 1
    fi
    local total
    total="$(wc -l <"$manifest")"
    info "清单里有 $total 个文件"
    printf '将要删除（前 15 个）：\n'
    head -15 "$manifest" | sed 's|^|  /|'
    [ "$total" -gt 15 ] && printf '  … 其余 %d 个\n' "$((total - 15))"
    local keep_config=1
    if confirm "保留配置与按键绑定（$CONFIG_DIR）吗？"; then
        keep_config=1
    else
        keep_config=0
    fi
    if ! confirm "确认卸载？"; then
        warn "已取消"
        return 1
    fi

    local SUDO=""
    [ -z "$INSTALL_ROOT" ] && SUDO="sudo"

    if [ -z "$INSTALL_ROOT" ]; then
        info "停止并禁用守护进程"
        sudo systemctl disable --now "$SERVICE" 2>/dev/null || warn "$SERVICE 未在运行"
    fi

    info "按清单删除文件"
    local removed=0
    while IFS= read -r rel; do
        [ -z "$rel" ] && continue
        case "$rel" in
            etc/awcc/*)
                [ "$keep_config" -eq 1 ] && continue
                ;;
        esac
        if [ -n "$SUDO" ]; then
            $SUDO rm -f "${INSTALL_ROOT}/$rel" && removed=$((removed + 1))
        else
            rm -f "${INSTALL_ROOT}/$rel" && removed=$((removed + 1))
        fi
    done <"$manifest"
    ok "删了 $removed 个文件"

    # 空目录顺手清掉（只删空的，不动别的软件的东西）
    if [ -n "$SUDO" ]; then
        $SUDO find "${INSTALL_ROOT}/usr/share/awcc" "${INSTALL_ROOT}/usr/lib/awcc" \
            -type d -empty -delete 2>/dev/null || true
        $SUDO rm -f "$manifest"
    else
        find "${INSTALL_ROOT}/usr/share/awcc" "${INSTALL_ROOT}/usr/lib/awcc" \
            -type d -empty -delete 2>/dev/null || true
        rm -f "$manifest"
    fi

    # 运行时依赖：只删我们装的那份（安装时记在 DEP_MARKER 里），且走包管理器，
    # 这样 DKMS 模块移除与 initramfs 重建等钩子才会像当年 pacman -Rns 那样自动跑
    if [ -r "$DEP_MARKER" ] && [ -z "$INSTALL_ROOT" ]; then
        local dep
        dep="$(cat "$DEP_MARKER")"
        warn "acpi_call 内核模块是安装时由本脚本装的（$dep）"
        if confirm "一并移除 $dep 吗？（其它软件可能也要用它）"; then
            case "$(detect_distro)" in
                arch)   sudo pacman -R --noconfirm "$dep" && ok "已移除 $dep" ;;
                debian) sudo apt remove -y "$dep" && ok "已移除 $dep" ;;
                fedora) sudo dnf remove -y akmod-acpi_call && ok "已移除" ;;
                *)      warn "请手动移除" ;;
            esac
        fi
        [ -n "$SUDO" ] && $SUDO rm -f "$DEP_MARKER" || rm -f "$DEP_MARKER"
    fi

    if [ -z "$INSTALL_ROOT" ]; then
        info "刷新 udev 与 systemd"
        sudo udevadm control --reload 2>/dev/null || true
        sudo systemctl daemon-reload 2>/dev/null || true
    fi
    printf '\n卸载完成。\n'
    [ "$keep_config" -eq 1 ] && printf '  配置与按键绑定保留在 %s\n' "$CONFIG_DIR"
    return 0
}

# ── 菜单 ────────────────────────────────────────────────────────────────────
ui_header() {
    local ver="${C_DIM}未安装${C_RESET}" state=""
    if [ -x "${INSTALL_ROOT}/usr/bin/awcc" ]; then
        ver="$("${INSTALL_ROOT}/usr/bin/awcc" -h 2>/dev/null | head -1 | sed 's/Alienware Command Center //')"
    fi
    if [ -n "$INSTALL_ROOT" ]; then
        state="  ${C_DIM}·${C_RESET} 安装根 ${INSTALL_ROOT}"
    elif command -v systemctl >/dev/null 2>&1; then
        state="  ${C_DIM}·${C_RESET} 守护进程 $(systemctl is-active "$SERVICE" 2>/dev/null | head -1 || echo unknown)"
    fi
    rule
    printf '   %sAWCC%s  ·  Dell G16 7630 Linux 适配版\n' "$C_TITLE" "$C_RESET"
    rule
    printf '   %s源码%s  %s\n' "$C_DIM" "$C_RESET" "$SELF_DIR"
    printf '   %s状态%s  %s%s\n' "$C_DIM" "$C_RESET" "$ver" "$state"
    rule
}

menu() {
    while true; do
        printf '\n'
        ui_header
        choose "请选择操作" 1 \
            "构建并安装" \
            "只构建（不安装）" \
            "检查依赖" \
            "卸载" \
            "退出"
        printf '\n'
        case "$REPLY" in
            1) do_build && do_install ;;
            2) do_build ;;
            3) check_deps ;;
            4) do_uninstall ;;
            5) printf '  %s再见%s\n\n' "$C_DIM" "$C_RESET"; return 0 ;;
        esac
    done
}

case "${1:-}" in
    --check)     check_deps ;;
    --build)     do_build ;;
    --install)   do_build && do_install ;;
    --uninstall) do_uninstall ;;
    ""|--menu)   menu ;;
    -h|--help)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        ;;
    *) err "未知参数：$1（用 --help 看用法）"; exit 2 ;;
esac
