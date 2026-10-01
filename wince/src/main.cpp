// main.cpp - GLOAI 车机端入口（ARM WinCE 6.0）
// 关键修复：主(GUI)线程**永远**跑消息泵（GetMessage），窗口才能正常绘制/响应；
// 网络发现/连接/收帧/心跳全部在后台线程，避免“启动卡死”（窗口创建后无消息循环→不刷新、点不动）。
// 零配置：手机 App 启动后 UDP 广播自身 IP，本端监听自动连接；USB 直连走固定共享 IP。
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

static void SetStatus(const wchar_t* s) {
    if (g_hwnd) SetWindowText(g_hwnd, s);
}

// 读取同目录 config.txt 的显式 IP（可选覆盖）。格式：「host」「host port」「host:port」。
// 留空文件或不创建则纯自动发现。
static std::string readConfig() {
    std::string ip;
    WCHAR path[MAX_PATH] = {0};
    if (GetModuleFileName(NULL, path, MAX_PATH)) {
        WCHAR* p = wcsrchr(path, L'\\');
        if (p) wcscpy(p + 1, L"config.txt");
        HANDLE hf = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            char buf[128] = {0}; DWORD rd = 0;
            if (ReadFile(hf, buf, sizeof(buf)-1, &rd, NULL) && rd > 0) {
                buf[rd] = 0;
                char h[64]; int pnum = 0;
                if (sscanf(buf, "%63s %d", h, &pnum) == 2) ip = h;
                else if (sscanf(buf, "%63[^:]:%d", h, &pnum) == 2) ip = h;
                else if (sscanf(buf, "%63s", h) == 1) ip = h;
            }
            CloseHandle(hf);
        }
    }
    return ip;
}

// 接收线程：拉视频帧 → 解码 → 渲染
static DWORD WINAPI RecvThread(LPVOID) {
    VideoFrame f;
    while (g_running && g_net && g_net->connected()) {
        if (!g_net->recvVideoFrame(f)) break;
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
            for (size_t i = 0; i < cands.size() && !ok; i++) {
                SetStatus(TEXT("GLOAI 车机投屏 · 自动发现手机…"));
                Log("try connect %s:8686", cands[i].c_str());
                if (g_net->connectTimeout(A2W(cands[i].c_str()), 8686, 1500)) ok = true;
            }
            if (!ok) Sleep(1000);
        }
        if (!g_running) break;

        g_linkAlive = true; g_hbFail = 0;
        g_net->sendHandshakeHeadunit(800, 480);
        SetStatus(TEXT("GLOAI 车机投屏 · 已连接，镜像中"));
        Log("connected -> handshake sent, spawning recv/heartbeat");

        HANDLE hRecv = CreateThread(NULL, 0, RecvThread, NULL, 0, NULL);
        HANDLE hHb   = CreateThread(NULL, 0, HeartbeatThread, NULL, 0, NULL);

        // 等待断线（对端关闭 或 心跳连续失败）
        while (g_running && g_net->connected() && g_linkAlive) Sleep(200);

        Log("disconnect detected (connected=%d linkAlive=%d)",
            g_net ? (int)g_net->connected() : -1, (int)g_linkAlive);
        g_net->sendControl(0x04);
        g_net->close();
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
    case WM_LBUTTONDOWN: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        RECT rc; GetClientRect(hwnd, &rc);
        float nx = (float)x / (rc.right  ? rc.right  : 1);
        float ny = (float)y / (rc.bottom ? rc.bottom : 1);
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
                RECT rc; GetClientRect(hwnd, &rc);
                float nx = (float)x / (rc.right  ? rc.right  : 1);
                float ny = (float)y / (rc.bottom ? rc.bottom : 1);
                if (g_net) g_net->sendTouch(0x01, nx, ny); // MOVE
            }
        }
        break;
    case WM_LBUTTONUP: {
        int x = (int)(short)LOWORD(lp);
        int y = (int)(short)HIWORD(lp);
        RECT rc; GetClientRect(hwnd, &rc);
        float nx = (float)x / (rc.right  ? rc.right  : 1);
        float ny = (float)y / (rc.bottom ? rc.bottom : 1);
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
