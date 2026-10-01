#!/usr/bin/env python3
"""PE 依赖静态分析：解析 WinCE ARM 程序的导入表，判断在给定 NK 镜像上能否加载。

用法: python3 pecheck.py <nk_modules.txt> <exe1> [exe2 ...]
输出每个程序: 架构 / 子系统 / 需要的 DLL / 是否在 NK 中 / 是否随程序自带。
"""
import struct, sys, os


def rva2off(sections, rva):
    for va, vsz, praw, rsz in sections:
        if va <= rva < va + max(vsz, rsz):
            return praw + (rva - va)
    return None


def parse(path):
    d = open(path, "rb").read()
    if d[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe+4] != b"PE\0\0":
        return None
    machine = struct.unpack_from("<H", d, pe+4)[0]
    nsec = struct.unpack_from("<H", d, pe+6)[0]
    optsz = struct.unpack_from("<H", d, pe+20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    subsystem = struct.unpack_from("<H", d, opt+68)[0]
    # DataDirectory[1] = Import Table
    if magic == 0x10B:
        dd = opt + 96
    elif magic == 0x20B:
        dd = opt + 112
    else:
        return None
    id_rva, id_sz = struct.unpack_from("<II", d, dd + 8)  # index 1
    # sections
    sec = opt + optsz
    sections = []
    for i in range(nsec):
        off = sec + i*40
        vsz = struct.unpack_from("<I", d, off+8)[0]
        va = struct.unpack_from("<I", d, off+12)[0]
        rsz = struct.unpack_from("<I", d, off+16)[0]
        praw = struct.unpack_from("<I", d, off+20)[0]
        sections.append((va, vsz, praw, rsz))
    imports, delay = [], []
    seen = set()
    if id_rva:
        off = rva2off(sections, id_rva)
        if off:
            i = 0
            while True:
                base = off + i*20
                if base + 20 > len(d):
                    break
                oft, ts, fc, name_rva, fthunk = struct.unpack_from("<IIIII", d, base)
                if oft == 0 and name_rva == 0 and ts == 0:
                    break
                no = rva2off(sections, name_rva)
                if no:
                    end = d.index(b"\0", no)
                    nm = d[no:end].decode("latin1")
                    if nm.lower() not in seen:
                        seen.add(nm.lower())
                        imports.append(nm)
                i += 1
    return dict(machine=machine, subsystem=subsystem,
                imports=imports, magic=magic)


def norm(nm):
    """归一化模块名：去掉 .dll/.exe 后缀与路径，便于比较（CE 导入常写成 COREDLL 无后缀）。"""
    b = os.path.basename(nm.strip()).lower()
    for ext in (".dll", ".exe", ".cpl", ".ffp"):
        if b.endswith(ext):
            b = b[:-len(ext)]
    return b


def main():
    nkfile, exes = sys.argv[1], sys.argv[2:]
    nk = set()
    for line in open(nkfile, encoding="utf-8", errors="ignore"):
        t = line.strip()
        if t:
            nk.add(norm(t))
    ARCH = {0x1C0: "ARM", 0x1C2: "ARM(Thumb)", 0x1C4: "ARMv7", 0x14C: "x86",
            0x166: "MIPS", 0x169: "MIPS16", 0x1F0: "PowerPC", 0x1A2: "SH3"}
    SUBSYS = {1: "NATIVE", 2: "Win32GUI", 3: "Win32CUI", 9: "WinCE_GUI", 10: "WinCE_CUI"}
    # 核心 DLL（NK 里一定存在，但 strings 可能没单独列）
    core = {"coredll"}
    nk |= core
    for exe in exes:
        r = parse(exe)
        print("=" * 78)
        if not r:
            print("!! 非 PE: %s" % exe)
            continue
        print("程序: %s" % os.path.basename(exe))
        print("  架构: %s (0x%03x)   子系统: %s (%d)" %
              (ARCH.get(r["machine"], "?"),
               r["machine"], SUBSYS.get(r["subsystem"], "?"), r["subsystem"]))
        localdir = os.path.dirname(exe)
        locals_ = set()
        for root, _, files in os.walk(localdir):
            for f in files:
                if f.lower().endswith(".dll"):
                    locals_.add(norm(f))
        print("  依赖 DLL 共 %d 个:" % len(r["imports"]))
        miss, ok_nk, ok_local = [], [], []
        for nm in sorted(r["imports"]):
            low = norm(nm)
            if low in nk:
                ok_nk.append(nm)
            elif low in locals_:
                ok_local.append(nm)
            else:
                miss.append(nm)
        if ok_nk:
            print("    [NK 内置]   " + ", ".join(ok_nk))
        if ok_local:
            print("    [程序自带]   " + ", ".join(ok_local))
        if miss:
            print("    [!! 缺失 !!] " + ", ".join(miss))
        else:
            print("    ✓ 全部依赖可满足 —— 在本 NK 上具备加载条件")
        print("  结论: %s" % ("可尝试运行" if not miss else
              "缺 %d 个依赖，需补齐才能加载" % len(miss)))


if __name__ == "__main__":
    main()
