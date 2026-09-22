#!/usr/bin/env bash
# 从上游的 awcc-bin（以及手动装在 /usr/local/bin 的那份）迁移到本 fork 的发行包。
#
# 为什么必须一起处理：
#   1) 上游包 awcc-bin 与本包都拥有 /usr/bin/awcc，本包又声明了 conflicts=('awcc-bin')，
#      不先卸掉，pacman -U 会直接以冲突拒绝安装；
#   2) /usr/local/bin 在 PATH 里比 /usr/bin 靠前，手动装的那份会一直盖住包里的，
#      不挪走的话桌面入口 Exec=awcc 启动的还是上游 ImGui 版。
#
# 需要 root：脚本自己不提权，请用 sudo 运行。
# 回滚：本包与上游包可以互相替换——先 pacman -R awcc-g16-7630-linux，
#       再把备份的 awcc.upstream.bak 挪回 /usr/local/bin/awcc（或从 AUR 装回 awcc-bin）。
set -euo pipefail

# 与本仓库 tag 同串（版本号规则见 ADAPTATION.md 第六节）
version="v26.9.22-3"
asset="awcc-g16-7630-linux-26.9.22_3-1-x86_64.pkg.tar.zst"
# 国内可用 Gitee 镜像：AWCC_ASSET_URL=... 覆盖即可
url="${AWCC_ASSET_URL:-https://github.com/Grant-Felix/AWCC-G16-7630-Linux/releases/download/${version}/${asset}}"

if [ "$(id -u)" -ne 0 ]; then
    echo "请用 sudo 运行：sudo $0" >&2
    exit 1
fi

echo "== 1/6 停掉守护进程（它跑的就是 /usr/bin/awcc，换掉前先停）=="
systemctl stop awccd 2>/dev/null || echo "  awccd 未在运行，跳过"
# 桌面入口启动的 GUI 是用户态进程，停服务不会带走它；这里只提示
pgrep -x awcc >/dev/null 2>&1 && echo "  提示：前台还有 awcc 进程在跑，建议先关掉它的窗口"

echo "== 2/6 卸掉上游的 awcc-bin（它拥有 /usr/bin/awcc 与那几个共享文件）=="
if pacman -Qq awcc-bin >/dev/null 2>&1; then
    pacman -R --noconfirm awcc-bin
else
    echo "  没装 awcc-bin，跳过"
fi

echo "== 3/6 把手装的那份挪到一边（不直接删，留着好回滚）=="
if [ -e /usr/local/bin/awcc ]; then
    mv -v /usr/local/bin/awcc /usr/local/bin/awcc.upstream.bak
    echo "  已改名备份；确认不需要后自行 rm /usr/local/bin/awcc.upstream.bak"
else
    echo "  /usr/local/bin/awcc 不存在，跳过"
fi

echo "== 4/6 下载并安装本 fork 的包 =="
tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
curl -fL --progress-bar -o "$tmpdir/$asset" "$url"
pacman -U --noconfirm "$tmpdir/$asset"

echo "== 5/6 刷新 udev 规则与 systemd 单元 =="
udevadm control --reload
systemctl daemon-reload

echo "== 6/6 起守护进程并自检 =="
systemctl enable --now awccd
sleep 1
echo "-- 二进制自报版本 --"
/usr/bin/awcc -h 2>/dev/null | head -1 || true
echo "-- pacman 认到的包 --"
pacman -Q awcc-g16-7630-linux
echo "-- 守护进程 --"
systemctl is-active awccd
echo "-- 桌面入口指向哪个二进制 --"
grep -h '^Exec=' /usr/share/applications/awcc.desktop
command -v awcc
echo
echo "完成：现在 awcc 解析到 /usr/bin/awcc（本 fork 的 GTK4 版），守护进程也是它。"
