// renderer.cpp - 见 renderer.h
#include "renderer.h"
#include <windowsx.h>

Renderer::Renderer(HWND hwnd)
    : m_hwnd(hwnd), m_hdc(NULL), m_memDC(NULL), m_hbmp(NULL), m_bits(NULL),
      m_bmpW(0), m_bmpH(0), m_winW(0), m_winH(0) {
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
    HBITMAP old = (HBITMAP)SelectObject(m_memDC, m_hbmp);
    RECT rc; GetClientRect(m_hwnd, &rc);
    // 拉伸到窗口（车机分辨率可能 ≠ 手机分辨率）
    StretchBlt(m_hdc, 0, 0, rc.right, rc.bottom,
               m_memDC, 0, 0, w, h, SRCCOPY);
    SelectObject(m_memDC, old);
    return true;
}
