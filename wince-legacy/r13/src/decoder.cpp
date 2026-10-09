// decoder.cpp - 见 decoder.h
//
// MJPEG 解码：集成 NanoJPEG（MIT, KeyJ / Martin J. Fiedler, v1.3.5）——
// 一个微型基线 JPEG 解码器，输出 24-bit RGB。直接 #include 源文件，
// 不要把它再加入 CMake/编译源列表（否则会导致符号重复定义）。
// H264 解码（ffmpegce）为明确延后的可选路径，本版不实装（见 decodeH264 占位）。

#include "decoder.h"

// 微型 JPEG 解码器源文件（已 vendor 到 src/nanojpeg.c，MIT 许可）。
#include "nanojpeg.c"

bool Decoder::decode(BYTE codec, const BYTE* data, int len,
                     std::vector<BYTE>& rgbOut, int& outW, int& outH) {
    if (codec == 1) return decodeMJPEG(data, len, rgbOut, outW, outH);
    if (codec == 0) return decodeH264 (data, len, rgbOut, outW, outH);
    return false;
}

bool Decoder::decodeMJPEG(const BYTE* data, int len,
                          std::vector<BYTE>& rgb, int& w, int& h) {
    if (!data || len <= 0) return false;
    njInit();
    if (njDecode(data, len) != NJ_OK) { njDone(); return false; }

    int iw = njGetWidth();
    int ih = njGetHeight();
    if (iw <= 0 || ih <= 0) { njDone(); return false; }  // 防御：解码成功但产出 0 尺寸（损坏流）
    const unsigned char* src = njGetImage();
    if (!src) { njDone(); return false; }
    int ncomp = (njGetImageSize() / (iw * ih)); // 1(灰度) / 3(RGB) / 4(RGBA，极少见)

    // 渲染器要求 RGB32（4 字节/像素，top-down）：把 24-bit RGB 扩成 RGB32。
    // ⚠ 关键：32-bit BI_RGB 的 DIB 在内存中的字节布局是【B,G,R,X】（低字节在前是 Blue），
    // 而 NanoJPEG 输出的是逐像素【R,G,B】。若按原顺序写入，DIB 会把 R 当 Blue 解释，
    // 画面整体红蓝颠倒（人物皮肤发蓝、蓝天变红）——必须交叉写入。
    rgb.resize((size_t)iw * ih * 4);
    if (ncomp == 3) {
        for (size_t i = 0; (int)i < iw * ih; ++i) {
            rgb[i*4+0] = src[i*3+2];   // B
            rgb[i*4+1] = src[i*3+1];   // G
            rgb[i*4+2] = src[i*3+0];   // R
            rgb[i*4+3] = 0xFF;
        }
    } else if (ncomp == 4) { // 罕见 4 分量：同样按 BGRX 交叉取前三分量，避免被当成灰度误读
        for (size_t i = 0; (int)i < iw * ih; ++i) {
            rgb[i*4+0] = src[i*4+2];
            rgb[i*4+1] = src[i*4+1];
            rgb[i*4+2] = src[i*4+0];
            rgb[i*4+3] = 0xFF;
        }
    } else { // 灰度（极少见）：三通道同值，通道顺序无关紧要
        for (size_t i = 0; (int)i < iw * ih; ++i) {
            unsigned char v = src[i];
            rgb[i*4+0] = v; rgb[i*4+1] = v; rgb[i*4+2] = v; rgb[i*4+3] = 0xFF;
        }
    }
    w = iw; h = ih;
    njDone();
    return true;
}

bool Decoder::decodeH264(const BYTE* data, int len,
                         std::vector<BYTE>& rgb, int& w, int& h) {
    // 可选增强：ffmpeg 的 WinCE 移植（ffmpegce）解码 H264 Annex-B → YUV → RGB32。
    // 因体积与交叉编译复杂度，V1 不实装；车机端 V1 默认走 MJPEG（见 DESIGN.md）。
    (void)data; (void)len; (void)rgb; (void)w; (void)h;
    return false; // TODO: 接入 ffmpegce 后实现
}
