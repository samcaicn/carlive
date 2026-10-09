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
    // 当前帧在窗口中的【内容区】矩形（保持宽高比的 letterbox 布局）。
    // 触摸坐标归一化以此为基准，黑边区域 clamp 到边缘，避免点黑边映射到手机屏外。
    // 线程安全读取内容矩形（present 在收帧线程写、触摸归一化在 GUI 线程读）。
    // 四个坐标在 ARM 上不是原子写出，GUI 线程必须整组读取，否则可能读到半个更新后的矩形。
    void contentRect(int& x, int& y, int& w, int& h);
    // 断开/重连时清空内容区，使触摸归一化退回全客户区，避免用上一台手机的宽高比映射坐标
    void resetContentRect();

private:
    HWND m_hwnd;
    HDC  m_hdc;
    HDC  m_memDC;   // 复用内存 DC，避免每帧 CreateCompatibleDC/DeleteDC 开销
    HBITMAP m_hbmp;
    BYTE* m_bits;
    int m_bmpW, m_bmpH;
    int m_winW, m_winH;
    int m_cx, m_cy, m_cw, m_ch; // 内容区矩形（letterbox 居中）
    DWORD m_allocFailTick;      // 上次位图分配失败的 tick：低内存时节流重试，避免每帧重复失败造成抖动
    DWORD m_lastPaintTick;      // R9：上次请求重绘的 tick——StretchBlt 是单核车机最贵的 GUI 操作，
                                // 手机 15fps 每帧都 InvalidateRect 会把 CPU 吃满；节流到 ~12fps 无感差异
    CRITICAL_SECTION m_cs;      // 保护 位图句柄/像素缓冲/内容矩形：present 在收帧线程，blit 与 contentRect 在 GUI 线程
    void alloc(int w, int h);
    // 按“当前窗口客户区 + 当前位图尺寸”重算 letterbox 内容矩形。调用方必须已持有 m_cs。
    void computeLayoutLocked();
};
