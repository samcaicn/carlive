package com.gloai.mirror

import android.content.Intent
import android.media.MediaProjectionManager
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import android.util.Log

class MainActivity : AppCompatActivity() {

    private val REQ_MEDIA = 1001
    private lateinit var etIp: EditText
    private lateinit var tvStatus: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        etIp = findViewById(R.id.etIp)
        tvStatus = findViewById(R.id.tvStatus)

        val net = NetClient().also { n ->
            n.onTouch = { action, x, y ->
                runOnUiThread { MirrorAccessibilityService.instance?.injectTouch(x, y, action) }
            }
            n.onDisconnect = { runOnUiThread { tvStatus.text = "断开" } }
            n.onLog = { m -> Log.d("GLOAI", m) }
        }
        MirrorState.net = net

        findViewById<Button>(R.id.btnStart).setOnClickListener { startMirror() }
        findViewById<Button>(R.id.btnStop).setOnClickListener { stopMirror() }
    }

    private fun startMirror() {
        val target = etIp.text.toString().ifBlank { "192.168.1.50:8686" }
        val (host, port) = parseTarget(target)
        val net = MirrorState.net ?: return
        if (!net.connect(host, port)) {
            Toast.makeText(this, "连接车机失败，请检查 IP/端口与 WiFi", Toast.LENGTH_SHORT).show()
            return
        }
        net.sendHandshakePhone(maxW = 800, maxH = 480)
        net.sendVideoConfig(Protocol.CODEC_H264, 1280, 720, 30, 3_000_000)
        tvStatus.text = "连接中…"
        requestMedia()
    }

    private fun parseTarget(t: String): Pair<String, Int> {
        val parts = t.split(":")
        return parts[0] to (parts.getOrNull(1)?.toIntOrNull() ?: Protocol.PORT_DEFAULT)
    }

    @Suppress("DEPRECATION")
    private fun requestMedia() {
        val mgr = getSystemService(MediaProjectionManager::class.java)
        startActivityForResult(mgr.createScreenCaptureIntent(), REQ_MEDIA)
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_MEDIA && resultCode == RESULT_OK && data != null) {
            val mgr = getSystemService(MediaProjectionManager::class.java)
            MirrorState.mediaProjection = mgr.getMediaProjection(resultCode, data)
            startForegroundMirror()
        } else {
            tvStatus.text = "已取消录屏授权"
        }
    }

    private fun startForegroundMirror() {
        startService(Intent(this, MirrorForegroundService::class.java))
        tvStatus.text = "镜像中（请确认已开启无障碍服务以回注触摸）"
        if (MirrorAccessibilityService.instance == null) {
            Toast.makeText(this, R.string.toast_enable_a11y, Toast.LENGTH_LONG).show()
            startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
        }
    }

    private fun stopMirror() {
        MirrorState.net?.sendControl(Protocol.CTRL_BYE)
        MirrorState.net?.close()
        stopService(Intent(this, MirrorForegroundService::class.java))
        tvStatus.text = "已停止"
    }

    override fun onDestroy() {
        stopMirror()
        super.onDestroy()
    }
}
