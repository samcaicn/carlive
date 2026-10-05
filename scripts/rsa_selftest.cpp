// rsa_selftest.cpp - 宿主机自检（不编进车机 exe）
//
// 这段代码的价值：签名算法一旦带上车，出问题只能靠 adbd 的沉默式拒绝来猜，
// 排查成本极高。这里把它拉到 PC 上编译并用真实公钥反解验证：
//   EM = sig^e mod n，应当等于 "00 01 FF..FF 00 || DigestInfo || SHA1(token)"
//
// 为什么默认签 200 条向量而不是 1 条：约减（Barrett）的误差修正是有条件分支的，
// 单个样本碰巧落进“不需要修正”的区间就会蒙混过关。历史上这类 bug 的典型形态
// 就是「大部分 token 都对、某些值悄悄算错」，上车后表现为手机偶发拒绝握手。
//
// 用法（自动比对见 scripts/verify_rsa_selftest.py）：
//   cc -std=c++11 -I wince/src scripts/rsa_selftest.cpp wince/src/rsa.cpp -o /tmp/rsa_selftest
//   /tmp/rsa_selftest [向量条数] > /tmp/rsa_selftest.out
#include "rsa.h"
#include <stdio.h>
#include <stdlib.h>

// xorshift32：确定性伪随机，不依赖平台 rand() 实现，失败样本可原样复现。
static unsigned long s_randState = 0x12345678u;
static unsigned nextRand() {
    s_randState ^= (s_randState << 13); s_randState &= 0xFFFFFFFFu;
    s_randState ^= (s_randState >> 17);
    s_randState ^= (s_randState << 5);  s_randState &= 0xFFFFFFFFu;
    return (unsigned)s_randState;
}

int main(int argc, char** argv) {
    int vectors = (argc > 1) ? atoi(argv[1]) : 200;
    if (vectors < 1) vectors = 1;

    int failed = 0;
    for (int v = 0; v < vectors; v++) {
        unsigned char token[20];
        // 前 4 条用规律性取值便于肉眼核对，其余随机
        if      (v == 0) { for (int i = 0; i < 20; i++) token[i] = (unsigned char)(i * 7 + 3); }
        else if (v == 1) { for (int i = 0; i < 20; i++) token[i] = 0x00; }
        else if (v == 2) { for (int i = 0; i < 20; i++) token[i] = 0xFF; }
        else if (v == 3) { for (int i = 0; i < 20; i++) token[i] = 0x80; }
        else {
            for (int i = 0; i < 20; i++)
                token[i] = (unsigned char)((nextRand() >> ((i % 4) * 8)) & 0xFF);
        }

        unsigned char sig[256];
        for (int i = 0; i < 256; i++) sig[i] = 0;
        if (!RsaSignToken(token, sig)) { printf("SIGN_FAILED%d=\n", v); failed++; continue; }

        printf("TOKEN%d=", v);
        for (int i = 0; i < 20; i++) printf("%02x", token[i]);
        printf("\nSIG%d=", v);
        for (int i = 0; i < 256; i++) printf("%02x", sig[i]);
        printf("\n");
    }

    printf("COUNT=%d\n", vectors - failed);
    printf("SIGLEN=%d\n", RsaSignatureSize());

    int publen = 0;
    const unsigned char* blob = RsaPublicBlob(&publen);
    printf("PUBLEN=%d\n", publen);
    if (blob && publen >= 4) {
        // mincrypt 头是 size(word 数)，应为 64
        int words = (int)blob[0] | ((int)blob[1] << 8) | ((int)blob[2] << 16) | ((int)blob[3] << 24);
        printf("PUB_WORDS=%d\n", words);
    }
    return failed ? 1 : 0;
}
