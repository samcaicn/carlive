#!/usr/bin/env python3
"""给裸磁盘镜像写一个标准 MBR 分区表（单分区 FAT32-LBA，类型 0x0C）。

CE/WEC7 的 StorageManager 走 mspart 分区驱动，带 MBR 的盘兼容性最好。
用法: python3 mkmbr.py <image> [start_sector] [total_mb]
"""
import sys, struct, os

path = sys.argv[1]
start = int(sys.argv[2]) if len(sys.argv) > 2 else 2048
size = os.path.getsize(path)
total_sectors = size // 512
part_sectors = total_sectors - start

mbr = bytearray(512)
# 分区表项 1（偏移 446）
e = 446
mbr[e]     = 0x00            # 非活动分区
mbr[e+1:e+4] = b"\xfe\xff\xff"   # 起始 CHS（LBA 兼容值）
mbr[e+4]   = 0x0C            # 类型：FAT32 LBA
mbr[e+5:e+8] = b"\xfe\xff\xff"   # 结束 CHS
struct.pack_into("<I", mbr, e+8, start)
struct.pack_into("<I", mbr, e+12, part_sectors)
mbr[510:512] = b"\x55\xaa"

with open(path, "r+b") as f:
    f.write(mbr)
print("MBR written: start=%d sectors, partition=%d sectors (%.0f MB)" %
      (start, part_sectors, part_sectors * 512 / 1024 / 1024))
