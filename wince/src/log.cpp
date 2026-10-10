// log.cpp - 见 log.h
// 把启动/连接里程碑与错误写入 EXE 同目录 tuptup.log，便于在车机 SD 卡上排查“启动卡死”等问题。
#include "log.h"
#include "crashlog.h"
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
// R38：金丝雀 —— 紧贴 g_csLog 上方放已知常量，侦测“越界写破坏 g_csLog 邻域”。
// Log 进临界区前先校验，命中即把“被破坏发生在 [上一阶段→本阶段]”钉死并跳过 EnterCriticalSection（避免崩）。
static volatile unsigned g_logCanary[4] = {0xCAFEBABEu, 0xCAFEBABEu, 0xCAFEBABEu, 0xCAFEBABEu};
static bool LogCanaryIntact() {
    return g_logCanary[0]==0xCAFEBABEu && g_logCanary[1]==0xCAFEBABEu
        && g_logCanary[2]==0xCAFEBABEu && g_logCanary[3]==0xCAFEBABEu;
}
// 日志节流：完全相同内容的相邻日志 5s 内只落盘一次（见 Log 内注释）
static char     g_lastMsg[384] = "";
static DWORD    g_lastSameTick = 0;
static DWORD    g_lastFlush = 0;

// R27：日志文件名从 EXE 自身文件名派生 —— SD 卡上要同时放多个候选 exe 做 A/B 对比，
// 若都写死 tuptup.log，后启动的会覆盖前者的日志（甚至滚动成 .bak 丢掉），
// 排障时只能看到最后跑的那一个，对比实验直接失效。
// "\SDMEMORY2\carlive\wince\tuptup-usbnet.exe" → "...\tuptup-usbnet.log"
// 无扩展名（或点在目录分隔符左侧）时追加后缀而不是替换。
static void logPathFromModule(WCHAR* path, DWORD cap, const WCHAR* ext) {
    WCHAR* slash = wcsrchr(path, L'\\');
    WCHAR* dot   = wcsrchr(path, L'.');
    if (dot && dot > (slash ? slash : path - 1)) *dot = 0;   // 截掉原扩展名
    // 追加 ext（长度受 cap 约束，cap 按 MAX_PATH 传入足够）
    size_t used = wcslen(path), add = wcslen(ext);
    if (used + add < (size_t)cap) wcscat(path, ext);
}

void LogInit() {
    g_startTick = GetTickCount();
#ifndef TLTP_UNIFY_FILELOCK
    InitializeCriticalSection(&g_csLog);
#else
    // R39（test24）：日志与 crash.log 统一到同一把锁，消除跨锁并发写文件踩堆。
    TltpFileInit();
#endif
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        logPathFromModule(path, MAX_PATH, L".log");
    } else {
        wcscpy(path, L"tuptup.log");   // 取不到模块名时的兜底：回退到当前目录默认名
    }
    {
        // 日志滚动：SD 卡空间有限，超过 256KB 时把旧日志改名（仅保留一份备份），避免无限增长。
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
    // R38：金丝雀校验——若 g_csLog 邻域已被越界写破坏，先记录再跳过 EnterCriticalSection，
    // 避免进程消失，并精确定位“被破坏发生在进入本 Log 之前（即上一条成功 Log 与本 Log 之间）”。
    if (!LogCanaryIntact()) {
        CrashSetStage("CANARY_HIT:g_csLog邻域被破坏(写点在上一条Log之后)");
        return;
    }
#ifdef TLTP_LOG_STAGES
    // R36：Log 内部插桩——test9 的死点已钉在 "discovery started" 这条 Log 完成之前，
    // 但 Log 已成功跑了十几条。这三个 stage 把 EnterCS / VSNPRINTF / WriteFile+flush
    // 三段分开，下次崩溃时 crash.log 最后一行直接指出死在 Log 的哪一段。
    // 代价：每条 Log 多 3 次 SD flush（启动期约 +0.5s），仅诊断构建启用。
    CrashSetStage("Log:cs");
#endif
#ifndef TLTP_UNIFY_FILELOCK
    EnterCriticalSection(&g_csLog);
#else
    TltpFileLock();
#endif

    char msg[384];
    va_list ap; va_start(ap, fmt);
    int n = VSNPRINTF(msg, sizeof(msg) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n > sizeof(msg) - 1) n = (int)(sizeof(msg) - 1);
    msg[n] = 0;
#ifdef TLTP_LOG_STAGES
    CrashSetStage("Log:fmt");
#endif

    // 节流：内容完全相同的日志，5s 内只写一次。
    // 重连循环每轮会对多个候选各打一条 try/FAIL，手机没开 App 时每秒数条雷同日志持续写 SD 卡，
    // 既抢占单核 CPU 又加速闪存磨损；真正的状态变化（新 IP、连上、断开）不受影响。
    DWORD now = GetTickCount();
    if (strcmp(msg, g_lastMsg) == 0) {
        if (now - g_lastSameTick < 5000) {
#ifndef TLTP_UNIFY_FILELOCK
            LeaveCriticalSection(&g_csLog);
#else
            TltpFileUnlock();
#endif
            return;
        }
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

#ifdef TLTP_LOG_STAGES
    CrashSetStage("Log:wrote");
#endif
#ifndef TLTP_UNIFY_FILELOCK
    LeaveCriticalSection(&g_csLog);
#else
    TltpFileUnlock();
#endif
}
