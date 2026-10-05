#pragma once
// log.h - 极简文件日志，写入 EXE 同目录的 tuptup.log（车机 SD 卡上可读取，用于诊断启动/连接问题）
//
// 本头文件只声明两个函数，不出现任何 Win32 类型，因此 windows.h 由实现
// （log.cpp）自己去包含。这样依赖 Log() 的传输层代码（adb.cpp 等）才能在
// 宿主机上编译做端到端回归——POSIX 侧用 scripts/log_stub.cpp 顶替。
#include <stddef.h>

void LogInit();
void Log(const char* fmt, ...);
