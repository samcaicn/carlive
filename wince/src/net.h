#pragma once
// net.h - WinCE 车机端 TCP 客户端 + GLOA 协议解析
// 对应 proto/protocol.md。WinCE 使用 Winsock2 (ws2.dll)。

#include <windows.h>
#include <winsock2.h>
#include <vector>
#include <string>

struct VideoFrame {
    bool  isKey;
    DWORD timestamp;
    std::vector<BYTE> data;   // Annex-B H264 或 完整 JPEG
};

class NetClient {
public:
    NetClient();
    ~NetClient();

    bool connect(const std::wstring& host, int port);
    void sendHandshakeHeadunit(int maxW, int maxH);
    void sendTouch(BYTE action, float nx, float ny);
    void sendControl(BYTE code);
    void sendHeartbeat();

    // 阻塞读取下一个视频帧（自动跳过非视频消息）。返回 false=断线。
    bool recvVideoFrame(VideoFrame& out);
    void close();
    bool connected() const { return m_sock != INVALID_SOCKET; }
    int  codec() const { return m_codec; }   // 0=H264, 1=MJPEG（由 VIDEO_CONFIG 设置）

private:
    SOCKET m_sock;
    int m_codec;   // 当前视频编解码，默认 MJPEG(1)
    bool sendMsg(BYTE type, const BYTE* payload, int len);
    bool readExact(BYTE* buf, int n);
    bool readMsg(BYTE& type, std::vector<BYTE>& payload); // 读一个完整消息
};
