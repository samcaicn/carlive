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
