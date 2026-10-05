#pragma once
// decoder.h - 解码器：编码帧 → RGB32
// V1 主路径 MJPEG，集成 NanoJPEG（vendor 到 src/nanojpeg.c，MIT）做微型 JPEG 解码；
// H264 为可选增强，需 ffmpegce（见 decoder.cpp 注释，本版未实装）。
#include <windows.h>
#include <vector>

class Decoder {
public:
    Decoder() {}
    ~Decoder() {}

    // codec: 0=H264(Annex-B), 1=MJPEG(完整 JPEG)
    bool decode(BYTE codec, const BYTE* data, int len,
                std::vector<BYTE>& rgbOut, int& outW, int& outH);

private:
    // R20：解码前从 JPEG 字节流读取 SOF 声明的宽高（零分配，只扫 marker）。
    // 用于在 nanojpeg 巨量分配**发生之前**拦截畸形/超大尺寸帧，防 OOM 崩溃。
    static bool peekJpegSize(const BYTE* d, int len, int& outW, int& outH);
    bool decodeMJPEG(const BYTE* data, int len, std::vector<BYTE>& rgb, int& w, int& h);
    bool decodeH264 (const BYTE* data, int len, std::vector<BYTE>& rgb, int& w, int& h);
};
