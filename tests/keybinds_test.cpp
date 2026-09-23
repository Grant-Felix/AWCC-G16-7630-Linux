// 绑定表的离线自测（议题 #2）：解析 / 校验 / 存取 / 原子保存。
//
// 本仓库没有测试框架，就一个独立程序。编译运行（LOGURU_WITH_STREAMS 是工程里定义的）：
//   LOGURU=$(find build -name libloguru.a | head -1)
//   g++ -std=c++23 -Iinclude -Ibuild/_deps/loguru-src -DLOGURU_WITH_STREAMS=1 \
//       tests/keybinds_test.cpp src/KeyBinds.cpp "$LOGURU" -o /tmp/keybinds_test && /tmp/keybinds_test
// 期望输出末行「全部通过」。
#include "KeyBinds.h"
#include <cstdio>
#include <fstream>
#include <unistd.h>
int main() {
    int failed = 0;
    auto check = [&](bool ok, const char *what) {
        printf("  %s %s\n", ok ? "✓" : "✗ 失败:", what);
        if (!ok) failed++;
    };
    // 解析
    check(KeyBinds::ParseLine("104 gmode-toggle")->action == "gmode-toggle", "解析正常行");
    check(!KeyBinds::ParseLine("# 注释").has_value(), "跳过注释");
    check(!KeyBinds::ParseLine("104 乱写").has_value(), "拒绝未知动作");
    check(!KeyBinds::ParseLine("abc none").has_value(), "拒绝非数字扫描码");
    check(!KeyBinds::ParseLine("104").has_value(), "拒绝缺动作");
    // 动作校验
    check(KeyBinds::ValidAction("mode:performance"), "接受 mode:performance");
    check(!KeyBinds::ValidAction("mode:overclock"), "拒绝 mode:overclock");
    check(!KeyBinds::ValidAction("rm -rf /"), "拒绝任意命令");
    // 默认表用的是实测扫描码
    KeyBinds def = KeyBinds::Defaults();
    check(def.Bound(104) && def.ActionFor(104) == "gmode-toggle", "默认：G 模式键 -> gmode-toggle");
    check(def.Bound(148) && def.ActionFor(148) == "none", "默认：F4(148) 先设为 none");
    check(std::string(KeyBinds::KeyLabel(150)) == "F6", "键名映射 F6");
    // 保存 -> 读回
    const std::string path = "/tmp/awcc-keybinds-test.conf";
    check(def.Save(path), "保存成功");
    KeyBinds back = KeyBinds::Load(path);
    check(back.ActionFor(104) == "gmode-toggle" && back.Items().size() == def.Items().size(),
          "读回与写入一致");
    check(back.Set(147, "mode:performance"), "改绑定");
    check(!back.Set(147, "错的"), "非法动作被拒且不改动");
    check(back.ActionFor(147) == "mode:performance", "改动生效");
    // 文件不存在时回落默认
    KeyBinds missing = KeyBinds::Load("/tmp/awcc-keybinds-not-exist.conf");
    check(missing.Bound(104), "文件缺失时回落默认");
    printf("%s\n", failed == 0 ? "全部通过" : "有失败项");
    unlink(path.c_str());
    return failed;
}
