// decoder.cpp - 见 decoder.h
//
// MJPEG 解码：集成 NanoJPEG（MIT, KeyJ / Martin J. Fiedler, v1.3.5）——
// 一个微型基线 JPEG 解码器，输出 24-bit RGB。直接 #include 源文件，
// 不要把它再加入 CMake/编译源列表（否则会导致符号重复定义）。
// H264 解码（ffmpegce）为明确延后的可选路径，本版不实装（见 decodeH264 占位）。

#include "decoder.h"
#include "log.h"

// 微型 JPEG 解码器源文件（已 vendor 到 src/nanojpeg.c，MIT 许可）。
#include "nanojpeg.c"

bool Decoder::decode(BYTE codec, const BYTE* data, int len,
                     std::vector<BYTE>& rgbOut, int& outW, int& outH) {
    if (codec == 1) return decodeMJPEG(data, len, rgbOut, outW, outH);
    if (codec == 0) return decodeH264 (data, len, rgbOut, outW, outH);
    return false;
}

// R20：在真正解码**之前**从 JPEG 字节流里读出 SOF 声明的宽高。
// 用途：nanojpeg 会在 njDecode 内部按 SOF 宽高直接分配 width*height*ncomp 内存，
// 所以"先解码再检查尺寸"这种保护形同虚设（巨量分配已经发生，进程已死）。
// 这里只扫 marker 段、不做任何解码，零分配，可以安全地用来做前置拦截。
// 支持 SOF0(0xC0)/SOF2(0xC2) 等基线/渐进式；跳过 APPn/DHT/SOS 等待变尺寸的段。
bool Decoder::peekJpegSize(const BYTE* d, int len, int& outW, int& outH) {
    if (!d || len < 4) return false;
    if (d[0] != 0xFF || d[1] != 0xD8) return false;        // 缺 SOI
    int i = 2;
    while (i + 3 < len) {
        if (d[i] != 0xFF) { i++; continue; }              // 填充字节，继续找 marker
        BYTE m = d[i + 1];
        if (m == 0xFF) { i++; continue; }                 // 连续 FF
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }  // 独立标记
        if (i + 3 >= len) break;
        int segLen = (d[i + 2] << 8) | d[i + 3];
        if (segLen < 2 || i + 2 + segLen > len) break;   // 段长非法/截断
        // SOF0..SOF15，排除 DHT(C4)/JPG(C8)/DAC(CC)
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (segLen < 7) return false;                 // SOF 段至少 8 字节
            outH = (d[i + 5] << 8) | d[i + 6];
            outW = (d[i + 7] << 8) | d[i + 8];
            return outW > 0 && outH > 0;
        }
        if (m == 0xDA) return false;                      // SOS：图像数据开始，再往前没有尺寸
        i += 2 + segLen;
    }
    return false;
}

bool Decoder::decodeMJPEG(const BYTE* data, int len,
                          std::vector<BYTE>& rgb, int& w, int& h) {
    if (!data || len <= 0) return false;
    // R20 致命修复（OOM 防护前置）：原代码先 njDecode 再查尺寸，**顺序完全错了**——
    //   nanojpeg 在 njDecode 内部就按 SOF 里未经校验的宽高分配
    //   （nanojpeg.c: `nj.rgb = njAllocMem(width*height*ncomp)`），
    //   一张声明 20000×20000 的畸形 JPEG 会在分配阶段申请 ~1.2GB，进程当场死亡，
    //   后面的 8M 像素检查形同虚设。
    // 现改为：解码**之前**先扫 SOF0/SOF2 标记读出真实宽高，超限直接拒——
    //   巨量分配被挡在发生之前。像素上限按车机实际画布收紧到 2M
    //  （车机客户区仅 800×480≈0.38M 像素，2M 已远超需要，远低于 64MB 设备的承受力）。
    {
        const long long MAX_PIXELS = 2000000LL;   // ≈1400×1430
        int jw = 0, jh = 0;
        if (peekJpegSize(data, len, jw, jh) && (long long)jw * (long long)jh > MAX_PIXELS) {
            Log("decodeMJPEG: SOF 声明 %dx%d 超上限(%lld像素)，解码前拒绝防OOM", jw, jh, MAX_PIXELS);
            return false;   // 关键：巨量分配**尚未发生**
        }
    }
    njInit();
    if (njDecode(data, len) != NJ_OK) { njDone(); return false; }

    int iw = njGetWidth();
    int ih = njGetHeight();
    if (iw <= 0 || ih <= 0) { njDone(); return false; }  // 防御：解码成功但产出 0 尺寸（损坏流）
    // R15/R20：后置像素上限保护保留（作为第二道防线），上限与前置检查统一为 2M。
    // 车机客户区仅 800×480≈0.38M 像素，2M 已远超需要；8M（≈32MB RGB32 + nanojpeg
    // 内部 24MB + 视频缓冲）在 64MB 级 WinCE 上必然触发 OOM。
    if ((long long)iw * (long long)ih > 2000000LL) {
        Log("decodeMJPEG: 单帧 %dx%d 超像素上限(2M)，丢弃防OOM", iw, ih);
        njDone(); return false;
    }
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
