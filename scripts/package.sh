#!/usr/bin/env bash
# 三个发行版的二进制包，各用**各自官方的打包方案**：
#   Debian → debian/ + dpkg-buildpackage（官方 debhelper 流程）
#   Fedora → packaging/rpm/awcc.spec + rpmbuild
#   Arch   → packaging/aur/PKGBUILD + makepkg
#
# 为什么在容器里打 deb/rpm：本机是 Arch，没有 debhelper/dpkg-dev，也没有 rpm-build；
# 这些官方工具链只有各自发行版里有，所以在对应官方镜像里跑（需要 docker 可用）。
#
# 产物名里的版本串与 tag 同串（ADAPTATION.md 第六节）。Arch 是例外：pkgver 不允许带连字符，
# 26.9.22-2 在 Arch 包版本里写作 26.9.22_2；RPM 则拆成 Version=26.9.22 / Release=2。
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

version=$(scripts/release.sh) # 当天该用的版本串，如 26.9.22-2
outdir="$(realpath -m "${1:-.cache/packages}")"
mkdir -p "$outdir"
uid=$(id -u)
gid=$(id -g)
srcurl="https://github.com/Grant-Felix/AWCC-G16-7630-Linux/archive/refs/tags/v${version}.tar.gz"

echo "== 版本：$version（RPM 拆成 ${version%-*} / ${version##*-}）=="
echo "   源码包：$srcurl"
echo "   输出：$outdir"

if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  echo
  echo "== Debian 包（debian:trixie + dpkg-buildpackage）=="
  # 用 trixie 而不是 bookworm：bookworm 的 GTK 只有 4.8，而代码用了 GTK 4.10 才有的
  # GtkColorDialogButton（libadwaita 也需要 ≥1.4）。deb 依赖里的最低版本由 dh_shlibdeps 算。
  docker run --rm -v "$PWD:/src:ro" -v "$outdir:/out" debian:trixie bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    # ca-certificates 必须显式装：官方镜像里没有它，git 拉 FetchContent 依赖会报
    #   "server certificate verification failed. CAfile: none"
    # build-essential 是 dpkg-buildpackage 隐含要求的构建依赖
    apt-get install -y -qq --no-install-recommends ca-certificates build-essential \
        debhelper cmake ninja-build meson git pkg-config libgtk-4-dev libadwaita-1-dev \
        libudev-dev >/dev/null
    cp -a /src /work && cd /work
    # 必须丢掉本机已有的 build/ 与 .cache/：CMakeCache 里记着原路径，带进去会直接报错
    rm -rf build .cache
    dpkg-buildpackage -b -us -uc
    cp /*.deb /out/
    chown -R '"$uid:$gid"' /out
  '

  echo
  echo "== Fedora 包（fedora + rpmbuild + awcc.spec）=="
  docker run --rm -v "$PWD:/src:ro" -v "$outdir:/out" -e "SRCURL=$srcurl" -e "VER=$version" \
    fedora:latest bash -c '
    set -e
    dnf install -y -q ca-certificates rpm-build cmake ninja-build gcc-c++ meson git \
        curl pkgconf-pkg-config gtk4-devel libadwaita-devel systemd-devel >/dev/null
    mkdir -p /work && cp -a /src/. /work/ && cd /work && rm -rf build .cache
    mkdir -p rpmbuild/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}
    curl -fsSL "$SRCURL" -o "rpmbuild/SOURCES/v$VER.tar.gz"
    rpmbuild -bb --define "_topdir $PWD/rpmbuild" packaging/rpm/awcc.spec
    cp rpmbuild/RPMS/*/*.rpm /out/
    chown -R '"$uid:$gid"' /out
  '
else
  echo
  echo "!! docker 不可用，跳过 deb 与 rpm。"
  echo "   Debian 包：在 Debian/Ubuntu 上 apt install debhelper cmake ninja-build meson libgtk-4-dev libadwaita-1-dev，然后 dpkg-buildpackage -b -us -uc"
  echo "   Fedora 包：在 Fedora 上 dnf install rpm-build cmake ninja-build gtk4-devel libadwaita-devel，然后 rpmbuild -bb packaging/rpm/awcc.spec"
fi

echo
echo "== Arch 包（makepkg + packaging/aur/PKGBUILD）=="
# makepkg 的 -ffile-prefix-map 会被路径里的空格拆断，所以必须在无空格的临时目录里跑
workdir=$(mktemp -d /tmp/awcc-aur.XXXXXX)
cp packaging/aur/PKGBUILD "$workdir/"
( cd "$workdir" && makepkg -f --nodeps --noconfirm )
cp "$workdir"/*.pkg.tar.zst "$outdir"/
rm -rf "$workdir"

echo
echo "== 产物 =="
ls -lh "$outdir"
