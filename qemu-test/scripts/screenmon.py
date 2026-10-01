#!/usr/bin/env python3
"""通过 QEMU HMP monitor 的 screendump 把 ARM WinCE 虚拟机的帧缓冲抓成 PNG。

绕过 RFB/VNC 客户端握手（QEMU VNC 在 ClientInit 后会因单客户端/版本协商断连），
直接从 QEMU 内部 dump 帧缓冲，最稳定。

前置：
  - 容器内 QEMU 用 -monitor tcp:0.0.0.0:4444 启动
  - docker run 把 4444 映射到宿主机（如 -p 127.0.0.1:14444:4444）
  - screendump 写到的路径在挂载卷里（如 /wince/images/screen.ppm），宿主机可读

用法: python3 screenmon.py [host] [port] [ppm_in_container] [out.png]
"""
import socket, struct, zlib, sys, time


def hmp(cmd, host="127.0.0.1", port=14444, timeout=15):
    s = socket.create_connection((host, port), timeout=timeout)
    s.settimeout(timeout)
    buf = b""
    # 读到首个 (qemu) 提示符
    def read_prompt():
        nonlocal buf
        while b"(qemu)" not in buf:
            try:
                d = s.recv(4096)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
    read_prompt()
    s.sendall((cmd + "\n").encode())
    # 等命令执行完，再次出现提示符
    time.sleep(1.0)
    buf = b""
    read_prompt()
    s.close()


def ppm_to_png(ppm_path, out_path):
    with open(ppm_path, "rb") as f:
        data = f.read()
    # 解析 P6 头
    idx = 0
    assert data[:2] == b"P6", "not a P6 PPM"
    idx = 2
    fields = []
    while len(fields) < 3:
        # 跳过空白和注释
        while idx < len(data) and data[idx] in b" \t\r\n":
            idx += 1
        if idx < len(data) and data[idx] == ord("#"):
            while idx < len(data) and data[idx] != ord("\n"):
                idx += 1
            continue
        start = idx
        while idx < len(data) and data[idx] not in b" \t\r\n":
            idx += 1
        fields.append(int(data[start:idx]))
    w, h, maxval = fields
    # 跳过紧跟的单个换行
    while idx < len(data) and data[idx] in b" \t\r\n":
        idx += 1
    px = data[idx:]
    if maxval == 65535:
        # 16-bit，降采样到 8-bit
        rgb = bytearray()
        for i in range(0, len(px) - 1, 2):
            rgb.append(px[i] >> 8)
        px = bytes(rgb)
    assert len(px) >= w * h * 3, "ppm data too short: %d < %d" % (len(px), w*h*3)
    rgba = bytearray(w * h * 4)
    for i in range(w * h):
        rgba[i*4]   = px[i*3]
        rgba[i*4+1] = px[i*3+1]
        rgba[i*4+2] = px[i*3+2]
        rgba[i*4+3] = 255

    def chunk(tag, cdata):
        return struct.pack(">I", len(cdata)) + tag + cdata + struct.pack(">I", zlib.crc32(tag + cdata) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgba[y*4*w:(y+1)*4*w] for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    with open(out_path, "wb") as f:
        f.write(out)
    print("saved %s  (%dx%d)" % (out_path, w, h))


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 14444
    ppm = sys.argv[3] if len(sys.argv) > 3 else "/wince/images/screen.ppm"
    host_ppm = sys.argv[4] if len(sys.argv) > 4 else ppm
    out = sys.argv[5] if len(sys.argv) > 5 else "screen.png"
    print("[*] screendump via HMP %s:%d -> %s" % (host, port, ppm))
    hmp("screendump %s" % ppm, host, port)
    time.sleep(0.8)
    ppm_to_png(host_ppm, out)


if __name__ == "__main__":
    main()
