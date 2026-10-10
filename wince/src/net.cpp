// net.cpp - 见 net.h
#include "net.h"
#include "log.h"
#include "tcptransport.h"
// R27：USB_NET_ONLY 构建变体 —— 本二进制完全不含 ADB 隧道实现（adb.cpp/rsa.cpp 也不进链接）。
// 用途：真车 A/B 对比。ADB 模块含 RSA 密钥握手与 ADB 协议解析，占可观的代码与静态数据量；
// 砍掉它能一次性排除"ADB 相关静态初始化/类构造在 CE 上出问题"这一整类可能，
// 同时验证 exe 体积与静态数据是否触及 WinCE 的加载限制。
#ifndef USB_NET_ONLY
#include "adb.h"
#endif
#include "thread.h"
#include "crashlog.h"
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <cstring>
#include <ctime>

static const BYTE MAGIC[4] = { 0x54, 0x55, 0x50, 0x54 }; // "TUPT"

// 发送互斥：主线程(触摸)与心跳线程会并发 send 同一 socket，无锁会导致两条消息字节交错、
// 对端协议解析错位。所有发送统一走 sendMsg 并在此加锁串行化。
static CRITICAL_SECTION g_csSend;

// 连接世代号（详见 net.h ConnEpoch）：Interlocked 递增，供后台线程确定性判断“本连接是否已作废”。
static volatile LONG g_epoch = 0;

// R20：最后一次成功收到对端任意字节的时刻（readExact 每次成功进入时刷新）。
// 用于接收侧活性判定 —— 区分"对端真死"与"对端只是安静（静帧/手机端未实现心跳）"。
static volatile LONG g_lastRecvTick = 0;
DWORD NetClient::LastRecvTick() { return (DWORD)g_lastRecvTick; }
void NetClient::ResetRecvTick() { g_lastRecvTick = GetTickCount(); }
long NetClient::ConnEpoch() { return g_epoch; }

// 连接模式：默认直连（USB 网络共享）。可在 config.txt 设 mode=usb_adb 切到 ADB 隧道。
static int s_mode = NetClient::CONN_MODE_USB_NET;
void NetClient::SetMode(int m) {
#ifdef USB_NET_ONLY
    // 单 usb_net 变体：无论 config.txt 写什么都落到 USB 共享网络直连。
    (void)m;
    s_mode = CONN_MODE_USB_NET;
#else
    if (m == CONN_MODE_USB_ADB) s_mode = CONN_MODE_USB_ADB;
    else s_mode = CONN_MODE_USB_NET;
#endif
    Log("NetClient: 模式切换为 %s", ModeName());
}
int NetClient::GetMode() { return s_mode; }
const char* NetClient::ModeName() {
    return s_mode == CONN_MODE_USB_ADB ? "usb_adb" : "usb_net";
}

// 单帧/单消息上限（收发两侧统一 8MB）：超过即视为损坏或异常流，直接跳过，
// 避免在 64MB 级车机上做一次足以触发 OOM 的巨量分配。
static const int MAX_FRAME_BYTES = 8 * 1024 * 1024;

NetClient::NetClient() : m_transport(NULL), m_codec(1) {
    WSADATA wsa = {0};
    WSAStartup(MAKEWORD(2,2), &wsa);
    InitializeCriticalSection(&g_csSend);
}

NetClient::~NetClient() { close(); DeleteCriticalSection(&g_csSend); WSACleanup(); }

bool NetClient::connect(const std::wstring& host, int port) {
    close();
    // 与 connectTimeout 保持一致：按模式选传输（ADB 隧道 vs 直连），
    // 避免将来有人调用本接口时落到错误的底层连接。
#ifdef USB_NET_ONLY
    m_transport = new TcpTransport();
#else
    if (s_mode == CONN_MODE_USB_ADB) m_transport = new AdbTransport();
    else m_transport = new TcpTransport();
#endif
    if (!m_transport->connect(host, port, 10000)) { delete m_transport; m_transport = NULL; return false; }
    // 记录命中 IP（配合 device_id 记忆）
    char ipbuf[64] = {0};
    WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, ipbuf, sizeof(ipbuf), NULL, NULL);
    m_connectedIP = ipbuf;
    static std::string s_lastSavedIP;
    if (m_connectedIP != s_lastSavedIP) { SaveKnownPhone("-", m_connectedIP); s_lastSavedIP = m_connectedIP; }
    InterlockedIncrement(&g_epoch);
    return m_transport->connected();
}

bool NetClient::connectTimeout(const std::wstring& host, int port, int timeoutMs) {
    close(); // 防御：丢弃任何残留连接，避免重连路径下泄漏/复用旧连接
    // 按模式选择底层传输：usb_adb 走 ADB 隧道（连手机 5555 → OPEN tcp:8686），
    // 其余直连手机 8686（原有行为）。候选 host 都是手机 IP，ADB 模式忽略 port（用 5555）。
#ifdef USB_NET_ONLY
    m_transport = new TcpTransport();
#else
    if (s_mode == CONN_MODE_USB_ADB) {
        char ipbuf[64] = {0};
        WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, ipbuf, sizeof(ipbuf), NULL, NULL);
        Log("NetClient: 以 ADB 模式连接 %s（adbd:5555 → OPEN tcp:8686）", ipbuf);
        m_transport = new AdbTransport();
    } else {
        m_transport = new TcpTransport();
    }
#endif
    if (!m_transport->connect(host, port, timeoutMs)) {
        delete m_transport; m_transport = NULL;
        return false;
    }
    // 记录本次命中 IP（与握手解析到的 device_id 配对落盘，实现“记住这台手机”）
    char ipbuf[64] = {0};
    WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, ipbuf, sizeof(ipbuf), NULL, NULL);
    m_connectedIP = ipbuf;
    static std::string s_lastSavedIP;
    if (m_connectedIP != s_lastSavedIP) { SaveKnownPhone("-", m_connectedIP); s_lastSavedIP = m_connectedIP; }
    InterlockedIncrement(&g_epoch); // 新连接：作废此前所有后台线程持有的 socket
    return true;
}

bool NetClient::sendMsg(BYTE type, const BYTE* payload, int len) {
    if (!m_transport || !m_transport->connected()) return false;
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
    // write() 已实现“精确写满 n 字节或断链返回 false”，无需上层续发。
    bool ok = m_transport->write(msg.data(), (int)msg.size());
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
    if (!m_transport || !m_transport->connected()) return;   // 未连接时直接丢弃，避免无谓加锁/失败发送
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
    // 防御：未建连（m_transport 为 NULL）时直接失败，避免空指针解引用。
    if (!m_transport) return false;
    // R20接收侧活性：每收到任意字节就刷新时间戳。上层心跳线程据此判断
    //   "对端真的死了（长时间无任何字节）" vs "对端只是安静（静帧/无心跳）"。
    //   旧实现只看 sendHeartbeat() 的返回值，而 send 成功仅代表本地写缓冲接受了字节，
    //   对端已死时 TCP 重传机制会让 send 持续"成功"数十分钟 → 永远不判断线。
    g_lastRecvTick = GetTickCount();
    // ITransport::read() 已实现“精确读满 n 字节或断链返回 false”，无需上层续读。
    return m_transport->read(buf, n);
}

// R20：上次收包失败是否只是接收超时（对端仍在线，只是安静）。
bool NetClient::lastRecvWasTimeout() {
    if (!m_transport) return false;
    return m_transport->lastFailWasTimeout();
}

bool NetClient::readMsg(BYTE& type, std::vector<BYTE>& payload) {
    if (!m_transport) return false;   // 防御：未建连时直接失败
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
            // R20 修复"连上却永久黑屏"：原逻辑 `backlog > 96KB 就 continue` 是无滞回的
            //   bang-bang 控制器——单核 ARM 解码 800×480 约 20-50ms（12-25fps 上限），
            //   手机按 15fps 推送。只要码率略高于解码能力，backlog 就持续超阈值、
            //   于是**每一帧都被丢弃**，backlog 永不下降 → 稳定态就是"永远丢帧"，
            //   画面停在黑屏（用户看到"已连接，镜像中"却什么都没）。
            // 改为：① 高水位丢弃（96KB）；② 低水位(32KB)以下**强制放行**至少一帧，
            //   让缓冲有机会排空；③ 每 500ms 至少放行一帧作为兜底，保证画面会更新。
            int backlog = m_transport ? m_transport->backlog() : 0;
            static DWORD s_lastForceFrame = 0;
            DWORD nowTick = GetTickCount();
            bool forceFrame = (nowTick - s_lastForceFrame > 500);   // 兜底：每 500ms 必放一帧
            if (backlog > 96*1024 && !forceFrame) {
                continue;   // 真堆积（非兜底周期）才丢
            }
            if (forceFrame) s_lastForceFrame = nowTick;
            // R20：复用 payload 缓冲，避免 15fps 持续为每帧新建/销毁 vector 造成堆抖动
            //   （WinCE 64MB 级设备上这种抖动就是卡顿与碎片化的主因）。
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
    if (m_transport) {
        // 先递增世代号，让仍在 recv/send 的旧线程尽快自检退出：
        // 同一个底层句柄号可能在 close 后被下一次连接立刻复用，
        // 旧线程继续读会读到新连接的数据流，导致协议错位与解码错帧。
        InterlockedIncrement(&g_epoch);
        m_transport->close();
        delete m_transport;
        m_transport = NULL;
    }
}

///////////////////////////////////////////////////////////////////////////////
// 自动发现（纯探测，不写死任何地址）
// 三级探测，全部动态：
//   1) UDP 信标（端口 8687）：手机侧每隔 1s 向广播地址发送 "TUPTUP|<ip>|<port>"，
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
static std::vector<std::string> g_beaconIPs;   // UDP 信标带来的手机真实 IP（**多台**，按到达顺序）
static std::string g_beaconIP;                 // 最近一次信标 IP（仅用于日志比对，不参与候选唯一性）
// 【test13/14】主线程预创建的信标 socket（TLTP_DISC_SOCK_MAIN / TLTP_DISC_BIND_MAIN）——
// 隔离"非主线程做网络栈调用"是否为本 CE ROM 的崩溃条件。
static SOCKET g_discSock = INVALID_SOCKET;
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
    // USB tether（Android rndis / 以太网共享）在部分 ROM 上会分配 100.64.0.0/10（CGNAT 段），
    // 旧判定把它当公网地址 → 整个子网被判"非本地"，扫描线程直接跳过该网卡，
    // 候选恒为空（实测症状：日志"0 个候选" + 永不停歇重连）。补上这一段。
    if (a == 100 && b >= 64 && b <= 127) return true;  // 100.64.0.0/10
    return false;
}

// 解析 "a.b.c.d" → 4 字节；失败返回 false。严格逐段校验，不接受 1234 / 0x7f.1 等 inet_addr 宽松形态。
static bool parseIPv4(const char* s, unsigned out[4]) {
    if (!s || !*s) return false;
    unsigned v[4] = {0, 0, 0, 0};
    int consumed = 0;
    if (sscanf(s, "%u.%u.%u.%u%n", &v[0], &v[1], &v[2], &v[3], &consumed) != 4) return false;
    if (!s[consumed]) return false;                    // 必须整行消费完，防 "1.2.3.4abc"
    for (int i = 0; i < 4; i++) if (v[i] > 255) return false;
    for (int i = 0; i < 4; i++) out[i] = v[i];
    return true;
}

// 本机是否持有与 target 同一 /24 子网的地址。
// 用途：config.txt 里写死的 IP 只有落在本机任一网卡子网内才值得优先尝试；
// 否则它属于别的网段（典型：手机换网络后 IP 段整个变了），留着只会每轮白等一个超时，
// 并把真正的自动发现候选挤到后面 —— 实测这就是"日志刷 6.8 小时、全是同一个死 IP"的根因。
//
// R21 曾把这里的 buflen 上限当作"启动即闪退"的修复并归因于巨量分配 OOM，
// R22 又把崩溃归因于"Log()嵌套加锁"—— **这两个归因都是错的**（见 thread.h）：
//   · 真正原因是 WinCE 忽略 CreateThread 的 dwStackSize、线程默认栈仅 64KB，
//     ConnThread 一进函数就栈溢出，进程瞬间消失，位置恰好在 discovery 日志之后；
//   · "Log(参数)→LocalIPv4()→Log()"也不存在嵌套持锁：实参在 EnterCriticalSection
//     之前就求值完了。
// 但 R21 加的三重防御本身是**有价值且应保留**的：buflen 上限封顶（防脏数据导致巨量分配）、
// resize 包 try/catch（防异常逃逸线程）、分配后 memset清零（防未初始化结构里的 Next 野指针）。
// R27：此开关由 config.txt 的 `noLocalIP=1` 打开。打开后**所有**走 GetAdaptersInfo 的路径
// 全部跳过网卡枚举（返回"无网卡"），四个调用点无需各自改动。
// 存在的理由：这是当前唯一还没排查干净的崩溃嫌疑——crash.log 已把崩溃点锁到
// LocalIPv4/GetAdaptersInfo 附近，但需要确定性证据。开关打开后若不再闪退，
// 即可 100% 确认元凶在这条路径，且**不必重新构建/CI**，改 SD 卡上的 config.txt 即可反复 A/B。
static bool g_skipLocalIP = false;
void NetClient::SetSkipLocalIP(bool on) { g_skipLocalIP = on; }

// R29：ADB 模式下【完全不需要】GetAdaptersInfo / 网卡枚举——
//   手机经 USB 网络共享后，adbd 在 5555 监听，车机用 ADB 协议鉴权后 OPEN "tcp:8686"
//   即打通 TUPT；候选 IP 直接来自 UDP 8687 信标（recvfrom 拿源 IP，零网卡依赖）。
//   因此 ADB 模式下所有走 GetAdaptersInfo 的路径一律短路返回，既消除
//   "并发 GetAdaptersInfo 踩堆导致 ConnThread::readConfig 崩溃"，也让 ADB 成为最稳的连接
//   方式（亿连等车机投屏正是走这条路，权限最高、无需猜网段）。
//   usb_net 模式仍维持原枚举逻辑；noLocalIP 开关效果与 ADB 等价（都无法判定子网，
//   故信标/配置 IP 全部接受）。
static bool adbMode() { return s_mode == NetClient::CONN_MODE_USB_ADB; }

// R29：安全封装 GetAdaptersInfo，彻底消除“并发枚举踩堆”崩溃。
// 旧实现的崩溃链：StartDiscovery（main.cpp:863）在 ConnThread 之前就 spawn 了
// SubnetScanThread，后者在车机这个 CE ROM 上调 GetAdaptersInfo 时，首次调用报告的
// buflen 偏小，按该值分配缓冲后再调一次 → 第二次写入越界踩坏进程堆；随后 ConnThread
// 在 readConfig() 里做 content += buf 的堆分配时踩到坏块 → 进程消失，
// crash.log 恰好停在 ConnThread:readConfig（见 R13~R28 的全部排查）。
// 修复：① 分配 2x 缓冲 + 最多 4 次重试，保证缓冲始终足够大，杜绝越界；
//       ② ADB / noLocalIP 模式下本函数直接返回 false，上层一律走“无网卡”分支，
//          不再触碰 GetAdaptersInfo（ADB 模式候选全来自 UDP 8687 信标，根本不需要它）。
static bool GetAdaptersSafe(std::vector<BYTE>& buf) {
    if (adbMode() || g_skipLocalIP) return false;
    ULONG need = 0;
    if (GetAdaptersInfo(NULL, &need) != ERROR_BUFFER_OVERFLOW) {
        // 首次调用非 OVERFLOW：要么真无网卡(need 仍可能被置 0)，要么 ROM 异常。
        // need==0 视为无网卡；否则按报告值继续（极少见的 ROM 行为）。
        if (need == 0) return false;
    }
    if (need == 0) need = 16 * 1024;   // 兜底初值，针对“首次调用就把 need 置 0”的退化 ROM
    for (int tries = 0; tries < 4; tries++) {
        ULONG cap = need * 2;          // 预留 2 倍，吸收 ROM 报告的偏差（核心修复点）
        if (cap > 256 * 1024) cap = 256 * 1024;
        try { buf.resize(cap); } catch (...) { return false; }
        if (buf.empty()) return false;
        memset(&buf[0], 0, cap);
        ULONG r = GetAdaptersInfo((PIP_ADAPTER_INFO)&buf[0], &need);
        if (r == NO_ERROR) return true;
        if (r != ERROR_BUFFER_OVERFLOW) return false;   // 其它错误：放弃枚举
        // r == OVERFLOW：need 已被更新为真实需求，下一轮以 2x 重试
    }
    return false;
}

bool NetClient::IsSameSubnet(const char* ip) {
    unsigned t[4];
    if (!parseIPv4(ip, t)) return false;
    std::vector<BYTE> buf;
    if (!GetAdaptersSafe(buf)) return false;   // R29：含 ADB/noLocalIP 短路 + 2x 安全缓冲
    PIP_ADAPTER_INFO pAdapters = (PIP_ADAPTER_INFO)&buf[0];
    CrashSetStage("IsSameSubnet:walkChain");
    for (PIP_ADAPTER_INFO p = pAdapters; p; p = p->Next) {
        for (PIP_ADDR_STRING addr = &p->IpAddressList; addr; addr = addr->Next) {
            unsigned a, b, c, d;
            if (sscanf(addr->IpAddress.String, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
            if (a == 0 || a == 127) continue;
            if (a == t[0] && b == t[1] && c == t[2]) return true;
        }
    }
    return false;
}

// 本机当前主地址（判定网段变化用）：取第一个私有网段 IPv4，取不到返回空。
std::string NetClient::LocalIPv4() {
    std::vector<BYTE> buf;
    if (!GetAdaptersSafe(buf)) return "";   // R29：含 ADB/noLocalIP 短路 + 2x 安全缓冲
    PIP_ADAPTER_INFO pAdapters = (PIP_ADAPTER_INFO)&buf[0];
    CrashSetStage("LocalIPv4:walkChain");
    for (PIP_ADAPTER_INFO p = pAdapters; p; p = p->Next) {
        for (PIP_ADDR_STRING addr = &p->IpAddressList; addr; addr = addr->Next) {
            unsigned a, b, c, d;
            if (sscanf(addr->IpAddress.String, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
            if (a == 0 || a == 127 || d == 0 || d == 255) continue;
            if (!isLocalSubnet(a, b, c, d)) continue;
            char out[32]; snprintf(out, sizeof(out), "%u.%u.%u.%u", a, b, c, d);
            return std::string(out);
        }
    }
    return "";
}

// 探测手段 2：枚举本机接口，动态推导手机（网关/服务端）地址，绝不写死段号。
// 关键修正：网关/.1 必须【先探测 8686 通了才加为候选】——否则车机自身的 WiFi 路由器网关
// （如 192.168.43.1）会被当成手机反复连、每轮白等 1.5s。路由器没有 8686，探测必失败，自然被排除。
static void AddInterfaceGateways() {
    std::vector<BYTE> buf;
    if (!GetAdaptersSafe(buf)) return;   // R29：含 ADB/noLocalIP 短路 + 2x 安全缓冲
    PIP_ADAPTER_INFO pAdapters = (PIP_ADAPTER_INFO)&buf[0];
    CrashSetStage("AddInterfaceGateways:walkChain");
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

// 关闭 Nagle：探测/连接阶段小包（SYN 探测、握手首包）要立刻发出，不能等凑批，否则首连延迟被放大。
static void setNoDelay(SOCKET s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}

// 对单个 IP 的 8686 端口做快速 TCP 探测：开放返回 true。超时短，避免拖慢连接。
static bool probePort(unsigned a, unsigned b, unsigned c, unsigned d, int timeoutMs) {
    char ip[32]; snprintf(ip, sizeof(ip), "%u.%u.%u.%u", a, b, c, d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    setNoDelay(s);
    // R15：探测会高频新建/关闭 socket，USB 共享拓扑下一旦触发全段扫描，短时间内成百上千个
    // 半开连接进入 TIME_WAIT；WinCE 默认本地端口池很小，耗尽后新 socket() 直接失败，
    // 导致扫描“看起来在跑却永远发现不了手机”。允许地址复用，避免这一隐形失败路径。
    int reuse = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
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
    // R23 存活哨兵：本线程第一条语句。见 thread.h —— WinCE 线程默认栈仅 64KB，
    // 栈上要放 std::vector<BYTE> buf 并调 GetAdaptersInfo/probePort/sscanf，
    // 64KB 会溢出并连带杀掉整个进程。真车日志曾稳定停在本行之前。
    Log("[stage] SubnetScanThread entered");
    while (g_discoveryOn) {
        if (g_linkUp) { Sleep(1000); continue; }   // 已连上：暂停扫描，不浪费资源/不打扰手机

        CrashSetStage("SubnetScan:GetAdaptersSafe");
        std::vector<BYTE> buf;
        if (!GetAdaptersSafe(buf)) { Sleep(2000); continue; }   // R29：2x 安全缓冲，杜绝越界踩堆
        PIP_ADAPTER_INFO pAdapters = (PIP_ADAPTER_INFO)&buf[0];
        CrashSetStage("SubnetScan:walkChain");

        // R19 首次连接健壮性：WinCE 开机后 WiFi 网卡往往**几十秒后才拿到 IP**，
        // 而旧的 AddInterfaceGateways() 只在发现线程启动那一刻调一次 ——
        // 那时网卡还没地址 → 网关候选永远补不上 → 首次连接只能干等。
        // 现每轮扫描前都重新推导一次网关：网卡一旦就绪，下一轮（≤5s）自动补上候选。
        AddInterfaceGateways();

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
    // R23 存活哨兵：本线程第一条语句。见 thread.h（WinCE 默认栈仅 64KB）。
    Log("[stage] DiscoveryThread entered");
#if defined(TLTP_DISC_NO_SOCKET)
    // 【test10】信标线程纯空转——跳过 socket/setsockopt/bind/select 全部网络栈调用。
    // 真车日志钉死的死亡窗口 = entered 之后、bind 结果日志之前（这段只有 Winsock 调用）。
    Log("[dbg] disc: test10 空转模式（不创建任何 socket）");
    while (g_discoveryOn) Sleep(500);
    return 0;
#endif
#if defined(TLTP_DISC_WSA_SELF)
    // 【test15】线程内自行 WSAStartup——验证此 CE ROM 是否要求每线程自己初始化 Winsock。
    WSADATA wsa;
    int wsaRet = WSAStartup(MAKEWORD(2, 2), &wsa);
    Log("[dbg] disc: test15 线程内 WSAStartup -> %d", wsaRet);
#endif
#if defined(TLTP_DISC_SOCK_MAIN)
    // 【test13/14】socket 已在主线程（StartDiscovery）预创建，本线程直接使用——
    // 隔离"非主线程调用 socket()"这一步。
    SOCKET s = g_discSock;
    if (s == INVALID_SOCKET) { Log("disc: 主线程预建 socket 无效，信标不可用"); return 0; }
    Log("[dbg] disc: 使用主线程预建 socket");
#else
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { Log("disc: 创建 UDP socket 失败 (WSA=%d)，信标发现不可用", WSAGetLastError()); return 0; }
    Log("[dbg] disc: 线程内 socket() 成功");
#endif
    BOOL reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#if defined(TLTP_DISC_SOCK_ONLY)
    // 【test11】socket()+setsockopt 之后立即关闭退出——最小化隔离"线程内创建 socket"。
    Log("[dbg] disc: test11 socket() 成功，立即 closesocket 退出线程");
    closesocket(s);
    return 0;
#endif
    sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)DISCOVERY_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
#if defined(TLTP_DISC_NO_BIND)
    // 【test12】跳过 bind：select 挂在未绑定 socket 上（永不可读，仅探活线程与 socket 生命周期）。
    Log("[dbg] disc: test12 跳过 bind，直接进 select 空转");
#elif defined(TLTP_DISC_BIND_MAIN)
    // 【test14】bind 也由主线程完成，本线程直接进 select/recvfrom。
    Log("[dbg] disc: test14 使用主线程预 bind 的 socket，直接进 select 循环");
#else
    // R19 首次连接健壮性：旧版 bind 失败就 `return 0` 静默退出 —— 无日志、无重试。
    // 车机环境 bind 8687 失败很常见（端口被上次残留进程占用 / Winsock 尚未就绪 /
    // 前一次崩溃没释放），一旦发生**信标发现永久失效**，只剩扫描兜底，
    // 表现为"开机后怎么都连不上"（首次连接失败），且日志里毫无线索。
    // 现改为：退避重试（最长 5s 一次，最多 60 次 ≈ 5 分钟内自愈），
    // 且每次失败都打日志，最后一次明确告知"信标不可用、仅靠扫描兜底"。
    int bindFail = 0;
    while (g_discoveryOn) {
        if (bind(s, (SOCKADDR*)&sa, sizeof(sa)) != SOCKET_ERROR) break;   // 绑定成功
        bindFail++;
        int err = WSAGetLastError();
        if (bindFail == 1 || bindFail % 10 == 0) {
            Log("disc: bind UDP %d 失败 (WSA=%d, 第%d次)，退避重试中…", DISCOVERY_PORT, err, bindFail);
        }
        if (bindFail >= 60) {
            Log("disc: bind UDP %d 持续失败 60 次，放弃信标发现（仅靠扫描/网关兜底，连接会变慢）", DISCOVERY_PORT);
            closesocket(s);
            return 0;
        }
        Sleep(bindFail < 10 ? 500 : 5000);
    }
    if (!g_discoveryOn) { closesocket(s); return 0; }
    Log("disc: 信标监听就绪 UDP %d（0.0.0.0）", DISCOVERY_PORT);
#endif
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
                // 格式：TUPTUP|<ip>|<port>
                if (strncmp(buf, "TUPTUP|", 7) == 0) {
                    char* ip = buf + 7;
                    char* sep = strchr(ip, '|');
                    if (sep) {
                        *sep = 0;
                        // R17：信标可能来自**多台手机**（车上不止一部 / 热点里有旁人的机）。
                        //   旧实现用单个 g_beaconIP，多台会互相覆盖 —— 最后发信标的那台独占候选，
                        //   另一台彻底连不上。现改为追加到列表，全部参与候选。
                        //   同时过滤：非本机任何子网的信标直接丢弃（与 configIP 同口径），
                        //   避免邻居热点/其它网段的手机污染候选、白等 1.5s 超时。
                        unsigned ba[4];
                        if (!parseIPv4(ip, ba)) {
                            // 非法 IPv4，直接丢弃
                        } else if (!adbMode() && !NetClient::IsSameSubnet(ip)) {
                            static long long lastBadLog = 0;
                            long long now = GetTickCount();
                            if (now - lastBadLog > 60000) {
                                lastBadLog = now;
                                Log("disc: 忽略信标 %s（不在本机任一子网）", ip);
                            }
                        } else {
                            AddPriority(ip);   // 置入高可信候选（多台共存）
                            EnterCriticalSection(&g_csCand);
                            // R18：信标列表**最近活跃的排最前**（移动到队首 = 刷新优先），
                            //   并封顶 8 条。车机换手机场景下，旧手机的信标会长期滞留，
                            //   若不刷新顺序 + 不封顶，昨天那台的 IP 会一直占着候选前排，
                            //   拖慢换机后的首次连接。队首 = 最后一次听到的 = 最可能正在用的那台。
                            for (size_t k = 0; k < g_beaconIPs.size(); k++) {
                                if (g_beaconIPs[k] == ip) { g_beaconIPs.erase(g_beaconIPs.begin() + k); break; }
                            }
                            g_beaconIPs.insert(g_beaconIPs.begin(), std::string(ip));
                            if (g_beaconIPs.size() > 8) g_beaconIPs.pop_back();   // 封顶，防无限增长
                            bool changed = (g_beaconIP != ip);   // R8：仅在变化时记日志，防刷屏
                            g_beaconIP = ip;
                            int n = (int)g_beaconIPs.size();
                            LeaveCriticalSection(&g_csCand);
                            if (changed) Log("disc: beacon 收到手机 IP %s (信标池%d个)", ip, n);
                        }
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
        else wcscpy(path, L"known_phones.cfg");   // R15：exe 在根目录（路径无 \）时回退到当前目录文件名
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
    // R15：原子写。先写临时文件，确认整段写入并刷盘后再覆盖正式文件，
    // 避免 SD 卡满/写保护时 WriteFile 静默失败却已用 CREATE_ALWAYS 截断原文件，导致下次加载读到空。
    std::wstring mainPath = knownPhonesPath();
    WCHAR tmpPath[MAX_PATH] = {0};
    wcsncpy(tmpPath, mainPath.c_str(), MAX_PATH - 1);
    wcscat(tmpPath, L".tmp");
    std::vector<KnownPhone> sorted = g_knownPhones;
    for (size_t i = 0; i + 1 < sorted.size(); i++)
        for (size_t j = i + 1; j < sorted.size(); j++)
            if (sorted[j].ts > sorted[i].ts) std::swap(sorted[i], sorted[j]);
    std::string out;
    for (size_t i = 0; i < sorted.size(); i++)
        out += sorted[i].id + " " + sorted[i].ip + " " + std::to_string(sorted[i].ts) + "\n";
    bool written = false;
    HANDLE hf = CreateFile(tmpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        if (WriteFile(hf, out.c_str(), (DWORD)out.size(), &wr, NULL)
            && wr == (DWORD)out.size()
            && FlushFileBuffers(hf)) {
            written = true;
        }
        CloseHandle(hf);
    }
    if (written) {
        DeleteFile(mainPath.c_str());
        if (MoveFile(tmpPath, mainPath.c_str())) {
            Log("known_phones: saved %d (id=%s ip=%s)", (int)sorted.size(), id.c_str(), ip.c_str());
        } else {
            Log("known_phones: 临时文件写入成功但 rename 失败 (err=%u)", (unsigned)GetLastError());
            DeleteFile(tmpPath);
        }
    } else {
        Log("known_phones: 写入失败 (SD卡满/写保护?)，放弃保存以免破坏已有记录");
        DeleteFile(tmpPath);
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
    // 【诊断埋点 R34】test9（无信标线程）稳定死在主线程 StartDiscovery 之前/之中，
    // 而 readConfig 之后所有 Log 均成功 → 死点被钉在 StartDiscovery 内部。
    // 这里用细粒度 stage 把 InitializeCriticalSection(&g_csCand) 与 LoadKnownPhones 两个
    // 最早的可能踩堆/踩临界区操作分开，下一次上车复测即可确定死在哪一步。
    CrashSetStage("Disc:enter");
#if defined(TLTP_DISC_NONE)
    // 【test18】StartDiscovery 入口即返回——隔离 InitCS(g_csCand)/LoadKnownPhones/线程 spawn 全部逻辑。
    // 若 test18 仍死在 WM:preDiscLog 之前 → 元凶不在发现逻辑，是更早的破坏 + Log 触发。
    Log("[dbg] disc: test18 DISC_NONE — StartDiscovery 立即返回");
    return;
#endif
    InitializeCriticalSection(&g_csCand);
    CrashSetStage("Disc:csCand");
    if (g_discoveryOn) return;
#if !defined(TLTP_DISC_NO_LOADKNOWN)
    LoadKnownPhones();   // 启动时读取“记住的手机”，供本轮回合优先直连
#endif
    CrashSetStage("Disc:loadKnown");
    g_discoveryOn = true;
    CrashSetStage("Disc:flag");
#if defined(TLTP_DISC_SOCK_MAIN)
    // 【test13/14】主线程预创建 socket（TLTP_DISC_BIND_MAIN 时连 bind 也在主线程做）——
    // 真车死亡窗口钉在 DiscoveryThread 的 socket()/bind() 一带（entered 之后、
    // bind 结果日志之前），test8 证明与 ConnThread 无关。
    g_discSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    Log("[dbg] disc: 主线程 socket() -> %s", g_discSock == INVALID_SOCKET ? "FAIL" : "ok");
#if defined(TLTP_DISC_BIND_MAIN)
    if (g_discSock != INVALID_SOCKET) {
        BOOL reuse = 1;
        setsockopt(g_discSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((u_short)DISCOVERY_PORT);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(g_discSock, (SOCKADDR*)&sa, sizeof(sa)) == SOCKET_ERROR)
            Log("[dbg] disc: 主线程 bind 失败 (WSA=%d)", WSAGetLastError());
        else
            Log("[dbg] disc: 主线程 bind 成功 UDP %d", DISCOVERY_PORT);
    }
#endif
#endif
#ifndef TLTP_DISC_NO_THREAD
    // R23：走 TltpCreateThread 显式保留 128KB 栈。WinCE 忽略 CreateThread 的
    // dwStackSize（默认只有 64KB），而这两个探测线程栈上要放 std::vector<BYTE> buf
    // 等对象并调用 GetAdaptersInfo/probePort/sscanf，64KB 会溢出。
    // 症状同样是"进程瞬间消失、日志停在StartDiscovery 之前"。
    CrashSetStage("Disc:spawnDisc");
    g_hDiscThread = TltpCreateThread(DiscoveryThread, NULL);
    CrashSetStage("Disc:spawnDiscOk");
#else
    // 【test9】真·无发现：连信标线程都不 spawn。旧 test4/5 的宏只挡了扫描线程，
    // 信标线程从未被关掉（日志可见 DiscoveryThread entered 照常出现）——结论作废。
    // 若 test9 活 → 元凶在 DiscoveryThread（大概率是其 Winsock 调用）；
    // 若 test9 也死 → 元凶在主线程侧（窗口/渲染/NetClient 构造/ConnThread）。
    Log("[dbg] disc: test9 信标线程不启动（真·无发现）");
#endif
    // R29：ADB 模式下彻底不跑子网扫描线程 —— 它内部唯一的网卡枚举用途是为
    // usb_net 推导候选，而 ADB 模式候选全来自 UDP 8687 信标（无需 GetAdaptersInfo，
    // 也避免并发踩堆导致 ConnThread 崩溃）。usb_net 模式仍照常扫描。
    CrashSetStage("Disc:scanChk");
    if (!adbMode())
        g_hScanThread = TltpCreateThread(SubnetScanThread, NULL);
    CrashSetStage("Disc:scanDone");
}

void NetClient::StopDiscovery() {
    g_discoveryOn = false;
    // 等待两个探测线程真正退出并回收句柄：此前 CreateThread 返回的句柄从未 CloseHandle，
    // 长期运行会持续泄漏内核对象；更关键的是退出阶段若线程仍在跑，
    // 会继续访问已被 delete 的渲染器/网络对象（崩溃）。
    // R20：等待上限从 8s 提到 25s —— SubnetScanThread 一轮全段扫描最坏耗时
    //   254×(60ms 探测 + 8ms 节流) ≈ 17s，原 8s 必然不够，线程仍在跑就被上层销毁对象。
    if (g_hDiscThread) {
        if (WaitForSingleObject(g_hDiscThread, 25000) == WAIT_TIMEOUT) Log("warn: discovery thread join timeout");
        CloseHandle(g_hDiscThread); g_hDiscThread = NULL;
    }
    if (g_hScanThread) {
        if (WaitForSingleObject(g_hScanThread, 25000) == WAIT_TIMEOUT) Log("warn: scan thread join timeout");
        CloseHandle(g_hScanThread); g_hScanThread = NULL;
    }
}

void NetClient::SetLinkUp(bool up) {
    g_linkUp = up;
}

// config.txt 显式 IP 的失败计数（连续失败达阈值后本轮跳过它，让自动发现顶上）。
// 解决"一个写死的过期 IP 独占每轮 1.5s 超时、真实候选永远排不到"的死锁。
// 声明前置到 ClearScanned 之前（连上时要复位它）。
static int g_cfgFailStreak = 0;
static std::string g_cfgIPUsed;             // 记录上次用的是哪个 configIP，换 IP 时清零计数

void NetClient::ClearScanned() {
    EnterCriticalSection(&g_csCand);
    g_priIPs.clear();   // 仅清扫描/网关候选；g_beaconIP 保留（信标持续刷新）
    LeaveCriticalSection(&g_csCand);
    g_cfgFailStreak = 0;   // 连上即复位：下次断线时 configIP 又值得优先试
}

void NetClient::NoteConfigIPResult(bool ok) {
    if (g_cfgIPUsed.empty()) return;
    if (ok) g_cfgFailStreak = 0;
    else   g_cfgFailStreak++;
}

void NetClient::GetCandidates(const std::string& configIP, std::vector<std::string>& out) {
    out.clear();
    std::vector<std::string> tmp;
    // 优先级：config.txt 显式 IP【仅当同子网 且 未连续失败过多】> 已知手机上次IP > UDP 信标真实IP > 接口网关/扫描确认
    // R16 网段健壮性：原实现无条件把 configIP 排最前，导致
    //   "config.txt 里的 IP 与当前网络无关" 时每轮都先撞这个死 IP（白等 1.5s），
    //   真正由信标/网关/扫描发现的候选被挤到后面 —— 实测 6.8 小时 10851 行日志全在撞同一个 IP。
    // 现在三重保护：① 不同子网直接丢弃并记日志；② 连续失败 ≥CFG_IP_FAIL_MAX 轮则本轮让位；
    // ③ 换IP / 连上 / 拉黑时状态复位。
    const int CFG_IP_FAIL_MAX = 3;
    if (!configIP.empty()) {
        if (configIP != g_cfgIPUsed) { g_cfgIPUsed = configIP; g_cfgFailStreak = 0; }
        bool cfgInSubnet = adbMode() ? true : NetClient::IsSameSubnet(configIP.c_str());
        if (!cfgInSubnet) {
            static DWORD s_lastWarn = 0;
            if (GetTickCount() - s_lastWarn > 60000) {   // 同一原因每分钟只提醒一次，避免刷屏
                s_lastWarn = GetTickCount();
                Log("cfg: 忽略 config 指定的 %s（不在本机任一子网内，疑似换网后失效）", configIP.c_str());
            }
        } else if (g_cfgFailStreak >= CFG_IP_FAIL_MAX) {
            // 静默让位（不刷日志，round 日志已能反映候选数变化）
        } else {
            tmp.push_back(configIP);
        }
    }
    // R18 换机健壮性（车机场景：今天用手机 A，明天换手机 B）：
    // 旧顺序是 configIP > **known(历史手机IP)** > 信标(当下真实手机) > 网关/扫描。
    // 问题：known 排在信标前面。今天 A 手机连过后 IP 被存进 known_phones.cfg，
    // 明天 B 手机上线发信标，程序却先去撞 A 的历史 IP —— 白等 1.5s 超时，
    // 而真正在线的 B 排在后面，甚至因 A 恰好占用该 IP 而连错机/连不上。
    // 现把 **信标提到 known 之前**：信标 = 此刻网络上真实发信标的手机，最可信、最新鲜；
    // known 降级为"信标不来时的兜底"（手机没开信标/休眠时仍能直连上次那台）。
    // 这样换手机后只要新手机开着 App 发信标，就一定能被优先连上，无需清任何配置。
    std::vector<std::string> beacon;   // 信标快照（加锁复制，避免与Discovery 线程竞态）
    {
        EnterCriticalSection(&g_csCand);
        for (size_t i = 0; i < g_beaconIPs.size(); i++) beacon.push_back(g_beaconIPs[i]);
        LeaveCriticalSection(&g_csCand);
    }
    // 组装顺序：① 信标（当下在线，最可信）② known（历史兜底）③ 网关/扫描（被动探测）
    for (size_t i = 0; i < beacon.size(); i++) tmp.push_back(beacon[i]);
    if (!g_deferKnown) {   // R12：连续失败多轮后本轮跳过 known，给信标/网关让路（防失效 IP 拖死每轮）
        std::vector<std::string> known; GetKnownIPs(known);
        for (size_t i = 0; i < known.size(); i++) tmp.push_back(known[i]);
    }
    EnterCriticalSection(&g_csCand);
    for (size_t i = 0; i < g_priIPs.size(); i++) tmp.push_back(g_priIPs[i]);
    LeaveCriticalSection(&g_csCand);
    // 保序去重 + 过滤空串（R8：旧版空候选会打出 `try connect #:8686` 空转、污染日志）
    for (size_t i = 0; i < tmp.size(); i++) {
        if (!tmp[i].empty() && !HasIP(out, tmp[i].c_str())) out.push_back(tmp[i]);
    }
}
