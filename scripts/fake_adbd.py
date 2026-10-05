#!/usr/bin/env python3
# scripts/fake_adbd.py - 模拟 Android adbd（仅用于主机端验证 AdbTransport 握手/流封装）
#
# 作用：在 127.0.0.1:5555 扮演 adbd，完成 CNXN / AUTH(TOKEN→SIGNATURE→RSAPUBLICKEY) 鉴权，
#       接受 OPEN "tcp:8686"，随后把收到的流数据原样回显（echo），并主动下发一段大 payload，
#       以此验证车机端 AdbTransport 的 WRTE/OKAY 窗口流控与分块重组是否正确。
#
# 验证强度：AUTH SIGNATURE 用与车机相同的公钥（见 adbkey.h 的 ADB_RSA_PUB）做
#           sig^e mod n == EMSA(SHA1(token)) 校验，确保真实 adbd 会接受我们的签名。
import socket, struct, sys, time, subprocess, re, base64

HOST = "127.0.0.1"
PORT = 5555
ADB_MAX_DATA = 4096   # 故意设小，强制车机端把大包分块成多个 WRTE

def u32(b): return struct.unpack("<I", b)[0]
def p32(v): return struct.pack("<I", v)
def msg(cmd, a1, a2, data=b""):
    chk = sum(data) & 0xFFFFFFFF
    # 24 字节小端头：cmd,arg0,arg1,len,checksum,magic(=cmd^0xFFFFFFFF)
    return (p32(cmd) + p32(a1) + p32(a2) + p32(len(data)) + p32(chk)
            + p32(cmd ^ 0xFFFFFFFF)) + data
def recv_msg(s):
    hdr = recvn(s, 24)
    cmd, a1, a2, dlen, chk, mag = struct.unpack("<IIIIII", hdr)
    assert mag == (cmd ^ 0xFFFFFFFF), "bad magic"
    data = recvn(s, dlen) if dlen else b""
    assert (sum(data) & 0xFFFFFFFF) == chk, "bad checksum"
    return cmd, a1, a2, data
def recvn(s, n):
    b = b""
    while len(b) < n:
        d = s.recv(n - len(b))
        if not d: raise EOFError("closed")
        b += d
    return b

A_SYNC, A_CNXN, A_AUTH, A_OPEN, A_OKAY, A_WRTE, A_CLSE = (
    0x434e5953, 0x4e584e43, 0x48545541, 0x4e45504f, 0x59414b4f, 0x45545257, 0x45534c43)
(TOKEN, SIGNATURE, RSAPUBKEY) = (1, 2, 3)

# ---- 解析 adbkey.h 的 ADB_RSA_PUB，取出 n / e 用于验签 ----
def load_pubkey():
    txt = open("wince/src/adbkey.h").read()
    m = re.search(r"ADB_RSA_PUB\[(\d+)\]\s*=\s*\{([^}]*)\}", txt, re.S)
    hexes = re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))
    blob = bytes(int(h, 16) for h in hexes)
    size = u32(blob[0:4])
    n0inv = u32(blob[4:8])
    n = int.from_bytes(blob[8:8+size*4], "little")
    e = u32(blob[8+size*4+size*4:8+size*4+size*4+4])
    return n, e
N, E = load_pubkey()

SHA1_DIGESTINFO = bytes.fromhex("3021300906052b0e03021a05000414")

def verify_sig(token, sig):
    em_int = pow(int.from_bytes(sig, "big"), E, N)
    em = em_int.to_bytes(256, "big")
    padlen = 256 - 3 - 35
    expect = b"\x00\x01" + b"\xff"*padlen + b"\x00" + SHA1_DIGESTINFO + sha1(token)
    return em == expect
import hashlib
def sha1(b): return hashlib.sha1(b).digest()

def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT)); srv.listen(1)
    print(f"[fake_adbd] listening on {HOST}:{PORT}, ADB_MAX_DATA={ADB_MAX_DATA}")
    conn, addr = srv.accept()
    print(f"[fake_adbd] client connected from {addr}")

    # 1) 设备侧先发 CNXN
    conn.sendall(msg(A_CNXN, 0x01000000, ADB_MAX_DATA, b"device::\0"))
    # 2) 发 AUTH TOKEN（20 字节随机）
    token = bytes(range(20))  # 用确定性 token 便于复现
    conn.sendall(msg(A_AUTH, TOKEN, 0, token))
    print("[fake_adbd] sent CNXN + AUTH TOKEN")

    local_id = None
    remote_id = None
    authorized = False
    sent_reauth = False
    while True:
        cmd, a1, a2, data = recv_msg(conn)
        if cmd == A_CNXN:
            # 车机也发了 CNXN（无害），忽略
            continue
        elif cmd == A_AUTH:
            if a1 == SIGNATURE:
                if not verify_sig(token, data):
                    print("[fake_adbd] 验签失败 → 发新 TOKEN 触发重签")
                    token = bytes((i*7+3) & 0xFF for i in range(20))
                    conn.sendall(msg(A_AUTH, TOKEN, 0, token))
                    continue
                # 验签通过
                if not authorized:
                    authorized = True
                    # 模拟“用户允许”：第一次通过后不直接 OKAY，而是再发一次 TOKEN 让车机走重签分支
                    if not sent_reauth:
                        sent_reauth = True
                        token = bytes((i*13+5) & 0xFF for i in range(20))
                        conn.sendall(msg(A_AUTH, TOKEN, 0, token))
                        print("[fake_adbd] 首次验签通过，模拟待授权 → 发新 TOKEN 触发重签")
                        continue
                print("[fake_adbd] 验签通过（已授权）")
            elif a1 == RSAPUBKEY:
                #  enrollment：记录公钥（此处仅打印长度）
                print(f"[fake_adbd] 收到 RSAPUBLICKEY ({len(data)} 字节)，登记公钥")
                continue
            else:
                continue
        elif cmd == A_OPEN:
            local_id = a1                     # 车机为 OPEN 指定的本地 id
            remote_id = (a2 if a2 else 0x02000000)
            if not authorized:
                print("[fake_adbd] 收到 OPEN 但尚未授权，忽略（车机应重发）")
                continue
            conn.sendall(msg(A_OKAY, local_id, ADB_MAX_DATA))
            print(f"[fake_adbd] OPEN 已确认, remote_id=0x{remote_id:08X}, 进入流阶段")
            break
        else:
            print(f"[fake_adbd] 流前收到未预期 cmd=0x{cmd:08X}")
            break

    # ---- 流阶段：车机→我们 的数据回显；我们主动下发大 payload ----
    # 先下发 300KB 大包（分块 <= ADB_MAX_DATA 发送，验证车机端重组）
    banner = bytearray()
    banner += b"ADB_ECHO_BANNER_"
    while len(banner) < 300 * 1024:
        banner += bytes(range(256))
    banner = bytes(banner[:300 * 1024])
    print(f"[fake_adbd] 下发 {len(banner)} 字节 banner（分块）")

    # ---- 流阶段：全双工、零丢失 ----
    # 车机端（adb.cpp 读泵）对收到的每个 WRTE 立即回 OKAY，且可能手持窗口额度对上行数据
    # 做 pipeline。因此本仿真器在“下发 banner”与“回显”两段都必须能在等待对端 OKAY 的
    # 同时，把对端并发发来的 WRTE 载荷入队、绝不可丢弃——否则回显会少字节、车机 read()
    # 永久阻塞（早前版本用裸 conn.recv(24) 吞掉 WRTE 头，正是这个 bug）。
    # `pending` 按到达顺序保存车机上行、尚未回显的字节。
    pending = bytearray()
    echoed_total = 0
    def on_car_wrte(data):
        pending.extend(data)
        conn.sendall(msg(A_OKAY, local_id, ADB_MAX_DATA))

    off = 0
    while off < len(banner):
        chunk = banner[off:off+ADB_MAX_DATA]
        conn.sendall(msg(A_WRTE, local_id, 0, chunk))
        off += len(chunk)
        # 收对端消息：OKAY=确认我们的 WRTE；WRTE=车机在 banner 期间就发来的上行（入队不丢）
        try:
            m = recv_msg(conn)
        except EOFError:
            print("[fake_adbd] 车机断开"); return
        if not m:
            print("[fake_adbd] banner 期收到坏消息"); return
        if m[0] == A_OKAY:
            pass
        elif m[0] == A_WRTE:
            on_car_wrte(m[3])
        elif m[0] == A_CLSE:
            print("[fake_adbd] 收到 CLSE"); return
    print("[fake_adbd] banner 下发完毕，开始 echo 车机上行数据")

    while True:
        # 先把已入队的回显发完：每段发完等车机 OKAY，期间仍接收车机新上行（不丢弃）
        while pending:
            chunk = bytes(pending[:ADB_MAX_DATA])
            del pending[:len(chunk)]
            conn.sendall(msg(A_WRTE, local_id, 0, chunk))
            echoed_total += len(chunk)
            try:
                m = recv_msg(conn)
            except EOFError:
                print("[fake_adbd] 车机断开"); break
            if not m: break
            if m[0] == A_OKAY:
                continue
            elif m[0] == A_WRTE:
                on_car_wrte(m[3])
            elif m[0] == A_CLSE:
                print("[fake_adbd] 收到 CLSE"); break
        if not pending:
            # 队列空：阻塞等车机下一个消息
            try:
                m = recv_msg(conn)
            except EOFError:
                print("[fake_adbd] 车机断开"); break
            if not m: break
            if m[0] == A_WRTE:
                on_car_wrte(m[3])
            elif m[0] == A_OKAY:
                continue
            elif m[0] == A_CLSE:
                print("[fake_adbd] 收到 CLSE，关闭"); break
    print(f"[fake_adbd] echo 上行共 {echoed_total} 字节，退出")

if __name__ == "__main__":
    main()
