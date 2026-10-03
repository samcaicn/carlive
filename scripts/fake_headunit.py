#!/usr/bin/env python3
# 假车机：直连手机 GLOAI App，验证 协议握手/视频配置/收帧 全链路
import socket, struct, json, sys, time

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.10.5"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8686
MAGIC = b"GLOA"  # 0x47 0x4C 0x4F 0x41

def msg(t, payload=b""):
    return MAGIC + bytes([t]) + struct.pack(">I", len(payload)) + payload

def read_exact(s, n):
    buf = b""
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d:
            raise EOFError("closed")
        buf += d
    return buf

def read_msg(s):
    hdr = read_exact(s, 9)
    assert hdr[:4] == MAGIC, f"bad magic {hdr[:4]!r}"
    t = hdr[4]
    ln = struct.unpack(">I", hdr[5:9])[0]
    payload = read_exact(s, ln) if ln else b""
    return t, payload

s = socket.create_connection((HOST, PORT), timeout=5)
s.settimeout(10)
print(f"[1] TCP 已连上手机 {HOST}:{PORT}")

hs = json.dumps({"role": "headunit", "proto_ver": 1,
                 "caps": {"video_decoders": ["mjpeg"], "max_w": 800, "max_h": 480, "touch": True}})
s.sendall(msg(0x01, hs.encode()))
print("[2] 已发车机握手:", hs)

# 手机回握手 (0x01) + 视频配置 (0x02) + 视频帧 (0x03)
got_hs = got_cfg = False
frames = 0
t0 = time.time()
while time.time() - t0 < 15 and frames < 3:
    try:
        t, p = read_msg(s)
    except (socket.timeout, EOFError) as e:
        print(f"[!] 读超时/断开: {e}")
        break
    if t == 0x01:
        print("[3] 收到手机握手:", p.decode(errors="replace")[:200])
        got_hs = True
    elif t == 0x02:
        print("[4] 收到视频配置: codec=%s %s" % (p[0] if p else "?", p[1:].hex()))
        got_cfg = True
    elif t == 0x03:
        iskey = p[0] if p else 0
        ts = struct.unpack(">I", p[1:5])[0] if len(p) >= 5 else 0
        dlen = struct.unpack(">I", p[5:9])[0] if len(p) >= 9 else 0
        frames += 1
        print(f"[5] 收到视频帧 #{frames}: key={iskey} ts={ts} jpeg={dlen}B "
              f"jpeg头={'FFD8' + p[9:11].hex().upper() if len(p) >= 11 else '?'}")
    elif t == 0x06:
        pass  # 心跳
    else:
        print(f"[?] 其他消息 type=0x{t:02x} len={len(p)}")

# 发触摸事件验证下行
s.sendall(msg(0x04, struct.pack(">ffB", 0.5, 0.5, 0)))
print("[6] 已发触摸测试事件 (0.5,0.5 DOWN)")
time.sleep(0.5)
s.close()

ok = got_hs and (got_cfg or frames)
print("=== 结果:", "PASS ✅ 手机端协议链路通" if ok else "PARTIAL（握手通了但没收到流？手机可能未授权录屏）", "===")
