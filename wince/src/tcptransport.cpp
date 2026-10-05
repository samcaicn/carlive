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
    int to = 5000;   // 取 5s > 心跳间隔(3s)，避免正常空闲被误判断链
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
}

TcpTransport::TcpTransport() : m_sock(INVALID_SOCKET) {}

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
        if (r <= 0) return false;
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
