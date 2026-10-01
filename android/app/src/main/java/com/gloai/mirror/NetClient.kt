package com.gloai.mirror

import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.Socket
import kotlin.concurrent.thread

/**
 * 与车机端（WinCEClient）的 TCP 通信。
 * 同一连接双向复用：视频下行 / 触摸上行 / 控制 / 心跳。
 */
class NetClient {

    private var socket: Socket? = null
    private var out: DataOutputStream? = null
    @Volatile private var `in`: DataInputStream? = null
    @Volatile private var running = false

    // 回调（在接收线程触发，调用方自行切 UI 线程）
    var onTouch: ((action: Byte, nx: Float, ny: Float) -> Unit)? = null
    var onControl: ((code: Byte) -> Unit)? = null
    var onDisconnect: (() -> Unit)? = null
    var onLog: ((String) -> Unit)? = null

    fun connect(host: String, port: Int): Boolean {
        return try {
            attach(Socket(host, port))
        } catch (e: Exception) {
            onLog?.invoke("connect fail: ${e.message}")
            false
        }
    }

    /**
     * 复用已建立的 Socket（来自 ServerSocket.accept()）。车机端(本 App 作服务端)接受连接后调用。
     * 与 connect() 共用同一套收发/心跳/触摸逻辑。
     */
    fun attach(accepted: Socket): Boolean {
        return try {
            socket = accepted
            socket!!.soTimeout = 0 // 读循环自行按心跳判定断线
            socket!!.tcpNoDelay = true // 关闭 Nagle：触摸/视频小包立即发出，降低交互延迟
            out = DataOutputStream(BufferedOutputStream(socket!!.getOutputStream()))
            `in` = DataInputStream(BufferedInputStream(socket!!.getInputStream()))
            running = true
            thread(name = "gloai-net-rx") { readLoop() }
            true
        } catch (e: Exception) {
            onLog?.invoke("attach fail: ${e.message}")
            false
        }
    }

    // ---------- 发送 ----------
    @Synchronized private fun writeMessage(type: Byte, payload: ByteArray) {
        try {
            out?.let { o ->
                o.write(Protocol.MAGIC)
                o.writeByte(type.toInt())
                o.writeInt(payload.size)
                o.write(payload)
                o.flush()
            }
        } catch (e: Exception) {
            onLog?.invoke("write fail: ${e.message}")
            close()
        }
    }

    fun sendHandshakePhone(maxW: Int, maxH: Int) {
        // 简化：用 JSON 表达本端能力（仅握手一次）
        val json = """{"role":"phone","proto_ver":1,"caps":{"video_encoders":["h264","mjpeg"],"max_w":$maxW,"max_h":$maxH,"touch":true}}"""
        writeMessage(Protocol.TYPE_HANDSHAKE, json.toByteArray(Charsets.UTF_8))
    }

    fun sendVideoConfig(codec: Byte, w: Int, h: Int, fps: Int, bitrateK: Int) {
        val p = ByteArray(10)
        p[0] = codec
        p[1] = (w ushr 8).toByte(); p[2] = (w and 0xFF).toByte()
        p[3] = (h ushr 8).toByte(); p[4] = (h and 0xFF).toByte()
        p[5] = fps.toByte()
        p[6] = (bitrateK ushr 24).toByte(); p[7] = (bitrateK ushr 16).toByte()
        p[8] = (bitrateK ushr 8).toByte(); p[9] = (bitrateK and 0xFF).toByte()
        writeMessage(Protocol.TYPE_VIDEO_CONFIG, p)
    }

    /** @param isKey 是否关键帧；@param timestampMs 毫秒；@param annexb 已转为 Annex-B 的 H264 或完整 JPEG */
    fun sendVideoFrame(isKey: Boolean, timestampMs: Long, annexb: ByteArray) {
        val body = ByteArray(9 + annexb.size)
        body[0] = if (isKey) 1 else 0
        body[1] = (timestampMs ushr 24).toByte(); body[2] = (timestampMs ushr 16).toByte()
        body[3] = (timestampMs ushr 8).toByte(); body[4] = (timestampMs and 0xFF).toByte()
        body[5] = (annexb.size ushr 24).toByte(); body[6] = (annexb.size ushr 16).toByte()
        body[7] = (annexb.size ushr 8).toByte(); body[8] = (annexb.size and 0xFF).toByte()
        System.arraycopy(annexb, 0, body, 9, annexb.size)
        writeMessage(Protocol.TYPE_VIDEO_FRAME, body)
    }

    fun sendTouch(action: Byte, nx: Float, ny: Float) {
        val p = ByteArray(10)
        p[0] = action
        // float 大端
        java.nio.ByteBuffer.allocate(4).putFloat(nx).array().copyInto(p, 1)
        java.nio.ByteBuffer.allocate(4).putFloat(ny).array().copyInto(p, 5)
        p[9] = 0 // pointer_id
        writeMessage(Protocol.TYPE_TOUCH_EVENT, p)
    }

    fun sendControl(code: Byte) = writeMessage(Protocol.TYPE_CONTROL, byteArrayOf(code))
    fun sendHeartbeat() = writeMessage(Protocol.TYPE_HEARTBEAT, byteArrayOf())

    // ---------- 接收 ----------
    private fun readLoop() {
        val input = `in` ?: return
        try {
            while (running) {
                // MAGIC (4)
                val magic = ByteArray(4)
                if (!readFully(input, magic, 4)) break
                if (!magic.contentEquals(Protocol.MAGIC)) {
                    // 重同步：丢弃直到下一个 MAGIC
                    resync(input)
                    continue
                }
                val type = input.readByte()
                val len = input.readInt()
                if (len < 0 || len > 0x00FFFFFF) { close(); break }
                val payload = ByteArray(len)
                if (!readFully(input, payload, len)) break
                dispatch(type, payload)
            }
        } catch (e: Exception) {
            onLog?.invoke("readLoop end: ${e.message}")
        } finally {
            running = false
            onDisconnect?.invoke()
        }
    }

    private fun dispatch(type: Byte, payload: ByteArray) {
        when (type) {
            Protocol.TYPE_TOUCH_EVENT -> {
                if (payload.size >= 10) {
                    val action = payload[0]
                    val nx = java.nio.ByteBuffer.wrap(payload, 1, 4).float
                    val ny = java.nio.ByteBuffer.wrap(payload, 5, 4).float
                    onTouch?.invoke(action, nx, ny)
                }
            }
            Protocol.TYPE_CONTROL -> if (payload.isNotEmpty()) onControl?.invoke(payload[0])
            Protocol.TYPE_HEARTBEAT -> { /* 保活，无需处理 */ }
            else -> onLog?.invoke("unhandled type $type")
        }
    }

    private fun readFully(input: DataInputStream, buf: ByteArray, n: Int): Boolean {
        var off = 0
        while (off < n) {
            val r = input.read(buf, off, n - off)
            if (r < 0) return false
            off += r
        }
        return true
    }

    /** 流错位时扫描到下一个 MAGIC 为止 */
    private fun resync(input: DataInputStream) {
        // 简单策略：逐字节读，尝试匹配 MAGIC[1..]
        try {
            var expect = 1
            while (expect < 4) {
                val b = input.readByte()
                if (b == Protocol.MAGIC[expect]) expect++ else expect = if (b == Protocol.MAGIC[0]) 1 else 0
            }
        } catch (_: Exception) { }
    }

    @Synchronized fun close() {
        running = false
        try { out?.close() } catch (_: Exception) { }
        try { `in`?.close() } catch (_: Exception) { }
        try { socket?.close() } catch (_: Exception) { }
    }

    fun isConnected() = running && socket?.isConnected == true
}
