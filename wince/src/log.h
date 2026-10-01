#pragma once
// log.h - 极简文件日志，写入 EXE 同目录的 gloai.log（车机 SD 卡上可读取，用于诊断启动/连接问题）
#include <windows.h>

void LogInit();
void Log(const char* fmt, ...);
