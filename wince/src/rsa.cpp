// rsa.cpp - 见 rsa.h
#include "rsa.h"
#include "adbkey.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;

#define NW   32           // CRT 素数对应的字数：1024 bit / 32 = 32
#define NWX  (NW + 1)     // Barrett 参数字数（k+1）
#define NW2  (NW * 2)     // 2048 位字的宽度
#define NB   256          // 签名/模长字节数

// ---------------------------------------------------------------- SHA1 ----
static inline u32 rotl(u32 x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1Block(u32 h[5], const unsigned char p[64]) {
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i*4] << 24) | ((u32)p[i*4+1] << 16) | ((u32)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                      k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);    k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                      k = 0xCA62C1D6u; }
        u32 t = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const unsigned char* data, size_t len, unsigned char out[20]) {
    u32 h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
    unsigned char blk[64];
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha1Block(h, data + i);
    size_t rem = len - i;
    memcpy(blk, data + i, rem);
    blk[rem] = 0x80;
    if (rem >= 56) {                       // 放不下长度字段 → 先补满一整块
        memset(blk + rem + 1, 0, 64 - (rem + 1));
        sha1Block(h, blk);
        memset(blk, 0, 64);
    } else {
        memset(blk + rem + 1, 0, 56 - (rem + 1));
    }
    u64 bits = (u64)len * 8;
    for (int j = 0; j < 8; j++) blk[63 - j] = (unsigned char)(bits >> (8 * j));
    sha1Block(h, blk);
    for (int j = 0; j < 5; j++) {
        out[j*4+0] = (unsigned char)(h[j] >> 24);
        out[j*4+1] = (unsigned char)(h[j] >> 16);
        out[j*4+2] = (unsigned char)(h[j] >> 8);
        out[j*4+3] = (unsigned char)h[j];
    }
}

// ------------------------------------------------------ 多精度辅助 ----
// 内部一律用【小端字】(least significant word first)。
static void zero(u32* a, int n)                       { memset(a, 0, sizeof(u32) * (size_t)n); }
static void copyw(u32* d, const u32* s, int n)        { memcpy(d, s, sizeof(u32) * (size_t)n); }

// 数值比较（从最高位字开始）。⚠ 绝不能用 memcmp：内存里是小端字（低位字节在前），
// memcmp 会从最低位开始比，对多精度整数没有数值序意义。
static int cmpw(const u32* a, const u32* b, int n) {
    for (int i = n - 1; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

// 大端字节串 → 小端字；nw 为字数，be 长度必须是 nw*4
static void be2w(u32* w, int nw, const unsigned char* be) {
    for (int i = 0; i < nw; i++) {
        const unsigned char* p = be + (nw - 1 - i) * 4;
        w[i] = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    }
}
// 小端字形式的字节串 → 小端字（用于直接载入已按 word 排好序的常量，如 mu）
static void le2w(u32* w, int nw, const unsigned char* le) {
    for (int i = 0; i < nw; i++) {
        const unsigned char* p = le + i * 4;
        w[i] = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    }
}
static void w2be(const u32* w, int nw, unsigned char* be) {
    for (int i = 0; i < nw; i++) {
        u32 v = w[i];
        unsigned char* p = be + (nw - 1 - i) * 4;
        p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
        p[2] = (unsigned char)(v >> 8);  p[3] = (unsigned char)v;
    }
}

// r = a - b，返回借位
static u32 subw(u32* r, const u32* a, const u32* b, int n) {
    u64 borrow = 0;
    for (int i = 0; i < n; i++) {
        u64 cur = (u64)a[i] - b[i] - borrow;
        r[i] = (u32)cur;
        borrow = (cur >> 63) ? 1 : 0;      // 借位是 0 或 1（64 位表示下的负数）
    }
    return (u32)borrow;
}

// 把 n 字的 in 加到 dst（2n 字）的低位，进位向上传播
static void addInto(u32* dst, int dstN, const u32* in, int inN) {
    u64 carry = 0;
    int i = 0;
    for (; i < inN; i++) {
        u64 cur = (u64)dst[i] + in[i] + carry;
        dst[i] = (u32)cur;
        carry = cur >> 32;
    }
    for (; i < dstN && carry; i++) {
        u64 cur = (u64)dst[i] + carry;
        dst[i] = (u32)cur;
        carry = cur >> 32;
    }
}

// r = a * b（r 必须容纳 la+lb 个字，函数内部先清零）
static void mulw(u32* r, const u32* a, int la, const u32* b, int lb) {
    zero(r, la + lb);
    for (int i = 0; i < la; i++) {
        u64 carry = 0;
        if (!a[i]) continue;
        for (int j = 0; j < lb; j++) {
            u64 cur = (u64)r[i + j] + (u64)a[i] * b[j] + carry;
            r[i + j] = (u32)cur;
            carry = cur >> 32;
        }
        int k = i + lb;
        while (carry) { u64 cur = (u64)r[k] + carry; r[k] = (u32)cur; carry = cur >> 32; k++; }
    }
}

// --------------------------------------------------- Barrett 约减 ----
// r = x mod m。x 长 xl 字（<= 2*NW），m 长 NW 字，mu 长 NWX 字（生成期预计算）。
// 参考 HAC 算法 14.42：q1=x>>(k-1) → q2=q1*mu → q3=q2>>(k+1) → r = x - q3*m*b^(k+1)。
// 理论保证 |x - q3*m*b^(k+1)| < 2m < b^(k+1)，因此做【全宽带符号减法】后把结果
// 直接归一化到 [0, m)，完全规避「截断 mod b^(k+1) 后负号丢失」的老 bug。
static void barrettRed(const u32* x, int xl, const u32* m, const u32* mu, u32* r) {
    const int k = NW;
    // q1 = floor(x / b^(k-1))：丢掉低 k-1 个字
    u32 q1[NWX]; zero(q1, NWX);
    int q1l = xl - (k - 1);
    if (q1l > NWX) q1l = NWX;
    if (q1l < 0) q1l = 0;
    for (int i = 0; i < q1l; i++) q1[i] = x[i + (k - 1)];

    // q2 = q1 * mu  （NWX * NWX -> 2*NWX 字）
    u32 q2[2 * NWX];
    mulw(q2, q1, NWX, mu, NWX);

    // q3 = floor(q2 / b^(k+1)) = q2 >> (k+1) 个字
    u32 q3[NWX]; zero(q3, NWX);
    for (int i = 0; i + (k + 1) < 2 * NWX; i++) q3[i] = q2[i + (k + 1)];

    // prod = q3 * m  （NWX * NW -> 至多 NWX+NW 字）。
    // HAC 14.42 的还原量是 r = x - q3*m（绝不能再额外左移 b^(k+1)——
    // 之前多移一次导致结果恒为负、归一化失效，所有输入吐同一常数）。
    u32 prod[NWX + NW]; zero(prod, NWX + NW);
    mulw(prod, q3, NWX, m, NW);

    // rr = x - prod，全宽带符号减法。HAC 保证 |x - q3*m| < 2m。
    int maxi = (xl > (NWX + NW)) ? xl : (NWX + NW);
    u32 rr[NW2 + 2]; zero(rr, NW2 + 2);
    for (int i = 0; i < xl; i++) rr[i] = x[i];
    u64 borrow = 0;
    for (int i = 0; i <= maxi; i++) {
        u64 a = rr[i];
        u64 b = (i < (NWX + NW)) ? prod[i] : 0;
        u64 cur = a - b - borrow;
        rr[i] = (u32)cur;
        borrow = (cur >> 63) & 1u;
    }
    // 归一化到 [0, m)：|val| < 2m。rr 是 (NW2+2) 字 2s 补码，符号看最高字最高位。
    // 若为负：全宽加 m（进位必须一路传播到高位以清掉符号位），直到非负。
    for (int c = 0; c < 4; c++) {
        if (rr[NW2 + 1] >> 31) {                 // 仍为负
            u64 carry = 0;
            for (int i = 0; i < NW2 + 2; i++) {
                u64 cur = (u64)rr[i] + (i < NW ? m[i] : 0) + carry;
                rr[i] = (u32)cur;
                carry = cur >> 32;
            }
        } else break;
    }
    // 现在 rr >= 0 且 < 2m。注意 rr 的最高有效字可能是第 NW 字（rr[NW]，值为 0 或 1，
    // 因为 < 2m < 2*b^32）；比较 / 减法必须含第 NW 字，否则 1025 位的 val 会被误判为 < m。
    for (int c = 0; c < 3; c++) {
        int cmp = 0;
        for (int i = NW; i >= 0; i--) {
            u32 a = rr[i], b = (i < NW ? m[i] : 0);
            if (a > b) { cmp = 1; break; }
            if (a < b) { cmp = -1; break; }
        }
        if (cmp < 0) break;
        u64 b2 = 0;
        for (int i = 0; i <= NW; i++) {
            u64 cur = (u64)rr[i] - (i < NW ? m[i] : 0) - b2;
            rr[i] = (u32)cur;
            b2 = (cur >> 63) & 1u;
        }
        if (b2) {  // 减过头（理论不会发生）→ 加回并停
            u64 c2 = 0;
            for (int i = 0; i <= NW; i++) {
                u64 cur = (u64)rr[i] + (i < NW ? m[i] : 0) + c2;
                rr[i] = (u32)cur;
                c2 = cur >> 32;
            }
            break;
        }
    }
    for (int i = 0; i < NW; i++) r[i] = rr[i];
}

// out = (a * b) mod m
static void modMul(u32* out, const u32* a, const u32* b, const u32* m, const u32* mu) {
    u32 t[NW2];
    mulw(t, a, NW, b, NW);
    barrettRed(t, NW2, m, mu, out);
}

// out = base^exp mod m，exp 为大端字节串
static void modPow(u32* out, const u32* base, const unsigned char* exp, int expLen,
                   const u32* m, const u32* mu) {
    u32 r[NW]; zero(r, NW); r[0] = 1;
    for (int i = 0; i < expLen; i++) {
        unsigned char byte = exp[i];
        for (int bit = 7; bit >= 0; bit--) {
            modMul(r, r, r, m, mu);                              // 平方
            if (byte & (unsigned char)(1u << bit)) modMul(r, r, base, m, mu);
        }
    }
    copyw(out, r, NW);
}

// ------------------------------------------------------------ 签名 ----
static const unsigned char SHA1_DIGESTINFO[15] = {
    0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2B, 0x0E,
    0x03, 0x02, 0x1A, 0x05, 0x00, 0x04, 0x14
};

bool RsaSignToken(const unsigned char token[20], unsigned char sig[256]) {
    // 1) EM = 0x00 || 0x01 || FF... || 0x00 || DigestInfo || H
    static unsigned char em[NB];
    unsigned char h[20];
    sha1(token, 20, h);
    int diLen = 15 + 20;                        // DigestInfo 总长 35
    int padLen = NB - 3 - diLen;                // FF 个数 = 256-3-35 = 218
    if (padLen < 8) return false;
    em[0] = 0x00; em[1] = 0x01;
    memset(em + 2, 0xFF, (size_t)padLen);
    em[2 + padLen] = 0x00;
    memcpy(em + 3 + padLen, SHA1_DIGESTINFO, 15);
    memcpy(em + 3 + padLen + 15, h, 20);

    // 2) CRT：m = m2 + q * (qinv * (m1 - m2) mod p)
    u32 c[NW2];  be2w(c, NW2, em);
    u32 p[NW], q[NW], dp[NW], dq[NW], qinv[NW];
    u32 muP[NWX], muQ[NWX];
    be2w(p, NW, ADB_RSA_P);   be2w(q, NW, ADB_RSA_Q);
    be2w(dp, NW, ADB_RSA_DP); be2w(dq, NW, ADB_RSA_DQ);
    be2w(qinv, NW, ADB_RSA_QINV);
    le2w(muP, NWX, ADB_RSA_MU_P); le2w(muQ, NWX, ADB_RSA_MU_Q);

    u32 cp[NW], cq[NW];
    barrettRed(c, NW2, p, muP, cp);
    barrettRed(c, NW2, q, muQ, cq);

    u32 m1[NW], m2[NW];
    modPow(m1, cp, ADB_RSA_DP, 128, p, muP);
    modPow(m2, cq, ADB_RSA_DQ, 128, q, muQ);

    // diff = (m1 - m2) mod p
    u32 diff[NW];
    if (cmpw(m1, m2, NW) >= 0) {
        subw(diff, m1, m2, NW);
    } else {
        u32 tmp[NW];
        subw(tmp, m2, m1, NW);          // diff = p - (m2 - m1)
        subw(diff, p, tmp, NW);
    }
    if (cmpw(diff, p, NW) >= 0) subw(diff, diff, p, NW);

    u32 hq[NW];
    modMul(hq, qinv, diff, p, muP);     // h = qinv*(m1-m2) mod p

    u32 res[NW2];
    mulw(res, hq, NW, q, NW);           // res = h*q
    addInto(res, NW2, m2, NW);          // res = m2 + h*q  (< n)

    w2be(res, NW2, sig);
    return true;
}

const unsigned char* RsaPublicBlob(int* outLen) {
    if (outLen) *outLen = (int)sizeof(ADB_RSA_PUB);
    return ADB_RSA_PUB;
}

int RsaSignatureSize() { return NB; }

// SHA1 导出：供 adb.cpp 对 AUTH token 做摘要（与 RsaSignToken 内部用的是同一实现，避免重复）。
void RsaSha1(const unsigned char* data, size_t len, unsigned char out[20]) {
    sha1(data, len, out);
}
