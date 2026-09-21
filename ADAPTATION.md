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

改动只落在两个文件，各自对应一个独立提交：

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `src/EffectController.cpp` | 7 个灯效（`StaticColor` / `Breathe` / `Spectrum` / `Wave` / `Rainbow` / `BackAndForth` / `DefaultBlue`）在 `SendAnimationConfigSave` + `SendAnimationSetDefault(0x0061)` 之后再发一次 `SendAnimationPlay(0x0061)`；并把逐 zone 的 `SendZoneSelect(1, {zone})` 改成一次 `SendZoneSelect(1, m_zoneAll)` | 见第三节 |
| `CMakeLists.txt` | imgui 的 `GIT_TAG` 从 `master` 钉到 `6acba3b47d2ac4c7bb5ffb6ab04bcd896b3d3658`（2026-06-03，`1.92.9 WIP`），并关掉 `GIT_SHALLOW` | imgui master 是 1.93 WIP 分支。2026-09 那批提交在本机上把 Color 的取色弹窗渲染成全黑（还能选、也能 Apply，只是看不见颜色）。上游 v1.19.0 的发布二进制用的是 2026-06-03 的版本，取色器正常 |

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

## 六、与上游同步

```bash
git remote add upstream https://github.com/tr1xem/AWCC.git   # 只需一次
git fetch upstream
git log --oneline upstream/main..main       # 看本 fork 领先的提交
git rebase upstream/main                    # 冲突通常只在上表那两个文件（README 标题也会冲突）
```

上游若自行修好了上面两点，本地补丁就可以丢掉：

```bash
git checkout upstream/main -- src/EffectController.cpp CMakeLists.txt
```

## 七、验证

```bash
awcc static 00ff00             # 键盘应立刻变绿
awcc device-info               # 不应出现 "ACPI module not found in kernel"
```

GUI：打开 Alienware Command Center → 选 `Static` 或其它灯效 → 点 Color（取色弹窗应正常显示）
→ Apply。

## 八、后续计划

- 重做前端 UI，使布局与官方 AWCC 更接近，还原使用体验（现有界面在 `src/gui/Gui.cpp` 与
  `src/gui/Render.cpp`，用 ImGui 绘制）。
- 保持与上游同步；上游修好第三节两条后即丢弃本地补丁。

## 九、许可与义务

本 fork 继承上游的 **GPL-3.0**（见 `LICENSE`）。分发本 fork 的二进制时，按 GPLv3 需要：保留
许可证与版权声明、保留修改声明（见文件头与本文件）、提供对应源码、不给下游附加限制（§10）、
不借上游作者或 Dell 的商标暗示背书（§7e）。仅自用、不分发时这些义务不触发。

依赖许可上有一点要留意：`libusb` 是 **LGPL-2.1**，而本工程把它静态链接进可执行文件
（`_deps/libusb-build/libusb-1.0.a`）。发布二进制时需按 LGPL 提供可重新链接的形式，
或改为动态链接系统 libusb。内置 Roboto 字体为 Apache-2.0。
