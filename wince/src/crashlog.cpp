// crashlog.cpp - 崩溃现场捕获（R24 新增）详见 crashlog.h 的说明。
//
// 核心约束：这套代码运行在崩溃现场，必须极度克制 ——
// 只用最原始的 Win32 API 与定长缓冲，不碰可能二次崩溃的 C++ 设施
// （不用 std::string / sprintf / 格式化函数，字符串全部手工逐字节拼）。
#include "crashlog.h"
#include "log.h"

#ifdef _WIN32

#include <windows.h>

// 崩溃记录文件句柄。与 log.cpp 的 g_hLog 分开，避免互抢。
static HANDLE g_hCrash = INVALID_HANDLE_VALUE;
static char   g_stage[64] = "init";

// ASCII 十六进制输出（不用 sprintf）
static void putHex(char* dst, unsigned v) {
    const char* d = "0123456789ABCDEF";
    dst[0] = d[(v >> 28) & 0xF]; dst[1] = d[(v >> 24) & 0xF];
    dst[2] = d[(v >> 20) & 0xF]; dst[3] = d[(v >> 16) & 0xF];
    dst[4] = d[(v >> 12) & 0xF]; dst[5] = d[(v >>  8) & 0xF];
    dst[6] = d[(v >>  4) & 0xF]; dst[7] = d[(v &  0xF)]; dst[8] = 0;
}

// 不依赖 CRT 的极简字符串拷贝，避免越界把日志搞坏。
static void safeCpy(char* dst, const char* src, unsigned cap) {
    unsigned i = 0;
    if (!dst || cap == 0) return;
    while (src && i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// WinCE 的 GetAdaptersInfo 等 API 失败时返回的 HRESULT 风格错误码，
// 与 winerror.h 常量可能不完全一致，故只做名称提示，不参与逻辑。
static const char* excName(unsigned c) {
    switch (c) {
    case 0xC0000005u: return "ACCESS_VIOLATION";   // EXCEPTION_ACCESS_VIOLATION
    case 0xC00000FDu: return "STACK_OVERFLOW";     // EXCEPTION_STACK_OVERFLOW
    case 0xC000001Du: return "ILLEGAL_INSTRUCTION";// EXCEPTION_ILLEGAL_INSTRUCTION
    case 0xC0000094u: return "INT_DIVIDE_BY_ZERO";
    case 0xC000007Fu: return "DATATYPE_MISALIGNMENT";
    case 0xC0000096u: return "PRIV_INSTRUCTION";
    case 0xC000008Cu: return "FLT_DIVIDE_BY_ZERO";
    case 0xC0000006u: return "IN_PAGE_ERROR";
    case 0xE06D7363u: return "CXX_EXCEPTION";      // MSVC C++ 异常标记
    default:          return "UNKNOWN";
    }
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
    if (g_hCrash != INVALID_HANDLE_VALUE) {
        // 追加而非覆盖：一次运行可能崩多次，保留全部现场
        SetFilePointer(g_hCrash, 0, NULL, FILE_END);
        static const char sep[] = "\r\n---- crash ----\r\n";
        DWORD wr = 0;
        WriteFile(g_hCrash, sep, (DWORD)(sizeof(sep) - 1), &wr, NULL);
        FlushFileBuffers(g_hCrash);
    }
}

// 把现场写入 crash.log。
// 注意：这里刻意【不读 CONTEXT 的寄存器字段】——
// ARM CE 的 CONTEXT 是 R0-R12/Sp/Lr/Pc 布局，与 x86 的 Eip/Esp/Ebp 完全不同，
// 而 mingw-w64 对 CE 的 CONTEXT 定义与 MSVC 并不完全一致，硬取字段有编译失败风险。
// EXCEPTION_RECORD 的字段（Code/Flags/Address/Information）是架构无关的，
// 且异常码本身已足够区分访问冲突 / 栈溢出 / 非法指令 —— 收益远大于风险。
static void writeScene(const EXCEPTION_RECORD* rec) {
    openCrashFile();
    if (g_hCrash == INVALID_HANDLE_VALUE) return;

    char  hex[16];
    char  buf[512];
    int   n = 0;
    DWORD wr = 0;
    const char* nl = "\r\n";

    #define APPEND(s)  do { const char* _s = (s); while (*_s && n < (int)sizeof(buf) - 12) buf[n++] = *_s++; } while (0)
    #define APPENDU(v) do { putHex(hex, (unsigned)(v)); APPEND(hex); } while (0)

    APPEND("stage="); APPEND(g_stage); APPEND(nl);
    if (rec) {
        APPEND("code=");   APPENDU(rec->ExceptionCode);
        APPEND("  type="); APPEND(excName(rec->ExceptionCode)); APPEND(nl);
        APPEND("flags=");  APPENDU(rec->ExceptionFlags); APPEND(nl);
        APPEND("addr=");   APPENDU((unsigned)rec->ExceptionAddress); APPEND(nl);
        APPEND("params="); APPENDU(rec->NumberParameters); APPEND(nl);
        if (rec->NumberParameters > 0) {
            APPEND("info0="); APPENDU(rec->ExceptionInformation[0]); APPEND(nl);
            if (rec->NumberParameters > 1) {
                APPEND("info1="); APPENDU(rec->ExceptionInformation[1]); APPEND(nl);
            }
        }
        // 出错指令附近的机器码：判断是跳飞（乱码）还是数据踩踏（有指令但操作数离谱）。
        // 读 16 字节是安全的：ExceptionAddress 必然落在本进程已映射的代码段内，
        // 跨页最多读1 个未映射页，但 ARM 的取指粒度为 4/2 字节，实际不会跨到无映射页。
        APPEND("code[16]=");
        const unsigned char* p = (const unsigned char*)rec->ExceptionAddress;
        for (int i = 0; i < 16; i++) { APPENDU(p[i]); APPEND(" "); }
        APPEND(nl);
    } else {
        APPEND("code=NO_RECORD (unhandled filter 未提供 EXCEPTION_RECORD)\r\n");
    }
    APPEND("ThreadId="); APPENDU(GetCurrentThreadId()); APPEND(nl);

    #undef APPEND
    #undef APPENDU

    WriteFile(g_hCrash, buf, (DWORD)n, &wr, NULL);
    FlushFileBuffers(g_hCrash);
}

// 未捕获异常过滤器。这是唯一能在任意线程崩溃时拿到现场的钩子。
// 回调签名自带 EXCEPTION_POINTERS*，因此不需要 GetExceptionPointers()（MSVC 专有）。
static LONG WINAPI onUnhandled(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : NULL;
    writeScene(rec);
    // 尽力往主日志也打一行，便于与 tuptup.log 的时间线对齐。
    // 栈溢出时这条 Log 本身可能失败，但 crash.log 已落盘，不影响取证。
    if (rec) {
        Log("!!! CRASH stage=%s code=%08X type=%s addr=%08X",
            g_stage, (unsigned)rec->ExceptionCode,
            excName(rec->ExceptionCode), (unsigned)rec->ExceptionAddress);
    } else {
        Log("!!! CRASH stage=%s (no exception record)", g_stage);
    }
    // 返回 EXCEPTION_EXECUTE_HANDLER 交回系统，通常进程就此终止 —— 这是期望行为。
    return EXCEPTION_EXECUTE_HANDLER;
}

void CrashSetStage(const char* stage) { safeCpy(g_stage, stage, sizeof(g_stage)); }

void CrashLogInit() {
    openCrashFile();
    SetUnhandledExceptionFilter(onUnhandled);
}

#else // !_WIN32 —— 宿主回归测试用的空实现

void CrashSetStage(const char*) {}
void CrashLogInit() {}

#endif // _WIN32
