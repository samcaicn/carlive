#pragma once
// net.h - WinCE 车机端 TCP 客户端 + TUPT 协议解析
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

    // 已连手机的稳定标识（握手阶段从手机侧 HANDSHAKE 的 device_id 解析得到），
    // 用于“记住这台手机”：下次启动优先直连其上次 IP，免等信标。
    std::string phoneId() const { return m_phoneId; }
    // 本次连接命中的手机 IP（connect/connectTimeout 成功后记录），与 device_id 配对落盘。
    void setConnectedIP(const std::string& ip) { m_connectedIP = ip; }

    // ---- 已知手机（记住连接参数）----
    // known_phones.cfg 落盘与加载：每行 "device_id ip last_ok_ts"。
    static void LoadKnownPhones();                 // 启动时读取，填充 g_knownIPs（按最近成功倒序）
    static void SaveKnownPhone(const std::string& id, const std::string& ip); // 连上后写入/更新
    static void GetKnownIPs(std::vector<std::string>& out); // 已知 IP 列表（最近优先）

    // 阻塞读取下一个视频帧（自动跳过非视频消息）。返回 false=断线。
    bool recvVideoFrame(VideoFrame& out);
    void close();
    bool connected() const { return m_sock != INVALID_SOCKET; }
    int  codec() const { return m_codec; }   // 0=H264, 1=MJPEG（由 VIDEO_CONFIG 设置）
    SOCKET sock() const { return m_sock; }   // 供积压检测（FIONREAD）使用

    // ---- 自动发现（纯探测，不写死任何地址）----
    static void StartDiscovery();   // 启动后台监听/探测线程，持续发现手机 IP
    static void StopDiscovery();    // 停止并等待探测线程真正退出后再返回（避免线程句柄泄漏/退出后仍写日志）

    // 连接世代号：每次成功建立连接 / 关闭连接都会递增。后台线程启动时记录当时的世代号，
    // 循环中一旦发现世代号变化，说明本线程持有的 socket 已被关闭——而 Winsock 可能立即把同一个
    // 句柄号分配给下一次连接，若旧线程仍在 recv 就会读到新连接的字节流，造成协议错位。
    // 相比“等待 N 秒”的时间兜底，这是确定性的判据。
    static long ConnEpoch();
    // 链路连通状态标记：由连接线程在握手成功/断开时调用，供探测线程在已连上时暂停，避免无意义探测
    static void SetLinkUp(bool up);
    // 清空【已扫描/网关推导】候选（保留 UDP 信标 IP）。连接成功后调用：当前连接已采纳，
    // 旧候选若失效会拖累重连（白等 1.5s×N），下次断开重连由扫描线程重新发现。
    static void ClearScanned();
    // 返回候选 IP 列表：config.txt 显式 IP（若有）优先，其次【高可信候选】——
    //   含 UDP 信标发现的真实 IP、本机接口网关推导、主动扫描确认。全部动态，无写死地址。
    static void GetCandidates(const std::string& configIP, std::vector<std::string>& out);
    // R12：known 候选防拖死开关。手机换了网络（IP 变化）后，known IP 每轮都排最前、
    // 每个白等 1.5s。连续失败多轮时置 true，本轮忽略 known，让信标/网关探测先试；
    // 连接成功（或回到首轮）复位为 false。
    static void SetDeferKnown(bool defer);
    // 注：原先在此声明过一个成员函数 PriorityCount()，但 net.cpp 中实际存在的同名函数是文件内
    // 静态自由函数、并非本类成员；一旦有人按 NetClient::PriorityCount() 调用将直接链接失败。
    // 候选计数目前仅扫描线程内部使用，故移除这个会误导人的悬空声明。

private:
    SOCKET m_sock;
    int m_codec;   // 当前视频编解码，默认 MJPEG(1)
    std::string m_phoneId;     // 对端手机稳定标识（握手解析）
    std::string m_connectedIP; // 本次命中 IP
    bool sendMsg(BYTE type, const BYTE* payload, int len);
    bool readExact(BYTE* buf, int n);
    bool readMsg(BYTE& type, std::vector<BYTE>& payload); // 读一个完整消息
};
