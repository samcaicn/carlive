#!/usr/bin/env python3
"""用真实公钥反向验证 scripts/rsa_selftest.cpp 产出的签名。

为什么必须验证：车机端签名逻辑一旦有偏差（字节序、填充长度、CRT 组合），
上车后 adbd 只会沉默地拒绝握手，排查成本极高。这里在 PC 上做等价把他们看见了：

    EM = sig^e mod n   应精确等于  00 01 FF..FF 00 || DigestInfo || SHA1(token)

用法：
    cc -std=c++11 -I wince/src scripts/rsa_selftest.cpp wince/src/rsa.cpp -o /tmp/rsa_selftest
    /tmp/rsa_selftest > /tmp/rsa_selftest.out
    python3 scripts/verify_rsa_selftest.py /tmp/rsa_selftest.out [私钥.pem]

关于公钥来源（重要）：
    私钥 PEM 是刻意不入库的，因此纯 clone 后没有 key 文件就无法验签。
    这里优先用私钥，缺失时自动回退到【从已入库的 wince/src/adbkey.h 里
    解析 ADB_RSA_PUB 得到 N/E】——mincrypt blob 布局见 public_blob()：
        words(4,LE) | n0inv(4,LE) | n[words](小端 word 数组) |
        rr[words](小端 word 数组) | e(4,LE)
    这样 CI 与任何 checkout 都能复现验证，不必依赖密钥文件。
"""
import hashlib
import os
import re
import sys

from gen_adbkey import parse_key

SHA1_DIGESTINFO = bytes.fromhex("3021300906052b0e03021a05000414")
HEADER_CANDIDATES = ("wince/src/adbkey.h", "../wince/src/adbkey.h")


def _int_from_le_words(raw):
    """把若干 32 位小端 word 拼成整数（mincrypt 里 n/rr 的排布）。"""
    return sum(w << (32 * i) for i, w in enumerate(
        int.from_bytes(raw[j:j + 4], "little") for j in range(0, len(raw), 4)
    ))


def public_key_from_header():
    """从已入库的 adbkey.h 解出 {'n', 'e'}；找不到或格式不符返回 None。"""
    here = os.path.dirname(os.path.abspath(__file__))
    for rel in HEADER_CANDIDATES:
        path = os.path.join(here, rel)
        if not os.path.exists(path):
            continue
        src = open(path, encoding="utf-8").read()
        m = re.search(
            r"ADB_RSA_PUB\s*\[\s*\d+\s*\]\s*=\s*\{(.*?)\};", src, re.S)
        if not m:
            continue
        blob = bytes(int(b, 16) for b in
                     re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
        if len(blob) < 12:
            continue
        words = int.from_bytes(blob[0:4], "little")
        n_off, rr_off = 8, 8 + 4 * words
        if len(blob) < rr_off + 4 * words + 4:
            continue
        return {
            "n": _int_from_le_words(blob[n_off:n_off + 4 * words]),
            "e": int.from_bytes(blob[rr_off + 4 * words:][:4], "little"),
        }
    return None


def load_public_key(key_path):
    """优先私钥文件，缺失时回退到 adbkey.h。返回 (dict, 来源说明)。"""
    if key_path and os.path.exists(key_path):
        return parse_key(key_path), f"私钥 {key_path}"
    pub = public_key_from_header()
    if pub:
        return pub, "wince/src/adbkey.h 中的公钥 blob（私钥 PEM 未入库，已回退）"
    sys.exit("找不到私钥 PEM，也无法从 adbkey.h 解析公钥，无法验签")


def _vectors(fields):
    """取出全部 (token, sig) 向量；兼容旧的单一 TOKEN=/SIG= 输出格式。"""
    out = []
    for idx in sorted(int(k[len("TOKEN"):]) for k in fields if k.startswith("TOKEN") and k[len("TOKEN"):].isdigit()):
        out.append((idx, fields[f"TOKEN{idx}"], fields[f"SIG{idx}"]))
    # 旧格式兜底
    if not out and "TOKEN" in fields and "SIG" in fields:
        out.append((0, fields["TOKEN"], fields["SIG"]))
    return out


def _locate_mismatch(em, expect, padlen):
    """逐段定位差异，返回一行人类可读的原因。"""
    if em[0:2] != expect[0:2]:
        return "→ 帧头(00 01)错：通常是签名整体算错或字节序反了"
    if em[2:2 + padlen].count(0xFF) != padlen:
        return "→ FF 填充异常：可能是模幂结果大于 n，或约减出错"
    if em[2 + padlen] != 0x00:
        return "→ 分隔字节不是 00：填充长度算错"
    if em[3 + padlen:3 + padlen + 15] != SHA1_DIGESTINFO:
        return "→ DigestInfo 前缀不符：SHA1 的 DER 前缀写错了"
    return f"→ DigestInfo 正确但摘要不符：SHA1 实现有误" \
           f"（得到 {em[3 + padlen + 15:].hex()[:16]}…）"


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/rsa_selftest.out"
    key_path = sys.argv[2] if len(sys.argv) > 2 else "keys/tuptup_adb.key"

    fields = {}
    for line in open(out_path):
        line = line.strip()
        if "=" in line:
            k, v = line.split("=", 1)
            fields[k] = v

    vectors = _vectors(fields)
    if not vectors:
        sys.exit(f"{out_path} 中没有可验证的 TOKEN/SIG 向量")
    k, source = load_public_key(key_path)

    bad = []
    for idx, tok_hex, sig_hex in vectors:
        token = bytes.fromhex(tok_hex)
        sig = bytes.fromhex(sig_hex)
        if len(sig) != 256:
            bad.append((idx, f"签名长度应为 256，实际 {len(sig)}"))
            continue
        recovered_int = pow(int.from_bytes(sig, "big"), k["e"], k["n"])
        em = recovered_int.to_bytes(256, "big")
        padlen = 256 - 3 - 35
        expect = (
            b"\x00\x01" + b"\xff" * padlen + b"\x00"
            + SHA1_DIGESTINFO + hashlib.sha1(token).digest()
        )
        if em != expect:
            bad.append((idx, _locate_mismatch(em, expect, padlen)))

    if bad:
        print(f"✗ {len(bad)}/{len(vectors)} 条向量验证失败")
        for idx, reason in bad[:5]:
            print(f"  #{idx}: {reason}")
            print(f"     token = {fields[f'TOKEN{idx}']}")
        print(f"  验证向量 {len(vectors)} 条，公钥来源 = {source}")
        sys.exit(1)

    print(f"✓ {len(vectors)}/{len(vectors)} 条向量全部通过："
          f"EM = 00 01 FF.. 00 || DigestInfo || SHA1(token)")
    if "TOKEN0" in fields:
        print(f"  样例摘要 = {hashlib.sha1(bytes.fromhex(fields['TOKEN0'])).hexdigest()}")
    print(f"  公钥来源 = {source}")


if __name__ == "__main__":
    main()
