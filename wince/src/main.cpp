// main.cpp - GLOAI 车机端入口（ARM WinCE 6.0）
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

static wchar_t g_statusText[128] = L"GLOAI 车机投屏 · 等待手机";
static volatile bool g_hasFrame = false;
static void SetStatus(const wchar_t* s) {
    wcsncpy(g_statusText, s, 127); g_statusText[127] = 0;
    g_hasFrame = false; // 状态切换（含重连）时重置，重新显示文字
    if (g_hwnd) { SetWindowText(g_hwnd, s); InvalidateRect(g_hwnd, NULL, FALSE); }
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

// 读取同目录 config.txt 的显式 IP（可选覆盖）。支持：注释行(#开头)、空行、
// 「host」「host port」「host:port」「host=...」。无有效配置则返回空（纯自动发现）。
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
                if (sscanf(body.c_str(), "host=%63s", h) == 1) { ip = h; break; }
                if (sscanf(body.c_str(), "%63[^: ]%*[: ]%d", h, &pnum) >= 1) { ip = h; break; }
            }
        }
    }
    return ip;
}

// 接收线程：拉视频帧 → 解码 → 渲染
static DWORD WINAPI RecvThread(LPVOID) {
    VideoFrame f;
    while (g_running && g_net && g_net->connected()) {
        if (!g_net->recvVideoFrame(f)) {
            // 对端关闭/协议错位：立即标记断链，触发 ConnThread 秒级重连，
            // 不必等心跳(3s×2)判定，避免车机长时间显示“镜像中”却实则黑屏。
            if (g_running && g_net && g_net->connected()) g_linkAlive = false;
            break;
        }
        g_linkAlive = true;
        // 防积压：车机解码跟不上发送节奏时，内核收包缓冲会堆积，延迟将无限累积。
        // 堆积超过阈值则跳过本帧解码（继续收包排空积压），只解最新的帧。
        u_long backlog = 0;
        if (ioctlsocket(g_net->sock(), FIONREAD, &backlog) == 0 && backlog > 96*1024) {
            continue;
        }
        std::vector<BYTE> rgb; int w = 0, h = 0;
        if (g_decoder.decode((BYTE)g_net->codec(), f.data.data(), (int)f.data.size(), rgb, w, h)) {
            if (g_renderer) g_renderer->present(rgb.data(), w, h);
            if (!g_hasFrame) { g_hasFrame = true; InvalidateRect(g_hwnd, NULL, FALSE); }
        }
    }
    Log("RecvThread exit (connected=%d)", g_net ? (int)g_net->connected() : -1);
    return 0;
}

// 心跳线程
static DWORD WINAPI HeartbeatThread(LPVOID) {
    while (g_running && g_net && g_net->connected()) {
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
    std::string cfg = readConfig();
    Log("ConnThread start, configIP='%s'", cfg.c_str());

    while (g_running) {
        bool ok = false;
        while (g_running && !ok) {
            std::vector<std::string> cands;
            NetClient::GetCandidates(cfg, cands);
            if (cands.empty()) {
                // 还没有任何候选：后台轻量扫描在跑，提示“正在扫描”但不要卡住 UI/系统
                SetStatus(TEXT("GLOAI 车机投屏 · 未发现手机，正在扫描网络…\r\n请在手机打开 GLOAI App 并点「启动投屏服务」+允许录屏"));
                Sleep(1000);
                continue;
            }
            // 有高可信候选（信标/网关/.1）：快速逐个连接，几乎秒连，不依赖全段扫描
            for (size_t i = 0; i < cands.size() && !ok; i++) {
                std::wstring w = L"GLOAI 车机投屏 · 正在连接 ";
                w += A2W(cands[i].c_str());
                w += L":8686…";
                SetStatus(w.c_str());
                Log("try connect %s:8686", cands[i].c_str());
                if (g_net->connectTimeout(A2W(cands[i].c_str()), 8686, 1500)) ok = true;
                else Log("connect FAIL %s:8686 (手机端未监听/未启动App?)", cands[i].c_str());
            }
            if (!ok) Sleep(500); // 本轮候选均未响应，稍后（扫描线程可能已补充新候选）再试
        }
        if (!g_running) break;

        g_linkAlive = true; g_hbFail = 0;
        g_net->sendHandshakeHeadunit(800, 480);
        NetClient::SetLinkUp(true);   // 通知探测线程：已连上，暂停主动扫描
        SetStatus(TEXT("GLOAI 车机投屏 · 已连接，等待手机画面…"));
        Log("connected -> handshake sent, spawning recv/heartbeat");

        HANDLE hRecv = CreateThread(NULL, 0, RecvThread, NULL, 0, NULL);
        HANDLE hHb   = CreateThread(NULL, 0, HeartbeatThread, NULL, 0, NULL);

        // 等待断线（对端关闭 或 心跳连续失败）。连上后若 6s 内未收到任何视频帧，
        // 明确提示“手机未发送画面”（多为 GLOAI App 未授权录屏/未在前台），避免用户
        // 误以为已镜像却黑屏、无从排障。
        DWORD t0 = GetTickCount();
        bool shownMirroring = false;
        while (g_running && g_net->connected() && g_linkAlive) {
            if (!shownMirroring) {
                if (g_hasFrame) {
                    SetStatus(TEXT("GLOAI 车机投屏 · 已连接，镜像中"));
                    shownMirroring = true;
                } else if (GetTickCount() - t0 > 6000) {
                    SetStatus(TEXT("GLOAI 车机投屏 · 已连接，但手机未发送画面\r\n请确认手机 GLOAI App 已允许录屏并在前台运行"));
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
        if (hRecv) { WaitForSingleObject(hRecv, 2000); CloseHandle(hRecv); }
        if (hHb)   { WaitForSingleObject(hHb,   2000); CloseHandle(hHb); }
        SetStatus(TEXT("GLOAI 车机投屏 · 连接断开，重新发现…"));
        Sleep(500);
    }
    Log("ConnThread exit");
    return 0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        if (g_hasFrame && g_renderer) {
            g_renderer->blit(hdc);
        } else {
            RECT rc; GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetTextColor(hdc, RGB(0, 200, 255));
            SetBkMode(hdc, TRANSPARENT);
            DrawText(hdc, g_statusText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
            float nx = 0, ny = 0;
            computeTouchNorm(hwnd, x, y, nx, ny);
            if (g_net) g_net->sendTouch(0x00, nx, ny);   // DOWN
        break;
    }
    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON) {
            // 限流：OS 可能每秒产生上千次 MOVE，全部发送会洪泛网络并加剧抖动；
            // 限到 ~120Hz 足够顺滑，且最后的落点由 WM_LBUTTONUP 保证送达。
            static DWORD s_lastMove = 0;
            DWORD now = GetTickCount();
            if (now - s_lastMove >= 8) {
                s_lastMove = now;
                int x = (int)(short)LOWORD(lp);
                int y = (int)(short)HIWORD(lp);
                float nx = 0, ny = 0;
                computeTouchNorm(hwnd, x, y, nx, ny);
                if (g_net) g_net->sendTouch(0x01, nx, ny); // MOVE
            }
        }
        break;
    case WM_LBUTTONUP: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
            float nx = 0, ny = 0;
            computeTouchNorm(hwnd, x, y, nx, ny);
            if (g_net) g_net->sendTouch(0x02, nx, ny);   // UP
        break;
    }
    case WM_DESTROY:
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
    Log("WinMain enter");

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInst;
    wc.lpszClassName = TEXT("GLOAIWinCE");
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClass(&wc)) { Log("RegisterClass failed"); return 1; }

    g_hwnd = CreateWindowEx(0, TEXT("GLOAIWinCE"), TEXT("GLOAI 车机投屏 · 等待手机"),
        WS_VISIBLE | WS_CAPTION | WS_SYSMENU, 0, 0, 800, 480, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { Log("CreateWindowEx failed"); return 1; }
    Log("window created");

    g_renderer = new Renderer(g_hwnd);
    g_net = new NetClient();
    Log("renderer+net created (WSAStartup done)");

    // 启动 UDP 自动发现（监听手机广播的 IP）
    NetClient::StartDiscovery();
    Log("discovery started");

    // 连接管理放到后台线程，主线程只跑消息泵 → 窗口可正常绘制/关闭，不再“启动卡死”
    CreateThread(NULL, 0, ConnThread, NULL, 0, NULL);

    // 主线程：永久消息泵
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Log("message pump exited, cleanup");
    NetClient::StopDiscovery();
    if (g_net) { g_net->close(); delete g_net; }
    if (g_renderer) delete g_renderer;
    Log("==== GLOAI exit ====");
    return 0;
}
