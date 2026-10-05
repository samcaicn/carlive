// log_stub.cpp - 宿主机（POSIX）测试用的 Log 实现。
//
// 车机上的真身是 wince/src/log.cpp（写 tuptup.log 到 SD 卡）；这里换成 stderr 输出，
// 让 adb.cpp 这类传输层代码能在 PC 上原样编译并跑端到端回归。
// 与 log.cpp 的差异只在“往哪写”，【函数签名必须与 log.h 完全一致】
// （LogInit 无参），这样上层无需 #ifdef 分叉。
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#define STUB_TAG "host"

static unsigned long g_t0_ms = 0;

static unsigned long nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void LogInit() {
    if (g_t0_ms == 0) g_t0_ms = nowMs();
    Log("==== host LogInit ====");
}

void Log(const char* fmt, ...) {
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    // 用 stderr 而非 stdout：无缓冲，进程死锁/被杀时也能看到最后一行。
    fprintf(stderr, "[%s t+%lums] %s\n", STUB_TAG, nowMs() - g_t0_ms, msg);
}
