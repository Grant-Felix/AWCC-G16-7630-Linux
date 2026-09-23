#!/usr/bin/env bash
# daemon / 风扇控制 的诊断脚本（只读 + 一条无害的 ACPI 读，不改风扇转速）
#
# 为什么需要它：风扇与热模式都走 daemon（它以 root 跑、由它写 /proc/acpi/call），
# 而 daemon 的套接字在 /tmp，只有本机能连。把本脚本的输出贴回来就能定位。
set -uo pipefail

readonly SOCK="${AWCC_SOCK:-/tmp/awcc.sock}"
readonly BIN="/usr/bin/awcc"

hr() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }

hr "已装的版本与单元"
if [ -x "$BIN" ]; then
    "$BIN" -h 2>/dev/null | head -1
else
    echo "  $BIN 不存在（还没装？）"
fi
echo -n "  ExecStart: "
systemctl cat awccd 2>/dev/null | grep -m1 '^ExecStart=' || echo "（没有 awccd 单元）"
echo -n "  服务状态: "
systemctl is-active awccd 2>/dev/null | head -1 || echo unknown

hr "acpi_call 内核模块"
echo -n "  modinfo: "
modinfo acpi_call >/dev/null 2>&1 && echo "有（重启后也能用）" || echo "**没有**（重启后会失效）"
ls -l /proc/acpi/call 2>&1 | head -1
echo -n "  dkms: "
dkms status 2>/dev/null | grep -i acpi || echo "（dkms 里没有 acpi_call）"
echo -n "  当前内核: "; uname -r

hr "直连 daemon（用 python 读写套接字）"
python3 - "$SOCK" <<'PY'
import os, socket, sys, time

sock = sys.argv[1]
if not os.path.exists(sock):
    print(f"  {sock} 不存在 —— daemon 没在跑")
    sys.exit(0)

def talk(cmd, wait=1.5):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(wait)
    try:
        s.connect(sock)
        s.sendall(cmd.encode())
        data = b""
        while True:
            try:
                chunk = s.recv(512)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
        return data.decode(errors="replace").strip()
    finally:
        s.close()

print("  subscribe 的初始快照：")
for line in talk("subscribe").splitlines():
    print("    " + line)

print("  keybind-list：")
for line in talk("keybind-list").splitlines()[:3]:
    print("    " + line)

print("  mode-set performance ->", repr(talk("mode-set performance")))
print("  mode-set balanced    ->", repr(talk("mode-set balanced")))

# 直接问硬件要一次当前模式（这条会真的读 ACPI，是只读命令）
print("  再订阅一次看 daemon 推了什么：")
for line in talk("subscribe").splitlines():
    print("    " + line)
PY

hr "daemon 最近日志（看有没有 ACPI 命令被拒 / 返回异常）"
journalctl -u awccd -n 40 --no-pager 2>/dev/null | tail -40 || echo "（读不到 journal）"

hr "手工验一次 ACPI 读（只读，不改转速）"
echo '  echo "\_SB.AMWW.WMAX 0 0x14 {0xb,0x0,0x0,0x00}" > /proc/acpi/call && cat /proc/acpi/call'
if [ -w /proc/acpi/call ] || [ "$(id -u)" = 0 ]; then
    sh -c 'echo "\_SB.AMWW.WMAX 0 0x14 {0xb,0x0,0x0,0x00}" > /proc/acpi/call && cat /proc/acpi/call' 2>&1 | head -2
else
    echo "  （当前用户写不了 /proc/acpi/call，需要 sudo；这一步可选）"
    echo -n "  要不要 sudo 试一次？[y/N] "
    read -r a || true
    case "$a" in
        [yY]*) sudo sh -c 'echo "\_SB.AMWW.WMAX 0 0x14 {0xb,0x0,0x0,0x00}" > /proc/acpi/call && cat /proc/acpi/call' 2>&1 | head -2 ;;
        *) echo "  跳过" ;;
    esac
fi

hr "完成"
echo "  把以上输出整段贴回来即可。"
