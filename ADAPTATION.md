# 适配说明（AWCC-G16-7630-Linux 相对上游的改动）

本仓库是 [tr1xem/AWCC](https://github.com/tr1xem/AWCC) 的 fork，原名 `AWCC`，现更名为
**AWCC-G16-7630-Linux**。提交历史与文件头的改动声明是权威记录，这份文件说明「适用范围、
改了什么、为什么、怎么用」。

## 一、适用范围与已知限制

**只在 Dell G16 7630 上验证过**，验证环境：DMI `Dell G16 7630`、Intel（ACPI 前缀 `AMWW`）、
单区 RGB 键盘（USB `187c:0551`，AW-ELC）、Arch Linux + 内核 7.x。

要点：

- 两处**灯效**改动依赖本机固件的两条行为（见第三节）。其他机型——尤其是多区键盘——行为可能
  不同，**未验证、不保证**，甚至可能把原本正常的灯效弄坏。不是 G16 7630 请用上游版本。
- **imgui 钉版本**那一处与机型无关：上游把 imgui 钉在 `master`（1.93 WIP），2026-09 那批提交
  会让 Color 的取色弹窗渲染成黑窗。这一处对任何机型都适用。
- 作者只有这一台设备，没有条件做跨机型回归测试。

## 二、改了什么

改动只落在下面这些文件，每处对应一个独立提交：

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `src/EffectController.cpp` | 7 个灯效（`StaticColor` / `Breathe` / `Spectrum` / `Wave` / `Rainbow` / `BackAndForth` / `DefaultBlue`）在 `SendAnimationConfigSave` + `SendAnimationSetDefault(0x0061)` 之后再发一次 `SendAnimationPlay(0x0061)`；并把逐 zone 的 `SendZoneSelect(1, {zone})` 改成一次 `SendZoneSelect(1, m_zoneAll)` | 见第三节 |
| `CMakeLists.txt` | imgui 的 `GIT_TAG` 从 `master` 钉到 `6acba3b47d2ac4c7bb5ffb6ab04bcd896b3d3658`（2026-06-03，`1.92.9 WIP`），并关掉 `GIT_SHALLOW` | imgui master 是 1.93 WIP 分支。2026-09 那批提交在本机上把 Color 的取色弹窗渲染成全黑（还能选、也能 Apply，只是看不见颜色）。上游 v1.19.0 的发布二进制用的是 2026-06-03 的版本，取色器正常 |
| `CMakeLists.txt` | 版本号改为按 `version:refname` 取版本最大的日期式 tag，`VERSION` 宏用完整串 | 见第六节 |
| `scripts/release.sh` | 新增：按日期算当天第几次打包，可选打 tag 与推送 | 见第六节 |
| `.github/workflows/build.yml` | tag 触发规则、版本传参与产物命名改用日期式 | 见第六节 |

## 三、为什么灯效在本机型不生效

这台机器的灯控固件有两条行为，上游代码没有满足：

1. **只显示「最后一次 `Play` 的那一帧」**。上游每个灯效只写 `SendAnimationSetDefault`，
   固件于是保持上一帧不变，改颜色、换灯效在键盘上表现为「什么都没发生」。
2. **只认「一次选满全部 zone」的 `SendZoneSelect`**。上游按 zone 逐个单发，本机型会忽略
   逐 zone 的选择（上游 issue #8 里同机型用户报告过同样结论）。

实测能点亮键盘的完整链路是：`dim` 调暗量给 0（最亮）→ 一次选满 zone → 加动作 →
`SendAnimationConfigSave` → `SendAnimationSetDefault` → `SendAnimationPlay`。

另外，EC 侧 `stop_timeout=1m`：闲置约一分钟后背光自动熄灭、按键后重新点亮。这是固件行为，
不是故障；`dim` 则相反是即时生效的（`Brightness()` 只发 `SendSetDim` 就够）。

## 四、构建

依赖：`cmake`、`ninja`、`meson`（libevdev 由 meson 配置），其余（loguru / nlohmann-json /
glfw / imgui / libusb / libevdev / stb）由 CMake 自己拉取。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

换过 imgui 的 `GIT_TAG` 之后要先清一次缓存再配置，否则 CMake 会在旧的浅克隆里找不到指定提交：

```bash
rm -rf build/_deps/imgui-src build/_deps/imgui-subbuild build/_deps/imgui-build
```

## 五、安装

```bash
sudo install -Dm755 build/awcc /usr/local/bin/awcc
```

装到 `/usr/local/bin` 而不是覆盖包管理器装的 `/usr/bin/awcc`：多数发行版的 PATH 里
`/usr/local/bin` 在前，桌面入口 `Exec=awcc --gui` 会自动用到这份，也便于随时回滚。
注意 `pkexec` 会把工作目录切成 `/`，写相对路径会失败——所以上面用绝对路径或 `"$PWD/build/awcc"`。

可执行文件名仍是 `awcc`：仓名改了，但 systemd 单元、桌面入口与脚本都按这个名字调用，
改名会牵连这些集成，因此保持不动。

## 六、版本号与打包

本 fork 的版本号用**发布日期式**：`v<YY>.<M>.<D>-<x>`，`x` 是当天第几次打包，**从 1 起算、
不设上限**（当天第 101 次打包就是 `-101`，不封顶也不回绕），跨天重新从 1 起算。例如
`v26.9.22-1` 是 2026-09-22 当天第 1 次打包，同日第 2 次是 `v26.9.22-2`。**年月日不补零**——
`26.09.22-1` 里的前导零会让它不再是合法 semver（npm 与 Cargo 都拒）；也不写四段式。标签、
`VERSION` 宏、发布标题与产物文件名用同一串，产物为 `AWCC-v26.9.22-1.tar.gz`。

上游用 release-please 产 semver（`v1.19.0` 那批），与本方案冲突，故本 fork 不启用它：
`.github/workflows/release.yml`、`release-please-config.json` 与 `.release-please-manifest.json`
保留上游原样、不参与本 fork 发布（本机 Forgejo 的 Actions 也未启用）。CHANGELOG 里上游那段
历史照旧，本 fork 的日期版从头往下追加。

版本串的**唯一出处是 git tag**：`CMakeLists.txt` 按 `version:refname` 取版本最大的日期式 tag
（不用 `git describe`——同一天多次打包会把多个 tag 打在同一个提交上，实测 describe 会挑到前一天
的 tag），所以每次发布都不必改这个文件（改了就每次与上游 rebase 都冲突）；CI 在 tag 上 checkout，
显式传 `-DAWCC_VERSION=<串>`。CMake 的 `project(VERSION)` 只收纯数字 `major.minor.patch[.tweak]`，
装不下 `-x` 序号，因此喂给它的数字部分剥掉了序号（`26.9.22-1` → `26.9.22`），完整串只进
`VERSION` 宏（`awcc help` 首行、`awcc device-info` 的 `Version:` 行与 GUI 标题都取它）。

打包与发布：

```bash
scripts/release.sh                 # 打印当天该用的版本串，如 26.9.22-1
scripts/release.sh --tag           # 顺手打上 v26.9.22-1
scripts/release.sh --tag --push    # 再推到 origin（本机 Forgejo）

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # 不传 -DAWCC_VERSION 时从 tag 取版本
ninja -C build
build/awcc -h | head -1            # 应打印 Alienware Command Center v26.9.22-1
```

没有日期式 tag 时（例如从 tarball 构建）版本串回落到占位 `0.0.0-0`，绝不会被当成已发布版本。

## 七、与上游同步

```bash
git remote add upstream http://127.0.0.1:3000/Felix/AWCC-upstream.git   # 只需一次，本机镜像
git fetch upstream
git log --oneline upstream/main..main       # 看本 fork 领先的提交
git rebase upstream/main                    # 冲突通常只在 CMakeLists.txt、README 标题与 .github/workflows/build.yml
```

上游若自行修好了上面两点，本地补丁就可以丢掉（注意 `CMakeLists.txt` 里还有版本号取法这一处
fork 改动，整个文件 checkout 掉会连它一起丢，需要按第六节重打）：

```bash
git checkout upstream/main -- src/EffectController.cpp CMakeLists.txt
```

## 八、验证

```bash
awcc -h | head -1              # 应打印 Alienware Command Center v<版本串>（见第六节）
awcc static 00ff00             # 键盘应立刻变绿
awcc device-info               # 不应出现 "ACPI module not found in kernel"
```

GUI：打开 Alienware Command Center → 选 `Static` 或其它灯效 → 点 Color（取色弹窗应正常显示）
→ Apply。

## 九、后续计划

- 重做前端 UI，**改用 GTK4**（2026-09-22 定），布局对齐戴尔官方 AWCC，还原使用体验。现有界面在
  `src/gui/Gui.cpp` 与 `src/gui/Render.cpp`，用 ImGui 绘制；GTK4 落地后这套渲染代码与
  `CMakeLists.txt` 里 imgui 钉版本那处改动（第二节）会一并退出。
- 保持与上游同步；上游修好第三节两条后即丢弃本地补丁。

## 十、许可与义务

本 fork 继承上游的 **GPL-3.0**（见 `LICENSE`）。分发本 fork 的二进制时，按 GPLv3 需要：保留
许可证与版权声明、保留修改声明（见文件头与本文件）、提供对应源码、不给下游附加限制（§10）、
不借上游作者或 Dell 的商标暗示背书（§7e）。仅自用、不分发时这些义务不触发。

依赖许可上有一点要留意：`libusb` 是 **LGPL-2.1**，而本工程把它静态链接进可执行文件
（`_deps/libusb-build/libusb-1.0.a`）。发布二进制时需按 LGPL 提供可重新链接的形式，
或改为动态链接系统 libusb。内置 Roboto 字体为 Apache-2.0。
