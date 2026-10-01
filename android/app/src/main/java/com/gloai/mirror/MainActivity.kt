package com.gloai.mirror

import android.accessibilityservice.AccessibilityServiceInfo
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.View
import android.view.accessibility.AccessibilityManager
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import android.util.Log
import java.io.File

class MainActivity : AppCompatActivity() {

    private val REQ_MEDIA = 1001
    private lateinit var tvStatus: TextView
    private var mediaRequested = false
    private var a11yPromptedOnce = false   // 避免每次 onResume 都把用户弹到设置页

    // 前台时低频刷新状态（车机连上的瞬间由服务侧驱动，界面需自行感知）
    private val statusHandler = Handler(Looper.getMainLooper())
    private val statusTick = object : Runnable {
        override fun run() {
            refreshStatus()
            statusHandler.postDelayed(this, 2000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        installCrashHandler()
        reportLastCrash()
        setContentView(R.layout.activity_main)

        tvStatus = findViewById(R.id.tvStatus)

        findViewById<Button>(R.id.btnStart).setOnClickListener { startMirror() }
        findViewById<Button>(R.id.btnStop).setOnClickListener { stopMirror() }
        findViewById<Button>(R.id.btnA11y).setOnClickListener {
            openAccessibilitySettings()
        }

        // 首次进入即请求录屏授权，授权后自动启动投屏服务（车机端会被自动发现并连接）。
        refreshStatus()
        if (!isAccessibilityEnabled()) {
            Toast.makeText(this, R.string.toast_enable_a11y, Toast.LENGTH_LONG).show()
            openAccessibilitySettings()
            a11yPromptedOnce = true
        }
        requestMedia()
    }

    override fun onResume() {
        super.onResume()
        // 从系统设置返回后重新判定：无障碍是否已开、录屏是否已授权
        refreshStatus()
        statusHandler.removeCallbacks(statusTick)
        statusHandler.post(statusTick)
    }

    override fun onPause() {
        super.onPause()
        statusHandler.removeCallbacks(statusTick)
    }

    private fun startMirror() {
        requestMedia()
    }

    private fun stopMirror() {
        startService(Intent(this, MirrorForegroundService::class.java).apply {
            action = MirrorForegroundService.ACTION_STOP
        })
        refreshStatus()
    }

    private fun openAccessibilitySettings() {
        startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS))
    }

    /**
     * 判定无障碍服务是否已在系统设置中启用。
     * 不能用 MirrorAccessibilityService.instance 判空 —— 那是系统绑定服务后才赋值的（异步），
     * 首次启动/刚开完设置时大概率还没绑定，会被误判成"未开启"。
     * 这里以系统 Settings.Secure 的已启用列表为准，再用 AccessibilityManager 与运行期实例兜底。
     */
    private fun isAccessibilityEnabled(): Boolean {
        val expected = ComponentName(this, MirrorAccessibilityService::class.java).flattenToString()

        // 1) 系统已启用列表（权威）
        val enabled = Settings.Secure.getString(contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES)
        if (!enabled.isNullOrEmpty()) {
            for (token in enabled.split(":")) {
                val t = token.trim()
                if (t.equals(expected, ignoreCase = true)) return true
                // 某些 ROM 存的是完整类名或 pkg/.Cls 变体
                if (t.contains(packageName, ignoreCase = true) &&
                    t.contains(MirrorAccessibilityService::class.java.simpleName, ignoreCase = true)
                ) return true
            }
        }

        // 2) AccessibilityManager 兜底
        val am = getSystemService(Context.ACCESSIBILITY_SERVICE) as? AccessibilityManager
        val infos = am?.getEnabledAccessibilityServiceList(AccessibilityServiceInfo.FEEDBACK_ALL_MASK)
        if (infos != null) {
            for (info in infos) {
                val si = info.resolveInfo?.serviceInfo ?: continue
                if (si.packageName == packageName &&
                    si.name == MirrorAccessibilityService::class.java.name
                ) return true
            }
        }

        // 3) 运行期实例（已绑定则必然已启用）
        return MirrorAccessibilityService.instance != null
    }

    /** 把「触摸回注 / 录屏授权 / 连接」三项状态显式呈现，未开的给入口。 */
    private fun refreshStatus() {
        val a11y = isAccessibilityEnabled()
        val media = MirrorState.mediaProjection != null

        findViewById<Button>(R.id.btnA11y).visibility =
            if (a11y) View.GONE else View.VISIBLE

        val link = when {
            MirrorState.net != null -> getString(R.string.link_streaming)
            MirrorForegroundService.isRunning -> getString(R.string.link_waiting)
            else -> getString(R.string.link_idle)
        }

        tvStatus.text = listOf(
            if (a11y) getString(R.string.touch_ready) else getString(R.string.touch_off),
            if (media) getString(R.string.media_ready) else getString(R.string.media_off),
            link
        ).joinToString("\n")

        // 未开无障碍时只提示一次，避免反复弹设置页困住用户
        if (!a11y && !a11yPromptedOnce) {
            a11yPromptedOnce = true
            Toast.makeText(this, R.string.toast_enable_a11y, Toast.LENGTH_LONG).show()
        }
    }

    @Suppress("DEPRECATION")
    private fun requestMedia() {
        if (mediaRequested) return
        mediaRequested = true
        val mgr = getSystemService(MediaProjectionManager::class.java)
        startActivityForResult(mgr.createScreenCaptureIntent(), REQ_MEDIA)
    }

    @Suppress("DEPRECATION")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_MEDIA && resultCode == RESULT_OK && data != null) {
            // 不在此处 getMediaProjection：Android 14 必须先以 mediaProjection 前台服务身份运行才能获取，
            // 否则 SecurityException 闪退。把 resultCode/data 透传给服务，由服务前台化之后再取。
            startMirrorService(Intent(this, MirrorForegroundService::class.java).apply {
                action = MirrorForegroundService.ACTION_START
                putExtra(MirrorForegroundService.EXTRA_MP_RESULT, resultCode)
                putExtra(MirrorForegroundService.EXTRA_MP_DATA, data)
            })
            // 若车机已连上，立即起推流
            startMirrorService(Intent(this, MirrorForegroundService::class.java).apply {
                action = MirrorForegroundService.ACTION_START_SENDER
            })
            refreshStatus()
        } else {
            mediaRequested = false
            Toast.makeText(this, R.string.toast_need_media, Toast.LENGTH_LONG).show()
            refreshStatus()
        }
    }

    /** API 26+ 用 startForegroundService（避免「未调用 startForeground」ANR/崩溃），低版本回退 startService */
    private fun startMirrorService(intent: Intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent)
        } else {
            @Suppress("DEPRECATION")
            startService(intent)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.d("GLOAI", "MainActivity destroyed")
    }

    /** 全局未捕获异常落盘：下次还崩可直接读到真实堆栈，无需 adb。 */
    private fun installCrashHandler() {
        val def = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { t, e ->
            try {
                val f = File(getExternalFilesDir(null), "crash.log")
                f.writeText("${System.currentTimeMillis()}\n${Log.getStackTraceString(e)}")
            } catch (_: Exception) { }
            def?.uncaughtException(t, e)
        }
    }

    /** 若上次崩溃留有 crash.log，启动时读出前若干行提示用户（便于反馈）。 */
    private fun reportLastCrash() {
        try {
            val f = File(getExternalFilesDir(null), "crash.log")
            if (f.exists()) {
                val lines = f.readLines().take(12).joinToString("\n")
                Toast.makeText(this, "上次崩溃日志:\n$lines", Toast.LENGTH_LONG).show()
            }
        } catch (_: Exception) { }
    }
}
