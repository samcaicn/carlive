// adbtest.cpp - 主机端验证 AdbTransport（连 fake_adbd.py，跑通握手 + 分块双向流）
//
// 与 wince/src/adb.cpp 是同一份实现（仅平台宏切换为 POSIX 分支）。这里只验证“逻辑正确性”，
// 真正的 ARM WinCE 二进制由 CI（CeGCC）产出。验证点：
//   1) connect() 成功 = AUTH 签名被假 adbd 接受（用同一公钥做 sig^e mod n == EMSA 校验）。
//   2) 读完 300KB banner（假 adbd 分块下发 → 车机分块重组）内容完全一致。
//   3) 上行 100KB 被原样回显（验证车机 WRTE 分块 + 假 adbd 回 OKAY 窗口流控）。
#include "adb.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

static bool readExactly(AdbTransport& t, unsigned char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = t.read(buf + got, n - got);
        if (!r) return false;
        got += (n - got);   // read() 写满剩余才返回 true
        got = n;            // 等价：read 成功即写满
    }
    return true;
}

int main() {
    // ⚠️ 关键：stdout 在管道下是【全缓冲】的，run_host_tests.py 在 60s 用 SIGKILL 杀进程，
    // 缓冲里的最后几行（含 PASS 结论）会被直接丢弃，导致“明明跑通却被误判成挂死”。
    // 强制无缓冲，让每行 printf 立即落盘/落管道，SIGKILL 也丢不掉。
    setvbuf(stdout, NULL, _IONBF, 0);
    AdbTransport t;
    std::wstring host = L"127.0.0.1";
    if (!t.connect(host, 5555, 5000)) {
        printf("FAIL: connect (handshake) 失败\n");
        return 1;
    }
    printf("OK: 握手成功（AUTH 签名被接受，隧道已建立）\n");

    // 1) 读 300KB banner 并校验内容
    const int BANNER = 300 * 1024;
    std::vector<unsigned char> buf(BANNER);
    if (!readExactly(t, buf.data(), BANNER)) { printf("FAIL: 读 banner 失败\n"); return 1; }
    std::vector<unsigned char> expect(BANNER);
    const char* prefix = "ADB_ECHO_BANNER_";
    int plen = (int)strlen(prefix);
    memcpy(expect.data(), prefix, plen);
    int i = plen;
    while (i < BANNER) { expect[i] = (unsigned char)((i - plen) & 0xFF); i++; }
    if (memcmp(buf.data(), expect.data(), BANNER) != 0) { printf("FAIL: banner 内容不匹配\n"); return 1; }
    printf("OK: 300KB banner 重组正确（分块 WRTE → 连续流）\n");

    // 2) 上行 100KB，等待回显并校验
    const int UP = 100 * 1024;
    std::vector<unsigned char> up(UP);
    for (int k = 0; k < UP; k++) up[k] = (unsigned char)((k * 31 + 7) & 0xFF);
    if (!t.write(up.data(), UP)) { printf("FAIL: 上行 100KB 失败\n"); return 1; }
    std::vector<unsigned char> echo(UP);
    if (!readExactly(t, echo.data(), UP)) { printf("FAIL: 读回显失败\n"); return 1; }
    if (memcmp(up.data(), echo.data(), UP) != 0) { printf("FAIL: 回显内容不匹配\n"); return 1; }
    printf("OK: 100KB 上行被原样回显（车机 WRTE 分块 + 窗口流控正确）\n");

    printf("=== ADB 传输层自测 PASS ✅ ===\n");
    t.close();
    return 0;
}
