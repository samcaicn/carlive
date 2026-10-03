// renderer.cpp - 见 renderer.h
#include "renderer.h"
#include <windowsx.h>

Renderer::Renderer(HWND hwnd)
    : m_hwnd(hwnd), m_hdc(NULL), m_memDC(NULL), m_hbmp(NULL), m_bits(NULL),
      m_bmpW(0), m_bmpH(0), m_winW(0), m_winH(0),
      m_cx(0), m_cy(0), m_cw(0), m_ch(0), m_allocFailTick(0), m_lastPaintTick(0) {
    InitializeCriticalSection(&m_cs);
    m_hdc = GetDC(hwnd);
    RECT rc; GetClientRect(hwnd, &rc);
    m_winW = rc.right; m_winH = rc.bottom;
    if (m_hdc) m_memDC = CreateCompatibleDC(m_hdc); // 构造时建一次，避免每帧申请/释放 GDI 句柄
}

Renderer::~Renderer() {
    DeleteCriticalSection(&m_cs);
    if (m_hbmp) DeleteObject(m_hbmp);
    if (m_memDC) DeleteDC(m_memDC);
    if (m_hdc) ReleaseDC(m_hwnd, m_hdc);
}

void Renderer::alloc(int w, int h) {
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize       = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth      = w;
    bmi.bmiHeader.biHeight     = -h;   // top-down
    bmi.bmiHeader.biPlanes     = 1;
    bmi.bmiHeader.biBitCount   = 32;
    bmi.bmiHeader.biCompression= BI_RGB;

    BYTE* nbBits = NULL;
    HBITMAP nb = CreateDIBSection(m_hdc, &bmi, DIB_RGB_COLORS, (void**)&nbBits, NULL, 0);
    if (!nb || !nbBits) {
        // 分配失败（车机内存紧张）：保留旧位图与旧尺寸，不提交新尺寸。
        // 若仍写入新尺寸，后续同分辨率帧会因“尺寸未变”跳过 alloc，却始终拿不到有效位图，
        // 画面将永久黑屏，直到手机端碰巧换分辨率才恢复。
        if (nb) DeleteObject(nb);
        m_allocFailTick = GetTickCount();
        return;
    }
    if (m_hbmp) DeleteObject(m_hbmp);
    m_hbmp = nb; m_bits = nbBits;
    m_bmpW = w;  m_bmpH = h;
    m_allocFailTick = 0;
}

// 只负责“更新最新一帧像素 + 计算布局”，真正的 GDI 绘制统一在 GUI 线程的 blit() 完成。
// 背景：收帧线程与 GUI 线程同时对同一窗口 HDC / 同一内存 bitmap 做 SelectObject+StretchBlt
// 并非线程安全，且原本每帧会被拉伸绘制两次（present 一次、WM_PAINT 再一次），白白双倍开销。
bool Renderer::present(const BYTE* rgb, int w, int h) {
    if (!m_hdc || !rgb || w <= 0 || h <= 0) return false;

    EnterCriticalSection(&m_cs);
    if ((w != m_bmpW || h != m_bmpH) && m_bmpW > 0) {
        // 分辨率变化：重建位图。若刚刚分配失败过，节流 2s 再重试，避免每帧都走失败路径抖动
        if (m_allocFailTick == 0 || GetTickCount() - m_allocFailTick > 2000) alloc(w, h);
    } else if (m_bmpW == 0) {
        alloc(w, h); // 首帧
    }
    bool ok = (m_hbmp && m_bits && m_bmpW == w && m_bmpH == h);
    if (ok) memcpy(m_bits, rgb, (size_t)w * h * 4);
    if (ok) computeLayoutLocked();  // 新帧到位：同步更新 letterbox 内容矩形
    LeaveCriticalSection(&m_cs);
    if (!ok) return false;

    // 请求 GUI 线程重绘（WM_PAINT → blit）。不在此处直接绘制，保证绘制唯一发生在 GUI 线程。
    // R9 节流：m_bits 始终是最新帧，跳过中间的 InvalidateRect 是安全的——最终任意一次
    // blit 画出的都是最新画面。80ms 窗口 ≈ 上限 12.5fps，肉眼无差异，省下大半 GUI CPU。
    DWORD now = GetTickCount();
    if (now - m_lastPaintTick >= 80) {
        m_lastPaintTick = now;
        InvalidateRect(m_hwnd, NULL, FALSE);
    }
    return true;
}

// 保持宽高比（letterbox）：手机竖屏(如1080x1920)投到车机横屏(800x480)时居中显示、黑边填充，
// 避免直接拉伸导致的严重变形。
void Renderer::computeLayoutLocked() {
    RECT rc; GetClientRect(m_hwnd, &rc);
    int ww = rc.right, wh = rc.bottom;
    if (ww <= 0 || wh <= 0 || m_bmpW <= 0 || m_bmpH <= 0) { m_cw = 0; m_ch = 0; return; }
    float scale = (ww / (float)m_bmpW < wh / (float)m_bmpH)
                    ? (ww / (float)m_bmpW) : (wh / (float)m_bmpH);
    int dw = (int)(m_bmpW * scale), dh = (int)(m_bmpH * scale);
    m_cx = (ww - dw) / 2; m_cy = (wh - dh) / 2; m_cw = dw; m_ch = dh;
}

// 唯一执行 GDI 绘制的地方（GUI 线程调用）：背景填充 + 内容区拉伸。
void Renderer::blit(HDC dst) {
    RECT rc; GetClientRect(m_hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    EnterCriticalSection(&m_cs);
    // 每次重绘都按最新窗口尺寸重算：窗口大小变化后即使还没到新帧（或处于后台暂停解码状态），
    // 画面与触摸归一化也能立即用上正确的内容矩形，而不是沿用变化前的旧布局。
    computeLayoutLocked();
    HBITMAP old = NULL;
    bool has = (m_memDC && m_hbmp && m_bmpW > 0);
    int cx = m_cx, cy = m_cy, cw = m_cw, ch = m_ch;
    if (has) old = (HBITMAP)SelectObject(m_memDC, m_hbmp);
    // 先整窗填黑（清掉缩放/黑边区残留），再把内容拉伸到 letterbox 矩形
    FillRect(dst, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (has && cw > 0 && ch > 0) {
        StretchBlt(dst, cx, cy, cw, ch, m_memDC, 0, 0, m_bmpW, m_bmpH, SRCCOPY);
    }
    if (has && old) SelectObject(m_memDC, old);
    LeaveCriticalSection(&m_cs);
}

void Renderer::contentRect(int& x, int& y, int& w, int& h) {
    EnterCriticalSection(&m_cs);
    x = m_cx; y = m_cy; w = m_cw; h = m_ch;
    LeaveCriticalSection(&m_cs);
}

void Renderer::resetContentRect() {
    EnterCriticalSection(&m_cs);
    m_cx = 0; m_cy = 0; m_cw = 0; m_ch = 0;
    LeaveCriticalSection(&m_cs);
}
