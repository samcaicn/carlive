// crashlog.cpp - 崩溃定位（stage 主动落盘）R24 新增 / R25 修正实现方式。
// 设计理由与踩坑记录见 crashlog.h。
//
// 核心约束：这份文件必须在**没有任何平台扩展**的前提下工作。
// 已确认在 CeGCC (arm-mingw32ce, GCC 9.3) 上不可用的写法，勿再尝试：
//   1. __try/__except            —— MSVC 专有 SEH 语法，GCC 不支持
//   2. GetExceptionPointers()     —— MSVC CRT 函数，mingw-w64 无此符号
//   3. #include <ex.h>            —— mingw 下无此头
//   4. CONTEXT 的 Eip/Esp/Ebp等   —— ARM CE 是 R0-R12/Sp/Lr/Pc 布局，与 x86 完全不同
//   5. SetUnhandledExceptionFilter—— 【R25 新增】WinCE coredll 未导出，链接即失败
// 结论：不要写任何"崩溃时才执行"的代码，改为关键步骤主动落盘。
#include "crashlog.h"

// 二分变体 ba：crashlog.h 把 CrashSetStage 定义成了 no-op 宏，
// 但本文件要保留真实函数定义（链接层仍需要它），no-op 只作用于各调用点。
#ifdef CrashSetStage
#undef CrashSetStage
#endif

#ifdef _WIN32

#include <windows.h>
#include <string.h>

static HANDLE g_hCrash = INVALID_HANDLE_VALUE;
static char   g_stage[64] = "";

// R39（test24）：统一文件写锁。Log(gloai.log) 与 CrashSetStage(crash.log) 共用这一把锁，
// 把跨锁并发的两个文件句柄写串行化，消除 WinCE 内核堆踩踏。
// 必须比任何 Log/CrashSetStage 调用都早初始化：WinMain 里 LogInit() 先于 CrashLogInit()
// 执行，因此 TltpFileInit() 在 LogInit 里也会调一次（幂等，g_csFileInit 守卫）。
#ifdef TLTP_UNIFY_FILELOCK
static CRITICAL_SECTION g_csFile;
static bool g_csFileInit = false;
void TltpFileInit() {
    if (!g_csFileInit) { InitializeCriticalSection(&g_csFile); g_csFileInit = true; }
}
void TltpFileLock()   { EnterCriticalSection(&g_csFile); }
void TltpFileUnlock() { LeaveCriticalSection(&g_csFile); }
#endif
// R-debug：crashlog 被 WinMain / ConnThread / DiscoveryThread 并发写（g_stage 与 g_hCrash 均无锁），
// 多线程同时写同一文件句柄+静态缓冲是数据竞争 → 会损坏堆/句柄，本身就可能引发或加剧闪退。
// 加一把锁把整段写操作串行化（零平台依赖，WinCE 的 InitializeCriticalSection 稳定可用）。
static CRITICAL_SECTION g_csCrash;
static bool g_csCrashInit = false;

static void safeCpy(char* dst, const char* src, unsigned cap) {
    unsigned i = 0;
    if (!dst || cap == 0) return;
    while (src && i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// R27：与 log.cpp 同理，crash.log 名要从 EXE 自身文件名派生。
// SD 卡上同时放多个候选 exe 做 A/B 对比时，若都写 crash.log，
// 后启动者覆盖前者，对比实验得到的 stage 就不再是自己那次崩溃的。
// "\...\tuptup-usbnet.exe" → "\...\tuptup-usbnet.crash.log"
// 本函数只做纯 WCHAR 处理，不引入任何动态分配或库依赖（保持本文件零依赖原则）。
static void crashPathFromModule(WCHAR* path, const WCHAR* ext) {
    WCHAR* slash = wcsrchr(path, L'\\');
    WCHAR* dot   = wcsrchr(path, L'.');
    if (dot && dot > (slash ? slash : path - 1)) *dot = 0;  // 截掉 ".exe"
    // 手工拼接，避免依赖 wcscat（本文件刻意只用最小 Win32 面）
    unsigned i = 0; while (path[i]) i++;
    unsigned j = 0;
    while (ext[j] && i < MAX_PATH - 1) { path[i++] = ext[j++]; }
    path[i] = 0;
}

static void openCrashFile(void) {
    if (g_hCrash != INVALID_HANDLE_VALUE) return;
    WCHAR path[MAX_PATH];
    memset(path, 0, sizeof(path));
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        crashPathFromModule(path, L".crash.log");
    } else {
        // 取不到模块名时的兜底（与 log.cpp 一致）
        const WCHAR* d = L"tuptup.crash.log";
        unsigned i = 0; while (d[i]) { path[i] = d[i]; i++; }
        path[i] = 0;
    }
    g_hCrash = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hCrash != INVALID_HANDLE_VALUE)
        SetFilePointer(g_hCrash, 0, NULL, FILE_END); // 追加
}

void CrashSetStage(const char* stage) {
    if (!stage) return;
#ifdef TLTP_UNIFY_FILELOCK
    TltpFileInit();
    EnterCriticalSection(&g_csFile);
#else
    if (!g_csCrashInit) { InitializeCriticalSection(&g_csCrash); g_csCrashInit = true; }
    EnterCriticalSection(&g_csCrash);
#endif
    // 绝大多数调用点是在循环里反复设同一个 stage。不比较的话每秒会写几十次盘，
    // 白白磨损 SD 卡闪存并抢占单核 CPU。比较后只有真正换步骤才落盘。
    if (strcmp(g_stage, stage) == 0) {
#ifdef TLTP_UNIFY_FILELOCK
        LeaveCriticalSection(&g_csFile);
#else
        LeaveCriticalSection(&g_csCrash);
#endif
        return;
    }
    safeCpy(g_stage, stage, sizeof(g_stage));

    openCrashFile();
    if (g_hCrash == INVALID_HANDLE_VALUE) {
#ifdef TLTP_UNIFY_FILELOCK
        LeaveCriticalSection(&g_csFile);
#else
        LeaveCriticalSection(&g_csCrash);
#endif
        return;
    }
    // 手工拼行：这里刻意不用 std::string / sprintf —— 本文件要在
    // "刚Detect 到低内存或栈已紧张" 的场景下也能工作，任何动态分配都可能二次崩溃。
    char line[160];
    char hex[16];
    int  n = 0;
    const char* pfx = "[stage] ";
    while (*pfx && n < (int)sizeof(line) - 32) line[n++] = *pfx++;
    // R38：把线程 ID 写进每行，三角定位到底哪条线程在踩堆（主线程/ConnThread/DiscoveryThread）。
    DWORD tid = GetCurrentThreadId();
    line[n++] = '['; line[n++] = 't'; line[n++] = '=';
    const char* d = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) { line[n++] = d[(tid >> ((7 - i) * 4)) & 0xF]; }
    line[n++] = ']'; line[n++] = ' ';
    const char* s = g_stage;
    while (*s && n < (int)sizeof(line) - 20) line[n++] = *s++;
    line[n++] = ' '; line[n++] = 't';
    // 毫秒时间戳：与 tuptup.log 的 t+xxx 对齐，方便两次日志交叉比对
    // （d 已在上方线程 ID 块声明，复用同一张 16 进制表）
    unsigned ms = GetTickCount();
    for (int i = 0; i < 8; i++) { line[n++] = d[(ms >> ((7 - i) * 4)) & 0xF]; }
    line[n++] = '\r'; line[n++] = '\n';

    DWORD wr = 0;
    WriteFile(g_hCrash, line, (DWORD)n, &wr, NULL);
    // 必须每次 flush：崩溃时进程直接消失，缓冲区里的内容会一起丢。
    // 这是本方案存在的全部意义 —— 落盘时机必须与崩溃无关。
    FlushFileBuffers(g_hCrash);
#ifdef TLTP_UNIFY_FILELOCK
    LeaveCriticalSection(&g_csFile);
#else
    LeaveCriticalSection(&g_csCrash);
#endif
    (void)hex;
}

void CrashLogInit() {
    openCrashFile();
    if (g_hCrash == INVALID_HANDLE_VALUE) return;
    static const char hdr[] = "---- tuptup crashloc start ----\r\n";
    DWORD wr = 0;
    WriteFile(g_hCrash, hdr, (DWORD)(sizeof(hdr) - 1), &wr, NULL);
    FlushFileBuffers(g_hCrash);
    CrashSetStage("WinMain:init");
}

#else // !_WIN32 —— 宿主回归测试用的空实现

void CrashSetStage(const char*) {}
void CrashLogInit() {}

#endif // _WIN32
