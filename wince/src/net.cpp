// net.cpp - 见 net.h
#include "net.h"
#include "log.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cstring>

static const BYTE MAGIC[4] = { 0x47, 0x4C, 0x4F, 0x41 }; // "GLOA"

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x1
#endif

// 关闭 Nagle：触摸/视频小包立即发出，降低交互延迟。
static void setNoDelay(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

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
    setNoDelay(m_sock);
    if (::connect(m_sock, (SOCKADDR*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        close(); return false;
    }
    return true;
}

bool NetClient::connectTimeout(const std::wstring& host, int port, int timeoutMs) {
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
    if (ok) { m_sock = s; return true; }
    closesocket(s);
    return false;
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
    std::string json = "{\"role\":\"headunit\",\"proto_ver\":1,\"caps\":{\"video_decoders\":[\"mjpeg\"],\"max_w\":";
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
bool NetClient::sendHeartbeat() { BYTE b=0; return sendMsg(0x06, &b, 1); }

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
        if (!readMsg(type, payload)) { Log("recvVideoFrame: readMsg failed -> disconnect"); return false; }
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

///////////////////////////////////////////////////////////////////////////////
// 自动发现（纯探测，不写死任何地址）
// 三级探测，全部动态：
//   1) UDP 信标（端口 8687）：手机侧每隔 1s 向广播地址发送 "GLOAI|<ip>|<port>"，
//      车机监听即可拿到手机【真实】IP——最可靠，无需任何假设。
//   2) 本机接口网关推导：枚举车机自身网卡，若本端位于某私有子网 x.y.z.w(w>1)，
//      则手机（网关/服务端）必在该子网，优先取网卡真实网关，否则推断为 x.y.z.1。
//   3) 主动子网扫描：对车机所在每个私有子网逐地址 TCP 探测 8686 端口，开放者即手机。
//      ——真正的“探测”，不依赖手机恰好是 .1 的假设，应对任何 tether 拓扑。
///////////////////////////////////////////////////////////////////////////////

static const int DISCOVERY_PORT = 8687;
static const int PHONE_PORT    = 8686;

static CRITICAL_SECTION g_csCand;
static std::vector<std::string> g_beaconIPs;   // 信标/扫描发现的手机 IP
static volatile bool g_discoveryOn = false;
static volatile bool g_linkUp      = false;    // 链路已连通（握手成功）→ 暂停探测

static void AddCandidate(const char* ip) {
    if (!ip || !*ip) return;
    EnterCriticalSection(&g_csCand);
    bool found = false;
    for (size_t i = 0; i < g_beaconIPs.size(); i++) {
        if (g_beaconIPs[i] == ip) { found = true; break; }
    }
    if (!found) g_beaconIPs.push_back(ip);
    LeaveCriticalSection(&g_csCand);
}

// 判断是否为私有/链路本地地址（USB 共享、WiFi 局域网均落在此范围）
static bool isLocalSubnet(unsigned a, unsigned b, unsigned c, unsigned d) {
    if (a == 10) return true;                         // 10.0.0.0/8
    if (a == 172 && b >= 16 && b <= 31) return true;  // 172.16.0.0/12
    if (a == 192 && b == 168) return true;            // 192.168.0.0/16
    if (a == 169 && b == 254) return true;            // 169.254.0.0/16 链路本地
    return false;
}

// 探测手段 2：枚举本机接口，动态推导手机（网关/服务端）地址，绝不写死段号。
static void AddInterfaceGateways() {
    ULONG buflen = 0;
    if (GetAdaptersInfo(NULL, &buflen) != ERROR_BUFFER_OVERFLOW || buflen == 0)
        return;
    std::vector<BYTE> buf(buflen);
    PIP_ADAPTER_INFO pAdapters = (PIP_ADAPTER_INFO)buf.data();
    if (GetAdaptersInfo(pAdapters, &buflen) != NO_ERROR) return;
    for (PIP_ADAPTER_INFO p = pAdapters; p; p = p->Next) {
        // 优先使用网卡自身上报的真实网关（USB 共享下即手机地址）
        unsigned ga, gb, gc, gd;
        if (sscanf(p->GatewayList.IpAddress.String, "%u.%u.%u.%u", &ga, &gb, &gc, &gd) == 4
            && (ga | gb | gc | gd) != 0) {
            AddCandidate(p->GatewayList.IpAddress.String);
        }
        for (PIP_ADDR_STRING addr = &p->IpAddressList; addr; addr = addr->Next) {
            unsigned a, b, c, d;
            if (sscanf(addr->IpAddress.String, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
                continue;
            if (a == 0 || a == 127 || d == 0 || d == 255)
                continue; // 跳过 0.0.0.0 / 回环 / 网络号 / 广播
            // 网卡未上报网关时，推断本子网 .1 为网关（车机自身 w>1，手机一般为 .1）
            if ((ga | gb | gc | gd) == 0 && d != 1) {
                char gw[32];
                snprintf(gw, sizeof(gw), "%u.%u.%u.1", a, b, c);
                AddCandidate(gw);
            }
        }
    }
}

// 探测手段 3：主动扫描车机所在私有子网，对 8686 端口做 TCP 探测，开放者即手机。
// 这是真正的“探测”，不假设手机是 .1，可应对任意 tether/网段拓扑。
static DWORD WINAPI SubnetScanThread(LPVOID) {
    while (g_discoveryOn) {
        if (g_linkUp) { Sleep(1000); continue; }   // 已连上：暂停扫描，不浪费资源/不打扰手机
        ULONG buflen = 0;
        if (GetAdaptersInfo(NULL, &buflen) != ERROR_BUFFER_OVERFLOW || buflen == 0) {
            Sleep(2000); continue;
        }
        std::vector<BYTE> buf(buflen);
        PIP_ADAPTER_INFO pAdapters = (PIP_ADAPTER_INFO)buf.data();
        if (GetAdaptersInfo(pAdapters, &buflen) != NO_ERROR) { Sleep(2000); continue; }

        for (PIP_ADAPTER_INFO p = pAdapters; p && !g_linkUp; p = p->Next) {
            for (PIP_ADDR_STRING addr = &p->IpAddressList; addr && !g_linkUp; addr = addr->Next) {
                unsigned a, b, c, d;
                if (sscanf(addr->IpAddress.String, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
                    continue;
                if (!isLocalSubnet(a, b, c, d)) continue;
                if (a == 0 || a == 127 || d == 0 || d == 255) continue;
                // 扫描该子网 .1..254（先扫网关 .1，命中即停；否则兜底全段扫描）
                for (unsigned h = 1; h <= 254; h++) {
                    if (g_linkUp) break;
                    if (h == d) continue; // 跳过车机自身
                    char ip[32];
                    snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, h);
                    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                    if (s == INVALID_SOCKET) continue;
                    u_long mode = 1;
                    ioctlsocket(s, FIONBIO, &mode);
                    sockaddr_in sa; memset(&sa, 0, sizeof(sa));
                    sa.sin_family = AF_INET;
                    sa.sin_port   = htons((u_short)PHONE_PORT);
                    sa.sin_addr.s_addr = inet_addr(ip);
                    ::connect(s, (SOCKADDR*)&sa, sizeof(sa)); // 立即返回 WSAEWOULDBLOCK
                    fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
                    timeval tv; tv.tv_sec = 0; tv.tv_usec = 120000; // 120ms 探测超时
                    int r = select(0, NULL, &wf, NULL, &tv);
                    int err = 0, el = sizeof(err);
                    if (r == 1) getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
                    mode = 0; ioctlsocket(s, FIONBIO, &mode);
                    closesocket(s);
                    if (r == 1 && err == 0) {
                        AddCandidate(ip);
                        Log("scan: 探测到手机 %s:%d", ip, PHONE_PORT);
                        h = 255; // 该子网找到即停，避免无谓扫描
                    }
                }
            }
        }
        Sleep(3000); // 一轮扫描后歇一会
    }
    return 0;
}

static DWORD WINAPI DiscoveryThread(LPVOID) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    BOOL reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)DISCOVERY_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (SOCKADDR*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    // 探测手段 2（接口网关推导）立即做一次，之后持续监听信标(手段1)
    AddInterfaceGateways();

    fd_set rfds;
    timeval tv;
    char buf[256];
    while (g_discoveryOn) {
        FD_ZERO(&rfds);
        FD_SET(s, &rfds);
        tv.tv_sec = 1; tv.tv_usec = 0;
        int r = select(0, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            sockaddr_in from; int fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (SOCKADDR*)&from, &fl);
            if (n > 0) {
                buf[n] = 0;
                // 格式：GLOAI|<ip>|<port>
                if (strncmp(buf, "GLOAI|", 6) == 0) {
                    char* ip = buf + 6;
                    char* sep = strchr(ip, '|');
                    if (sep) { *sep = 0; AddCandidate(ip); }
                }
            }
        }
    }
    closesocket(s);
    return 0;
}

void NetClient::StartDiscovery() {
    InitializeCriticalSection(&g_csCand);
    if (g_discoveryOn) return;
    g_discoveryOn = true;
    CreateThread(NULL, 0, DiscoveryThread, NULL, 0, NULL);
    CreateThread(NULL, 0, SubnetScanThread, NULL, 0, NULL);
}

void NetClient::StopDiscovery() {
    g_discoveryOn = false;
}

void NetClient::SetLinkUp(bool up) {
    g_linkUp = up;
}

void NetClient::GetCandidates(const std::string& configIP, std::vector<std::string>& out) {
    out.clear();
    if (!configIP.empty()) out.push_back(configIP); // 显式覆盖优先
    EnterCriticalSection(&g_csCand);
    for (size_t i = 0; i < g_beaconIPs.size(); i++) out.push_back(g_beaconIPs[i]);
    LeaveCriticalSection(&g_csCand);
}
