# 设计（GTK4 前端重做）

本文件讲**为什么这样切**、切到哪、以及明确不做什么。任务的拆解、验证命令与完成判据见
`TODO.md`；本 fork 与上游的差异见 `ADAPTATION.md`；官方界面参考图的逐张索引在 `ADAPTATION.md`
第九节。

## 一、要解决的问题

现有前端是 ImGui 画的单窗口（`src/gui/Gui.cpp` 561 行 + `src/gui/Render.cpp` 153 行）：一个模式
下拉、两个风扇 boost 滑块、7 个灯效按钮、一个亮度滑块，仅此。目标是对齐戴尔官方 AWCC 的
多页面桌面界面（14 张参考图）。

**为什么不继续用 ImGui**：ImGui 是即时模式 UI，为游戏内叠加层与调试工具而生，没有原生控件、
没有系统主题与无障碍、没有页面导航；要做「左侧导航 + 面包屑 + 分页 + 表格 + 环形仪表」这种
界面几乎全靠自绘，总代价高于直接用 GTK4。另外本 fork 为修 ImGui 取色弹窗黑屏钉过 imgui 提交
（`ADAPTATION.md` 第二节），换掉 ImGui 后那处补丁自然退役。

## 二、技术选型（已定）

| 决策 | 选择 | 为什么 | 备选与代价 |
| --- | --- | --- | --- |
| 工具链 | **GTK4 C API 4.22.5** + libadwaita 1.9.4 | GTK 官方参考实现，文档、示例、`gtk4-builder-tool` 全围绕它；libadwaita 只借导航骨架与深色切换 | gtkmm-4.0（官方 C++ 绑定）：写起来更像 C++23，但文档/示例远少于 C API，且 libadwaita 没有 C++ 绑定（本机与 Arch 仓库都没有 `libadwaitamm`），要用就得混写 |
| 界面描述 | `GtkBuilder` `.ui`（XML）+ `GResource` | 官方声明式路线，本机有 `gtk4-builder-tool` 可 `validate`；资源编译进二进制，不用装数据文件 | 纯代码建控件：动态部分照样得写代码，静态布局反而更啰嗦 |
| 样式 | CSS（`GtkCssProvider`）+ 自定义深色 | 参考图是戴尔自绘深色风，GTK 默认皮肤必须覆盖 | 用 libadwaita 的 Adwaita 皮肤：那是 GNOME 观感，与参考图差得远 |
| 环形仪表 | `GtkDrawingArea` + cairo | 环形/锯齿环是参考图的核心视觉，自绘最直接；要发光模糊时上 `GtkSnapshot`/GSK | 现成图表库：为四个环引一个绘图库不划算 |
| 构建 | **保持 CMake** + `pkg-config` 找 gtk4/libadwaita | 项目本来就是 CMake + FetchContent；GTK 通过 pkg-config 支持任意构建系统 | 引 Meson：会把构建系统劈成两半 |
| 配置持久化 | **`$XDG_CONFIG_HOME/awcc/config.ini`**（GKeyFile） | 配置要**同时被普通用户身份的 GUI 与 root 身份的 daemon** 读；GSettings 后端 dconf 是 per-user 的，root 读用户的 dconf 很别扭 | 用 GSettings（官方做法）：需装 schema 到 `/usr/share/glib-2.0/schemas`，且 root 侧读取要多绕一层 |
| 字体 | 系统字体，CSS 指定 `Noto Sans CJK SC` | 本机**没装 Roboto**；中文标签必须走 Noto Sans CJK，且默认 `fc-match sans-serif:lang=zh` 会落到 **KR**（韩文变体），必须显式写 **SC** | 继续内嵌 Roboto：它没有 CJK 字形，中文标签会变豆腐块 |
| 国际化 | gettext（GLib 的 `gi18n.h`）：**源串写英文**，译文放 `po/zh_CN.po`，按 `LANG` / `LC_MESSAGES` 自动切 | GTK/GLib 的官方做法；源串是英文，非中文环境会回落到英文，仍可读 | 源串写中文 + `po/en.po`：`LANG=C` 时 gettext 根本不会去找 `en`，英文用户会看到中文 |
| 界面串的标记方式 | 代码里的串用 `_()`；表格等静态串用 `N_()`；`.ui` 里用 `translatable="yes"` | `N_()` 是给 `xgettext` 看的标记——表格里的串不是字面量，不加它提取不到，翻译模板会缺条目 | —— |
| 图标随窗口缩放 | 帧时钟回调读内容区尺寸，按短边算 16–40px，只设 `GtkImage` 的 `pixel-size` | 只给正方形边长 ⇒ **长宽比固定**；尺寸没变立即返回，开销是几次整数比较 | `GtkWidgetClass.size_allocate` vfunc：文档写明只对**没有 layout manager** 的控件调用，而 `GtkBox` 自带 `GtkBoxLayout`——实测加探针一次都没进（见下） |

`src/resources.cpp`（15 万行）里是 9 个数组：5 个模式 PNG 图标 + 4 套 Roboto 字体，全部只服务
ImGui。换 GTK4 后**4 套字体退役**（改用系统字体），5 个模式图标可留作 GResource，或改换成
矢量图标。

**一条踩过的 GTK4 行为**（写下来免得再踩）：想拿「窗口尺寸变化」时，直觉是覆盖
`GtkWidgetClass.size_allocate` vfunc，但它的文档写着 *called to set the allocation, **if the
widget does not have a layout manager***。`GtkBox` 自带 `GtkBoxLayout`，所以这个 vfunc 实测
**一次都不会被调用**（加 `g_print` 探针确认）。为一个缩放钩子去写自定义容器 + `GtkBuildable`
不划算，最终用帧时钟回调：每帧读一次内容区尺寸，没变就直接返回。同理，`GtkWindow:default-width`
只是「默认尺寸」，窗口 map 之后再 `gtk_window_set_default_size()` 也改不动实际尺寸。

## 三、分层与权限边界

```
前端层（新，src/ui/）      GTK4 C API + libadwaita，以普通用户运行
     │
服务层（复用，不动）        Thermals / AcpiUtils / EffectController / LightFX / KeyBinder
     │
数据层（新，src/telemetry/） 只读采样 hwmon / procfs / statvfs / nvidia-smi，无特权
     │
系统                       USB(udev uaccess) · daemon socket /tmp/awcc.sock(root) · pkexec 兜底
```

**关键结论：GUI 不需要 root。** `--gui` 不检查 euid；真正需要 root 的只有 ACPI 侧（热模式、
风扇 boost、turbo），它们走 root daemon 的 `/tmp/awcc.sock`；灯效走 USB，udev 规则
`app/70-awcc.rules` 已用 `TAG+="uaccess"` 覆盖 `187c:0551` 与 `187c:0550`。

按操作列权限：

| 操作 | 需要权限 | 通道 | 现状 |
| --- | --- | --- | --- |
| 键盘灯效 / 亮度 | USB 设备访问 | libusb 直连（udev `uaccess`） | 可用，普通用户即可 |
| 热模式 / CPU·GPU 风扇 boost / turbo | root（写 `/proc/acpi/call`） | daemon socket；daemon 没跑时 fallback 到 `pkexec`（会弹授权框） | 可用 |
| 遥测（温度/频率/功耗/内存/磁盘/风扇转速） | 无 | sysfs / procfs / statvfs | 可用（GPU 见第五节） |

**daemon 白名单是一条硬约束**：`Daemon::m_CommandAllowed` 目前只放行 3 条精确正则 + `stop`，
且命令最终由 `popen` 执行。新增受控操作时**只能再加一条精确正则**，不允许为了省事把校验放宽成
「放行任意命令」——那会把一个 root 服务变成任意命令执行入口。

## 四、页面与控件映射

导航结构：libadwaita 的 `AdwNavigationSplitView` 做「左栏 + 内容」；左栏用
`AdwViewSwitcherSidebar`（图标栏）或自绘 `GtkBox` + `GtkToggleButton` 组；顶部面包屑用
`AdwNavigationView` 的标题机制或 `GtkLabel` 组合；内容区 `GtkStack`。二级页面（设置里的六项）
用嵌套 `GtkStack` + `GtkListBox`。深色切换用 `AdwStyleManager`。（以上类已在本机
libadwaita 1.9.4 里确认存在。）

| 页面（参考图） | 主要 GTK4 组件 | 后端状态 |
| --- | --- | --- |
| 主页 · 当前生效 | 模式按钮组（`GtkToggleButton` 组）、四个自绘环形表、`GtkSwitch`（性能/散热）、游戏库卡片 | 模式与灯效有后端；游戏库**施工中** |
| 性能 · 概况 | 模式按钮组、四列环形表、`GtkGrid` 参数表 | CPU/内存/磁盘可做；**GPU 施工中**（见第五节） |
| 性能 · 散热 | 两个温度环、两个锯齿环（风扇转速） | 可做（coretemp + alienware_wmi/dell_smm） |
| ALIENFX™ · 灯效 | 键盘可视化（自绘或分组 `GtkToggleButton`）、`GtkDropDown`（效果）、`GtkScale`（亮度/持续时间）、`GtkColorDialogButton`（颜色） | 7 种灯效现成；颜色选择器用 GTK 自带的，顺带替掉 ImGui 那个黑屏取色弹窗 |
| ALIENFX™ · 按键绑定 | 键盘可视化 + 绑定列表 | 只有 `KeyBinder` 能监听 G 键/灯键，**改不了映射 → 施工中（只读）** |
| 设置 · 关于 | `GtkLabel` / `GtkLinkButton` 列表 | 可做，版本号取 `VERSION` 宏 |
| 设置 · 外观 | `GtkSwitch`（动画背景）、三选一按钮组（主题） | 本地项可做；本期只实现深色，浅色/跟随系统**施工中** |
| 设置 · 覆盖 | `GtkSwitch` + 快捷键展示 | Linux 无此叠加层 → **不适用** |
| 设置 · 新手入门 | 按钮 | 取决于是否做引导流程 → **施工中** |
| 设置 · 全局预设 | 开关组 + 预设列表 | 本地偏好可做；「按游戏自动切换」无游戏检测 → **施工中** |
| 设置 · 性能 | 单选（超频控制）、通知开关 | 超频受 EC 限制，能力未知 → **施工中**；Windows 节能模式 → **不适用** |
| 库 · 名称 | `GtkSearchEntry`、`GtkFlowBox`、排序 `GtkDropDown` | 无游戏扫描后端 → **施工中（占位）** |

## 五、遥测数据源（本机实测）

| 字段 | 来源 | 本机实测 | 状态 |
| --- | --- | --- | --- |
| CPU 频率 | `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` | 1381875 kHz | 可用 |
| CPU 温度 | hwmon `coretemp`（15 路） | 55–72 °C | 可用 |
| CPU 功耗 | `/sys/class/powercap/intel-rapl:0/energy_uj`（package-0） | 有 | 可用（按时间差算瓦特） |
| CPU 电压 | —— | 只有 BAT0 / typec 的 `in0_input` | **拿不到**，显示「—」 |
| 风扇转速 | hwmon `alienware_wmi` / `dell_ddv` / `dell_smm` | fan1=944、fan2=1131 RPM | 可用 |
| 内存 | `/proc/meminfo` | 有 | 可用 |
| 内存温度 | hwmon `spd5118` | 有 | 可用 |
| 磁盘容量 / 活动 | `statvfs` / `/proc/diskstats`；`nvme` 温度 | 953G 总、901G 可用 | 可用 |
| GPU 利用率 / 显存 / 温度 / 功耗 | `nvidia-smi` | **失败**：模块已加载但 dGPU `runtime_status=suspended`，`/dev/nvidia*` 不存在 | **拿不到 → 施工中** |

GPU 那一列在驱动/唤醒问题解决前**一律标「施工中」并显示占位符**，不许显示 0——0% 和 0 W 会被
当成真实读数。

## 六、「施工中 / 不适用」的约定

三种状态在界面上必须一眼可分：

| 标记 | 含义 | 做法 |
| --- | --- | --- |
| 正常 | 后端接通 | 无额外标记 |
| **施工中** | 这功能我们要做，但后端还没接 | 页头右侧一枚「施工中」徽标；相关控件 `gtk_widget_set_sensitive(false)` 置灰，tooltip 写明缺什么 |
| **不适用** | 平台没有这个功能（如 Windows 节能模式、Dell 覆盖层） | 页面内一句说明，不放灰控件假装能做 |

数据层同理：拿不到就显示「—」，**禁止用 0 或假值冒充**。这条是本设计的验收项之一。

## 七、明确不做 / 不防

**本期不做**：游戏库真实扫描（占位）、游戏内覆盖层、Dell 账号登录、Windows 节能模式、
超频控制（EC 能力未知）、按键绑定写入（EC 是否支持未知）。

**明确不防**：多用户并发（这是单用户桌面工具，配置文件按用户走）、非 Dell 机型
（本 fork 只在 G16 7630 验证，见 `ADAPTATION.md` 第一节）、上游 `src/gui/` 的冲突
（换 UI 后这条路径必然分叉，处理方式已在 `ADAPTATION.md` 第七节写明）。
