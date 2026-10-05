#!/usr/bin/env python3
"""把 ADB 鉴权用的 RSA 私钥转成 WinCE 端可直接编译的 adbkey.h。

背景：手机 adbd 要求 ADB 客户端完成 AUTH 鉴权（证明持有某个已授权的 RSA 私钥），
车机(WinCE)端不支持生成密钥，故在开发机上预生成、把参数以常量形式编进 exe。

产出的最小值：
  - CRT 私钥参数 p/q/dp/dq/qinv（用于 RSA 签名时的中国剩余定理加速）
  - mincrypt 格式的公钥 blob（ADB 的 RSAPublicKey：size/n0inv/n/rr/exponent），
    用于 AUTH 时向 adbd 提交公钥触发"允许 USB 调试"弹窗。
    n0inv 与 rr 在开发期算好，避免车机端为一次性握手去做 4096 位模幂。

私钥文件不入库（见 .gitignore），任何人拿到自己的钥可用本脚本重新生成头文件。

用法：
    python3 scripts/gen_adbkey.py [私钥.pem] [输出.h]
"""
import re
import subprocess
import sys


def parse_key(path):
    """调 openssl 输出私钥参数文本，解析为 {字段名: int}。"""
    out = subprocess.run(
        ["openssl", "rsa", "-in", path, "-noout", "-text"],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        sys.exit(f"openssl 解析失败: {out.stderr.strip()}")
    blocks, cur = {}, None
    for line in out.stdout.splitlines():
        m = re.match(r"^(\s*)(\w[\w-]*):\s*(.*)$", line)
        if m and m.group(1) == "":
            cur = m.group(2)
            blocks[cur] = [m.group(3).strip()] if m.group(3).strip() else []
        elif cur is not None and line.strip():
            blocks.setdefault(cur, []).append(line.strip())

    def as_int(name):
        if name not in blocks:
            sys.exit(f"私钥缺少字段 {name}")
        raw = "".join(blocks[name]).strip()
        if ":" in raw:
            # 多行形式：每行都是 xx:xx:... 的十六进制分组
            hexpart = raw.replace(":", "").replace(" ", "").split("(")[0]
            return int(hexpart, 16)
        # 单行形式：openssl 给的是十进制，可能附 "(0x10001)" 之类的括号注释
        return int(raw.split("(")[0].strip(), 10)

    return {
        "n": as_int("modulus"),
        "e": as_int("publicExponent"),
        "p": as_int("prime1"),
        "q": as_int("prime2"),
        "dp": as_int("exponent1"),
        "dq": as_int("exponent2"),
        "qinv": as_int("coefficient"),
    }


def be_bytes(value, nbytes):
    """整数 → 固定长度大端字节串。"""
    return value.to_bytes(nbytes, "big")


def le_words(value, nwords):
    """整数 → nwords 个 32 位小端字的字节串（mincrypt 内部表示）。"""
    return b"".join(
        ((value >> (32 * i)) & 0xFFFFFFFF).to_bytes(4, "little")
        for i in range(nwords)
    )


def inv_mod_pow2(a, bits=32):
    """a 关于 2^bits 的乘法逆元（a 必须奇数）。牛顿迭代，每轮精度翻倍。"""
    x = 1                      # a*1 ≡ 1 (mod 2) 起步
    for _ in range(bits.bit_length()):
        x = (x * (2 - a * x)) & 0xFFFFFFFF
    return x & 0xFFFFFFFF


def barrett_mu(m, k):
    """Barrett 约减参数 mu = floor(2^(64*k) / m)，按 k+1 个 32 位小端字返回。

    在开发期算好并编进 exe：车机端做模乘只需「移位 + 乘法 + 相减」，
    无需实现多精度除法(Knuth D)，代码量与出错概率都大幅下降。
    """
    return le_words((1 << (64 * k)) // m, k + 1)


def public_blob(n, e):
    """构造 ADB(mincrypt) 公钥：size, n0inv, n[size], rr[size], exponent。"""
    words = n.bit_length() // 32
    if n.bit_length() % 32:
        words += 1
    low_word = n & 0xFFFFFFFF          # 最低有效 word，即大端表示的最后 4 字节
    n0inv = (-inv_mod_pow2(low_word)) & 0xFFFFFFFF
    # rr = R^2 mod n，R = 2^(32*words)，故 rr = 2^(64*words) mod n
    rr = pow(2, 64 * words, n)
    blob = (
        words.to_bytes(4, "little")
        + n0inv.to_bytes(4, "little")
        + le_words(n, words)
        + le_words(rr, words)
        + e.to_bytes(4, "little")
    )
    return blob, words


def c_array(name, data):
    """把一个字节串输出成 C 静态数组（每行 12 字节，便于 diff）。"""
    lines = [f"static const unsigned char {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 12]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    key_path = sys.argv[1] if len(sys.argv) > 1 else "keys/tuptup_adb.key"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "wince/src/adbkey.h"

    k = parse_key(key_path)
    bits = k["n"].bit_length()
    if bits != 2048:
        sys.exit(f"仅支持 2048 位密钥（当前 {bits} 位）：更短的密钥 adbd 会拒绝")
    if k["e"] != 65537:
        sys.exit(f"期望公钥指数 65537，实际 {k['e']}")

    blob, words = public_blob(k["n"], k["e"])

    body = []
    body.append("// 自动生成，请勿手改 —— 由 scripts/gen_adbkey.py 从私钥生成")
    body.append("// 作用：车机端作为 ADB 客户端完成 adbd 的 AUTH 鉴权（见 adb.cpp）")
    body.append("#pragma once")
    body.append("")
    body.append(f"#define ADB_RSA_BITS   {bits}")
    body.append(f"#define ADB_RSA_WORDS  {words}")
    body.append(f"#define ADB_RSA_E      {k['e']}")
    body.append("")
    # CRT 参数：p/q 为素因子，dp=d mod (p-1)，dq=d mod (q-1)，qinv=q^-1 mod p
    for cname, field, nb in (
        ("ADB_RSA_P", "p", 128),
        ("ADB_RSA_Q", "q", 128),
        ("ADB_RSA_DP", "dp", 128),
        ("ADB_RSA_DQ", "dq", 128),
        ("ADB_RSA_QINV", "qinv", 128),
    ):
        body.append(c_array(cname, be_bytes(k[field], nb)))
        body.append("")
    body.append("// Barrett 约减参数（分别针对模 p 与模 q，各 k+1 个 32 位小端字）")
    # p/q 是 1024 位 → 各 32 个 word（素数位数 = 模 n 的一半）
    for cname, field in (("ADB_RSA_MU_P", "p"), ("ADB_RSA_MU_Q", "q")):
        body.append(c_array(cname, barrett_mu(k[field], words // 2)))
        body.append("")
    body.append("// mincrypt 公钥结构：word count / n0inv / n / rr / exponent")
    body.append(c_array("ADB_RSA_PUB", blob))
    body.append("")
    body.append("#define ADB_RSA_PUB_LEN  (int)sizeof(ADB_RSA_PUB)")
    body.append("")

    with open(out_path, "w") as f:
        f.write("\n".join(body))
    print(f"已生成 {out_path}（{bits} 位密钥，公钥 blob {len(blob)} 字节）")


if __name__ == "__main__":
    main()
