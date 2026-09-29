package com.gloai.mirror

/**
 * GLOAI 投屏镜像协议常量与字节工具。
 * 详见 proto/protocol.md。所有多字节字段大端（网络序）。
 */
object Protocol {
    val MAGIC: ByteArray = byteArrayOf(0x47, 0x4C, 0x4F, 0x41) // "GLOA"
    const val PORT_DEFAULT = 8686

    // ---- 消息类型 ----
    const val TYPE_HANDSHAKE: Byte = 0x01
    const val TYPE_VIDEO_CONFIG: Byte = 0x02
    const val TYPE_VIDEO_FRAME: Byte = 0x03
    const val TYPE_TOUCH_EVENT: Byte = 0x04
    const val TYPE_CONTROL: Byte = 0x05
    const val TYPE_HEARTBEAT: Byte = 0x06

    // ---- 编解码 ----
    const val CODEC_H264: Byte = 0x00
    const val CODEC_MJPEG: Byte = 0x01

    // ---- 触摸动作 ----
    const val TOUCH_DOWN: Byte = 0x00
    const val TOUCH_MOVE: Byte = 0x01
    const val TOUCH_UP: Byte = 0x02

    // ---- 控制码 ----
    const val CTRL_REQUEST_IDR: Byte = 0x01
    const val CTRL_PAUSE: Byte = 0x02
    const val CTRL_RESUME: Byte = 0x03
    const val CTRL_BYE: Byte = 0x04

    /** 把 avcc（4 字节长度前缀）字节流转为 Annex-B（00 00 00 01 起始码），供车机 ffmpeg 解码 */
    fun avccToAnnexb(src: ByteArray): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        var i = 0
        while (i + 4 <= src.size) {
            val len = ((src[i].toInt() and 0xFF) shl 24) or
                    ((src[i + 1].toInt() and 0xFF) shl 16) or
                    ((src[i + 2].toInt() and 0xFF) shl 8) or
                    (src[i + 3].toInt() and 0xFF)
            i += 4
            out.write(0); out.write(0); out.write(0); out.write(1) // start code
            if (i + len <= src.size) {
                out.write(src, i, len)
                i += len
            } else break
        }
        return out.toByteArray()
    }
}
