// adb.cpp - 见 adb.h
//
// ADB 协议要点（与 AOSP adbd 对齐，已用假 adbd 在主机端端到端验证）：
//   · 消息头固定 24 字节，小端 6 个 u32：command, arg1, arg2, data_length, data_check, magic(=command^0xFFFFFFFF)
//   · 校验和 = 数据字节累加和 mod 2^32（空数据则为 0）
//   · 命令：CNXN / AUTH / OPEN / OKAY / WRTE / CLSE / SYNC
//   · AUTH arg1=类型：1=TOKEN(设备下发随机挑战), 2=SIGNATURE(我们对 SHA1(token) 的 RSA 签名), 3=RSAPUBLICKEY(公钥 base64)
//   · 流控：OPEN 后双方各有窗口=各自 MAX_DATA；WRTE 后发送方窗口递减，收到 OKAY 后恢复
#include "adb.h"
#include "rsa.h"
#include "adbkey.h"
#include "log.h"
#include "thread.h"
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------- 平台抽象 ----
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET Sock;
  #define SOCK_BAD INVALID_SOCKET
  #define SOCK_CLOSE ::closesocket
  static unsigned long nowMs() { return GetTickCount(); }
#else
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <sys/ioctl.h>   // FIONREAD：backlog() 需要知道内核缓冲里的待读字节数
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <time.h>
  typedef int Sock;
  #define SOCK_BAD (-1)
  // ⚠️ 必须加全局限定符 :: ：在成员函数里写裸 close/connect 会被名字查找
  // 优先解析成 AdbTransport 自己的成员函数（签名不符 → 编译失败）。这也是
  // POSIX 分支此前从未真正编译通过的证据。
  #define SOCK_CLOSE ::close
  #include <time.h>
  static unsigned long nowMs() {
      struct timeval tv; gettimeofday(&tv, NULL);
      return (unsigned long)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
  }
#endif

// ADB 命令字（小端存盘即字符序 'CNXN' 等）
#define A_SYNC 0x434e5953u
#define A_CNXN 0x4e584e43u
#define A_AUTH 0x48545541u
#define A_OPEN 0x4e45504fu
#define A_OKAY 0x59414b4fu
#define A_CLSE 0x45534c43u
#define A_WRTE 0x45545257u
// AUTH 子类型
#define ADB_AUTH_TOKEN        1
#define ADB_AUTH_SIGNATURE    2
#define ADB_AUTH_RSAPUBLICKEY 3
#define ADB_VERSION 0x01000000u

// 写窗口最长等待时间：对端沉默（不发 OKAY）时不能无限等下去，否则上层
// sendMsg 永久阻塞，触摸线程与心跳线程一起挂死，却看不出任何错误。
// 超时后置位断链，交由上层（NetClient）触发重连。
#define WRITE_MAX_WAIT_MS 15000

static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
}
static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 标准 base64（仅编码，用于 RSAPUBLICKEY 载荷）
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64encode(const unsigned char* in, int len) {
    std::string out;
    int i = 0;
    while (i + 3 <= len) {
        unsigned v = (in[i] << 16) | (in[i+1] << 8) | in[i+2];
        out.push_back(B64[(v >> 18) & 63]); out.push_back(B64[(v >> 12) & 63]);
        out.push_back(B64[(v >> 6) & 63]);   out.push_back(B64[v & 63]);
        i += 3;
    }
    int rem = len - i;
    if (rem == 1) {
        unsigned v = in[i] << 16;
        out.push_back(B64[(v >> 18) & 63]); out.push_back(B64[(v >> 12) & 63]);
        out.push_back('='); out.push_back('=');
    } else if (rem == 2) {
        unsigned v = (in[i] << 16) | (in[i+1] << 8);
        out.push_back(B64[(v >> 18) & 63]); out.push_back(B64[(v >> 12) & 63]);
        out.push_back(B64[(v >> 6) & 63]); out.push_back('=');
    }
    return out;
}

// ⚠️ socket 句柄现在是 adb.h 中的【每实例成员】 uintptr_t m_sock
// （约定 (uintptr_t)-1 表示无效）。早前这里是 `#define m_sock m_sockImpl`
// + 文件级 static 变量，导致所有 AdbTransport 实例共享同一个句柄。
// 成员函数内一律用 sock() 取值，避免散落的类型转换写错。

// ---------------------------------------------------------------- 构造/析构 ----
AdbTransport::AdbTransport() {}
AdbTransport::~AdbTransport() { close(); }

bool AdbTransport::connected() const {
    return m_open && !m_closed && sock() != SOCK_BAD;
}

// ---------------------------------------------------------------- 连接外层 TCP ----
bool AdbTransport::sockConnectTimeout(const char* ip, int port, int timeoutMs) {
    Sock s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_BAD) return false;
    // 允许地址复用（高频试连避免 TIME_WAIT 耗尽端口池）
    int reuse = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = inet_addr(ip);
    if (sa.sin_addr.s_addr == (unsigned)-1) { SOCK_CLOSE(s); return false; }   // INADDR_NONE

    // 非阻塞 connect + select 超时
#ifdef _WIN32
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
    // :: 不可省：否则命中 AdbTransport::connect(const wstring&, int, int)
    ::connect(s, (struct sockaddr*)&sa, sizeof(sa));
    fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
    struct timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    int r = select(0, NULL, &wf, NULL, &tv);
#else
    int r = select(s + 1, NULL, &wf, NULL, &tv);
#endif
    bool ok = false;
    if (r == 1) {
        int err = 0; socklen_t el = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        ok = (err == 0);
    }
#ifdef _WIN32
    u_long nb2 = 0; ioctlsocket(s, FIONBIO, &nb2);
#else
    fcntl(s, F_SETFL, fl);
#endif
    if (ok) { m_sock = (uintptr_t)s; return true; }
    SOCK_CLOSE(s);
    return false;
}

void AdbTransport::setRecvTimeout(int ms) {
    Sock s = sock();
    if (s == SOCK_BAD) return;
#ifdef _WIN32
    DWORD t = (DWORD)ms; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
    struct timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// ---------------------------------------------------------------- 发送/接收 ----
bool AdbTransport::sendMsg(uint32_t cmd, uint32_t a1, uint32_t a2, const BYTE* data, int dlen) {
    Sock s = sock();
    if (s == SOCK_BAD) return false;
    unsigned char hdr[24];
    uint32_t chk = 0;
    if (data && dlen > 0) for (int i = 0; i < dlen; i++) chk = (chk + data[i]) & 0xFFFFFFFFu;
    put32(hdr + 0, cmd);
    put32(hdr + 4, a1);
    put32(hdr + 8, a2);
    put32(hdr + 12, (uint32_t)dlen);
    put32(hdr + 16, chk);
    put32(hdr + 20, cmd ^ 0xFFFFFFFFu);

    m_sendMutex.lock();
    // 头 + 数据 一起发（小消息，循环确保发完）
    const BYTE* p = hdr; int total = 24; int off = 0;
    while (off < total) {
        int sent = send(s, (const char*)p + off, total - off, 0);
        if (sent <= 0) { m_sendMutex.unlock(); return false; }
        off += sent;
    }
    if (data && dlen > 0) {
        off = 0;
        while (off < dlen) {
            int sent = send(s, (const char*)data + off, dlen - off, 0);
            if (sent <= 0) { m_sendMutex.unlock(); return false; }
            off += sent;
        }
    }
    m_sendMutex.unlock();
    return true;
}

bool AdbTransport::recvExact(BYTE* buf, int n) {
    Sock s = sock();
    if (s == SOCK_BAD) return false;
    int off = 0;
    while (off < n) {
        int r = recv(s, (char*)buf + off, n - off, 0);
        if (r <= 0) return false;
        off += r;
    }
    return true;
}

AdbTransport::RecvState AdbTransport::recvMsg(int timeoutMs, uint32_t& cmd, uint32_t& a1,
                                             uint32_t& a2, std::vector<BYTE>& data) {
    Sock s = sock();
    if (s == SOCK_BAD) return RS_ERROR;
    fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
    struct timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
    int r = select(0, &rf, NULL, NULL, &tv);
#else
    int r = select(s + 1, &rf, NULL, NULL, &tv);
#endif
    if (r == 0) return RS_TIMEOUT;
    if (r < 0) return RS_ERROR;

    unsigned char hdr[24];
    if (!recvExact(hdr, 24)) return RS_ERROR;
    cmd = get32(hdr + 0);
    a1  = get32(hdr + 4);
    a2  = get32(hdr + 8);
    uint32_t dlen = get32(hdr + 12);
    uint32_t chk  = get32(hdr + 16);
    uint32_t mag  = get32(hdr + 20);
    if (mag != (cmd ^ 0xFFFFFFFFu)) return RS_ERROR;
    if (dlen > 4 * 1024 * 1024) return RS_ERROR;   // 防御：异常大消息
    data.resize(dlen);
    if (dlen > 0 && !recvExact(data.data(), (int)dlen)) return RS_ERROR;
    uint32_t chk2 = 0;
    for (uint32_t i = 0; i < dlen; i++) chk2 = (chk2 + data[i]) & 0xFFFFFFFFu;
    if (chk2 != chk) return RS_ERROR;
    return RS_OK;
}

// ---------------------------------------------------------------- 握手辅助 ----
bool AdbTransport::sendAuthPublicKey() {
    int publen = (int)sizeof(ADB_RSA_PUB);
    std::string b64 = b64encode(ADB_RSA_PUB, publen);
    std::string payload = b64 + " tuptup@car";
    Log("adb: 发送 RSAPUBLICKEY (%d base64 字节) 用于一次性授权", (int)payload.size());
    return sendMsg(A_AUTH, ADB_AUTH_RSAPUBLICKEY, 0, (const BYTE*)payload.c_str(), (int)payload.size());
}

bool AdbTransport::sendOpen() {
    // OPEN 的载荷为"tcp:8686" + 结尾 '\0'
    const char* svc = "tcp:8686";
    unsigned char open[10];
    memcpy(open, svc, 9); open[9] = 0;
    Log("adb: OPEN tcp:8686 (localId=0x%08X)", m_localId);
    return sendMsg(A_OPEN, m_localId, 0, open, 10);
}

// 完整握手：CNXN → 响应 AUTH(TOKEN) 签名 → OPEN；返回 true=流已建立
bool AdbTransport::doHandshake(int timeoutMs) {
    // 1) 发本端 CNXN（声明 MAX_DATA）
    const char banner[] = "host::\0";
    if (!sendMsg(A_CNXN, ADB_VERSION, m_ourMaxData, (const BYTE*)banner, sizeof(banner))) {
        Log("adb: 发送 CNXN 失败"); return false;
    }

    bool gotCnxn = false, opened = false;
    unsigned long deadline = nowMs() + 60000;   // 给用户最多 60s 点“允许 USB 调试”
    unsigned long lastOpen = 0;
    while (nowMs() < deadline && !opened) {
        uint32_t cmd = 0, a1 = 0, a2 = 0;
        std::vector<BYTE> data;
        RecvState st = recvMsg(3000, cmd, a1, a2, data);
        if (st == RS_TIMEOUT) {
            // 没回消息：若已经过 CNXN+签名，重发 OPEN（手机可能刚被授权）
            if (gotCnxn && nowMs() - lastOpen > 1500) { sendOpen(); lastOpen = nowMs(); }
            continue;
        }
        if (st == RS_ERROR) { Log("adb: 握手期 recv 错误"); return false; }

        if (cmd == A_CNXN) {
            gotCnxn = true;
            m_adbMaxData = a2 ? a2 : 4096;
            m_remoteWindow = (long)m_adbMaxData;
            Log("adb: 收到设备 CNXN, adbd MAX_DATA=%u", m_adbMaxData);
        } else if (cmd == A_AUTH) {
            if (a1 == ADB_AUTH_TOKEN) {
                if ((int)data.size() < 20) { Log("adb: TOKEN 长度异常 %d", (int)data.size()); return false; }
                unsigned char sig[256];
                RsaSignToken(data.data(), sig);   // 对 SHA1(token) 做 PKCS#1 v1.5 签名
                Log("adb: 收到 TOKEN, 发送 SIGNATURE + RSAPUBLICKEY");
                if (!sendMsg(A_AUTH, ADB_AUTH_SIGNATURE, 0, sig, 256)) return false;
                if (!sendAuthPublicKey()) return false;
                m_localId = 0x01000000;
                sendOpen(); lastOpen = nowMs();
            } else if (a1 == ADB_AUTH_RSAPUBLICKEY) {
                if (!sendAuthPublicKey()) return false;
            } else {
                Log("adb: 收到 AUTH 子类型 %u（忽略）", a1);
            }
        } else if (cmd == A_OKAY) {
            if (a1 == m_localId) {
                m_remoteId = a2;
                opened = true;
                Log("adb: OPEN 被确认, remoteId=0x%08X, 隧道建立", m_remoteId);
            }
        } else if (cmd == A_CLSE) {
            Log("adb: 收到 CLSE（OPEN 被拒），重发 OPEN");
            if (gotCnxn && nowMs() - lastOpen > 1500) { sendOpen(); lastOpen = nowMs(); }
        } else if (cmd == A_SYNC) {
            if (a1 == 0) sendMsg(A_SYNC, 1, 0, NULL, 0);
        } else {
            Log("adb: 握手中收到未预期命令 0x%08X", cmd);
        }
    }
    if (!opened) { Log("adb: 握手超时（60s 内未完成鉴权/OPEN）"); return false; }
    return true;
}

// ---------------------------------------------------------------- 读泵线程 ----
#ifdef _WIN32
DWORD WINAPI AdbTransport::ReadPumpThunk(LPVOID p) { ((AdbTransport*)p)->ReadPump(); return 0; }
#else
void* AdbTransport::ReadPumpThunk(void* p) { ((AdbTransport*)p)->ReadPump(); return NULL; }
#endif

void AdbTransport::ReadPump() {
    while (m_running) {
        uint32_t cmd = 0, a1 = 0, a2 = 0;
        std::vector<BYTE> data;
        RecvState st = recvMsg(2000, cmd, a1, a2, data);
        if (st == RS_TIMEOUT) continue;       // 仅超时：继续等待（2s 一探，便于响应 m_running/close）
        if (st == RS_ERROR) {
            m_closed = true;
            m_readEvent.set();                // 唤醒任何阻塞在 read() 的调用方
            Log("adb: 读泵 recv 错误 → 断链");
            break;
        }
        if (cmd == A_WRTE) {
            // 数据流向我们：arg1 == m_localId（adbd 用我们的本地 id 标识该流）
            if (a1 == m_localId) {
                if (!data.empty()) {
                    m_bufMutex.lock();
                    m_readBuf.insert(m_readBuf.end(), data.begin(), data.end());
                    m_bufMutex.unlock();
                    m_readEvent.set();
                }
                // 回 OKAY（ack 这段 WRTE），窗口保持我们声明的 MAX_DATA（我们即时消费）
                sendMsg(A_OKAY, m_remoteId, m_ourMaxData, NULL, 0);
            }
        } else if (cmd == A_OKAY) {
            // 确认我们发出的 WRTE：按 FIFO 归还额度（严格有序，一个 OKAY 对应一个 WRTE）。
            // 对端 id 位置有两种约定（arg0 填我方 id 或对方据此回填），这里宽松匹配，
            // 避免因为 id 语义差异把 ack 丢掉 → 写窗口永不恢复。
            if (a1 == m_localId || a1 == m_remoteId) {
                m_sendMutex.lock();
                if (!m_outstanding.empty()) {
                    m_remoteWindow += m_outstanding.front();
                    m_outstanding.pop_front();
                } else {
                    // 无在途却收到 ack：多为握手期 OKAY 的余波，兜底恢复满窗避免永久卡死
                    m_remoteWindow = (long)m_adbMaxData;
                }
                m_sendMutex.unlock();
                m_windowEvent.set();   // 唤醒阻塞在 write() 的线程
            }
        } else if (cmd == A_CLSE) {
            m_closed = true;
            m_readEvent.set();
            Log("adb: 收到 CLSE → 断链");
            break;
        } else if (cmd == A_SYNC) {
            if (a1 == 0) sendMsg(A_SYNC, 1, 0, NULL, 0);
        }
        // CNXN/AUTH 在 OPEN 之后不应出现，忽略
    }
    Log("adb: 读泵退出");
}

// ---------------------------------------------------------------- 对外接口 ----
bool AdbTransport::connect(const std::wstring& host, int /*port*/, int timeoutMs) {
    close();   // 确保干净
    char ip[64];
#ifdef _WIN32
    WideCharToMultiByte(CP_ACP, 0, host.c_str(), -1, ip, sizeof(ip), NULL, NULL);
#else
    // 主机测试：host 为 ASCII（如 "127.0.0.1"），逐字节拷贝即可
    for (size_t i = 0; i < host.size() && i + 1 < sizeof(ip); i++) ip[i] = (char)host[i];
    ip[host.size()] = 0;
#endif
    Log("adb: 连接 adbd %s:%d", ip, ADB_PORT);
    if (!sockConnectTimeout(ip, ADB_PORT, timeoutMs)) {
        Log("adb: TCP 连接 %s:%d 失败", ip, ADB_PORT);
        return false;
    }
    setRecvTimeout(3000);   // 握手阶段每读 3s 兜底

    if (!doHandshake(timeoutMs)) { close(); return false; }

    // 流已建立：启动读泵
    m_open = true;
    m_closed = false;
    m_running = true;
    m_remoteWindow = (long)m_adbMaxData;
#ifdef _WIN32
    // R23：走 TltpCreateThread 显式保留 128KB 栈（WinCE 默认仅 64KB，
    // 读泵里 RSA/缓冲区处理会溢出）。详见 thread.h。
    m_readThread = TltpCreateThread(ReadPumpThunk, this);
    if (!m_readThread) { Log("adb: 创建读泵线程失败"); close(); return false; }
#else
    if (pthread_create(&m_readThread, NULL, ReadPumpThunk, this) != 0) {
        Log("adb: 创建读泵线程失败"); close(); return false;
    }
#endif
    Log("adb: 隧道就绪（read/write 字节流可用）");
    return true;
}

bool AdbTransport::read(BYTE* buf, int n) {
    int got = 0;
    while (got < n) {
        m_bufMutex.lock();
        int avail = (int)m_readBuf.size();
        if (avail > 0) {
            int take = n - got; if (take > avail) take = avail;
            memcpy(buf + got, m_readBuf.data(), (size_t)take);
            m_readBuf.erase(m_readBuf.begin(), m_readBuf.begin() + take);
            got += take;
            m_bufMutex.unlock();
            if (got == n) return true;
            continue;
        }
        m_bufMutex.unlock();
        if (m_closed) return false;            // 断链且无数据可读
        m_readEvent.wait(1000);                // 等数据或断链脉冲
    }
    return got == n;
}

int AdbTransport::backlog() const {
    // 隧道模式下常见的反压依据：已从 ADB 消息里解出、但上层还没取走的字节数。
    // 早期这里恒返回 0，导致 NetClient 的“积压 >96KB 就丢帧取最新”完全失效，
    // 隧道模式的视频延迟会随推流时长不断累积。
    int buffered = 0;
    m_bufMutex.lock();
    buffered = (int)m_readBuf.size();
    m_bufMutex.unlock();
    int pending = 0;
    Sock s = sock();
    if (s != SOCK_BAD) {
#ifdef _WIN32
        u_long v = 0;
        if (ioctlsocket(s, FIONREAD, &v) == 0) pending = (int)v;
#else
        int v = 0;
        if (ioctl(s, FIONREAD, &v) == 0) pending = v;
#endif
    }
    return buffered + pending;
}

bool AdbTransport::write(const BYTE* buf, int n) {
    int off = 0;
    unsigned long deadline = nowMs() + WRITE_MAX_WAIT_MS;
    while (off < n) {
        if (m_closed) return false;
        // 等写窗口（OKAY 到来时由读泵把额度加回来）。
        //
    // ⚠️ 不能忙自旋：车机是单核 SoC，抢占式轮询会把整个 CPU 吃满，
    // 表现为“画面卡死但进程还活着”。这里改成事件等待 + 有界超时。
        int waited = 0;
        while (true) {
            m_sendMutex.lock();
            long avail = m_remoteWindow;
            m_sendMutex.unlock();
            if (avail > 0) break;
            if (m_closed) return false;
            if (nowMs() > deadline) {
                Log("adb: 写窗口 %dms 未放开 → 判定链路异常", WRITE_MAX_WAIT_MS);
                m_closed = true;
                return false;
            }
            m_windowEvent.wait(50);   // 短等待：被唤醒即查窗口，最多空转 50ms
            waited += 50;
            (void)waited;
        }
        if (m_closed) return false;

        // 原子地预留一个 chunk 的窗口额度（与读泵里的 OKAY 处理互斥，
        // 避免“检查窗口→递减→记录在途”被对端 OKAY 打断导致额度错乱）。
        // ⚠️ 只锁“预留”这一段。sendMsg() 内部会【再次】加 m_sendMutex，
        // 若在这里仍持有锁就变成非递归互斥量自死锁——这正是早前 write 阶段
        // 卡死（握手/banner 都过、一上行就挂）的根因。
        m_sendMutex.lock();
        int chunk = n - off;
        if (chunk > m_remoteWindow) chunk = (int)m_remoteWindow;
        if (chunk <= 0) { m_sendMutex.unlock(); continue; }
        m_remoteWindow -= chunk;
        m_outstanding.push_back(chunk);   // 记录待 ack 的字节数
        m_sendMutex.unlock();

        // 在【不持有 m_sendMutex】的情况下发送；sendMsg 自己会加锁串行化所有 socket 写。
        bool ok = sendMsg(A_WRTE, m_remoteId, 0, buf + off, chunk);
        if (!ok) { m_closed = true; return false; }
        off += chunk;
    }
    return true;
}

void AdbTransport::close() {
    Sock s = sock();
    if (s == SOCK_BAD && !m_running) { return; }
    m_running = false;
    if (s != SOCK_BAD) {
        SOCK_CLOSE(s);      // 让读泵的 recv 立即返回错误 → 退出
        m_sock = (uintptr_t)SOCK_BAD;
    }
    m_readEvent.set();          // 唤醒阻塞在 read() 的调用方
#ifdef _WIN32
    if (m_readThread) {
        WaitForSingleObject(m_readThread, 4000);
        CloseHandle(m_readThread);
        m_readThread = NULL;
    }
#else
    if (m_readThread) {
        pthread_join(m_readThread, NULL);
        m_readThread = 0;
    }
#endif
    m_open = false;
    // ⚠️ 这里必须置 true 而不是复位为 false：否则 close() 之后再有人调用 read()
    // 会因为“既没数据、m_closed 又为假”而永远睡在 wait() 上，表现为应用卡死
    // 而非报错。connected() 靠 m_open 判断即可，不受影响。
    m_closed = true;
    m_remoteId = 0;
    m_sendMutex.lock();
    m_remoteWindow = (long)m_adbMaxData;
    m_outstanding.clear();
    m_sendMutex.unlock();
    m_windowEvent.set();          // 唤醒阻塞在 write() 的线程（否则它会等到超时）
    m_bufMutex.lock(); m_readBuf.clear(); m_bufMutex.unlock();
    m_readEvent.set();
}
