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

    fun start() {
        thread.start()
        val handler = Handler(thread.looper)
        reader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3)
        val surface = reader!!.surface
        virtualDisplay = mediaProjection.createVirtualDisplay(
            "GLOAI", width, height, context.resources.displayMetrics.densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_PUBLIC, surface, null, null
        )
        running = true
        handler.post { loop() }
    }

    private fun loop() {
        val r = reader ?: return
        val baos = ByteArrayOutputStream(256 * 1024)
        while (running) {
            val t0 = System.currentTimeMillis()
            val image = try { r.acquireLatestImage() } catch (_: Exception) { null }
            if (image == null) { sleepQuiet(5); continue }
            try {
                val plane = image.planes[0]
                val buffer = plane.buffer
                val rowStride = plane.rowStride
                val pixels = IntArray(width * height)
                var off = 0
                // 逐字节读取 RGBA（按内存顺序），规避字节序与行对齐差异。
                for (y in 0 until height) {
                    var p = y * rowStride
                    for (x in 0 until width) {
                        buffer.position(p)
                        val rr = buffer.get().toInt() and 0xFF
                        val gg = buffer.get().toInt() and 0xFF
                        val bb = buffer.get().toInt() and 0xFF
                        buffer.get() // 跳过 alpha
                        pixels[off++] = (0xFF shl 24) or (rr shl 16) or (gg shl 8) or bb
                        p += 4
                    }
                }
                val bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                bmp.setPixels(pixels, 0, width, 0, 0, width, height)
                baos.reset()
                bmp.compress(Bitmap.CompressFormat.JPEG, quality, baos)
                bmp.recycle()
                val jpeg = baos.toByteArray()
                // MJPEG：每帧独立可解，统一标记为关键帧（车机解码器忽略该位）。
                net.sendVideoFrame(true, t0, jpeg)
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

    private fun sleepQuiet(ms: Long) {
        try { Thread.sleep(ms) } catch (_: InterruptedException) { }
    }

    fun stop() {
        running = false
        try { virtualDisplay?.release() } catch (_: Exception) { }
        try { reader?.close() } catch (_: Exception) { }
        try { thread.quitSafely() } catch (_: Exception) { }
    }
}
