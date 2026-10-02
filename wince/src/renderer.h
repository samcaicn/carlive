#pragma once
// renderer.h - GDI 渲染：把 RGB32 位图拉伸到车机窗口
#include <windows.h>
#include <vector>

class Renderer {
public:
    explicit Renderer(HWND hwnd);
    ~Renderer();
    // 输入 RGB32 (top-down, 行宽=w*4)，拉伸呈现到窗口
    bool present(const BYTE* rgb, int w, int h);
    // 把最近一帧从内部内存 DC 复制到给定 HDC（WM_PAINT 时持久重绘，避免窗口重绘后画面丢失）
    void blit(HDC dst);
    int width() const { return m_winW; }
    int height() const { return m_winH; }

private:
    HWND m_hwnd;
    HDC  m_hdc;
    HDC  m_memDC;   // 复用内存 DC，避免每帧 CreateCompatibleDC/DeleteDC 开销
    HBITMAP m_hbmp;
    BYTE* m_bits;
    int m_bmpW, m_bmpH;
    int m_winW, m_winH;
    void alloc(int w, int h);
};
