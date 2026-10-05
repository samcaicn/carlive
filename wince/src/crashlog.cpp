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

#ifdef _WIN32

#include <windows.h>
#include <string.h>

static HANDLE g_hCrash = INVALID_HANDLE_VALUE;
static char   g_stage[64] = "";

static void safeCpy(char* dst, const char* src, unsigned cap) {
    unsigned i = 0;
    if (!dst || cap == 0) return;
    while (src && i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void openCrashFile(void) {
    if (g_hCrash != INVALID_HANDLE_VALUE) return;
    WCHAR path[MAX_PATH];
    memset(path, 0, sizeof(path));
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR* p = wcsrchr(path, L'\\');
        if (p) wcscpy(p + 1, L"crash.log");
        else wcscpy(path, L"crash.log");
    } else {
        wcscpy(path, L"crash.log");
    }
    g_hCrash = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_hCrash != INVALID_HANDLE_VALUE)
        SetFilePointer(g_hCrash, 0, NULL, FILE_END); // 追加
}

void CrashSetStage(const char* stage) {
    if (!stage) return;
    // 绝大多数调用点是在循环里反复设同一个 stage。不比较的话每秒会写几十次盘，
    // 白白磨损 SD 卡闪存并抢占单核 CPU。比较后只有真正换步骤才落盘。
    if (strcmp(g_stage, stage) == 0) return;
    safeCpy(g_stage, stage, sizeof(g_stage));

    openCrashFile();
    if (g_hCrash == INVALID_HANDLE_VALUE) return;
    // 手工拼行：这里刻意不用 std::string / sprintf —— 本文件要在
    // "刚Detect 到低内存或栈已紧张" 的场景下也能工作，任何动态分配都可能二次崩溃。
    char line[128];
    char hex[16];
    int  n = 0;
    const char* pfx = "[stage] ";
    while (*pfx && n < (int)sizeof(line) - 24) line[n++] = *pfx++;
    const char* s = g_stage;
    while (*s && n < (int)sizeof(line) - 20) line[n++] = *s++;
    line[n++] = ' '; line[n++] = 't';
    // 毫秒时间戳：与 tuptup.log 的 t+xxx 对齐，方便两次日志交叉比对
    const char* d = "0123456789";
    unsigned ms = GetTickCount();
    for (int i = 0; i < 8; i++) { line[n++] = d[(ms >> ((7 - i) * 4)) & 0xF]; }
    line[n++] = '\r'; line[n++] = '\n';

    DWORD wr = 0;
    WriteFile(g_hCrash, line, (DWORD)n, &wr, NULL);
    // 必须每次 flush：崩溃时进程直接消失，缓冲区里的内容会一起丢。
    // 这是本方案存在的全部意义 —— 落盘时机必须与崩溃无关。
    FlushFileBuffers(g_hCrash);
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
