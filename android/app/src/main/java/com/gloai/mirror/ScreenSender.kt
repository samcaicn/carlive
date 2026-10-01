package com.gloai.mirror

import android.content.Context
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.HandlerThread
import android.util.Log

/**
 * 屏幕采集 + 编码（MediaCodec H.264 Baseline）→ 通过 NetClient 发送 Annex-B 流。
 * 参考 scrcpy server：用 Surface 输入 + 硬件编码，零拷贝、低延迟。
 */
class ScreenSender(
    private val context: Context,
    private val mediaProjection: MediaProjection,
    private val net: NetClient,
    private val width: Int = 1280,
    private val height: Int = 720,
    private val fps: Int = 30,
    private val bitrate: Int = 3_000_000
) {
    private val TAG = "ScreenSender"
    private var codec: MediaCodec? = null
    private var virtualDisplay: VirtualDisplay? = null
    private val thread = HandlerThread("gloai-encode")
    @Volatile private var running = false

    fun start() {
        thread.start()
        val handler = Handler(thread.looper)
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
            setInteger("bitrate", bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1) // 每秒一个关键帧，断线恢复快
            setInteger("profile", MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.M) {
                setInteger("level", MediaCodecInfo.CodecProfileLevel.AVCLevel3)
            }
        }
        codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        codec!!.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        val surface = codec!!.createInputSurface()
        virtualDisplay = mediaProjection.createVirtualDisplay(
            "GLOAI", width, height, context.resources.displayMetrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC, surface, null, null
        )
        codec!!.start()
        running = true
        handler.post { drain() }
    }

    private fun drain() {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val outIdx = codec!!.dequeueOutputBuffer(info, 10_000)
            if (outIdx >= 0) {
                val buf = codec!!.getOutputBuffer(outIdx)
                if (buf != null) {
                    val data = ByteArray(info.size)
                    buf.get(data)
                    val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    val isKey = isConfig || (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0
                    // MediaCodec AVC 输出为 avcc，转 Annex-B 后车机 ffmpeg 才能解
                    val annexb = Protocol.avccToAnnexb(data)
                    net.sendVideoFrame(isKey, info.presentationTimeUs / 1000, annexb)
                }
                codec!!.releaseOutputBuffer(outIdx, false)
            } else if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER) {
                Thread.sleep(5)
            }
        }
    }

    /** 请求关键帧（断线恢复）。注：Android 无标准强制 IDR API，靠 I_FRAME_INTERVAL 周期保证；此处重启编码确保恢复 */
    fun requestIdr() {
        try { codec?.apply { stop(); start() } } catch (e: Exception) { Log.w(TAG, "requestIdr: $e") }
    }

    fun stop() {
        running = false
        try { virtualDisplay?.release() } catch (_: Exception) { }
        try { codec?.stop(); codec?.release() } catch (_: Exception) { }
        try { thread.quitSafely() } catch (_: Exception) { }
    }
}
