package top.tuptup.mirror

import android.util.Log
import java.net.ServerSocket

/**
 * 镜像 TCP 服务端（手机侧）。监听 [port]，接受车机端的连接。
 * 每接受一个连接，回调 onConnected(NetClient) —— 由调用方完成握手 / 启动发送。
 * 仅支持单路车机（同一时刻一台），新连接会替换旧连接。
 */
class MirrorServer(
    private val port: Int = Protocol.PORT_DEFAULT,
    private val onConnected: (NetClient) -> Unit,
    private val onLog: ((String) -> Unit)? = null
) {
    private val TAG = "MirrorServer"
    @Volatile private var running = false
    private var server: ServerSocket? = null
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({ runLoop() }, "tuptup-server").also { it.start() }
    }

    fun stop() {
        running = false
        try { server?.close() } catch (_: Exception) { }
        thread?.interrupt()
        thread = null
    }

    private fun runLoop() {
        try {
            server = ServerSocket(port)
            onLog?.invoke("server listening on $port")
        } catch (e: Exception) {
            onLog?.invoke("server bind fail: ${e.message}")
            running = false
            return
        }
        while (running) {
            try {
                val sock = server!!.accept()
                onLog?.invoke("car connected from ${sock.inetAddress.hostAddress}")
                val net = NetClient()
                Thread {
                    if (net.attach(sock)) {
                        onConnected(net)
                    } else {
                        try { sock.close() } catch (_: Exception) { }
                    }
                }.start()
            } catch (e: Exception) {
                if (running) onLog?.invoke("accept: ${e.message}")
                break
            }
        }
    }
}
