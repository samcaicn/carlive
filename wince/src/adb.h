#pragma once
// adb.h - ADB 隧道传输层（车机端作为 ADB 客户端，连手机 adbd 的 5555，OPEN tcp:8686 打通 TUPT）
//
// 为什么走 ADB：用户要的“USB 调试模式”在 WinCE 车机上没有 USB Host API（AOA 不可行），
// 但手机通过 USB 网络共享(或可联网)后，adbd 可在 TCP 5555 监听；车机用 ADB 协议完成鉴权后，
// OPEN "tcp:8686"（手机端 tuptup.top App 已有的 ServerSocket）即得到一个和普通直连等价的字节流。
// 上层 TUPT 协议完全无感——它只看 ITransport 的 read/write。
//
// 鉴权：adbd 下发 20 字节随机 token，车机用预置 RSA 私钥对 SHA1(token) 做 PKCS#1 v1.5 签名
// （见 rsa.cpp，已在 PC 上用 openssl 反向验证通过）；首次连接手机会弹“允许 USB 调试”，
// 同意后公钥被永久信任，之后每次连仍走同样签名流程（本类会主动附带 RSAPUBLICKEY 以便一次性授权）。
//
// 线程模型：独立“读泵线程”持续收 ADB 消息（WRTE→入读缓冲并回 OKAY；OKAY→恢复写窗口；CLSE→断链），
// 与上层并发 write 互不阻塞，保证触摸下行低延迟（与原有直连 TCP 的并发语义一致）。
#include "transport.h"
#include "syssync.h"
#include <stdint.h>
#include <deque>
#include <vector>

class AdbTransport : public ITransport {
public:
    AdbTransport();
    ~AdbTransport();
    bool connect(const std::wstring& host, int port, int timeoutMs) override;
    bool read(BYTE* buf, int n) override;
    bool write(const BYTE* buf, int n) override;
    bool connected() const override;
    void close() override;
    // ADB 模式下返回“已从隧道读出、尚未交给上层”的字节数 + 内核缓冲待读数，
    // 让 NetClient 的反压逻辑（backlog 过大就丢帧取最新）在隧道模式同样生效。
    int  backlog() const override;

private:
    // 平台原生句柄类型（与 adb.cpp 中的 Sock 一致），供 sock() 取值使用。
#ifdef _WIN32
    typedef UINT_PTR AdbSock;
#else
    typedef int AdbSock;
#endif
    AdbSock sock() const { return (AdbSock)m_sock; }

    // ---- 平台无关的 ADB 字节收发（内部使用 m_sendMutex 串行化所有 socket 写）----
    enum { ADB_PORT = 5555 };   // adbd 标准 TCP 监听端口
    enum RecvState { RS_OK, RS_TIMEOUT, RS_ERROR };

    bool    sockConnectTimeout(const char* ip, int port, int timeoutMs);
    void    setRecvTimeout(int ms);
    bool    sendMsg(uint32_t cmd, uint32_t a1, uint32_t a2, const BYTE* data, int dlen);
    RecvState recvMsg(int timeoutMs, uint32_t& cmd, uint32_t& a1, uint32_t& a2, std::vector<BYTE>& data);
    bool    recvExact(BYTE* buf, int n);

    // ---- 握手阶段（单线程，无并发）----
    bool    sendAuthPublicKey();
    bool    sendOpen();
    bool    doHandshake(int timeoutMs);

    // ---- 流式收发（多线程）----
#ifdef _WIN32
    static DWORD WINAPI ReadPumpThunk(LPVOID p);
#else
    static void* ReadPumpThunk(void* p);
#endif
    void    ReadPump();

    // ---- 状态 ----
    SysMutex     m_sendMutex;             // 保护所有 socket 写（WRTE 与回 OKAY 不能字节交错）
    mutable SysMutex m_bufMutex;          // 保护 m_readBuf（backlog() 是 const，故需 mutable）
    SysEvent     m_readEvent;             // 读缓冲有数据 / 断链时脉冲

    std::vector<BYTE> m_readBuf;          // 上层待读字节流
    bool         m_running = false;       // 读泵是否应继续
    bool         m_open    = false;       // 流（OPEN）是否已建立
    bool         m_closed  = false;       // 是否已断链

    // socket 句柄。刻意用 uintptr_t 而非平台原生类型：头文件不该暴露
    // SOCKET/int 差异，但它【必须】是每实例成员——早前写成 .cpp 里的
    // 文件级 static（外加 #define m_sock … 劫持改名），后果是多个
    // AdbTransport 实例共享同一个句柄：NetClient 每次重连都 new 一个，
    // 于是上一个实例的 close()/读写会直接打到新实例的 socket 上。
    // 约定：(uintptr_t)-1 表示无效句柄（Win32 INVALID_SOCKET 与 POSIX -1
    // 都是全 1 位模式，两者统一）。
    uintptr_t    m_sock = (uintptr_t)-1;

    uint32_t     m_localId = 0x01000000;  // 我们为 OPEN 指定的本地流 id
    uint32_t     m_remoteId = 0;          // adbd 为流指定的远端 id（我们发 WRTE 用它）
    uint32_t     m_adbMaxData = 4096;     // adbd 声明的 MAX_DATA（限制我们单个 WRTE 大小）
    uint32_t     m_ourMaxData = 262144;   // 我们声明的 MAX_DATA（限制 adbd→我们的单帧大小）
    long         m_remoteWindow = 4096;   // 我们尚可发送的字节窗口（收到 OKAY 恢复）

    // 在途 WRTE 的字节数队列，用于精确滑动窗口。
    //
    // 为什么必须自持：早期实现从对端 OKAY 的 arg1 读窗口值
    // （m_remoteWindow = a2 ? a2 : maxdata），这假设“每个 OKAY 都把窗口刷成
    // 对端 MAX_DATA”。真实 adbd 的 OKAY 只承担 ack 语义，arg1 通常根本不带窗口
    // （带时也五花八门），靠它会错幂恢复到满窗 → 无节制 pipeline。
    // 改法：adb 流是严格有序的，一个 OKAY 恰好按序 ack 一个我们发出的 WRTE，
    // 因此把每个已发 chunk 的大小入队，收到 OKAY 就出队首并把额度加回来。
    std::deque<int> m_outstanding;
    SysEvent     m_windowEvent;           // 写窗口恢复时被点亮，替代忙轮询

    // 读泵线程句柄（跨平台）
#ifdef _WIN32
    HANDLE       m_readThread = NULL;
#else
    pthread_t    m_readThread = 0;
#endif
};
