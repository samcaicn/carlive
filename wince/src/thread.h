#pragma once
// thread.h - 统一的 WinCE 线程创建封装（R23新增）
//
//【为什么必须有这个文件】——真车启动即死的根因：
//
// WinCE 的 CreateThread 与桌面 Windows 语义不同：
//   dwStackSize 参数**被忽略**，栈大小只由链接器 /STACK 决定，默认 64KB。
//   （唯一例外是 dwCreationFlags 带 STACK_SIZE_PARAM_IS_A_RESERVATION，
//     此时 dwStackSize 才表示"为该线程保留的虚拟内存"。）
//
// 车机上这64KB 根本不够用：
//   · ConnThread 栈上有 std::string cfg / std::string localIP /
//     std::vector<std::string> probe/cands 等对象；
//   ·再叠加 GetCandidates → AddInterfaceGateways → probePort → connect
//     → snprintf/sscanf 等调用链，实测会**瞬间溢出**。
//   · 一旦线程栈溢出，WinCE 直接终止整个进程 —— 症状就是
//     "窗口连闪都没闪就没了"，且日志恰好停在 CreateThread 之前的那一行。
//
// 这解释了 R21/R22 的全部现象：R21 加了 AdapterBufLen() 这层调用、R22 又加了
// std::string localIP = LocalIPv4()，栈压力逐步逼近 64KB 上限，
// 于是崩溃点从 "ConnThread start 之后" 一路前移到 "discovery 之后"。
// 之前误判为"Log()嵌套加锁"，是因为看漏了 C++ 的求值顺序：
// Log() 的实参（含 LocalIPv4()）在进入函数体、EnterCriticalSection **之前**
// 就已求值完毕，根本不存在嵌套持锁。R22 因此修了个不存在的问题，症状不变。
//
// 修法：所有 CreateThread 一律走本文件，显式加 STACK_SIZE_PARAM_IS_A_RESERVATION
// 保留足够栈。宿主（POSIX）侧退化为直接调用 CreateThread，保证主机端
// 回归测试仍可编译运行。
#ifdef _WIN32
#include <windows.h>
#endif

// 每个后台线程保留的栈（虚拟内存 reservation，不等于实际提交物理页，
// WinCE 按需提交，所以给大值不会明显吃 RAM，但能彻底消除栈溢出）。
// 128KB 足以覆盖 std::string/vector + Winsock + sscanf 的最深调用链。
#define TLTP_STACK_RESERVE (128 * 1024)

#ifdef _WIN32
// 统一入口：屏蔽 WinCE 的栈大小坑，等价于普通 CreateThread。
inline HANDLE TltpCreateThread(LPTHREAD_START_ROUTINE fn, LPVOID param) {
    return ::CreateThread(NULL, TLTP_STACK_RESERVE, fn, param,
                          STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
}
#else
// 宿主（POSIX）回归测试用不到本封装 —— adb.cpp 在 POSIX 分支走 pthread_create，
// 且 net.cpp/main.cpp 不参与主机编译。这里保留一个可编译的等价实现，
// 避免 adb.cpp 误 include 时报HANDLE/LPVOID 未定义。
#include <pthread.h>
inline pthread_t TltpCreateThread(void* (*fn)(void*), void* param) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, param) != 0) return 0;
    return t;
}
#endif