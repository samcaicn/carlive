#pragma once
// decoder.h - 解码器：编码帧 → RGB32
// V1 主路径 MJPEG（无需 ffmpeg，集成轻量 JPEG 解码器如 tinyjpeg/tjpgd）；
// H264 为可选增强，需 ffmpegce（见 decoder.cpp 注释）。
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
    bool decodeMJPEG(const BYTE* data, int len, std::vector<BYTE>& rgb, int& w, int& h);
    bool decodeH264 (const BYTE* data, int len, std::vector<BYTE>& rgb, int& w, int& h);
};
