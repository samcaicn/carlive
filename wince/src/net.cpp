// net.cpp - 见 net.h
#include "net.h"
#include <ws2tcpip.h>

static const BYTE MAGIC[4] = { 0x47, 0x4C, 0x4F, 0x41 }; // "GLOA"

NetClient::NetClient() : m_sock(INVALID_SOCKET), m_codec(1) {
    WSADATA wsa = {0};
    WSAStartup(MAKEWORD(2,2), &wsa);
}

NetClient::~NetClient() { close(); WSACleanup(); }

bool NetClient::connect(const std::wstring& host, int port) {
    // WinCE 无 getaddrinfo 的宽字符友好版，这里用 inet_addr 直连 IPv4
    char buf[64];
    WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, buf, sizeof(buf), NULL, NULL);
    sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = inet_addr(buf);
    if (sa.sin_addr.s_addr == INADDR_NONE) return false;

    m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_sock == INVALID_SOCKET) return false;
    if (::connect(m_sock, (SOCKADDR*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        close(); return false;
    }
    return true;
}

bool NetClient::sendMsg(BYTE type, const BYTE* payload, int len) {
    if (m_sock == INVALID_SOCKET) return false;
    std::vector<BYTE> msg;
    msg.reserve(9 + len);
    msg.insert(msg.end(), MAGIC, MAGIC+4);
    msg.push_back(type);
    // LEN 大端 uint32
    msg.push_back((BYTE)(len >> 24));
    msg.push_back((BYTE)(len >> 16));
    msg.push_back((BYTE)(len >> 8));
    msg.push_back((BYTE)(len & 0xFF));
    if (payload && len > 0) msg.insert(msg.end(), payload, payload+len);
    int sent = ::send(m_sock, (const char*)msg.data(), (int)msg.size(), 0);
    return sent == (int)msg.size();
}

void NetClient::sendHandshakeHeadunit(int maxW, int maxH) {
    // 声明本端能力（V1 默认 mjpeg 优先，避免 ffmpeg 依赖；H264 为可选）
    std::string json = "{\"role\":\"headunit\",\"proto_ver\":1,\"caps\":{\"video_decoders\":[\"mjpeg\",\"h264\"],\"max_w\":";
    json += std::to_string(maxW);
    json += ",\"max_h\":";
    json += std::to_string(maxH);
    json += ",\"touch\":true}}";
    sendMsg(0x01, (const BYTE*)json.c_str(), (int)json.size());
}

void NetClient::sendTouch(BYTE action, float nx, float ny) {
    BYTE p[10];
    p[0] = action;
    // float 大端
    BYTE* fx = (BYTE*)&nx; p[1]=fx[3]; p[2]=fx[2]; p[3]=fx[1]; p[4]=fx[0];
    BYTE* fy = (BYTE*)&ny; p[5]=fy[3]; p[6]=fy[2]; p[7]=fy[1]; p[8]=fy[0];
    p[9] = 0; // pointer_id
    sendMsg(0x04, p, 10);
}

void NetClient::sendControl(BYTE code) { sendMsg(0x05, &code, 1); }
void NetClient::sendHeartbeat() { BYTE b=0; sendMsg(0x06, &b, 1); }

bool NetClient::readExact(BYTE* buf, int n) {
    int off = 0;
    while (off < n) {
        int r = ::recv(m_sock, (char*)buf+off, n-off, 0);
        if (r <= 0) return false;
        off += r;
    }
    return true;
}

bool NetClient::readMsg(BYTE& type, std::vector<BYTE>& payload) {
    BYTE magic[4];
    if (!readExact(magic, 4)) return false;
    if (magic[0]!=MAGIC[0]||magic[1]!=MAGIC[1]||magic[2]!=MAGIC[2]||magic[3]!=MAGIC[3]) {
        return false; // 流错位，断开重连
    }
    type = 0; if (!readExact(&type, 1)) return false;
    BYTE lenBuf[4]; if (!readExact(lenBuf, 4)) return false;
    int len = (lenBuf[0]<<24)|(lenBuf[1]<<16)|(lenBuf[2]<<8)|lenBuf[3];
    if (len < 0 || len > 0x00FFFFFF) return false;
    payload.resize(len);
    if (len > 0 && !readExact(payload.data(), len)) return false;
    return true;
}

bool NetClient::recvVideoFrame(VideoFrame& out) {
    BYTE type; std::vector<BYTE> payload;
    while (connected()) {
        if (!readMsg(type, payload)) return false;
        if (type == 0x03) { // VIDEO_FRAME
            if (payload.size() < 9) continue;
            out.isKey = (payload[0] != 0);
            out.timestamp = (payload[1]<<24)|(payload[2]<<16)|(payload[3]<<8)|payload[4];
            int dlen = (payload[5]<<24)|(payload[6]<<16)|(payload[7]<<8)|payload[8];
            if (dlen < 0 || (size_t)dlen > payload.size()-9) continue;
            out.data.assign(payload.begin()+9, payload.begin()+9 + dlen);
            return true;
        } else if (type == 0x02) { // VIDEO_CONFIG：记录编解码类型
            if (!payload.empty()) m_codec = payload[0];
        }
        // 0x04 触摸 / 0x06 心跳 / 其他：忽略
    }
    return false;
}

void NetClient::close() {
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}
