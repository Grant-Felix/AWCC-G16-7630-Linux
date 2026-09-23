#!/usr/bin/env python3
"""假 daemon（验证用）。

真 daemon 要 root、还要真的键盘设备，没法在无人值守的验证里跑。这个假货只把协议摆出来，
用来验证 GUI 的客户端路径：订阅推送、keybind-list / keybind-set、mode-set / brightness-set。
协议与实现见 src/Daemon.cpp 与 DESIGN.md 第九节。

用法（要在同一个 shell 里跑，/tmp 才共享）：

    python3 tests/fake-daemon.py &
    GSETTINGS_BACKEND=memory XDG_CONFIG_HOME=$PWD/.cache/cfg \\
        ./build/awcc --ui-page=help --ui-subpage=keybinds --ui-snapshot=/tmp/kb.png
"""

import os
import socket
import sys
import threading
import time

SOCK = os.environ.get("AWCC_SOCK", "/tmp/awcc.sock")
# 与 KeyBinds::Defaults() 一致：G 模式键切 G 模式、灯键循环亮度、其余五个实测键先 none
BINDS = (
    "104 gmode-toggle   # F9\n"
    "105 brightness-cycle   # Light\n"
    "146 none   # F2\n"
    "147 none   # F3\n"
    "148 none   # F4\n"
    "149 none   # F5\n"
    "150 none   # F6\n"
)
state = {"mode": "balanced", "brightness": 50, "keybinds": 1}
subscribers = []  # 长连接：推送用


def snapshot() -> str:
    return (f"mode {state['mode']}\n"
            f"brightness {state['brightness']}\n"
            f"keybinds {state['keybinds']}\n")


def broadcast(line: str) -> None:
    print(f"[fake] 推送 {line.strip()!r}", flush=True)
    for conn in list(subscribers):
        try:
            conn.sendall(line.encode())
        except OSError:
            subscribers.remove(conn)


def handle(conn: socket.socket) -> None:
    try:
        data = conn.recv(256).decode(errors="replace").strip()
    except OSError:
        return
    print(f"[fake] 收到 {data!r}", flush=True)
    if data == "subscribe":
        subscribers.append(conn)
        conn.sendall(snapshot().encode())

        # 200ms 后推一次模式变化：用来验证「后端改 → 前端重画」这条路真的通了
        def later() -> None:
            time.sleep(0.2)
            state["mode"] = "performance"
            broadcast("mode performance\n")

        threading.Thread(target=later, daemon=True).start()
        return  # 长连接，别关
    if data.startswith("keybind-list"):
        conn.sendall(BINDS.encode())
    elif data.startswith("keybind-set"):
        state["keybinds"] += 1
        conn.sendall(b"ok\n")
        broadcast(f"keybinds {state['keybinds']}\n")
    elif data.startswith("mode-set"):
        state["mode"] = data.split(" ", 1)[1]
        conn.sendall(b"ok\n")
        broadcast(f"mode {state['mode']}\n")
    elif data.startswith("brightness-set"):
        state["brightness"] = int(data.split(" ", 1)[1])
        conn.sendall(b"ok\n")
        broadcast(f"brightness {state['brightness']}\n")
    else:
        conn.sendall(b"0x1\n")  # ACPI 读之类的，给个像样的回复
    conn.close()


def main() -> int:
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCK)
    server.listen(8)
    print(f"[fake] 监听 {SOCK}", flush=True)
    while True:
        conn, _ = server.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
