#pragma once
// crashlog.h - 崩溃现场捕获（R24 新增）
//
// ## 为什么必须加
// 在此之前项目里**零异常捕获**，任何崩溃都只表现为"日志停在某行 → 进程消失"，
// 排查只能靠排除法猜，已经连续猜错两次：
//   · R21 猜"buflen 异常大导致巨量分配 OOM" —— 不成立；
//   · R22 猜"Log() → LocalIPv4() → Log() 嵌套加锁" —— **不成立**（实参在进入
//     Log 函数体、EnterCriticalSection 之前就已求值完毕，根本不存在嵌套持锁）；
//   · R23 猜"WinCE 线程默认栈 64KB 导致栈溢出" —— **这一条是对的**，128KB 后线程
//     成功进入 ConnThread（日志出现 ConnThread entered），但随即死在下一行，
//     暴露了第二个独立问题。
// 三次猜测、三次落空，说明这条路上必须换成"机器把现场吐出来"而不是人肉推断。
//
// ## 崩溃在 WinCE 上有多难查
// 没有 core dump、没有 stderr、车机更没有调试器。唯一能拿到的信息就是
// **异常码 + 出错指令地址 + 出错指令的机器码**，必须第一时间落盘。
//
// ## 为什么不用 __try/__except
// CeGCC 是 GCC 9.3（arm-mingw32ce），**`__try/__except` 是 MSVC 专有语法，GCC 不支持**；
// `GetExceptionPointers()` 也是 MSVC CRT 函数，mingw-w64 没有。
// 若照搬网上 MSVC 方案的写法，CI 会在编译阶段直接失败。
//
// 改用**纯 Win32 SEH**：SetUnhandledExceptionFilter 的回调签名本身就带
// EXCEPTION_POINTERS*，直接取用即可，无需任何编译器扩展。
//
// ## ARM CE 的 CONTEXT 布局与 x86 完全不同
// x86 是 Eip/Esp/Ebp/Eax/Ebx/Ecx/Edx/Esi/Edi；ARM CE 是 R0-R12/Sp/Lr/Pc/Fpscr/Spsr。
// 若照抄 x86 字段名，编译不过；即使编过也会打出错误的寄存器值。
// 这里只输出 ARM 布局里确实存在的字段，并且全部走定长缓冲。
#ifndef CRASHLOG_H
#define CRASHLOG_H

void CrashLogInit();
// 设置"当前正在执行的关键步骤"。崩溃时由异常处理器写入，用于回答
// "崩在哪一步"，而不只是"崩了"。所有实现都在 crashlog.cpp 的 _WIN32 分支内，
// POSIX 侧为空实现，宿主回归测试仍可编译。
void CrashSetStage(const char* stage);

#endif // CRASHLOG_H
