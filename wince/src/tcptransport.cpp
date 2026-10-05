// tcptransport.cpp - 见 tcptransport.h
#include "tcptransport.h"
#include "log.h"
#include <ws2tcpip.h>

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x1
#endif

static void setNoDelay(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}
static void setKeepAlive(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
}
static void setRecvTimeout(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    // R20致命修复：原设 5s，注释理由是"5s > 心跳 3s，避免空闲误判断链"——
    //   但手机端**根本没有实现心跳**（NetClient.sendHeartbeat() 定义了但全工程零调用点），
    //   且 MjpegSender 在画面静止时 `continue` 一帧都不发（省流量设计）。
    //   三者叠加 = 画面一静止，5s 后 SO_RCVTIMEO 到期 → recv 返回错误 →
    //   read() false → 判定断链 → 重连 → 再静止 5s → 再断……
    //   表现就是"连上 5 秒就断、反复闪已连接/连接断开"，用户观感= 永远连不上。
    // 现在把超时放宽到 15s：即便手机端一个字节都不发，也 15s 才断；
    // 真正的死链判定交给上层心跳线程的"接收侧活性"检查（net.cpp 的 g_lastRecvTick），
    // 它能区分"对端真的死了"和"对端只是安静"。
    int to = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
}

TcpTransport::TcpTransport() : m_sock(INVALID_SOCKET), m_lastFailTimeout(false) {}

TcpTransport::~TcpTransport() { close(); }

bool TcpTransport::connect(const std::wstring& host, int port, int timeoutMs) {
    close(); // 防御：丢弃残留 socket
    char buf[64];
    WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, buf, sizeof(buf), NULL, NULL);
    sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = inet_addr(buf);
    if (sa.sin_addr.s_addr == INADDR_NONE) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    setNoDelay(s);
    setKeepAlive(s);
    // R15：高频短连（候选逐个试探）时大量半开连接进 TIME_WAIT，WinCE 本地端口池小易耗尽，
    // 允许地址复用避免“看似在跑却永远连不上”。
    int reuse = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    // 非阻塞 connect + select 超时
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
    ::connect(s, (SOCKADDR*)&sa, sizeof(sa)); // 立即返回 WSAEWOULDBLOCK

    fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
    timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(0, NULL, &wfds, NULL, &tv);
    bool ok = false;
    if (r == 1) {
        int err = 0, el = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        ok = (err == 0);
    }
    mode = 0; ioctlsocket(s, FIONBIO, &mode);
    if (ok) {
        setRecvTimeout(s);
        m_sock = s;
        return true;
    }
    closesocket(s);
    return false;
}

bool TcpTransport::read(BYTE* buf, int n) {
    int off = 0;
    while (off < n) {
        int r = ::recv(m_sock, (char*)buf + off, n - off, 0);
        if (r == 0) { m_lastFailTimeout = false; return false; }          // 对端正常关闭(FIN) = 真断链
        if (r < 0) {
            int err = WSAGetLastError();
            // R20：WSAETIMEDOUT / WSAEWOULDBLOCK 都属于"接收超时"——对端仍在线只是没发数据。
            // 原实现与"对端关闭"同等return false，导致手机端（无心跳+静帧不发字节）
            // 画面一静止 5s 就被判断链，反复重连。现如实区分语义，交上层决定是否重连。
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) { m_lastFailTimeout = true; return false; }
            m_lastFailTimeout = false;
            return false;                                                  // 其余错误码 = 真断链
        }
        off += r;
    }
    return true;
}

bool TcpTransport::write(const BYTE* buf, int n) {
    int off = 0;
    while (off < n) {
        int sent = ::send(m_sock, (const char*)buf + off, n - off, 0);
        if (sent <= 0) return false;
        off += sent;
    }
    return true;
}

int TcpTransport::backlog() const {
    if (m_sock == INVALID_SOCKET) return 0;
    u_long b = 0;
    if (ioctlsocket(m_sock, FIONREAD, &b) == 0) return (int)b;
    return 0;
}

void TcpTransport::close() {
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}
