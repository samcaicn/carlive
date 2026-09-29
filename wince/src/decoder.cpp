// decoder.cpp - 见 decoder.h
//
// 集成说明（重要）：
//  * MJPEG：把开源 tinyjpeg（https://github.com/chemeris/ujpeg 或 tjpgd）
//    的 .c/.h 加入工程，实现 decodeMJPEG() 调用其 API，输出 RGB32。
//    车机端 V1 默认走 MJPEG（每帧独立 JPEG，解码极简、无需 ffmpeg）。
//  * H264 ：可选增强。需 ffmpeg 的 WinCE 移植（如 ffmpegce / ffmpeg 0.6-2.x 的 CE 构建），
//    链接 avcodec/avutil，实现 decodeH264() 用 av_parser + avcodec_decode_video2，
//    再把 YUV420 转 RGB32。
//  本文件给出接口与 MJPEG 调用框架，具体第三方库编译进工程即可。

#include "decoder.h"

bool Decoder::decode(BYTE codec, const BYTE* data, int len,
                     std::vector<BYTE>& rgbOut, int& outW, int& outH) {
    if (codec == 1) return decodeMJPEG(data, len, rgbOut, outW, outH);
    if (codec == 0) return decodeH264 (data, len, rgbOut, outW, outH);
    return false;
}

bool Decoder::decodeMJPEG(const BYTE* data, int len,
                          std::vector<BYTE>& rgb, int& w, int& h) {
    // 框架：调用集成的 JPEG 解码器（tinyjpeg/tjpgd）。
    // 示例伪代码（接入真实解码器后替换）：
    //
    //   tjpgd_handle jd;
    //   rgb.resize(...);
    //   jd_decomp(&jd, data, len, rgb.data(), ...);  // 输出 RGB888/RGB32
    //
    // 因第三方库代码较长不内联，编译时把 tinyjpeg.c 一并加入工程即可。
    // 这里仅返回失败占位，确保工程可编译；接入后删除此 return。
    (void)data; (void)len; (void)rgb; (void)w; (void)h;
    return false; // TODO: 接入 tinyjpeg 后实现
}

bool Decoder::decodeH264(const BYTE* data, int len,
                         std::vector<BYTE>& rgb, int& w, int& h) {
    // 框架：ffmpegce 解码 H264 Annex-B → YUV → RGB32。
    //   av_parser_parse2(...) 切 NALU → avcodec_decode_video2(...) → sws_scale(...)
    // 见 decoder.h 顶部说明。接入 ffmpegce 后实现。
    (void)data; (void)len; (void)rgb; (void)w; (void)h;
    return false; // TODO: 接入 ffmpegce 后实现
}
