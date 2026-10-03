#!/usr/bin/env python3
"""CI 闸门：确认release APK 不是用 Android debug key 签的。

为什么需要：debug.keystore 在每个 CI runner 上都会新生成，签名随之改变，
覆盖安装会报 INSTALL_FAILED_UPDATE_INCOMPATIBLE，且构建日志里完全看不出来。

解析 APK Signing Block（magic "APK Sig Block 42"，位于EOCD 之前），
取v2/v3 签名携带的 X.509 证书可辨识名，命中 debug 特征就 exit 1。
"""

import struct
import sys
from pathlib import Path

MAGIC = b"APK Sig Block 42"

# APK Signature Scheme block IDs
V2 = 0x7109871A
V3 = 0xF05368C0
V31 = 0x1B93AD61

# 证书里出现这些片段就判定为 debug key
DEBUG_MARKERS = (
    b"Android Debug",
    b"CN=Android Debug",
    b"O=Android",
    b"androiddebugkey",
    b"CN=Android Debug,O=Android,C=US",
)


def iter_sig_blocks(data: bytes):
    """产出签名块 ID -> 该块的 value 字节。"""
    magic_at = data.rfind(MAGIC)
    if magic_at < 0:
        return
    if magic_at < 8:
        return
    size = struct.unpack_from("<Q", data, magic_at - 8)[0]
    start = magic_at + 8 - size
    if start < 0 or start +size > len(data):
        return
    block = data[start : magic_at + 8]
    if len(block) < 8 + 8 + len(MAGIC):
        return
    # 块结构：uint64 size | (uint64 pairLen, uint32 id, value) * n | uint64 size | magic
    off = 8
    end = len(block) - 8 - len(MAGIC)
    while off + 12 <= end:
        pair_len = struct.unpack_from("<Q", block, off)[0]
        if pair_len < 4 or off + 8 + pair_len > end + 8:
            break
        block_id = struct.unpack_from("<I", block, off + 8)[0]
        value = block[off + 12 : off + 8 + pair_len]
        yield block_id, value
        off += 8 + pair_len


def collect_certificates(apk: Path):
    """从 v2/v3 签名块提取全部 X.509 DER 证书字节。"""
    data = apk.read_bytes()
    if MAGIC not in data:
        return []
    certs = []
    for block_id, value in iter_sig_blocks(data):
        if block_id not in (V2, V3, V31):
            continue
        # signed data 布局：signer(s) 前置uint32 长度
        off = 0
        if len(value) < 4:
            continue
        (signers_len,) = struct.unpack_from("<I", value, 0)
        off = 4
        pos = 0
        while pos < signers_len and off + 12<= len(value):
            (signer_len,) = struct.unpack_from("<I", value, off)
            off += 4
            if signer_len <= 0 or off + signer_len > len(value):
                break
            signer = value[off : off + signer_len]
            off += signer_len
            pos += signer_len
            # signer 布局： signed data | signatures | public key
            (sd_len,) = struct.unpack_from("<I", signer, 0)
            if 4 + sd_len > len(signer):
                continue
            # 证书在 signed data 里不可直接切，需再走一层；这里用启发式：
            # 所有 DER 证书都以 0x30 0x82 <len> 开头，扫描sd 区间
            sd = signer[4 : 4 + sd_len]
            certs.extend(scan_der_certs(sd))
    return certs


def scan_der_certs(blob: bytes):
    """在字节流里扫出所有形如 30 82 xx xx 的 X.509 证书。"""
    found = []
    i = 0
    n = len(blob)
    while i + 4 <= n:
        if blob[i] == 0x30 and blob[i + 1] == 0x82:
            cert_len = struct.unpack_from(">H", blob, i + 2)[0]
            total = cert_len + 4
            if 0< total <= n - i:
                found.append(blob[i : i + total])
                i += total
                continue
        i += 1
    return found


def cert_common_name(cert: bytes) -> str:
    """从证书里粗提可辨识名（够用来判断是不是 debug key）。"""
    out = []
    i = 0
    n = len(cert)
    while i < n - 4:
        if cert[i] in (0x0C,) and cert[i + 1] == 0x03:  # UTF8String
            ln = cert[i + 2]
            if 0< ln <= 0x7F and i + 3 + ln <= n:
                out.append(cert[i + 3 : i + 3 + ln].decode("utf-8", "replace"))
                i += 3 + ln
                continue
        i += 1
    return " / ".join(out)


def certs_from_v1(apk: Path):
    """从 v1 (JAR) 签名的 META-INF/*.RSA 里提取证书。

    必须支持：只用 v1 签名的包在 Android 11+ 装不上，但更常见的是
    v1+v2+v3 全带；只看v2 块会漏掉 v1 里的那份证书。
    """
    import zipfile

    certs = []
    try:
        with zipfile.ZipFile(apk) as z:
            for name in z.namelist():
                upper = name.upper()
                if not upper.startswith("META-INF/"):
                    continue
                if not upper.endswith((".RSA", ".DSA", ".EC")):
                    continue
                try:
                    blob = z.read(name)
                except Exception:
                    continue
                certs.extend(scan_der_certs(blob))
    except zipfile.BadZipFile:
        return []
    return certs


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: verify_apk_signature.py <apk> [apk...]", file=sys.stderr)
        return 2

    failed = False
    for path_str in sys.argv[1:]:
        apk = Path(path_str)
        if not apk.is_file():
            print(f"FAIL {apk}: 文件不存在", file=sys.stderr)
            failed = True
            continue

        # v1 与 v2/v3 都要看：任一来源出现 debug 特征即判定失败
        certs = collect_certificates(apk) + certs_from_v1(apk)
        if not certs:
            print(f"FAIL {apk.name}: 完全找不到签名证书，该包未签名或签名格式异常", file=sys.stderr)
            failed = True
            continue

        print(f"--- {apk.name}: {len(certs)} 张证书")
        for cert in certs:
            # 原始 DER 里直接搜可辨识名字符串，规避严格 ASN.1 解析
            printable = bytes(b for b in cert if 0x20 <= b < 0x7F or b == 0x0A)
            if any(mark in printable for mark in DEBUG_MARKERS):
                print(f"FAIL {apk.name}: 证书是 Android debug key")
                failed = True
            else:
                cn = cert_common_name(cert)
                print(f"  OK  非debug 证书{f'（{cn}）' if cn else ''}")

    if failed:
        print(
            "\n拒绝发布：release APK仍是 debug key 签名，"
            "下次构建签名会变，覆盖安装将失败。"
            "请检查 SIGNING_KEY / SIGNING_STORE_PASSWORD secrets。",
            file=sys.stderr,
        )
        return 1

    print("\n签名校验通过：非 debug key。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
