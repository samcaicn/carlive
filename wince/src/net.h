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
    // 非阻塞带超时的连接（用于自动发现时对多个候选 IP 快速试探）
    bool connectTimeout(const std::wstring& host, int port, int timeoutMs);
    void sendHandshakeHeadunit(int maxW, int maxH);
    void sendTouch(BYTE action, float nx, float ny);
    void sendControl(BYTE code);
    bool sendHeartbeat();

    // 阻塞读取下一个视频帧（自动跳过非视频消息）。返回 false=断线。
    bool recvVideoFrame(VideoFrame& out);
    void close();
    bool connected() const { return m_sock != INVALID_SOCKET; }
    int  codec() const { return m_codec; }   // 0=H264, 1=MJPEG（由 VIDEO_CONFIG 设置）

    // ---- 自动发现（UDP 信标，端口 8687）----
    static void StartDiscovery();   // 启动后台监听线程，持续收集手机广播的 IP
    static void StopDiscovery();
    // 返回候选 IP 列表：config.txt 显式 IP（若有）优先，其次信标发现，最后内置 USB 共享固定 IP
    static void GetCandidates(const std::string& configIP, std::vector<std::string>& out);

private:
    SOCKET m_sock;
    int m_codec;   // 当前视频编解码，默认 MJPEG(1)
    bool sendMsg(BYTE type, const BYTE* payload, int len);
    bool readExact(BYTE* buf, int n);
    bool readMsg(BYTE& type, std::vector<BYTE>& payload); // 读一个完整消息
};
