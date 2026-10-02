// renderer.cpp - 见 renderer.h
#include "renderer.h"
#include <windowsx.h>

Renderer::Renderer(HWND hwnd)
    : m_hwnd(hwnd), m_hdc(NULL), m_memDC(NULL), m_hbmp(NULL), m_bits(NULL),
      m_bmpW(0), m_bmpH(0), m_winW(0), m_winH(0),
      m_cx(0), m_cy(0), m_cw(0), m_ch(0) {
    m_hdc = GetDC(hwnd);
    RECT rc; GetClientRect(hwnd, &rc);
    m_winW = rc.right; m_winH = rc.bottom;
}

Renderer::~Renderer() {
    if (m_hbmp) DeleteObject(m_hbmp);
    if (m_memDC) DeleteDC(m_memDC);
    if (m_hdc) ReleaseDC(m_hwnd, m_hdc);
}

void Renderer::alloc(int w, int h) {
    if (m_hbmp) { DeleteObject(m_hbmp); m_hbmp = NULL; }
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize       = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth      = w;
    bmi.bmiHeader.biHeight     = -h;   // top-down
    bmi.bmiHeader.biPlanes     = 1;
    bmi.bmiHeader.biBitCount   = 32;
    bmi.bmiHeader.biCompression= BI_RGB;
    m_hbmp = CreateDIBSection(m_hdc, &bmi, DIB_RGB_COLORS, (void**)&m_bits, NULL, 0);
    m_bmpW = w; m_bmpH = h;
}

bool Renderer::present(const BYTE* rgb, int w, int h) {
    if (!m_hdc || !rgb || w <= 0 || h <= 0) return false;
    if (w != m_bmpW || h != m_bmpH) alloc(w, h);
    if (!m_hbmp || !m_bits) return false;
    memcpy(m_bits, rgb, (size_t)w * h * 4);

    // 复用内存 DC（首次创建一次），避免每帧 CreateCompatibleDC/DeleteDC 的 GDI 句柄开销
    if (!m_memDC) m_memDC = CreateCompatibleDC(m_hdc);
    if (!m_memDC) return false;

    // 保持宽高比（letterbox）：手机竖屏(如1080x1920)投到车机横屏(800x480)时居中显示、黑边填充，
    // 避免直接拉伸导致的严重变形。
    RECT rc; GetClientRect(m_hwnd, &rc);
    int ww = rc.right, wh = rc.bottom;
    if (ww <= 0 || wh <= 0) return false;
    float scale = (ww / (float)w < wh / (float)h) ? (ww / (float)w) : (wh / (float)h);
    int dw = (int)(w * scale), dh = (int)(h * scale);
    m_cx = (ww - dw) / 2; m_cy = (wh - dh) / 2; m_cw = dw; m_ch = dh;

    HBITMAP old = (HBITMAP)SelectObject(m_memDC, m_hbmp);
    // 先整窗填黑（清掉上一帧残留 & 黑边），再拉伸内容到居中矩形
    FillRect(m_hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    StretchBlt(m_hdc, m_cx, m_cy, m_cw, m_ch,
               m_memDC, 0, 0, w, h, SRCCOPY);
    SelectObject(m_memDC, old);
    return true;
}

void Renderer::blit(HDC dst) {
    RECT rc; GetClientRect(m_hwnd, &rc);
    FillRect(dst, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (!m_memDC || !m_hbmp || m_bmpW == 0) return;
    HBITMAP old = (HBITMAP)SelectObject(m_memDC, m_hbmp);
    // 用与 present 一致的内容矩形重绘，保持黑边与比例
    StretchBlt(dst, m_cx, m_cy, m_cw, m_ch, m_memDC, 0, 0, m_bmpW, m_bmpH, SRCCOPY);
    SelectObject(m_memDC, old);
}
