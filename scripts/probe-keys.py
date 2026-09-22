#!/usr/bin/env python3
"""探测键盘按键的真实 evdev 事件（议题 #2 第 1 步）。

为什么要它：Dell G 系列那几个特殊键（G 模式键、F2～F6 那组 A～E）在各代机型上发的
事件不一样——有的是标准 EV_KEY，有的只发 EV_MSC 的扫描码。绑定表必须按实测的
(type, code, value) 来写，猜不得。

用法（需要读 /dev/input，用 sudo 跑）：

    sudo python3 scripts/probe-keys.py            # 自动挑有按键能力的设备
    sudo python3 scripts/probe-keys.py event6     # 只盯某一个设备
    sudo python3 scripts/probe-keys.py --list     # 只列出设备与能力，不监听

然后依次按：F9（G 模式键）、F2、F3、F4、F5、F6，按 Ctrl-C 结束，把打印的行贴回议题。

不依赖 libevdev 或 evtest：直接按 struct input_event 解包（64 位下 24 字节：
tv_sec 8 + tv_usec 8 + type 2 + code 2 + value 4）。
"""

import fcntl
import glob
import os
import select
import struct
import sys

# evdev 常量（linux/input-event-codes.h）
EV_SYN, EV_KEY, EV_MSC = 0x00, 0x01, 0x04
EV_TYPE_NAMES = {EV_SYN: "EV_SYN", EV_KEY: "EV_KEY", EV_MSC: "EV_MSC"}
# 常见的几个，方便对着看；其余只打印数字
KEY_NAMES = {
    59: "KEY_F1", 60: "KEY_F2", 61: "KEY_F3", 62: "KEY_F4", 63: "KEY_F5",
    64: "KEY_F6", 65: "KEY_F7", 66: "KEY_F8", 67: "KEY_F9", 68: "KEY_F10",
    87: "KEY_F11", 88: "KEY_F12", 104: "KEY_KPPLUS(?)",
    # EVIOCGBIT 用的能力位
}
EVENT = struct.Struct("qqHHi")  # tv_sec, tv_usec, type, code, value


def device_name(path):
    """读 /sys 下的设备名（读不到就返回问号）。"""
    base = os.path.basename(path)
    for candidate in (f"/sys/class/input/{base}/device/name",):
        try:
            with open(candidate) as fh:
                return fh.read().strip()
        except OSError:
            pass
    return "?"


def has_key_events(path):
    """用 EVIOCGBIT(0) 拿事件类型位图，判断这个设备会不会发 EV_KEY 或 EV_MSC。"""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        return False
    try:
        # EVIOCGBIT(ev,len) = _IOC(_IOC_READ,'E',0x20+ev,len)
        buf = bytearray(8)
        fcntl.ioctl(fd, 0x80084520, buf)  # EVIOCGBIT(0, 8)
        bits = 0
        for i, byte in enumerate(buf):
            bits |= byte << (8 * i)
        return bool(bits & (1 << EV_KEY)) or bool(bits & (1 << EV_MSC))
    except OSError:
        return False
    finally:
        os.close(fd)


def main():
    args = [a for a in sys.argv[1:]]
    list_only = "--list" in args
    wanted = [a for a in args if not a.startswith("-")]

    if wanted:
        paths = [p if p.startswith("/dev/") else f"/dev/input/{p}" for p in wanted]
    else:
        paths = sorted(glob.glob("/dev/input/event*"))

    if not paths:
        print("找不到 /dev/input/event*：如果是在受限沙箱里跑，请在真实主机上执行。")
        return 1

    usable = []
    for path in paths:
        if has_key_events(path):
            usable.append(path)
            print(f"  {path}  {device_name(path)}")
        elif list_only:
            print(f"  {path}  {device_name(path)}  （无按键事件能力）")

    if list_only:
        return 0
    if not usable:
        print("没有可用于监听的设备（要么没有权限，要么都被过滤掉了）。试着用 sudo 跑。")
        return 1

    fds = {}
    for path in usable:
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            fds[fd] = path
        except OSError as exc:
            print(f"  打不开 {path}: {exc}")
    if not fds:
        print("所有设备都打不开——多半是权限问题（用 sudo）。")
        return 1

    print()
    print("开始监听。请依次按：F9（G 模式键）、F2、F3、F4、F5、F6，然后 Ctrl-C 结束。")
    print("格式：设备  事件类型  code  value")
    print("-" * 62)
    try:
        while True:
            ready, _, _ = select.select(list(fds), [], [])
            for fd in ready:
                while True:
                    try:
                        data = os.read(fd, EVENT.size)
                    except BlockingIOError:
                        break
                    if len(data) < EVENT.size:
                        break
                    tv_sec, tv_usec, etype, code, value = EVENT.unpack(data)
                    if etype == EV_SYN:
                        continue  # 同步事件没有信息量，刷屏
                    tname = EV_TYPE_NAMES.get(etype, f"type{etype}")
                    extra = ""
                    if etype == EV_KEY:
                        extra = f"  {KEY_NAMES.get(code, '')}"
                    elif etype == EV_MSC:
                        extra = "  （Dell 的特殊键常只发扫描码）"
                    print(f"{os.path.basename(fds[fd]):>7}  {tname:<7} code={code:<5} "
                          f"value={value}{extra}")
    except KeyboardInterrupt:
        print()
        print("已停止。把上面的行贴回议题 #2 即可。")
        return 0
    finally:
        for fd in fds:
            os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
