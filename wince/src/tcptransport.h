#pragma once
// tcptransport.h - 直连 TCP 字节流（原 NetClient 的 SOCKET 逻辑抽出来）
#include "transport.h"

class TcpTransport : public ITransport {
public:
    TcpTransport();
    ~TcpTransport();
    bool connect(const std::wstring& host, int port, int timeoutMs) override;
    bool read(BYTE* buf, int n) override;
    bool write(const BYTE* buf, int n) override;
    bool connected() const override { return m_sock != INVALID_SOCKET; }
    void close() override;
    int  backlog() const override;
private:
    SOCKET m_sock;
};
