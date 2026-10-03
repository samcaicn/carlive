package top.tuptup.mirror

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InterfaceAddress
import java.net.NetworkInterface

/**
 * UDP 自动发现信标发送端（手机侧）。
 * 每隔 1s 向本机每个 IPv4 接口（WiFi / USB 网络共享）的广播地址 :8687 发送
 *   TUPTUP|<本机IPv4>|<端口>
 * 车机端监听 :8687 即可零配置学到手机 IP 并自动连接。
 */
class BeaconSender(private val port: Int = Protocol.PORT_DEFAULT) {

    private val TAG = "BeaconSender"
    @Volatile private var running = false
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({ runLoop() }, "tuptup-beacon").also { it.start() }
    }

    fun stop() {
        running = false
        thread?.interrupt()
        thread = null
    }

    private fun runLoop() {
        while (running) {
            try {
                val payload = buildString {
                    append("TUPTUP|")
                    append(localIpV4() ?: "0.0.0.0")
                    append("|")
                    append(port)
                }.toByteArray(Charsets.UTF_8)

                val nis = NetworkInterface.getNetworkInterfaces()
                for (ni in nis) {
                    if (!ni.isUp || ni.isLoopback || ni.isVirtual) continue
                    for (ia: InterfaceAddress in ni.interfaceAddresses) {
                        val addr = ia.address
                        val bc = ia.broadcast
                        if (addr is Inet4Address && bc != null) {
                            try {
                                DatagramSocket().use { ds ->
                                    ds.soTimeout = 1000
                                    ds.broadcast = true
                                    val p = DatagramPacket(payload, payload.size, bc, BEACON_PORT)
                                    ds.send(p)
                                }
                            } catch (e: Exception) {
                                Log.v(TAG, "send on ${ni.name} skip: ${e.message}")
                            }
                        }
                    }
                }
            } catch (e: Exception) {
                Log.v(TAG, "beacon loop: ${e.message}")
            }
            try { Thread.sleep(1000) } catch (_: InterruptedException) { break }
        }
    }

    /** 取一个可用的本机 IPv4（优先 WiFi / USB 共享，排除回环与虚拟）。 */
    private fun localIpV4(): String? {
        val nis = NetworkInterface.getNetworkInterfaces()
        var fallback: String? = null
        for (ni in nis) {
            if (!ni.isUp || ni.isLoopback || ni.isVirtual) continue
            for (ia in ni.interfaceAddresses) {
                val a = ia.address
                if (a is Inet4Address && !a.isLoopbackAddress) {
                    val ip = a.hostAddress ?: continue
                    if (ip.startsWith("192.168.42.") || ip.startsWith("192.168.43.") ||
                        ni.name.contains("wlan", true) || ni.name.contains("ap", true)
                    ) return ip
                    if (fallback == null) fallback = ip
                }
            }
        }
        return fallback
    }

    companion object {
        const val BEACON_PORT = 8687
    }
}
