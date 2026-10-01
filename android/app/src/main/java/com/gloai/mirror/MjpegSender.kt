package com.gloai.mirror

import android.content.Context
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import java.io.ByteArrayOutputStream

/**
 * 屏幕采集 + MJPEG 编码（每帧独立 JPEG）→ 经 NetClient 发送。
 * 车机端用集成的微型 JPEG 解码器（NanoJPEG）逐帧解码，无需 ffmpeg。
 * 因每帧是完整 JPEG，断线重连后首帧即可出图；采用 CPU 编码，分辨率/帧率取保守值。
 *
 * 性能优化（相对初版）：
 *  - 用一次 bulk ByteBuffer.get() 替代逐字节 150 万次 read，并把像素填进复用的 IntArray；
 *  - 复用 Bitmap / ByteArrayOutputStream，消除每帧 new Bitmap 引发的 GC 抖动；
 *  - 静态帧采样哈希跳过编码（画面不变时不占 CPU/带宽）；
 *  - 每 30 帧打印平均编码耗时，便于真机实测。
 */
class MjpegSender(
    private val context: Context,
    private val mediaProjection: MediaProjection,
    private val net: NetClient,
    private val width: Int = 800,
    private val height: Int = 480,
    private val fps: Int = 15,
    private val quality: Int = 70
) {
    private val TAG = "MjpegSender"
    private var reader: ImageReader? = null
    private var virtualDisplay: VirtualDisplay? = null
    private val thread = HandlerThread("gloai-mjpeg")
    @Volatile private var running = false
    private val frameIntervalMs = 1000L / fps.coerceAtLeast(1)

    // 复用缓冲，避免每帧分配导致 GC 抖动
    private var rgbBuf: ByteArray? = null
    private var pixels: IntArray? = null
    private var bmp: Bitmap? = null
    private var lastHash = -1

    fun start() {
        thread.start()
        val handler = Handler(thread.looper)
        reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3)
        val surface = reader!!.surface
        try {
            virtualDisplay = mediaProjection.createVirtualDisplay(
                "GLOAI", width, height, context.resources.displayMetrics.densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC, surface, null, null
            )
        } catch (e: Exception) {
            Log.e(TAG, "createVirtualDisplay failed: $e")
            running = false
            return
        }
        running = true
        handler.post { loop() }
    }

    private fun loop() {
        val r = reader ?: return
        val baos = ByteArrayOutputStream(256 * 1024)
        var frameCount = 0
        var accMs = 0L
        while (running) {
            val t0 = System.currentTimeMillis()
            val image = try { r.acquireLatestImage() } catch (_: Exception) { null }
            if (image == null) { sleepQuiet(5); continue }
            try {
                val plane = image.planes[0]
                val rowStride = plane.rowStride
                val w = width; val h = height
                val need = rowStride * h
                var rgb = rgbBuf
                if (rgb == null || rgb.size < need) { rgb = ByteArray(need); rgbBuf = rgb }
                // 一次性 bulk 拷贝，替代逐字节 150 万次 get()
                plane.buffer.get(rgb, 0, need)

                var px = pixels
                if (px == null || px.size < w * h) { px = IntArray(w * h); pixels = px }
                // 紧凑索引循环：RGBA → ARGB_8888 IntArray（无方法调用开销）
                var d = 0
                for (y in 0 until h) {
                    var ss = y * rowStride
                    for (x in 0 until w) {
                        val r8 = rgb[ss].toInt() and 0xFF
                        val g8 = rgb[ss + 1].toInt() and 0xFF
                        val b8 = rgb[ss + 2].toInt() and 0xFF
                        px[d++] = (0xFF shl 24) or (r8 shl 16) or (g8 shl 8) or b8
                        ss += 4
                    }
                }

                // 静态帧跳过：画面未变则只做哈希、不编码/不发送，省 CPU 与带宽
                val hsh = sampleHash(px, w * h)
                if (hsh == lastHash) {
                    image.close()
                    sleepQuiet(frameIntervalMs)   // 维持 ~fps 轮询节奏，变化可被及时检出
                    continue
                }
                lastHash = hsh

                var b = bmp
                if (b == null) { b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888); bmp = b }
                b.setPixels(px, 0, w, 0, 0, w, h)
                baos.reset()
                b.compress(Bitmap.CompressFormat.JPEG, quality, baos)
                val jpeg = baos.toByteArray()
                // MJPEG：每帧独立可解，统一标记为关键帧（车机解码器忽略该位）。
                net.sendVideoFrame(true, t0, jpeg)

                val used = System.currentTimeMillis() - t0
                accMs += used; frameCount++
                if (frameCount >= 30) {
                    val avg = accMs / frameCount
                    Log.i(TAG, "perf: avgEncode=${avg}ms estFps=${(1000 / avg)} frames=$frameCount")
                    accMs = 0; frameCount = 0
                }
            } catch (e: Exception) {
                Log.w(TAG, "frame err: $e")
            } finally {
                image.close()
            }
            val used = System.currentTimeMillis() - t0
            val sleep = frameIntervalMs - used
            if (sleep > 0) sleepQuiet(sleep)
        }
    }

    /** 轻量采样哈希：用于静态帧检测，碰撞概率足以忽略（不影响交互）。 */
    private fun sampleHash(px: IntArray, n: Int): Int {
        var h = 0
        val step = (n / 1023).coerceAtLeast(1)
        var i = 0
        while (i < n) { h = (h * 31 + (px[i] and 0xFFFF)) and 0x7FFFFFFF; i += step }
        return h
    }

    private fun sleepQuiet(ms: Long) {
        try { Thread.sleep(ms) } catch (_: InterruptedException) { }
    }

    fun stop() {
        running = false
        try { virtualDisplay?.release() } catch (_: Exception) { }
        try { reader?.close() } catch (_: Exception) { }
        try { thread.quitSafely() } catch (_: Exception) { }
        bmp?.recycle()
    }
}
