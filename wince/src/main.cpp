// main.cpp - tuptup.top 车机端入口（ARM WinCE 6.0）
// 关键修复：主(GUI)线程**永远**跑消息泵（GetMessage），窗口才能正常绘制/响应；
// 网络发现/连接/收帧/心跳全部在后台线程，避免“启动卡死”（窗口创建后无消息循环→不刷新、点不动）。
// 零配置：手机 App 启动后 UDP 广播自身 IP，本端监听自动连接；USB 共享网络下通过
// 主动探测（本机接口网关推导 + 子网 8686 端口扫描）发现手机，不写死任何地址。
#include "net.h"
#include "renderer.h"
#include "decoder.h"
#include "log.h"

#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <string>
#include <vector>

static NetClient* g_net = NULL;
static Renderer* g_renderer = NULL;
static Decoder  g_decoder;
static HWND     g_hwnd = NULL;
static bool     g_running = true;
static HANDLE   g_hConnThread = NULL;  // 连接线程句柄：退出时必须等它真正结束，否则会在其仍在运行时 delete 它正用的对象
// 窗口是否处于前台激活态：车机上切到导航/音乐等程序后，无需再为看不见的画面做高开销的解码+拉伸。
static volatile bool g_active = true;
// 客户区实际尺寸：窗口带 WS_CAPTION，实际可用高度小于名义的 800x480；握手时上报真实值，
// 手机端才能按车机真实画布缩放编码（省带宽、省车机解码开销），而不是按名义分辨率发更大的帧。
static int g_cliW = 800, g_cliH = 480;

// 链路存活：心跳连续失败 2 次判定断线，避免静默死链
static volatile bool g_linkAlive = true;
static volatile int  g_hbFail = 0;

static std::wstring A2W(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    std::wstring w;
    if (n > 0) { w.resize(n); MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
                 if (!w.empty() && w.back()==0) w.pop_back(); }
    return w;
}

static wchar_t g_statusText[128] = L"tuptup.top 车机投屏 · 等待手机";
static CRITICAL_SECTION g_csStatus;   // 保护 g_statusText：连接线程写、GUI 线程读，避免读到撕裂文本
static volatile bool g_hasFrame = false;
// resetFrame=true（默认）：非镜像态(连接中/断开/扫描)应清帧改显示文字；
// resetFrame=false：进入稳定镜像态时保留当前帧，避免把刚出的画面闪成状态文字。
static void SetStatus(const wchar_t* s, bool resetFrame = true) {
    EnterCriticalSection(&g_csStatus);
    // R9：状态未变则整路跳过。“未发现手机”分支每秒被调用一次且文本相同，
    // 旧版每次都 SetWindowText+InvalidateRect，白耗 GUI 线程并加速标题栏闪烁。
    bool same = (wcsncmp(g_statusText, s, 127) == 0);
    if (!same) { wcsncpy(g_statusText, s, 127); g_statusText[127] = 0; }
    LeaveCriticalSection(&g_csStatus);
    if (same) return;
    if (resetFrame) g_hasFrame = false;
    if (g_hwnd) {
        // 标题栏只吃一行：多行排障提示里的 \r\n 若交给 SetWindowText，WinCE 会显示成方块/乱码
        // 还可能把标题栏撑变形。这里只把首行写进标题栏，完整多行文本留在客户区由 WM_PAINT 绘制。
        std::wstring first(s);
        size_t cut = first.find(L'\n');
        size_t cutCR = first.find(L'\r');
        if (cutCR != std::wstring::npos && (cut == std::wstring::npos || cutCR < cut)) cut = cutCR;
        if (cut != std::wstring::npos) first.resize(cut);
        SetWindowText(g_hwnd, first.c_str());
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

// 把车机屏幕坐标归一化到 [0,1]（供手机端乘自身分辨率）。以【letterbox 内容区】为基准：
// 竖屏手机投到横屏车机时屏幕两侧为黑边，点黑边应 clamp 到边缘而非映射到手机屏外。
// 尚未收到首帧（无内容区）时退回全客户区归一化。
static void computeTouchNorm(HWND hwnd, int x, int y, float& nx, float& ny) {
    int cx = 0, cy = 0, cw = 0, ch = 0;
    if (g_renderer) g_renderer->contentRect(cx, cy, cw, ch);
    if (cw > 0 && ch > 0) {
        nx = (float)(x - cx) / cw;
        ny = (float)(y - cy) / ch;
    } else {
        RECT rc; GetClientRect(hwnd, &rc);
        float w = rc.right  ? (float)rc.right  : 1.0f;
        float h = rc.bottom ? (float)rc.bottom : 1.0f;
        nx = (float)x / w;
        ny = (float)y / h;
    }
    if (nx < 0) nx = 0; else if (nx > 1) nx = 1;
    if (ny < 0) ny = 0; else if (ny > 1) ny = 1;
}

// 严格 IPv4 校验：inet_addr 过于宽松，会把 "1234"、"1"、"0x7f.1" 之类也接受为地址。
// config.txt 里的笔误若不在此拦下，会被当成候选排在第一位，每轮重连都白等一次超时。
static bool validIPv4(const char* s) {
    if (!s || !*s) return false;
    unsigned a = 0, b = 0, c = 0, d = 0;
    char rest = 0;
    if (sscanf(s, "%u.%u.%u.%u %c", &a, &b, &c, &d, &rest) != 4) return false;
    return a <= 255 && b <= 255 && c <= 255 && d <= 255;
}

// 读取同目录 config.txt 的显式 IP（可选覆盖）。支持：注释行(#开头)、空行、
// 「host」「host port」「host:port」「host=...」。无有效配置则返回空（纯自动发现）。
// R10：显式端口透传到 g_cfgPort 生效（旧版解析了 pnum 却弃用，写非标端口无效）；
//      host 行非法不再 break（旧版一行笔误会废掉后面所有行）。
static int g_cfgPort = 8686;
static std::string readConfig() {
    std::string ip;
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR* p = wcsrchr(path, L'\\');
        if (p) wcscpy(p + 1, L"config.txt");
        HANDLE hf = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            char buf[512] = {0}; DWORD rd = 0;
            std::string content;
            while (ReadFile(hf, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0) {
                buf[rd] = 0; content += buf;
            }
            CloseHandle(hf);
            size_t pos = 0;
            while (pos < content.size()) {
                size_t nl = content.find('\n', pos);
                std::string line = content.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = (nl == std::string::npos) ? content.size() : nl + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                size_t s = line.find_first_not_of(" \t");
                if (s == std::string::npos) continue;   // 空行
                if (line[s] == '#') continue;           // 注释行
                std::string body = line.substr(s);
                char h[64]; int pnum = 0;
                if (sscanf(body.c_str(), "host=%63s", h) == 1) {
                    // 非法的 host 忽略并继续读下一行（R10：不再 break，避免一行笔误废掉全部配置）
                    if (validIPv4(h)) ip = h;
                    continue;
                }
                int n = sscanf(body.c_str(), "%63[^: \t]%*[: \t]%d", h, &pnum);
                if (n >= 1 && validIPv4(h)) {
                    if (n >= 2 && pnum > 1024 && pnum < 65536) g_cfgPort = pnum;  // R10：端口生效
                    ip = h;
                    break;
                }
            }
        }
    }
    return ip;
}

// 接收线程：拉视频帧 → 解码 → 渲染
static DWORD WINAPI RecvThread(LPVOID) {
    VideoFrame f;
    std::vector<BYTE> rgb; // 复用：避免每帧重新分配 8MB 级缓冲，降低低内存 WinCE 上的堆碎片与卡顿
    const long myEpoch = NetClient::ConnEpoch(); // 本线程所属“连接世代”
    while (g_running && g_net && g_net->connected() && NetClient::ConnEpoch() == myEpoch) {
        if (!g_net->recvVideoFrame(f)) {
            // 对端关闭/协议错位：立即标记断链，触发 ConnThread 秒级重连，
            // 不必等心跳(3s×2)判定，避免车机长时间显示“镜像中”却实则黑屏。
            if (g_running && g_net && g_net->connected()) g_linkAlive = false;
            break;
        }
        g_linkAlive = true; // 收到数据，链路活跃
        // 窗口不在前台（用户切到凯立德/音乐等程序）：仍然收包以维持链路与积压控制，
        // 但跳过解码与渲染——这两步是单核 ARM 上最贵的开销，后台时纯属浪费。
        if (!g_active) continue;
        int w = 0, h = 0;
        if (g_decoder.decode((BYTE)g_net->codec(), f.data.data(), (int)f.data.size(), rgb, w, h)) {
            if (g_renderer) g_renderer->present(rgb.data(), w, h);
            if (!g_hasFrame) { g_hasFrame = true; InvalidateRect(g_hwnd, NULL, FALSE); }
        }
    }
    Log("RecvThread exit (myEpoch=%ld cur=%ld connected=%d)", myEpoch, NetClient::ConnEpoch(),
        g_net ? (int)g_net->connected() : -1);
    return 0;
}

// 心跳线程
static DWORD WINAPI HeartbeatThread(LPVOID) {
    const long myEpoch = NetClient::ConnEpoch();
    while (g_running && g_net && g_net->connected() && NetClient::ConnEpoch() == myEpoch) {
        // R11：投屏前台运行时周期性重置系统空闲计时器，防止车机因“无操作”息屏/挂起——
        // 系统一挂起 socket 被冻结，恢复后必断链重连。仅在【已连接且前台激活】时重置，
        // 不改变未投屏时的正常息屏策略（后台挂起时仍收包维链，但不阻止息屏）。
        if (g_active) SystemIdleTimerReset();
        bool ok = g_net->sendHeartbeat();
        if (!ok) {
            if (++g_hbFail >= 2) g_linkAlive = false;
        } else {
            g_hbFail = 0;
        }
        Sleep(3000);
    }
    Log("HeartbeatThread exit");
    return 0;
}

// 连接管理线程：发现→连接→握手→收发→检测断线→重连。主线程只负责消息泵与 UI。
static DWORD WINAPI ConnThread(LPVOID) {
    int failRounds = 0;   // 连续连接失败轮次（用于退避，成功即清零）
    std::string cfg = readConfig();
    Log("ConnThread start, configIP='%s' port=%d", cfg.c_str(), g_cfgPort);

    while (g_running) {
        bool ok = false;
        while (g_running && !ok) {
            std::vector<std::string> cands;
            NetClient::GetCandidates(cfg, cands);
            if (cands.empty()) {
                // 还没有任何候选：后台轻量扫描在跑，提示“正在扫描”但不要卡住 UI/系统
                SetStatus(TEXT("tuptup.top 车机投屏 · 未发现手机，正在扫描网络…\r\n请在手机打开 tuptup.top App 并点「启动投屏服务」+允许录屏"));
                Sleep(1000);
                continue;
            }
            // 有高可信候选（信标/网关/.129/.1）：快速逐个连接，几乎秒连，不依赖全段扫描
            // R8 日志节流：旧版每 0.5s 一条 try/FAIL 把 256KB 日志 2 分钟刷满，改成每 10s 记一轮
            static DWORD s_lastCandLog = 0;
            bool logRound = (GetTickCount() - s_lastCandLog > 10000);
            if (logRound) {
                s_lastCandLog = GetTickCount();
                std::string joined;
                for (size_t i = 0; i < cands.size(); i++) { if (i) joined += ", "; joined += cands[i]; }
                Log("round: %d 候选 [%s]", (int)cands.size(), joined.c_str());
            }
            for (size_t i = 0; i < cands.size() && !ok; i++) {
                if (cands[i].empty()) continue;  // R8：双保险，空候选直接跳过
                std::wstring w = L"tuptup.top 车机投屏 · 正在连接 ";
                w += A2W(cands[i].c_str());
                w += L":";
                w += std::to_wstring(g_cfgPort);
                w += L"…";
                SetStatus(w.c_str());
                if (logRound) Log("try connect %s:%d", cands[i].c_str(), g_cfgPort);
                if (g_net->connectTimeout(A2W(cands[i].c_str()), g_cfgPort, 1500)) ok = true;
                else if (logRound) Log("connect FAIL %s:%d (手机端未监听/未启动App?)", cands[i].c_str(), g_cfgPort);
            }
            if (!ok) {
                // 退避：连续失败轮次越多间隔越久（500ms 起步，上限 3s）。手机 App 未启动时，
                // 原本每 500ms 就把全部候选连一遍，白占单核 CPU 与 USB 网络；成功即清零，不影响正常重连。
                failRounds++;
                // R12：连续失败≥3 轮后本轮起忽略 known IP——手机换网络（IP 变了）时，
                // 失效的 known IP 不再每轮排最前白等 1.5s×N，让信标/网关探测先命中。
                NetClient::SetDeferKnown(failRounds >= 3);
                int k = failRounds < 6 ? failRounds : 6;
                Sleep(500 * k);
            }
        }
        if (!g_running) break;
        failRounds = 0;         // 连接成功：退避清零
        NetClient::SetDeferKnown(false);   // R12：连上了，known 恢复正常优先级

        g_linkAlive = true; g_hbFail = 0;
        g_net->sendHandshakeHeadunit(g_cliW, g_cliH);   // 上报真实客户区而非名义的 800x480
        NetClient::SetLinkUp(true);   // 通知探测线程：已连上，暂停主动扫描
        NetClient::ClearScanned();    // 清掉已采纳之外的旧候选，避免重连时白等失效 IP
        SetStatus(TEXT("tuptup.top 车机投屏 · 已连接，等待手机画面…"));
        Log("connected -> handshake sent, spawning recv/heartbeat");

        HANDLE hRecv = CreateThread(NULL, 0, RecvThread, NULL, 0, NULL);
        HANDLE hHb   = CreateThread(NULL, 0, HeartbeatThread, NULL, 0, NULL);

        // 等待断线（对端关闭 或 心跳连续失败）。连上后若 6s 内未收到任何视频帧，
        // 明确提示“手机未发送画面”（多为 tuptup.top App 未授权录屏/未在前台），避免用户
        // 误以为已镜像却黑屏、无从排障。
        DWORD t0 = GetTickCount();
        bool shownMirroring = false;
        while (g_running && g_net->connected() && g_linkAlive) {
            if (!shownMirroring) {
                if (g_hasFrame) {
                    SetStatus(TEXT("tuptup.top 车机投屏 · 已连接，镜像中"), false); // 稳定镜像态：保留画面，不闪文字
                    shownMirroring = true;
                } else if (GetTickCount() - t0 > 10000) {
                    // 措辞中性：手机从点启动到真正出帧（授权弹窗+MediaProjection 初始化）可能 >6s，
                    // 不宜武断判定“未发送画面”，仅作信息性提示。
                    SetStatus(TEXT("tuptup.top 车机投屏 · 已连接，正在等待手机画面…\r\n（若长时间黑屏，请确认手机 tuptup.top App 已允许录屏并在前台运行）"));
                    shownMirroring = true;
                }
            }
            Sleep(200);
        }

        Log("disconnect detected (connected=%d linkAlive=%d)",
            g_net ? (int)g_net->connected() : -1, (int)g_linkAlive);
        g_net->sendControl(0x04);
        g_net->close();
        NetClient::SetLinkUp(false);  // 通知探测线程：已断开，恢复主动扫描
        // 等待两条后台线程真正退出再重连：close() 已让 recv 立即返回，线程会很快退出；
        // 以 4s 为上限兜底，避免极端情况下新旧 recv 线程并发读同一 socket 造成协议错位/双重解码。
        if (hRecv) { WaitForSingleObject(hRecv, 4000); CloseHandle(hRecv); hRecv = NULL; }
        if (hHb)   { WaitForSingleObject(hHb,   4000); CloseHandle(hHb);   hHb = NULL; }
        if (g_renderer) g_renderer->resetContentRect(); // 断开后清空内容区，避免重连间隙用上一台手机的宽高比映射触摸
        SetStatus(TEXT("tuptup.top 车机投屏 · 连接断开，重新发现…"));
        Sleep(500);
    }
    Log("ConnThread exit");
    return 0;
}

// 触摸按下状态：拖动中手指可能移出窗口，不 SetCapture 则 MOVE/UP 都收不到，
// 手机端会停在“按住不放”的状态（表现为某个点一直被压住、后续操作全部错乱）。
static bool  s_touchDown = false;
static POINT s_lastPt = {0, 0};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        // 视频帧与状态文字都会自行铺满整窗，不需要系统再用背景刷擦一遍：
        // 否则每次重绘都是「擦背景 → 再铺满」，连续出帧时肉眼可见闪烁。
        return 1;
    case WM_ACTIVATE:
        // 转入后台时暂停解码渲染（仍收包维链），切回前台立即恢复——省下车机上宝贵的单核 CPU。
        g_active = (LOWORD(wp) != WA_INACTIVE);
        Log("window activate active=%d", (int)g_active);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        if (g_hasFrame && g_renderer) {
            g_renderer->blit(hdc);
        } else {
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            wchar_t txt[128];
            EnterCriticalSection(&g_csStatus);
            wcsncpy(txt, g_statusText, 127); txt[127] = 0;
            LeaveCriticalSection(&g_csStatus);
            SetTextColor(hdc, RGB(0, 200, 255));
            SetBkMode(hdc, TRANSPARENT);
            // 支持多行状态文本（含 \r\n 的排障提示）：DT_SINGLELINE 会忽略 \r\n，
            // 故先以 DT_CALCRECT 量出文本高度，再整体垂直居中绘制。
            RECT tr = rc;
            DrawText(hdc, txt, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
            int th = tr.bottom - tr.top;
            RECT dr = rc; dr.top += (rc.bottom - th) / 2;
            DrawText(hdc, txt, -1, &dr, DT_CENTER | DT_WORDBREAK);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        // 首帧到达前 letterbox 内容区尚未确立，此刻映射出的坐标与画面比例不符（手机竖屏时偏差很大），直接忽略
        if (!g_hasFrame) break;
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        // 捕获鼠标：手指滑出窗口后仍继续收到 MOVE/UP，避免抬起事件丢失
        SetCapture(hwnd);
        s_touchDown = true;
        s_lastPt.x = x; s_lastPt.y = y;
        float nx = 0, ny = 0;
        computeTouchNorm(hwnd, x, y, nx, ny);
        if (g_net) g_net->sendTouch(0x00, nx, ny);   // DOWN
        break;
    }
    case WM_MOUSEMOVE:
        if ((wp & MK_LBUTTON) && g_hasFrame) {
            // 限流：OS 可能每秒产生上千次 MOVE，全部发送会洪泛网络并加剧抖动；
            // 限到 ~120Hz 足够顺滑，且最后的落点由 WM_LBUTTONUP 保证送达。
            static DWORD s_lastMove = 0;
            DWORD now = GetTickCount();
            if (now - s_lastMove >= 8) {
                s_lastMove = now;
                int x = (int)(short)LOWORD(lp);
                int y = (int)(short)HIWORD(lp);
                s_lastPt.x = x; s_lastPt.y = y;
                float nx = 0, ny = 0;
                computeTouchNorm(hwnd, x, y, nx, ny);
                if (g_net) g_net->sendTouch(0x01, nx, ny); // MOVE
            }
        }
        break;
    case WM_LBUTTONUP: {
        // R10：即使此刻无帧（断连/切后台中）也照发 UP——松手事件丢失会让手机一直停在
        // “按住”状态，后续所有触摸全部错乱；比“可能发出一个无意义 UP”严重得多。
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        float nx = 0, ny = 0;
        computeTouchNorm(hwnd, x, y, nx, ny);
        if (g_net) g_net->sendTouch(0x02, nx, ny);   // UP
        s_touchDown = false;
        ReleaseCapture();
        break;
    }
    case WM_CAPTURECHANGED: {
        // 捕获被系统剥夺（来电/系统弹窗/其他窗口抢焦点）：手指多半已经离开却收不到 UP，
        // 用最后落点补发一次 UP，避免手机端一直停留在“按住”状态。
        if (s_touchDown) {
            s_touchDown = false;
            float nx = 0, ny = 0;
            computeTouchNorm(hwnd, (int)s_lastPt.x, (int)s_lastPt.y, nx, ny);
            if (g_net) g_net->sendTouch(0x02, nx, ny);
            Log("capture lost -> 补发 TOUCH UP");
        }
        break;
    }
    case WM_DESTROY:
        // 退出时若手指还“按着”，补发一次 UP：否则车机端进程已退出、手机端却仍停留在按下状态，
        // 表现为回到手机界面后有一处触摸点一直被压住。
        if (s_touchDown && g_net) {
            float nx = 0, ny = 0;
            computeTouchNorm(hwnd, (int)s_lastPt.x, (int)s_lastPt.y, nx, ny);
            g_net->sendTouch(0x02, nx, ny);
            s_touchDown = false;
        }
        g_running = false;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPTSTR, int) {
    LogInit();
    // R10：单实例保护。双开会让两份 ConnThread 同时连手机、触摸双发、信标端口/日志互踩。
    HANDLE hSingle = CreateMutex(NULL, TRUE, TEXT("Tuptup_Mirror_SingleInstance"));
    if (!hSingle || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log("already running -> exit (single instance guard)");
        if (hSingle) CloseHandle(hSingle);
        return 2;
    }
    InitializeCriticalSection(&g_csStatus);
    Log("WinMain enter");
    Log("build %s %s (R13)", __DATE__, __TIME__);   // R11：启动日志带构建时间，排障先对版本

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInst;
    wc.lpszClassName = TEXT("TuptupWinCE");
    wc.hbrBackground = NULL;   // 自行铺满整窗（见 WM_ERASEBKGND），不交给系统擦背景，避免多余擦除导致闪烁
    if (!RegisterClass(&wc)) { Log("RegisterClass failed"); return 1; }

    // R11：窗口铺满物理屏（不再写死 800x480）。800x480 车机行为与旧版一致（客户区=屏高-标题栏）；
    // 更大分辨率的车机（如 1024x600）也能自动铺满，握手上报真实客户区，手机按它出图。
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    if (scrW < 100 || scrH < 100) { scrW = 800; scrH = 480; }  // 度量异常兜底
    g_hwnd = CreateWindowEx(0, TEXT("TuptupWinCE"), TEXT("tuptup.top 车机投屏 · 等待手机"),
        WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 0, 0, scrW, scrH, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { Log("CreateWindowEx failed"); return 1; }
    Log("window created");
    RECT crc; GetClientRect(g_hwnd, &crc);
    if (crc.right > 0)  g_cliW = crc.right;
    if (crc.bottom > 0) g_cliH = crc.bottom;
    Log("client area %dx%d (handshake 上报此尺寸)", g_cliW, g_cliH);

    g_renderer = new Renderer(g_hwnd);
    g_net = new NetClient();
    Log("renderer+net created (WSAStartup done)");

    // 启动 UDP 自动发现（监听手机广播的 IP）
    NetClient::StartDiscovery();
    Log("discovery started");

    // 连接管理放到后台线程，主线程只跑消息泵 → 窗口可正常绘制/关闭，不再“启动卡死”
    g_hConnThread = CreateThread(NULL, 0, ConnThread, NULL, 0, NULL);

    // 主线程：永久消息泵
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Log("message pump exited, cleanup");
    // 退出顺序至关重要：必须让后台线程真正停下来，才能销毁它们仍在使用的对象。
    // 此前 WM_DESTROY 只把 g_running 置 false 就继续往下走，连接/收帧线程很可能
    // 正在调用 SetStatus / renderer->present，从而访问到已被 delete 的对象（退出崩溃）。
    if (g_hConnThread) {
        if (WaitForSingleObject(g_hConnThread, 6000) == WAIT_TIMEOUT) Log("warn: conn thread join timeout");
        CloseHandle(g_hConnThread); g_hConnThread = NULL;
    }
    NetClient::StopDiscovery();     // 内部等待信标/扫描线程退出并回收句柄（此前句柄一直泄漏）
    if (g_net) { g_net->close(); delete g_net; g_net = NULL; }
    if (g_renderer) { delete g_renderer; g_renderer = NULL; }
    DeleteCriticalSection(&g_csStatus);
    Log("==== tuptup.top exit ====");
    return 0;
}
