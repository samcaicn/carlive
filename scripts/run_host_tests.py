#!/usr/bin/env python3
"""run_host_tests.py - 在宿主机上驱动「假 adbd + 车机 AdbTransport」端到端回归。

为什么需要这个驱动：直接用 shell 让两个进程协作不可靠——后台进程活不过一次
工具调用 / CI step，且「连一下端口探活」会【吃掉单会话服务唯一的那个 session】，
导致真正被测的客户端连不上。这里在同一进程里起仿真器子进程，
靠轮询它自己的启动日志判断就绪，并对被测进程设置硬 deadline 防止死锁挂死 CI。

用法：
    python3 scripts/run_host_tests.py [被测可执行文件路径]
"""
import os
import signal
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMULATOR = os.path.join(ROOT, "scripts", "fake_adbd.py")
READY_MARK = "listening on"
HARD_DEADLINE_SEC = 60


def run_emulator_ready():
    """启动 fake_adbd，返回 (proc, [输出行])。就绪判定只读日志不连端口。"""
    proc = subprocess.Popen(
        [sys.executable, "-u", EMULATOR],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=ROOT)
    lines = []

    def reader():
        for raw in iter(proc.stdout.readline, b""):
            lines.append(raw.decode(errors="replace").rstrip())
    threading.Thread(target=reader, daemon=True).start()

    for _ in range(200):     # 最多等 20s
        if any(READY_MARK in l for l in lines):
            return proc, lines
        if proc.poll() is not None:
            break
        time.sleep(0.1)
    return proc, lines


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "adbtest")
    if not os.path.exists(target):
        sys.exit(f"找不到被测程序：{target}（先跑 scripts/build_host_test.sh）")

    emu, emu_lines = run_emulator_ready()
    ready = any(READY_MARK in l for l in emu_lines)
    print(f"[driver] emulator ready = {ready}", flush=True)

    rc = None
    try:
        proc = subprocess.Popen([target], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, cwd=ROOT)
        out = []

        def rd():
            for raw in iter(proc.stdout.readline, b""):
                s = raw.decode(errors="replace").rstrip()
                out.append(s)
                print(s, flush=True)
        t = threading.Thread(target=rd, daemon=True)
        t.start()

        deadline = time.time() + HARD_DEADLINE_SEC
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.2)
        if proc.poll() is None:
            print(f"[driver] !!! 被测进程 {HARD_DEADLINE_SEC}s 未退出，判定死锁 → kill",
                  flush=True)
            proc.kill()
            rc = -9
        else:
            rc = proc.returncode
    finally:
        try:
            os.kill(emu.pid, signal.SIGKILL)
        except Exception:
            pass

    time.sleep(0.3)
    print("=== emulator log ===", flush=True)
    for l in emu_lines[-25:]:
        print(l, flush=True)
    print(f"=== 被测进程 exit code: {rc} ===", flush=True)
    sys.exit(0 if rc == 0 else 1)


if __name__ == "__main__":
    main()
