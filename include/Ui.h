#pragma once

// 前端入口。实现见 src/ui/Ui.cpp。
// 设计约束见 DESIGN.md：GTK4 C API + libadwaita，以普通用户运行，需要 root 的操作走
// daemon socket（没有 daemon 时由 AcpiUtils 退回 pkexec）。

class AcpiUtils;
class EffectController;
class Thermals;

namespace Ui {

/// 前端要用的服务层对象。指针不转移所有权——生命周期由 main.cpp 管理，前端只调用。
struct Services {
    Thermals *thermals = nullptr;
    AcpiUtils *acpi = nullptr;
    EffectController *effects = nullptr;
};

/// 启动 GTK4 前端。返回 g_application_run() 的退出码。
int Run(int argc, char **argv, const Services &services);

} // namespace Ui
