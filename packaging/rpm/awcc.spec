# RPM 官方打包方案：用 rpmbuild 构建（scripts/package.sh 会在 fedora 容器里跑）。
#
# 版本号遵循上游的日期式版本，但 RPM 的 Version 不允许含连字符，所以 v26.9.22-2 拆成
# Version=26.9.22 + Release=2。Release 里**不加 %{?dist}**，好让产物名与 tag 同串
# （AWCC-v26.9.22-2；见 ADAPTATION.md 第六节的版本号规则）。
Name:           awcc
Version:        26.9.22
Release:        2
Summary:        Alienware Command Center for Dell G16 7630 (GTK4 frontend)
License:        GPL-3.0-only
URL:            https://github.com/Grant-Felix/AWCC-G16-7630-Linux
Source0:        https://github.com/Grant-Felix/AWCC-G16-7630-Linux/archive/refs/tags/v%{version}-%{release}.tar.gz

BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  meson
BuildRequires:  git
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libadwaita-1)
# libusb 与 libevdev 由 CMake 自己拉取并静态链接，不是运行时依赖（ldd 实测确认）
Requires:       gtk4
Requires:       libadwaita
Requires:       glib2

%description
Unofficial Alienware Command Center for Linux: a fork of tr1xem/AWCC adapted for
the Dell G16 7630, with a GTK4 frontend. Provides thermal modes, CPU/GPU fan
boost, turbo boost, keyboard lighting effects and brightness control.

Only validated on the Dell G16 7630; other models are not supported.

%prep
%autosetup -n AWCC-G16-7630-Linux-%{version}-%{release}

%build
# 不要自己写 -B build：%cmake_build 用的是 %{_vpath_builddir}（redhat-linux-build），
# 两者不一致会报 "redhat-linux-build is not a directory"（实测踩到）。
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DAWCC_VERSION=%{version}-%{release}
%cmake_build

%install
# 只装我们自己那一个组件（COMPONENT awcc）：FetchContent 拉来的依赖自带 install 规则，
# 不过滤会把 /usr/include/libusb-1.0 与 /usr/lib/libusb-1.0.a 也装进系统
DESTDIR=%{buildroot} cmake --install %{_vpath_builddir} --component awcc

%files
%{_bindir}/awcc
%{_datadir}/applications/awcc.desktop
%{_datadir}/icons/awcc.png
%{_datadir}/locale/*/LC_MESSAGES/awcc.mo
%config(noreplace) /etc/awcc/database.json
%config(noreplace) /etc/udev/rules.d/70-awcc.rules
%config(noreplace) /etc/systemd/system/awccd.service

%changelog
* Tue Sep 22 2026 Felix <noreply@example.com> - 26.9.22-2
- GTK4 frontend replacing the upstream ImGui UI
- Official AWCC layout and colours sampled from the reference screenshots
- Automatic zh_CN / English switching (gettext)
- Configurable background (pure black / follow system / colour / image)
- Date-based versioning: v<YY>.<M>.<D>-<n>
