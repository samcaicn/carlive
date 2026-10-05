#!/usr/bin/env python3
"""车机 SD 卡日志回收分析 —— 拔卡插回 Mac 后一键出结论。

用法:
    python3 scripts/collect_car_logs.py              # 自动找 /Volumes/SD
    python3 scripts/collect_car_logs.py /Volumes/SD  # 或显式指定挂载点
    python3 scripts/collect_car_logs.py . --tail 40  # 看更多尾部日志

它做的事（以前要手工逐文件翻，容易看漏）:
  1. 列出 wince/ 下每个 exe 对应的 .log / .crash.log 是否存在、多大、什么时候写的
  2. 从每个日志里抽出"排障必需的那几行"：构建时间、variant、exe 真实路径、可用内存、
     noLocalIP 开关值、首次 GetCandidates 结果
  3. 取 crash.log 的最后一条 [stage] —— 那就是崩溃点
  4. 按判据给出下一步该做什么，而不是把原始日志丢给人自己猜

判据表（这些是花钱买来的经验，别再靠猜）:
  · 既没有 .log 也没有 .crash.log
        → 进程根本没进 WinMain：不是崩溃，是**加载失败**
          （缺 IPHLPAPI.DLL / WS2.DLL 等 ROM 组件，或 SD 卡文件系统只读/损坏）
  · 日志里有 "already running"
        → 单实例互斥冲突：上一次没退干净。R28 后互斥名带 exe 路径，理论上不会互拦
  · crash.log 停在某个 [stage]
        → 最后那条 stage 与下一条之间的代码区间就是崩溃现场
  · noLocalIP=0 且 stage 停在 LocalIPv4*/AdapterBufLen*
        → 网卡枚举是元凶，把 config.txt 的 noLocalIP=1 打开即可绕过
"""
import os
import re
import sys

EXES = ["tuptup.exe", "tuptup-full.exe", "tuptup-usbnet.exe"]
KEYS = [
    (re.compile(r"^\[t\+\d+\] build .*variant=\S+.*$", re.M), "build/variant"),
    (re.compile(r"^\[t\+\d+\] exe path = .*$", re.M), "exe path"),
    (re.compile(r"^\[t\+\d+\] mem: .*$", re.M), "mem"),
    (re.compile(r"^\[t\+\d+\] ConnThread start, .*$", re.M), "ConnThread start"),
    (re.compile(r"^\[t\+\d+\] \[stage\] first GetCandidates -> .*$", re.M), "first candidates"),
    (re.compile(r"^\[t\+\d+\] already running.*$", re.M), "already running"),
    (re.compile(r"^\[t\+\d+\] connected -> handshake sent.*$", re.M), "connected"),
]


def read_text(p):
    try:
        with open(p, "rb") as f:
            b = f.read()
    except OSError as e:
        return None, str(e)
    for enc in ("utf-8", "gbk", "latin-1"):
        try:
            return b.decode(enc), None
        except UnicodeDecodeError:
            continue
    return b.decode("utf-8", "replace"), None


def summarize(wince, tail_n):
    print("=" * 72)
    print(f"SD 日志目录: {wince}")
    logs = sorted(f for f in os.listdir(wince) if f.endswith(".log"))
    if not logs:
        print("\n!! 一个日志文件都没有 —— 没有任何 exe 成功进入 WinMain")
        print("   判据：不是崩溃，而是【加载失败】。常见原因：")
        print("   · ROM 缺 IPHLPAPI.DLL 或 WS2.DLL（进程起不来，连 MessageBox 都没有）")
        print("   · SD 卡接触不良 / 目录被写保护，CreateFile 建不出日志文件")
        print("   · 车机根本没执行到这个文件")
        return
    for name in logs:
        path = os.path.join(wince, name)
        st = os.stat(path)
        txt, err = read_text(path)
        print("\n" + "-" * 72)
        tag = "[crash]" if name.endswith(".crash.log") else "[ run ]"
        if err:
            print(f"{tag} {name}  读取失败: {err}")
            continue
        lines = [l for l in txt.replace("\r\n", "\n").split("\n") if l.strip()]
        import datetime
        mt = datetime.datetime.fromtimestamp(st.st_mtime).strftime("%m-%d %H:%M:%S")
        print(f"{tag} {name}  {st.st_size}B  最后写入 {mt}  共 {len(lines)} 行")

        if name.endswith(".crash.log"):
            stages = [l for l in lines if l.startswith("[stage]")]
            if not stages:
                print("    崩溃日志存在但没有任何 [stage] —— 进程在 CrashLogInit 之前就死了")
            else:
                print(f"    最后停在 → {stages[-1]}")
                print(f"    (共 {len(stages)} 个 stage，前一个是: {stages[-2] if len(stages)>1 else '无'})")
            continue

        for pat, label in KEYS:
            m = pat.search(txt)
            if m:
                print(f"    {label:20s} {m.group(0)[:110]}")
        print("    --- 尾部 ---")
        for l in lines[-tail_n:]:
            print(f"    {l[:110]}")
    print("\n" + "=" * 72)
    print("下一步建议：")

    any_log = any(not f.endswith(".crash.log") for f in logs)
    have_run = set(f for f in logs if not f.endswith(".crash.log"))
    miss = []
    for exe in EXES:
        base = exe[:-4] if exe != "tuptup.exe" else "tuptup"
        cand = base + ".log"
        if cand not in have_run:
            miss.append(exe)
    if miss:
        print(f"  · 还没有跑过的版本: {', '.join(miss)}")
    if any_log:
        stages_all = []
        for f in logs:
            if f.endswith(".crash.log"):
                t, _ = read_text(os.path.join(wince, f))
                if t:
                    stages_all += [(f, l) for l in t.replace("\r\n", "\n").split("\n")
                                   if l.strip().startswith("[stage]")]
        if stages_all:
            print("  · 按顺序看每个 crash.log 的最后 stage，它俩之间的代码区间就是崩溃现场：")
            seen = {}
            for f, l in stages_all:
                seen[f] = l
            for f, l in seen.items():
                print(f"      {f}: {l}")
        print("  · 若 stage 停在 LocalIPv4*/AdapterBufLen*：把 config.txt 的 noLocalIP=1 取消注释再跑一次")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    tail_n = 15
    for a in sys.argv[1:]:
        if a.startswith("--tail="):
            tail_n = int(a.split("=", 1)[1])
        elif a == "--tail" and len(sys.argv) > sys.argv.index(a) + 1:
            tail_n = int(sys.argv[sys.argv.index(a) + 1])
    root = args[0] if args else "/Volumes/SD"
    for cand in (os.path.join(root, "carlive", "wince"), os.path.join(root, "wince"), root):
        if os.path.isdir(cand):
            summarize(cand, tail_n)
            return
    print(f"找不到日志目录，请确认 SD 卡已挂载（试过 {root}/carlive/wince 等路径）")
    sys.exit(1)


if __name__ == "__main__":
    main()
