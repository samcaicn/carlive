// net.cpp - 见 net.h
#include "net.h"
#include "log.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cstring>
#include <ctime>

static const BYTE MAGIC[4] = { 0x47, 0x4C, 0x4F, 0x41 }; // "GLOA"

// 发送互斥：主线程(触摸)与心跳线程会并发 send 同一 socket，无锁会导致两条消息字节交错、
// 对端协议解析错位。所有发送统一走 sendMsg 并在此加锁串行化。
static CRITICAL_SECTION g_csSend;

// 连接世代号（详见 net.h ConnEpoch）：Interlocked 递增，供后台线程确定性判断“本连接是否已作废”。
static volatile LONG g_epoch = 0;
long NetClient::ConnEpoch() { return g_epoch; }

// 单帧/单消息上限（收发两侧统一 8MB）：超过即视为损坏或异常流，直接跳过，
// 避免在 64MB 级车机上做一次足以触发 OOM 的巨量分配。
static const int MAX_FRAME_BYTES = 8 * 1024 * 1024;

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x1
#endif

// 关闭 Nagle：触摸/视频小包立即发出，降低交互延迟。
static void setNoDelay(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

// TCP 保活：对端异常掉线（无 RST）时让协议栈主动探活，配合心跳更快发现死链。
// WinCE 保活间隔由注册表决定，默认较长，这里仅开启开关；真正的断线判定仍由心跳线程负责。
static void setKeepAlive(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));
}

// 接收超时：保证任何 recv 不会永久阻塞——对端异常静默（无 RST、无数据）时 5s 内返回，
// 触发断链重连，作为 close() 之外的双保险。取 5s > 心跳间隔(3s)，避免正常空闲（仅有心跳）被误判断链。
static void setRecvTimeout(SOCKET s) {
    if (s == INVALID_SOCKET) return;
    int to = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
}

NetClient::NetClient() : m_sock(INVALID_SOCKET), m_codec(1) {
    WSADATA wsa = {0};
    WSAStartup(MAKEWORD(2,2), &wsa);
    InitializeCriticalSection(&g_csSend);
}

NetClient::~NetClient() { close(); DeleteCriticalSection(&g_csSend); WSACleanup(); }

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
    setKeepAlive(m_sock);
    setRecvTimeout(m_sock);
    if (::connect(m_sock, (SOCKADDR*)&sa, sizeof(sa)) == SOCKET_ERROR) {
        close(); return false;
    }
    m_connectedIP = buf; // 记录命中 IP（配合 device_id 记忆）
    // R8 兜底：即使握手没解析到 device_id，也把“上次连通过的 IP”记住（id="-" 占位），
    // 下次启动可直连，不再完全依赖信标/网关发现。
    static std::string s_lastSavedIP;
    if (m_connectedIP != s_lastSavedIP) { SaveKnownPhone("-", m_connectedIP); s_lastSavedIP = m_connectedIP; }
    InterlockedIncrement(&g_epoch); // 新连接：作废此前所有后台线程持有的 socket
    return true;
}

bool NetClient::connectTimeout(const std::wstring& host, int port, int timeoutMs) {
    close(); // 防御：丢弃任何残留 socket，避免重连路径下泄漏/复用旧连接
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
        // 记录本次命中 IP（与握手解析到的 device_id 配对落盘，实现“记住这台手机”）
        char ipbuf[64] = {0};
        WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, ipbuf, sizeof(ipbuf), NULL, NULL);
        m_connectedIP = ipbuf;
        // R8 兜底：无 device_id 也记住“上次连通过的 IP”（同 connect()，去重防频繁写盘）
        static std::string s_lastSavedIP;
        if (m_connectedIP != s_lastSavedIP) { SaveKnownPhone("-", m_connectedIP); s_lastSavedIP = m_connectedIP; }
        InterlockedIncrement(&g_epoch); // 新连接：作废此前所有后台线程持有的 socket
        return true;
    }
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
    // 串行化发送（触摸/心跳来自不同线程），并循环发送直到整条消息发完——
    // MJPEG 单帧可达数百 KB，一次 send 在阻塞 socket 上可能只发一部分，必须续发，否则对端收到截断帧。
    EnterCriticalSection(&g_csSend);
    const char* p = (const char*)msg.data();
    int total = (int)msg.size();
    int off = 0;
    bool ok = true;
    while (off < total) {
        int sent = ::send(m_sock, p + off, total - off, 0);
        if (sent <= 0) { ok = false; break; }
        off += sent;
    }
    LeaveCriticalSection(&g_csSend);
    if (!ok) { close(); return false; }
    return true;
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
    if (m_sock == INVALID_SOCKET) return;   // 未连接时直接丢弃，避免无谓加锁/失败发送
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
    // 上限与单帧限制对齐（原为 16MB：在 64MB 级车机上一次 resize 就足以触发 OOM）
    if (len < 0 || len > MAX_FRAME_BYTES + 9) return false;
    payload.resize(len);
    if (len > 0 && !readExact(payload.data(), len)) return false;
    return true;
}

bool NetClient::recvVideoFrame(VideoFrame& out) {
    BYTE type; std::vector<BYTE> payload;
    while (connected()) {
        if (!readMsg(type, payload)) { Log("recvVideoFrame: readMsg failed -> disconnect"); return false; }
        if (type == 0x01) { // HANDSHAKE：手机侧会带 device_id，解析并“记住这台手机”
            if (!payload.empty()) {
                std::string s((const char*)payload.data(), payload.size());
                const char* p = strstr(s.c_str(), "\"device_id\"");
                if (p) {
                    p = strchr(p, ':');
                    if (p) {
                        p++;
                        while (*p == ' ' || *p == '\t') p++;
                        if (*p == '"') {
                            p++;
                            const char* e = strchr(p, '"');
                            if (e && e > p) {
                                std::string id(p, (size_t)(e - p));
                                if (!id.empty()) {
                                    m_phoneId = id;
                                    if (!m_connectedIP.empty())
                                        SaveKnownPhone(id, m_connectedIP);
                                    Log("handshake: phone device_id=%s ip=%s (remembered)", id.c_str(), m_connectedIP.c_str());
                                }
                            }
                        }
                    }
                }
            }
            continue; // 握手不产出视频帧
        } else if (type == 0x03) { // VIDEO_FRAME
            if (payload.size() < 9) continue;
            out.isKey = (payload[0] != 0);
            out.timestamp = (payload[1]<<24)|(payload[2]<<16)|(payload[3]<<8)|payload[4];
            int dlen = (payload[5]<<24)|(payload[6]<<16)|(payload[7]<<8)|payload[8];
            if (dlen < 0 || (size_t)dlen > payload.size()-9) continue; // 长度越界：丢弃本消息继续
            // 防御：单帧超过 8MB 视为异常（损坏/恶意流），直接跳过并继续读下一帧，
            // 避免 64MB 级 WinCE 设备为异常帧分配巨量内存导致 OOM。
            if (dlen > MAX_FRAME_BYTES) { Log("recvVideoFrame: 单帧过大 %d 字节，跳过", dlen); continue; }
            // 反压：若内核收包缓冲仍堆积大量数据，说明本端解码跟不上发送节奏，
            // 直接丢弃本帧继续读下一帧（取最新），避免无意义解码与内存拷贝。
            u_long backlog = 0;
            if (ioctlsocket(m_sock, FIONREAD, &backlog) == 0 && backlog > 96*1024) {
                continue;
            }
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
        // 先递增世代号，让仍在 recv/send 的旧线程尽快自检退出：
        // 同一个 socket 句柄号可能在 closesocket 后被下一次连接立刻复用，
        // 旧线程继续 recv 会读到新连接的数据流，导致协议错位与解码错帧。
        InterlockedIncrement(&g_epoch);
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
static std::vector<std::string> g_priIPs;      // 接口网关推导 + 扫描确认（均经 8686 探测命中）
static std::string g_beaconIP;                 // 最高优先级：UDP 信标带来的手机真实 IP
static volatile bool g_discoveryOn = false;
static volatile bool g_linkUp      = false;    // 链路已连通（握手成功）→ 暂停扫描

// 前向声明：AddInterfaceGateways 在定义前需要它（其定义在文件靠后）
static bool probePort(unsigned a, unsigned b, unsigned c, unsigned d, int timeoutMs);

static bool HasIP(const std::vector<std::string>& v, const char* ip) {
    for (size_t i = 0; i < v.size(); i++) if (v[i] == ip) return true;
    return false;
}
static void AddPriority(const char* ip) {
    if (!ip || !*ip) return;
    EnterCriticalSection(&g_csCand);
    if (!HasIP(g_priIPs, ip)) g_priIPs.push_back(ip);
    LeaveCriticalSection(&g_csCand);
}
static int PriorityCount() {
    EnterCriticalSection(&g_csCand);
    int n = (int)g_priIPs.size();
    LeaveCriticalSection(&g_csCand);
    return n;
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
// 关键修正：网关/.1 必须【先探测 8686 通了才加为候选】——否则车机自身的 WiFi 路由器网关
// （如 192.168.43.1）会被当成手机反复连、每轮白等 1.5s。路由器没有 8686，探测必失败，自然被排除。
static void AddInterfaceGateways() {
    ULONG buflen = 0;
    if (GetAdaptersInfo(NULL, &buflen) != ERROR_BUFFER_OVERFLOW || buflen == 0)
        return;
    std::vector<BYTE> buf(buflen);
    PIP_ADAPTER_INFO pAdapters = (PIP_ADAPTER_INFO)buf.data();
    if (GetAdaptersInfo(pAdapters, &buflen) != NO_ERROR) return;
    for (PIP_ADAPTER_INFO p = pAdapters; p; p = p->Next) {
        // 优先使用网卡自身上报的真实网关（USB 共享下即手机地址）；探测通才加
        unsigned ga, gb, gc, gd;
        bool hasGw = (sscanf(p->GatewayList.IpAddress.String, "%u.%u.%u.%u", &ga, &gb, &gc, &gd) == 4
                      && (ga | gb | gc | gd) != 0);
        if (hasGw && probePort(ga, gb, gc, gd, 200)) {
            AddPriority(p->GatewayList.IpAddress.String);
            Log("disc: 网关命中 %s:%d", p->GatewayList.IpAddress.String, PHONE_PORT);
        }
        for (PIP_ADDR_STRING addr = &p->IpAddressList; addr; addr = addr->Next) {
            unsigned a, b, c, d;
            if (sscanf(addr->IpAddress.String, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
                continue;
            if (a == 0 || a == 127 || d == 0 || d == 255)
                continue; // 跳过 0.0.0.0 / 回环 / 网络号 / 广播
            // 兜底1（R8）：USB 共享拓扑下手机规范地址是 .129（Android rndis 网关），不是 .1！
            // 实测（2026-10-03 SD 日志）：CE 的 GetAdaptersInfo 可能不回报网关，旧兜底只推 .1，
            // 导致候选全空、`try connect #:8686` 空转。.129 探测补上这个洞。
            if (d != 129 && probePort(a, b, c, 129, 200)) {
                char gw[32];
                snprintf(gw, sizeof(gw), "%u.%u.%u.129", a, b, c);
                AddPriority(gw);
                Log("disc: .129(USB共享手机) 命中 %s:%d", gw, PHONE_PORT);
            }
            // 兜底2：网卡未上报网关时，推断本子网 .1 为网关（WiFi 热点拓扑手机=.1）；同样探测通才加
            if (!hasGw && d != 1) {
                char gw[32];
                snprintf(gw, sizeof(gw), "%u.%u.%u.1", a, b, c);
                if (probePort(a, b, c, 1, 200)) {
                    AddPriority(gw);
                    Log("disc: .1 命中 %s:%d", gw, PHONE_PORT);
                }
            }
        }
    }
}

// 对单个 IP 的 8686 端口做快速 TCP 探测：开放返回 true。超时短，避免拖慢连接。
static bool probePort(unsigned a, unsigned b, unsigned c, unsigned d, int timeoutMs) {
    char ip[32]; snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    setNoDelay(s);
    u_long mode = 1; ioctlsocket(s, FIONBIO, &mode);
    sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((u_short)PHONE_PORT);
    sa.sin_addr.s_addr = inet_addr(ip);
    ::connect(s, (SOCKADDR*)&sa, sizeof(sa)); // 立即返回 WSAEWOULDBLOCK
    fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
    timeval tv; tv.tv_sec = timeoutMs/1000; tv.tv_usec = (timeoutMs%1000)*1000;
    int r = select(0, NULL, &wf, NULL, &tv);
    int err = 0, el = sizeof(err);
    if (r == 1) getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
    mode = 0; ioctlsocket(s, FIONBIO, &mode);
    closesocket(s);
    return (r == 1 && err == 0);
}

// 探测手段 3（主动扫描，【轻量】防止单核 WinCE 卡顿）：
//   阶段1：优先探测“真实网关 / 子网 .1”——USB 共享网络下手机即网关，几乎必中，秒级连接；
//   阶段2：仅当【毫无任何高可信候选】（非标准 tether 拓扑/异常网络）才做全段扫描兜底，
//          且每地址 Sleep(8) 节流，避免 254 次探测把 CPU 占满导致系统卡死。
//   每轮扫描后 Sleep(5000)，不空转。链路已连上时整段暂停。
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

        // 阶段1：快速确认网关 / .1（轻量，通常一轮即命中）
        for (PIP_ADAPTER_INFO p = pAdapters; p && !g_linkUp; p = p->Next) {
            unsigned ga, gb, gc, gd;
            bool hasGw = (sscanf(p->GatewayList.IpAddress.String, "%u.%u.%u.%u", &ga, &gb, &gc, &gd) == 4
                          && (ga | gb | gc | gd) != 0);
            for (PIP_ADDR_STRING addr = &p->IpAddressList; addr && !g_linkUp; addr = addr->Next) {
                unsigned a, b, c, d;
                if (sscanf(addr->IpAddress.String, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
                if (!isLocalSubnet(a, b, c, d)) continue;
                if (a == 0 || a == 127 || d == 0 || d == 255) continue;
                if (hasGw) {
                    if (probePort(ga, gb, gc, gd, 100)) {
                        AddPriority(p->GatewayList.IpAddress.String);
                        Log("scan: 网关命中 %s:%d", p->GatewayList.IpAddress.String, PHONE_PORT);
                    }
                } else if (d != 1) {
                    if (probePort(a, b, c, 1, 100)) {
                        char gw[32]; snprintf(gw, sizeof(gw), "%u.%u.%u.1", a, b, c);
                        AddPriority(gw);
                        Log("scan: .1 命中 %s:%d", gw, PHONE_PORT);
                    }
                }
                // R8：USB 共享手机=.129（Android 规范），与网关/.1 探测互补，防止候选全空
                if (d != 129 && probePort(a, b, c, 129, 100)) {
                    char gw[32]; snprintf(gw, sizeof(gw), "%u.%u.%u.129", a, b, c);
                    AddPriority(gw);
                    Log("scan: .129(USB共享手机) 命中 %s:%d", gw, PHONE_PORT);
                }
            }
        }

        // 阶段2：仅当无任何高可信候选时，全段扫描兜底（节流保护 CPU）
        if (!g_linkUp && PriorityCount() == 0) {
            for (PIP_ADAPTER_INFO p = pAdapters; p && !g_linkUp; p = p->Next) {
                for (PIP_ADDR_STRING addr = &p->IpAddressList; addr && !g_linkUp; addr = addr->Next) {
                    unsigned a, b, c, d;
                    if (sscanf(addr->IpAddress.String, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
                    if (!isLocalSubnet(a, b, c, d)) continue;
                    if (a == 0 || a == 127 || d == 0 || d == 255) continue;
                    for (unsigned h = 1; h <= 254 && !g_linkUp; h++) {
                        if (h == d) continue; // 跳过车机自身
                        if (probePort(a, b, c, h, 60)) {
                            char ip[32]; snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, h);
                            AddPriority(ip);
                            Log("scan: 全段命中 %s:%d", ip, PHONE_PORT);
                            h = 255; // 该子网找到即停
                        }
                        Sleep(8); // 节流：避免单核 WinCE CPU 满载导致系统卡顿
                    }
                }
            }
        }
        Sleep(5000); // 一轮后歇久一点，不空转
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
                    if (sep) {
                        *sep = 0; AddPriority(ip);
                        EnterCriticalSection(&g_csCand);
                        bool changed = (g_beaconIP != ip);   // R8：信标来源落日志（仅在 IP 变化时记，防刷屏）
                        g_beaconIP = ip;
                        LeaveCriticalSection(&g_csCand);
                        if (changed) Log("disc: beacon 收到手机 IP %s", ip);
                    }
                }
            }
        }
    }
    closesocket(s);
    return 0;
}

static HANDLE g_hDiscThread = NULL;
static HANDLE g_hScanThread = NULL;

///////////////////////////////////////////////////////////////////////////////
// 记住手机（known_phones.cfg）：连上后把 device_id + 上次 IP 落盘，
// 下次启动优先直连这些 IP，免等 UDP 信标，做到“下次记住这台手机的连接参数”。
///////////////////////////////////////////////////////////////////////////////
struct KnownPhone { std::string id; std::string ip; long long ts; };
static std::vector<KnownPhone> g_knownPhones;  // 按最近成功倒序
static std::vector<std::string> g_knownIPs;    // 仅 IP，供候选前置
static CRITICAL_SECTION g_csKnown;
static bool g_knownInit = false;
static volatile bool g_deferKnown = false;   // R12：known 候选让路开关（SetDeferKnown）

void NetClient::SetDeferKnown(bool defer) { g_deferKnown = defer; }

static std::wstring knownPhonesPath() {
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR* p = wcsrchr(path, L'\\');
        if (p) wcscpy(p + 1, L"known_phones.cfg");
    }
    return std::wstring(path);
}

void NetClient::LoadKnownPhones() {
    if (!g_knownInit) { InitializeCriticalSection(&g_csKnown); g_knownInit = true; }
    EnterCriticalSection(&g_csKnown);
    g_knownPhones.clear(); g_knownIPs.clear();
    HANDLE hf = CreateFile(knownPhonesPath().c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        char buf[512] = {0}; DWORD rd = 0; std::string content;
        while (ReadFile(hf, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0) { buf[rd] = 0; content += buf; }
        CloseHandle(hf);
        size_t pos = 0;
        while (pos < content.size()) {
            size_t nl = content.find('\n', pos);
            std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? content.size() : nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // 行格式：device_id ip ts
            char id[128] = {0}, ip[64] = {0}; long long ts = 0;
            if (sscanf(line.c_str(), "%127s %63s %lld", id, ip, &ts) >= 2) {
                if (id[0] && ip[0]) {
                    g_knownPhones.push_back(KnownPhone{id, ip, ts});
                    if (!HasIP(g_knownIPs, ip)) g_knownIPs.push_back(ip);
                }
            }
        }
        Log("known_phones: loaded %d", (int)g_knownPhones.size());
    }
    LeaveCriticalSection(&g_csKnown);
}

void NetClient::SaveKnownPhone(const std::string& id, const std::string& ip) {
    if (id.empty() || ip.empty()) return;
    if (!g_knownInit) { InitializeCriticalSection(&g_csKnown); g_knownInit = true; }
    EnterCriticalSection(&g_csKnown);
    bool found = false;
    for (size_t i = 0; i < g_knownPhones.size(); i++) {
        if (g_knownPhones[i].id == id) { g_knownPhones[i].ip = ip; g_knownPhones[i].ts = (long long)time(NULL); found = true; break; }
    }
    if (!found) g_knownPhones.push_back(KnownPhone{id, ip, (long long)time(NULL)});
    // R12：上限 8 条，超出删最旧——防长期使用后文件无限膨胀（SD 卡写入量 + 启动解析开销）
    while (g_knownPhones.size() > 8) {
        size_t oldest = 0;
        for (size_t i = 1; i < g_knownPhones.size(); i++)
            if (g_knownPhones[i].ts < g_knownPhones[oldest].ts) oldest = i;
        g_knownPhones.erase(g_knownPhones.begin() + oldest);
    }
    // 重排：刚连上的排最前（最近优先）
    // 写回文件
    HANDLE hf = CreateFile(knownPhonesPath().c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        // 按 ts 倒序写
        std::vector<KnownPhone> sorted = g_knownPhones;
        for (size_t i = 0; i + 1 < sorted.size(); i++)
            for (size_t j = i + 1; j < sorted.size(); j++)
                if (sorted[j].ts > sorted[i].ts) std::swap(sorted[i], sorted[j]);
        std::string out;
        for (size_t i = 0; i < sorted.size(); i++)
            out += sorted[i].id + " " + sorted[i].ip + " " + std::to_string(sorted[i].ts) + "\n";
        DWORD wr = 0; WriteFile(hf, out.c_str(), (DWORD)out.size(), &wr, NULL);
        CloseHandle(hf);
        Log("known_phones: saved %d (id=%s ip=%s)", (int)sorted.size(), id.c_str(), ip.c_str());
    }
    LeaveCriticalSection(&g_csKnown);
}

void NetClient::GetKnownIPs(std::vector<std::string>& out) {
    out.clear();
    if (!g_knownInit) return;
    EnterCriticalSection(&g_csKnown);
    for (size_t i = 0; i < g_knownIPs.size(); i++) if (!HasIP(out, g_knownIPs[i].c_str())) out.push_back(g_knownIPs[i]);
    LeaveCriticalSection(&g_csKnown);
}

void NetClient::StartDiscovery() {
    InitializeCriticalSection(&g_csCand);
    if (g_discoveryOn) return;
    LoadKnownPhones();   // 启动时读取“记住的手机”，供本轮回合优先直连
    g_discoveryOn = true;
    g_hDiscThread = CreateThread(NULL, 0, DiscoveryThread, NULL, 0, NULL);
    g_hScanThread = CreateThread(NULL, 0, SubnetScanThread, NULL, 0, NULL);
}

void NetClient::StopDiscovery() {
    g_discoveryOn = false;
    // 等待两个探测线程真正退出并回收句柄：此前 CreateThread 返回的句柄从未 CloseHandle，
    // 长期运行会持续泄漏内核对象；更关键的是退出阶段若线程仍在跑，
    // 会继续访问已被 delete 的渲染器/网络对象（崩溃）。
    if (g_hDiscThread) {
        if (WaitForSingleObject(g_hDiscThread, 8000) == WAIT_TIMEOUT) Log("warn: discovery thread join timeout");
        CloseHandle(g_hDiscThread); g_hDiscThread = NULL;
    }
    if (g_hScanThread) {
        if (WaitForSingleObject(g_hScanThread, 8000) == WAIT_TIMEOUT) Log("warn: scan thread join timeout");
        CloseHandle(g_hScanThread); g_hScanThread = NULL;
    }
}

void NetClient::SetLinkUp(bool up) {
    g_linkUp = up;
}

void NetClient::ClearScanned() {
    EnterCriticalSection(&g_csCand);
    g_priIPs.clear();   // 仅清扫描/网关候选；g_beaconIP 保留（信标持续刷新）
    LeaveCriticalSection(&g_csCand);
}

void NetClient::GetCandidates(const std::string& configIP, std::vector<std::string>& out) {
    out.clear();
    // 优先级：config.txt 显式覆盖 > 【已知手机上次IP(记住这台手机)】 > UDP 信标真实IP(最高可信) > 接口网关/扫描确认
    std::vector<std::string> tmp;
    if (!configIP.empty()) tmp.push_back(configIP);
    if (!g_deferKnown) {   // R12：连续失败多轮后本轮跳过 known，给信标/网关让路（防失效 IP 拖死每轮）
        std::vector<std::string> known; GetKnownIPs(known);
        for (size_t i = 0; i < known.size(); i++) tmp.push_back(known[i]);
    }
    EnterCriticalSection(&g_csCand);
    if (!g_beaconIP.empty()) tmp.push_back(g_beaconIP);
    for (size_t i = 0; i < g_priIPs.size(); i++) tmp.push_back(g_priIPs[i]);
    LeaveCriticalSection(&g_csCand);
    // 保序去重 + 过滤空串（R8：旧版空候选会打出 `try connect #:8686` 空转、污染日志）
    for (size_t i = 0; i < tmp.size(); i++) {
        if (!tmp[i].empty() && !HasIP(out, tmp[i].c_str())) out.push_back(tmp[i]);
    }
}
