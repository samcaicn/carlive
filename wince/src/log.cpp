// log.cpp - 见 log.h
// 把启动/连接里程碑与错误写入 EXE 同目录 tuptup.log，便于在车机 SD 卡上排查“启动卡死”等问题。
#include "log.h"
#include <windows.h>
#include <stdarg.h>
#include <string.h>

#ifdef __MINGW32__
#define SNPRINTF  _snprintf
#define VSNPRINTF _vsnprintf
#else
#define SNPRINTF  snprintf
#define VSNPRINTF vsnprintf
#endif

static HANDLE   g_hLog = INVALID_HANDLE_VALUE;
static DWORD    g_startTick = 0;
static CRITICAL_SECTION g_csLog;
// 日志节流：完全相同内容的相邻日志 5s 内只落盘一次（见 Log 内注释）
static char     g_lastMsg[384] = "";
static DWORD    g_lastSameTick = 0;
static DWORD    g_lastFlush = 0;

void LogInit() {
    g_startTick = GetTickCount();
    InitializeCriticalSection(&g_csLog);
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR* p = wcsrchr(path, L'\\');
        if (p) wcscpy(p + 1, L"tuptup.log");
        else wcscpy(path, L"tuptup.log");   // R15：exe 在根目录（无 \）时回退到当前目录文件名
        // 日志滚动：SD 卡空间有限，超过 256KB 时把旧日志改名为 tuptup.log.bak（仅保留一份备份），避免无限增长。
        HANDLE hSize = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hSize != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(hSize, NULL);
            CloseHandle(hSize);
            if (sz != INVALID_FILE_SIZE && sz > 256 * 1024) {
                WCHAR bak[MAX_PATH]; wcscpy(bak, path); wcscat(bak, L".bak");
                DeleteFile(bak);
                MoveFile(path, bak);
            }
        }
        g_hLog = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_hLog != INVALID_HANDLE_VALUE) {
            SetFilePointer(g_hLog, 0, NULL, FILE_END); // 追加
        }
    }
    Log("==== tuptup.top start (t=0) ====");
}

void Log(const char* fmt, ...) {
    if (g_hLog == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_csLog);

    char msg[384];
    va_list ap; va_start(ap, fmt);
    int n = VSNPRINTF(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(msg) - 1) n = (int)(sizeof(msg) - 1);
    msg[n] = 0;

    // 节流：内容完全相同的日志，5s 内只写一次。
    // 重连循环每轮会对多个候选各打一条 try/FAIL，手机没开 App 时每秒数条雷同日志持续写 SD 卡，
    // 既抢占单核 CPU 又加速闪存磨损；真正的状态变化（新 IP、连上、断开）不受影响。
    DWORD now = GetTickCount();
    if (strcmp(msg, g_lastMsg) == 0) {
        if (now - g_lastSameTick < 5000) { LeaveCriticalSection(&g_csLog); return; }
    } else {
        strncpy(g_lastMsg, msg, sizeof(g_lastMsg) - 1);
        g_lastMsg[sizeof(g_lastMsg) - 1] = 0;
    }
    g_lastSameTick = now;

    char line[448];
    DWORD t = GetTickCount() - g_startTick;
    int m = SNPRINTF(line, sizeof(line), "[t+%u] %s\r\n", t, msg);
    if (m < 0) m = 0;
    if ((size_t)m > sizeof(line) - 1) m = (int)(sizeof(line) - 1);

    DWORD wr = 0;
    WriteFile(g_hLog, line, (DWORD)m, &wr, NULL);
    // 落盘策略：前若干条（覆盖完整启动流程）+ 状态变化时立即 flush，其余按 2s 兜底。
    //
    // 为什么必须有"前 N 条立即 flush"：实测车机日志只有两行启动横幅就断了，
    // WinMain enter / client area / renderer+net / discovery / ConnThread start
    // 这些定位崩溃点的关键信息全在缓冲区里没落盘，进程一死就全丢了 ——
    // 排障时只能看到"啥也没发生"，等于没有日志。
    // 低开销做法：只在启动关键窗口（g_startTick 起 3s 内）强制刷，
    // 之后恢复 2s 节流，稳定镜像态的重复日志不会被打扰。
    DWORD age = now - g_startTick;
    if (age < 3000 || now - g_lastFlush > 2000) { FlushFileBuffers(g_hLog); g_lastFlush = now; }

    LeaveCriticalSection(&g_csLog);
}
