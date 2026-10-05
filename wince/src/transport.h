#pragma once
// transport.h - 字节流传输抽象层
//
// 为什么需要它：车机端（NetClient）上层的 TUPT 协议（握手/触摸/心跳/视频帧）与“字节从哪来”
// 完全无关——无论走【直连手机 8686】还是【ADB 隧道(连 5555→OPEN tcp:8686)】，上层逻辑都不变。
// 把底层收/发抽象成统一的 read/write 字节流，新增 ADB 模式就只是换一个实现，不动 TUPT。
//
// 调用约定（与原有 SOCKET 行为对齐）：
//   · read()/write() 都是“阻塞直到 n 字节或断链/超时”（超时由实现内部保证，避免永久卡死）。
//   · connected() 反映链路当前是否可用；断链后必须返回 false，让上层触发重连。
//   · 多线程并发 send 由上层 NetClient 的 g_csSend 串行化；但 ADB 实现内部还会给自己加锁，
//     因为读泵/窗口流控与上层写会同时触碰 socket。
// wstring/std::string 在两个平台都要显式包含：不能指望 windows.h 顺带带进来
// （POSIX 下没有它，且 MS 头文件也不保证导出 std::wstring）。
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
typedef unsigned char BYTE;   // 主机测试（POSIX）下补齐 Win32 类型；WinCE 由 windows.h 提供
#endif

class ITransport {
public:
    virtual ~ITransport() {}
    // 建立到远端端点的连接。host=IP/主机名，port=外层端口（ADB 模式会忽略并改用 5555）。
    // timeoutMs=连接超时（毫秒）。成功返回 true。
    virtual bool connect(const std::wstring& host, int port, int timeoutMs) = 0;
    // 精确读取 n 字节到 buf；断链/出错返回 false。
    virtual bool read(BYTE* buf, int n) = 0;
    // 精确写入 n 字节；断链/出错返回 false。
    virtual bool write(const BYTE* buf, int n) = 0;
    virtual bool connected() const = 0;
    virtual void close() = 0;
    // 内核收包缓冲中尚未读取的字节数（反压用）；不适用/未知时返回 0。
    virtual int  backlog() const { return 0; }
};
