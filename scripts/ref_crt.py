#!/usr/bin/env python3
"""用 Python 大整数参照实现同样的 RSA-CRT 签名，与 C 输出逐字节比对。
用法：ref_crt.py <sig_hex> <priv.pem>
"""
import sys, subprocess, re

def parse_key(path):
    out = subprocess.run(["openssl","rsa","-in",path,"-noout","-text"],capture_output=True,text=True)
    blocks, cur = {}, None
    for line in out.stdout.splitlines():
        m = re.match(r"^(\s*)(\w[\w-]*):\s*(.*)$", line)
        if m and m.group(1) == "":
            cur = m.group(2); blocks[cur]=[m.group(3).strip()] if m.group(3).strip() else []
        elif cur is not None and line.strip():
            blocks.setdefault(cur,[]).append(line.strip())
    def ai(n):
        raw="".join(blocks[n]).strip()
        return int(raw.replace(":"," ").split("(")[0].replace(" ",""),16) if ":" in raw else int(raw.split("(")[0].strip(),10)
    return {k:ai(k) for k in ("modulus","publicExponent","prime1","prime2","exponent1","exponent2","coefficient")}

def pkcs1_v15_em(token, nbits=2048):
    di = bytes([0x30,0x21,0x30,0x09,0x06,0x05,0x2B,0x0E,0x03,0x02,0x1A,0x05,0x00,0x04,0x14])
    h = __import__("hashlib").sha1(token).digest()
    nb=nbits//8; padlen=nb-3-len(di)-len(h)
    em=bytes([0])+bytes([1])+b"\xff"*padlen+bytes([0])+di+h
    assert len(em)==nb
    return int.from_bytes(em,"big")

def main():
    sig_hex = sys.argv[1]; key=parse_key(sys.argv[2])
    n,e,p,q,dp,dq,qinv = key["modulus"],key["publicExponent"],key["prime1"],key["prime2"],key["exponent1"],key["exponent2"],key["coefficient"]
    assert qinv == pow(q, -1, p), "qinv 定义"
    m = pkcs1_v15_em(bytes.fromhex("030a11181f262d343b424950575e656c737a8188"))
    # CRT
    m1=m%p; m2=m%q
    s1=pow(m1,dp,p); s2=pow(m2,dq,q)
    h=(qinv*(s1-s2))%p
    s=(m2+h*q)%n
    ref=s.to_bytes(256,"big").hex()
    print("REF="+ref)
    print("GOT="+sig_hex)
    print("MATCH" if ref==sig_hex else "MISMATCH")

if __name__=="__main__":
    main()
