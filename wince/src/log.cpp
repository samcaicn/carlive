// log.cpp - 见 log.h
// 把启动/连接里程碑与错误写入 EXE 同目录 gloai.log，便于在车机 SD 卡上排查“启动卡死”等问题。
#include "log.h"
#include <stdarg.h>

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

void LogInit() {
    g_startTick = GetTickCount();
    InitializeCriticalSection(&g_csLog);
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR* p = wcsrchr(path, L'\\');
        if (p) wcscpy(p + 1, L"gloai.log");
        // 日志滚动：SD 卡空间有限，超过 256KB 时把旧日志改名为 gloai.log.bak（仅保留一份备份），避免无限增长。
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
    Log("==== GLOAI start (t=0) ====");
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

    char line[448];
    DWORD t = GetTickCount() - g_startTick;
    int m = SNPRINTF(line, sizeof(line), "[t+%u] %s\r\n", t, msg);
    if (m < 0) m = 0;
    if ((size_t)m > sizeof(line) - 1) m = (int)(sizeof(line) - 1);

    DWORD wr = 0;
    WriteFile(g_hLog, line, (DWORD)m, &wr, NULL);
    FlushFileBuffers(g_hLog);

    LeaveCriticalSection(&g_csLog);
}
