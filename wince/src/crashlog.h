#pragma once

// crashlog.h - 崩溃定位（stage 主动落盘）R24 新增 / R25 修正实现方式
//
// ## 为什么需要它
// 在此之前项目里**零崩溃定位手段**，任何崩溃都只表现为"日志停在某行 → 进程消失"，
// 排查只能靠排除法猜，已经连续猜错三次：
//   · R21 猜"buflen 异常大导致巨量分配 OOM" —— 不成立；
//   · R22 猜"Log() → LocalIPv4() → Log() 嵌套加锁" —— **不成立**（C++ 实参在进入
//     Log 函数体、EnterCriticalSection 之前就已求值完毕，根本不存在嵌套持锁）；
//   · R23 猜"WinCE 线程默认栈仅 64KB" —— **这一条成立**，128KB 后真车日志第一次
//     出现 `ConnThread entered`，但随即死在下一行，暴露第二个独立故障。
//
// ## R25：为什么不用 SetUnhandledExceptionFilter
// R24 最初实现用的是 `SetUnhandledExceptionFilter` + `EXCEPTION_RECORD`，
// 编译阶段就被 CeGCC 拒绝：
//     undefined reference to `SetUnhandledExceptionFilter'
//
// 根因同"判PE 是不是 WinCE 程序不能只看 machine 字段"是同一类问题：
// **大量桌面 Windows(kernel32) 的 API 在 WinCE 的 coredll 里并不存在。**
// 与 R24 已规避的 `__try/__except`(MSVC 专有)、`GetExceptionPointers`(MSVC CRT)、
// `#include <ex.h>`、x86 CONTEXT 字段名并列，是第 5 个平台陷阱。
//
// 与其继续赌哪个 SEH/信号 API 在 WinCE 上真实存在（赌错=CI 编译失败，
// 又是好几分钟一轮回），改用**零平台依赖**的方案：
//
// ## 最终方案：stage 主动落盘
// CrashSetStage() 每次把"当前正在做什么"立即写进 crash.log 并 flush。
// 进程无论是被未捕获异常杀掉、栈溢出、还是直接 ExitProcess，
// **最后一行 stage 就是崩溃点**—— 不依赖任何异常捕获机制。
//
// 开销控制：只有 stage 名**发生变化**时才写盘（比较前后字符串），
// 故循环里反复设同一个 stage 不会产生任何 IO。
// 写盘频率实测约每秒 1-2 次（远低于 tuptup.log 本身已有的日志量），SD 卡无压力。
void CrashLogInit();
// 设置当前关键步骤。立即落盘到 crash.log（仅在值变化时）。
void CrashSetStage(const char* stage);

// 【二分变体 ba】CrashSetStage 编译期空操作（必须放在声明之后，否则上面的
// 原型也被吞掉）。用于验证 crashlog 模块本身（无锁静态缓冲 + 多线程并发写）
// 是否为真车闪退元凶。crashlog.cpp 里已 #undef 本宏保留真实定义，
// no-op 只作用于外部调用点。
#ifdef TLTP_BISECT_NO_CRASHLOG
#define CrashSetStage(stage) ((void)0)
#endif

// 【测试变体 test4】细粒度打点开关：在 readConfig 内部每个小步骤设 stage，
// 若 test4 闪退，crash.log 最后停在哪个子步骤就把死点缩到行级。
// 其他变体不受影响（FINE_STAGE 恒为空操作）。
#ifdef TLTP_TEST_FINE_STAGE
#define TLTP_FINE_STAGE(s) CrashSetStage(s)
#else
#define TLTP_FINE_STAGE(s) ((void)0)
#endif

// 【测试变体 test7】入口级逐语句探针：比 FINE_STAGE 更细一步，钉死 readConfig
// 函数最前几行（std::string 局部构造 / GetModuleFileName / 路径拼接）的具体死点——
// test4 已证明死点在 readCfg:open 之前，test7 负责回答"具体是哪一条语句"。
// 仅 test7 定义 TLTP_PROBE_ENTRY，其他变体恒为空操作。
#ifdef TLTP_PROBE_ENTRY
#define TLTP_PROBE(s) CrashSetStage(s)
#else
#define TLTP_PROBE(s) ((void)0)
#endif

// 【修复候选 test24 / R39】统一文件写锁。
// 根因面：Log() 写 gloai.log（锁 g_csLog），CrashSetStage() 写 crash.log（锁 g_csCrash），
// 这是两把**不同**的锁，于是主线程 + 后台线程可对两个文件句柄真正并发
// WriteFile+FlushFileBuffers，踩坏 WinCE 内核堆 → 下一次 EnterCriticalSection(&g_csLog)
// 崩（详见 R38 诊断结论）。test24 把两类写统一到同一把锁 g_csFile 串行化，
// 既保留全部诊断（含 TLTP_LOG_STAGES 的逐行 Log:cs/fmt/wrote），又消除跨锁并发。
// 函数声明不暴露 CRITICAL_SECTION 类型（避免头文件引入 <windows.h> 顺序问题），
// g_csFile 实体与初始化都放在 crashlog.cpp。
#ifdef TLTP_UNIFY_FILELOCK
void TltpFileInit();
void TltpFileLock();
void TltpFileUnlock();
#endif
