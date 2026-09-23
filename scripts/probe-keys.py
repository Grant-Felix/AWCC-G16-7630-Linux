#!/usr/bin/env python3
"""探测键盘按键的真实 evdev 事件（议题 #2 第 1 步）。

为什么要它：Dell G 系列那几个特殊键（G 模式键、F2～F6 那组）在各代机型上发的事件不一样——
有的是「EV_MSC 扫描码 + EV_KEY」，有的**只有 EV_MSC 扫描码**。绑定表必须按实测的
(type, code, value) 写，猜不得。

用法（要读 /dev/input，用 sudo 跑）：

    sudo python3 scripts/probe-keys.py                 # 引导模式：逐个提示，推荐
    sudo python3 scripts/probe-keys.py --keys F9 F2 F3 # 自定义要按的键
    sudo python3 scripts/probe-keys.py --device event2 # 指定设备，持续打印
    sudo python3 scripts/probe-keys.py --all           # 连鼠标等一起看（默认只看键盘）
    sudo python3 scripts/probe-keys.py --list          # 只列设备与能力

默认只看键盘设备，且只打印 EV_KEY / EV_MSC：上一版把所有有按键能力的设备都监听了，
于是无线鼠标（如「0 2.4G Wireless Receiver」）的 EV_REL 相对位移刷屏，看着像"没按键也在变"。

不依赖 libevdev 或 evtest：直接按 struct input_event 解包（64 位下 24 字节：
tv_sec 8 + tv_usec 8 + type 2 + code 2 + value 4）。
"""

import fcntl
import glob
import os
import select
import struct
import sys
import time

EV_SYN, EV_KEY, EV_MSC = 0x00, 0x01, 0x04
EV_TYPE_NAMES = {EV_SYN: "EV_SYN", EV_KEY: "EV_KEY", EV_MSC: "EV_MSC"}
KEY_NAMES = {
    59: "KEY_F1", 60: "KEY_F2", 61: "KEY_F3", 62: "KEY_F4", 63: "KEY_F5",
    64: "KEY_F6", 65: "KEY_F7", 66: "KEY_F8", 67: "KEY_F9", 68: "KEY_F10",
    87: "KEY_F11", 88: "KEY_F12", 212: "KEY_CAMERA", 226: "KEY_MEDIA",
    244: "厂商键码", 701: "厂商键码",
}
EVENT = struct.Struct("qqHHi")  # tv_sec, tv_usec, type, code, value
# 默认要按的键（议题 #2：G 模式键 + F2～F6 那组 A～E）
DEFAULT_KEYS = ["F9(G 模式键)", "F2", "F3", "F4", "F5", "F6"]


def device_name(path):
    base = os.path.basename(path)
    try:
        with open(f"/sys/class/input/{base}/device/name") as fh:
            return fh.read().strip()
    except OSError:
        return "?"


def event_bits(path):
    """EVIOCGBIT(0, 8) → 事件类型位图；打不开返回 None（多半没权限）。"""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        buf = bytearray(8)
        fcntl.ioctl(fd, 0x80084520, buf)  # EVIOCGBIT(0, 8)
        bits = 0
        for i, byte in enumerate(buf):
            bits |= byte << (8 * i)
        return bits
    except OSError:
        return None
    finally:
        os.close(fd)


def has_key(bits):
    if bits is None:
        return False
    return bool(bits & (1 << EV_KEY)) or bool(bits & (1 << EV_MSC))


def list_devices():
    return [(path, device_name(path), event_bits(path))
            for path in sorted(glob.glob("/dev/input/event*"))]


def pick_keyboard(rows):
    """优先挑 daemon 也在用的那块内置键盘，其次挑第一个有按键能力的设备。"""
    for path, name, bits in rows:
        if name == "AT Translated Set 2 keyboard" and has_key(bits):
            return path
    for path, name, bits in rows:
        if has_key(bits):
            return path
    return None


def read_events(fd, timeout):
    """在 timeout 内读一次，返回 (type, code, value) 列表（丢弃 EV_SYN）。"""
    events = []
    ready, _, _ = select.select([fd], [], [], timeout)
    if not ready:
        return events
    while True:
        try:
            data = os.read(fd, EVENT.size)
        except (BlockingIOError, OSError):
            break
        if len(data) < EVENT.size:
            break
        _, _, etype, code, value = EVENT.unpack(data)
        if etype == EV_SYN:
            continue
        events.append((etype, code, value))
    return events


def drain(fd):
    """丢掉设备缓冲里积压的事件，保证只看到下一次按键。"""
    while True:
        try:
            if not os.read(fd, EVENT.size):
                break
        except (BlockingIOError, OSError):
            break


def describe(events, show_all):
    for (etype, code, value) in events:
        if not show_all and etype not in (EV_KEY, EV_MSC):
            continue
        name = EV_TYPE_NAMES.get(etype, f"type{etype}")
        extra = f"  {KEY_NAMES.get(code, '')}" if etype == EV_KEY else ""
        print(f"    {name:<7} code={code:<6} value={value}{extra}")


def guided(rows, keys, show_all):
    path = pick_keyboard(rows)
    if path is None:
        print("没找到有按键能力的设备——要么权限不够（用 sudo），要么用 --device 指定。")
        return 1
    print(f"监听设备：{os.path.basename(path)}  {device_name(path)}")
    print("（只显示 EV_KEY / EV_MSC；鼠标那类 EV_REL 位移已过滤）")
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    drain(fd)
    results = []
    try:
        for label in keys:
            print()
            print(f"请按 {label} …（10 秒内，没反应就跳过）")
            burst = []
            deadline = time.time() + 10
            while time.time() < deadline and not burst:
                burst = read_events(fd, 0.5)
            if not burst:
                print("  （没等到事件）")
                results.append((label, [], []))
                continue
            # 再收 300ms，把这次按键的事件收全
            end = time.time() + 0.3
            while time.time() < end:
                burst += read_events(fd, 0.1)
            describe(burst, show_all)
            msc = sorted({v for (t, _, v) in burst if t == EV_MSC})
            keycodes = sorted({c for (t, c, v) in burst if t == EV_KEY and v == 1})
            results.append((label, msc, keycodes))
        print()
        print("=== 汇总（把这一段贴回议题 #2 就够）===")
        for label, msc, keycodes in results:
            scan = ", ".join(str(v) for v in msc) or "（无）"
            key = ", ".join(str(c) for c in keycodes) or "（无 EV_KEY，只有扫描码）"
            print(f"  {label:<14} EV_MSC 扫描码={scan:<12} EV_KEY code={key}")
    except KeyboardInterrupt:
        print()
    finally:
        os.close(fd)
    return 0


def stream(rows, show_all):
    fds = {}
    for path, name, bits in rows:
        if show_all or has_key(bits):
            try:
                fds[os.open(path, os.O_RDONLY | os.O_NONBLOCK)] = path
            except OSError:
                pass
    if not fds:
        print("没有可监听的设备（权限？用 sudo）。")
        return 1
    print("持续监听，Ctrl-C 结束。格式：设备  类型  code  value")
    print("-" * 62)
    try:
        while True:
            for fd in select.select(list(fds), [], [])[0]:
                events = read_events(fd, 0.0)
                if not show_all:
                    events = [e for e in events if e[0] in (EV_KEY, EV_MSC)]
                for (etype, code, value) in events:
                    name = EV_TYPE_NAMES.get(etype, f"type{etype}")
                    print(f"{os.path.basename(fds[fd]):>7}  {name:<7} code={code:<6} value={value}")
    except KeyboardInterrupt:
        print()
    finally:
        for fd in fds:
            os.close(fd)
    return 0


def main():
    args = sys.argv[1:]
    show_all = "--all" in args
    keys = DEFAULT_KEYS
    if "--keys" in args:
        keys = args[args.index("--keys") + 1:]

    rows = list_devices()
    if not rows:
        print("找不到 /dev/input/event*：受限沙箱里跑不了，请在真实主机上执行。")
        return 1
    if "--list" in args:
        for path, name, bits in rows:
            cap = "有按键能力" if has_key(bits) else "无按键事件能力"
            print(f"  {path}  {name}  （{cap}）")
        return 0
    if "--device" in args:
        device = args[args.index("--device") + 1]
        base = device if device.startswith("/dev/") else f"/dev/input/{device}"
        return stream([(base, device_name(base), None)], show_all)
    if "--stream" in args or "--all" in args:
        return stream(rows, show_all)
    return guided(rows, keys, show_all)


if __name__ == "__main__":
    sys.exit(main())
