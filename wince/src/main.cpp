// main.cpp - GLOAI 车机端入口（ARM WinCE 6.0）
// 流程：连接手机 → 握手脚手 → 收视频帧 → 解码 → GDI 渲染；触摸 → 编码 → 发回手机
#include "net.h"
#include "renderer.h"
#include "decoder.h"

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

static std::wstring A2W(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    std::wstring w;
    if (n > 0) { w.resize(n); MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
                 if (!w.empty() && w.back()==0) w.pop_back(); }
    return w;
}

// 读取同目录 config.txt：「host port」或「host:port」或「host」，默认 192.168.1.100:8686
static void readConfig(std::wstring& host, int& port) {
    host = L"192.168.1.100"; port = 8686;
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
                if (sscanf(buf, "%63s %d", h, &pnum) == 2) { host = A2W(h); port = pnum; }
                else if (sscanf(buf, "%63[^:]:%d", h, &pnum) == 2) { host = A2W(h); port = pnum; }
                else if (sscanf(buf, "%63s", h) == 1) { host = A2W(h); }
            }
            CloseHandle(hf);
        }
    }
}

// 接收线程：拉视频帧 → 解码 → 渲染
static DWORD WINAPI RecvThread(LPVOID) {
    VideoFrame f;
    while (g_running && g_net && g_net->connected()) {
        if (!g_net->recvVideoFrame(f)) break;
        std::vector<BYTE> rgb; int w = 0, h = 0;
        if (g_decoder.decode((BYTE)g_net->codec(), f.data.data(), (int)f.data.size(), rgb, w, h)) {
            if (g_renderer) g_renderer->present(rgb.data(), w, h);
        }
    }
    return 0;
}

// 心跳线程
static DWORD WINAPI HeartbeatThread(LPVOID) {
    while (g_running && g_net && g_net->connected()) {
        g_net->sendHeartbeat();
        Sleep(3000);
    }
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
            int x = (int)(short)LOWORD(lp);
            int y = (int)(short)HIWORD(lp);
            RECT rc; GetClientRect(hwnd, &rc);
            float nx = (float)x / (rc.right  ? rc.right  : 1);
            float ny = (float)y / (rc.bottom ? rc.bottom : 1);
            if (g_net) g_net->sendTouch(0x01, nx, ny); // MOVE
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
    std::wstring host; int port;
    readConfig(host, port);

    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance   = hInst;
    wc.lpszClassName = TEXT("GLOAIWinCE");
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClass(&wc)) return 1;

    g_hwnd = CreateWindowEx(0, TEXT("GLOAIWinCE"), TEXT("GLOAI 车机投屏"),
        WS_VISIBLE | WS_CAPTION, 0, 0, 800, 480, NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    g_net = new NetClient();
    if (!g_net->connect(host, port)) {
        MessageBox(g_hwnd, TEXT("无法连接手机，请检查 WiFi / IP / 端口"), TEXT("GLOAI"), MB_OK);
        delete g_net; g_net = NULL;
        return 1;
    }
    g_net->sendHandshakeHeadunit(800, 480);

    g_renderer = new Renderer(g_hwnd);

    CreateThread(NULL, 0, RecvThread, NULL, 0, NULL);
    CreateThread(NULL, 0, HeartbeatThread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_running = false;
    if (g_net) { g_net->sendControl(0x04); g_net->close(); delete g_net; }
    if (g_renderer) delete g_renderer;
    return 0;
}
