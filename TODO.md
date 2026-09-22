# 实施（GTK4 前端重做）

设计与边界见 `DESIGN.md`。本文件给拆解、接口 schema、验证命令与完成判据。每个里程碑都能单独
验证——「代码写完了」不算完成，必须跑得出下面的命令并看到预期输出。

## 一、全局完成判据

1. `pkg-config --exists gtk4 libadwaita-1` 且 `cmake -S . -B build -G Ninja && ninja -C build`
   无警告失败。
2. `./build/awcc --gui` 在 Wayland 会话下起窗口，左侧导航可切到**每一个**页面。
3. 每个页面要么后端接通、要么带「施工中」徽标且相关控件置灰、要么标「不适用」（三种状态见
   `DESIGN.md` 第六节）。
4. 任何读数都不出现假值：拿不到的显示「—」，不显示 0。
5. `./build/awcc telemetry` 能在命令行打印一份遥测样本（数据层不依赖 GUI 也能验）。
6. 回归不破：`./build/awcc static 00ff00` 键盘立刻变绿；`./build/awcc -h | head -1` 显示
   `Alienware Command Center v<tag 版本串>`。
7. 亮度跨重启记住（现在因为写 `/etc/awcc/brightness` 而无权限，普通用户每次回到 50%）。

## 二、接口 schema

### 2.1 用户配置 `$XDG_CONFIG_HOME/awcc/config.ini`（GKeyFile）

```
[lighting]
brightness=50            ; 0-100
effect=static            ; static|breathe|spectrum|wave|rainbow|backandforth|defaultblue
color=#00ff00            ; #RRGGBB
duration=15              ; spectrum / rainbow 用，单位秒
[thermal]
mode=balanced            ; battery|quiet|balanced|performance|custom
cpu_boost=0              ; 0-100
gpu_boost=0              ; 0-100
turbo=false
[ui]
theme=dark               ; 本期只实现 dark
animated_background=true
```

为什么放用户目录而不是 `/etc/awcc`：见 `DESIGN.md` 第二节配置那一行——GUI 是普通用户、daemon
是 root，配置得两边都能读；`/etc/awcc/brightness` 那个 root 所有的文件正是「亮度不记忆」的根因。
daemon 侧无状态：需要什么值由 GUI 通过 socket 下传。

### 2.2 遥测层 `src/telemetry/Telemetry.h`

```cpp
// 每个可选字段的语义：拿不到就是 nullopt，绝不填 0（见 DESIGN.md 第六节）
struct CpuSample {
    double freqMHz = 0;
    std::optional<double> tempC;       // coretemp
    std::optional<double> packageW;    // intel-rapl energy_uj 差分
};
struct FanSample { std::optional<int> cpuRpm, gpuRpm; };   // alienware_wmi / dell_smm
struct MemSample { unsigned long totalKiB, availableKiB, cachedKiB; };
struct DiskSample { unsigned long long totalBytes, freeBytes; std::optional<double> nvmeTempC; };
struct GpuSample { /* 全部 optional：nvidia-smi 当前不可用，见 DESIGN.md 第五节 */ };

struct Sample { CpuSample cpu; FanSample fan; MemSample mem; DiskSample disk; GpuSample gpu; };

class Telemetry {
  public:
    Sample sample();          // 一次只读采样，无特权、无副作用
};
```

### 2.3 前端入口

`include/Ui.h`：`namespace Ui { int Run(int argc, char **argv, /* 服务层引用 */ ...); }`，
由 `main.cpp` 的 `--gui` 分支调用；`GtkApplication` 的 `activate` 里建主窗口。

## 三、里程碑

### M0 骨架：能起窗口、能切页（全部页面先带「施工中」）

| 步骤 | 产物 | 验证命令 | 完成判据 |
| --- | --- | --- | --- |
| M0.1 构建整合 | `CMakeLists.txt` 用 `pkg_check_modules` 找 `gtk4` / `libadwaita-1`；新增 `src/ui/`、`include/Ui.h` | `pkg-config --exists gtk4 libadwaita-1 && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build` | 配置与编译通过 |
| M0.2 最小窗口 | `GtkApplication` + `GtkApplicationWindow`，标题 `Alienware Command Center v<版本>`（取 `VERSION` 宏） | `./build/awcc --gui` | Wayland 下出现窗口，标题带版本串 |
| M0.3 旧 UI 退役 | 删 `src/gui/`、`src/resources.cpp`、`include/Renderui.h`、`include/Gui.h`；CMake 去掉 imgui / glfw / OpenGL / X11 依赖 | `grep -rn "imgui\|glfw" src include CMakeLists.txt` | 无输出；构建仍通过（`ADAPTATION.md` 第二节的 imgui 钉版本补丁同步标注退役） |
| M0.4 导航与占位页 | `.ui` 文件 + `GResource`；左侧图标栏、顶部面包屑、`GtkStack` 内 12 个页面（每页一个标题 + 「施工中」徽标） | `gtk4-builder-tool validate src/ui/*.ui && ./build/awcc --gui` | 12 个页面都能切到，标题与面包屑同步 |
| M0.5 样式 | `src/ui/style.css`，深色底、强调色、`font-family: "Noto Sans CJK SC"` | 起窗口看中文标签不是豆腐块、字形不是韩文变体 | CSS 生效；中文正常 |

### M1 遥测层

| 步骤 | 产物 | 验证命令 | 完成判据 |
| --- | --- | --- | --- |
| M1.1 CPU / 内存 / 磁盘 / 风扇 | `src/telemetry/` + CLI 自检子命令 `awcc telemetry` | `./build/awcc telemetry` | 打印 freq/温度/功耗/内存/磁盘/风扇转速，字段与 `DESIGN.md` 第五节实测对得上 |
| M1.2 GPU 探测 | 探 `nvidia-smi` / `/dev/nvidia*` / `runtime_status`，失败原因落日志 | `./build/awcc telemetry --gpu` | 失败时明确报「dGPU 在 runtime suspend / 无 /dev/nvidia*」，而不是崩或静默填 0 |
| M1.3 采样节流 | 环形表 1 s 刷新，不忙等 | `./build/awcc telemetry --watch 5` | 5 s 内刷新 5 次，CPU 占用可忽略 |

### M2 主页 + 性能（概况 / 散热）

| 步骤 | 产物 | 验证命令 | 完成判据 |
| --- | --- | --- | --- |
| M2.1 自绘环形表控件 | `src/ui/RingGauge.*`（`GtkDrawingArea` 子类，值/量程/单位/颜色） | `./build/awcc --gui` | 环随真实数据动，数值与 `awcc telemetry` 一致 |
| M2.2 主页 | 模式按钮组（接 `Thermals`）、灯效/亮度（接 `EffectController`）、四个环 | 在机上点模式按钮 | EC 真的切模式（`awcc device-info` 前后一致）；灯效即时生效 |
| M2.3 性能 · 概况 | 四列环 + 参数表 | 对照 `awcc telemetry` | CPU/内存/磁盘三列真实；GPU 列标「施工中」并显示「—」 |
| M2.4 性能 · 散热 | 两个温度环 + 两个风扇锯齿环 | 手动拉高负载看转速变化 | 温度与转速随负载变化，来源为 coretemp 与 alienware_wmi |

### M3 ALIENFX 灯效 + 配置持久化

| 步骤 | 产物 | 验证命令 | 完成判据 |
| --- | --- | --- | --- |
| M3.1 配置层 | `src/config/Config.*`（GKeyFile 读写 `$XDG_CONFIG_HOME/awcc/config.ini`） | `./build/awcc --gui` 改亮度后 `cat ~/.config/awcc/config.ini` | 文件出现且值正确；重开 GUI 仍是该值（**亮度不记忆的坑由此修掉**） |
| M3.2 灯效页 | 效果 `GtkDropDown`、亮度 `GtkScale`、颜色 `GtkColorDialogButton`、持续时间 | 逐个选效果；选颜色后 Apply | 7 种灯效都生效；颜色选择器不再黑屏（替掉 ImGui 那个取色弹窗） |
| M3.3 键盘可视化 | 键盘布局控件（先用静态布局，逐键状态后续接） | 起窗口 | 布局与参考图 `ALIENFX™ · 灯效` 一致 |

### M4 按键绑定（只读）+ 设置六页

| 步骤 | 产物 | 验证命令 | 完成判据 |
| --- | --- | --- | --- |
| M4.1 按键绑定页 | 键盘图 + 绑定列表，接 `KeyBinder` 现状 | 按 G 键 / 灯键 | 界面能显示"监听到"，但编辑功能标「施工中」 |
| M4.2 设置 · 关于 / 外观 | 版本、设备信息；主题三选（只 dark 真生效） | `./build/awcc --gui` | 版本串与 `awcc -h` 首行一致 |
| M4.3 设置 · 覆盖 / 新手入门 | 标「不适用」/「施工中」 | 起窗口 | 不出现能点但没效果的控件 |
| M4.4 设置 · 全局预设 / 性能 | 本地偏好开关；超频标「施工中」、节能模式标「不适用」 | 起窗口 | 三种状态标记可分 |

### M5 库（占位）与收尾

| 步骤 | 产物 | 验证命令 | 完成判据 |
| --- | --- | --- | --- |
| M5.1 库页 | 搜索框 + 网格 + 排序，数据源标「施工中」 | 起窗口 | 页面结构与参考图一致，功能标施工中 |
| M5.2 文档收口 | `ADAPTATION.md` 第九节改为指向 `DESIGN.md` / `TODO.md`（参考图索引留在原处） | `grep -n "DESIGN.md" ADAPTATION.md` | 单一出处：设计只在 DESIGN，实施只在 TODO |
| M5.3 全量回归 | —— | `./build/awcc -h \| head -1`；`./build/awcc static 00ff00`；`./build/awcc telemetry`；`./build/awcc --gui` | 七条全局完成判据全过 |

### 待解的外部依赖（不属 UI，但挡着 GPU 那一列）

`nvidia-smi` 当前报无法与驱动通信：模块 `nvidia_uvm/nvidia_drm` 已加载，但 dGPU
`runtime_status=suspended` 且 `/dev/nvidia*` 不存在。**开工前或 M1.2 时单独查**：设备的 udev 是否
建了节点、`nvidia-persistenced` 是否该启用、是否被 runtime PM 挂起。解决了 GPU 那列才能从
「施工中」转正。

## 四、每步都要跑的验证命令（复制即用）

```bash
cd "/home/felix/项目/AWCC G16 7630 Linux"

# 1) 依赖与构建
pkg-config --exists gtk4 libadwaita-1 && echo "GTK4 依赖 ok"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build

# 2) 界面描述合法
gtk4-builder-tool validate src/ui/*.ui

# 3) 版本号仍从 tag 取（回归：不该被 UI 改动破坏）
./build/awcc -h | head -1        # 期望 Alienware Command Center v26.9.22-1

# 4) 数据层（不依赖 GUI）
./build/awcc telemetry

# 5) 起窗口
./build/awcc --gui

# 6) 硬件回归
./build/awcc static 00ff00       # 键盘应变绿
```

## 五、提交约定

按仓库既有习惯（约定式提交、中文描述）：构建整合 `build(gtk4)`、界面骨架 `feat(ui)`、
数据层 `feat(telemetry)`、配置层 `feat(config)`、文档 `docs`。一个里程碑一组小提交，每个提交
都能独立编译通过。
