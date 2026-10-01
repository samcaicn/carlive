package com.gloai.mirror

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.hardware.usb.UsbManager
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.ConnectivityManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.widget.Toast

/**
 * 镜像控制器（前台服务）。职责：
 *  - 启动 UDP 信标(BeaconSender) 让车机自动发现手机 IP；
 *  - 启动 TCP 服务端(MirrorServer) 接受车机连接；
 *  - 检测 USB 设备插入，尽量自动开启 USB 网络共享（车机走 USB 直连时手机侧 IP 固定）；
 *  - 车机连上后：设触摸回注回调、互发握手/视频配置、启动 MjpegSender 推流。
 * 全程零手动 IP 输入。
 */
class MirrorForegroundService : Service() {

    private val CHANNEL = "gloai_mirror"
    private val TAG = "MirrorFgSvc"

    private var server: MirrorServer? = null
    private var beacon: BeaconSender? = null
    private var sender: MjpegSender? = null
    private var usbReceiver: BroadcastReceiver? = null

    companion object {
        const val ACTION_START = "com.gloai.mirror.START"
        const val ACTION_START_SENDER = "com.gloai.mirror.START_SENDER"
        const val ACTION_STOP = "com.gloai.mirror.STOP"

        /** 录屏授权结果透传：Android 14 必须在已是 mediaProjection 前台服务后获取 MediaProjection */
        const val EXTRA_MP_RESULT = "mp_result"
        const val EXTRA_MP_DATA = "mp_data"

        /** 核心（信标/服务端）是否已启动，供界面显示连接状态 */
        @Volatile var isRunning = false
    }

    // 录屏授权透传（仅 ACTION_START 携带），由服务在前台化之后再取 MediaProjection
    private var hasProjectionData = false
    private var pendingResultCode = 0
    private var pendingData: Intent? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        val notification = buildNotification("GLOAI 车机投屏 · 等待连接")
        // Android 14(API 34) 起：声明了 mediaProjection 类型的前台服务必须用 3 参数 startForeground，
        // 否则抛 MissingForegroundServiceTypeException 直接闪退。
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                1, notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            )
        } else {
            @Suppress("DEPRECATION")
            startForeground(1, notification)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { stopEverything(); stopSelf(); return START_NOT_STICKY }
            ACTION_START_SENDER -> { startSenderIfReady(); return START_STICKY }
            ACTION_START -> {
                // 录屏授权结果透传给服务：Android 14 必须先以前台服务(mediaProjection 类型)身份运行，
                // 再 getMediaProjection，否则抛 SecurityException 闪退。故此处仅存参数，真正获取在 startCore。
                hasProjectionData = intent.hasExtra(EXTRA_MP_DATA)
                pendingResultCode = intent.getIntExtra(EXTRA_MP_RESULT, 0)
                pendingData = getParcelableExtraCompat(intent, EXTRA_MP_DATA)
                startCore(); return START_STICKY
            }
            else -> { startCore(); return START_STICKY }
        }
    }

    private fun getParcelableExtraCompat(intent: Intent, key: String): Intent? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            intent.getParcelableExtra(key, Intent::class.java)
        else
            @Suppress("DEPRECATION") intent.getParcelableExtra(key)
    }

    private fun startCore() {
        // 在已是 mediaProjection 前台服务的前提下获取 MediaProjection（Android 14 强制要求）。
        // pendingData/pendingResultCode 是 var，Kotlin 无法智能转型，先捕获到局部 val。
        if (hasProjectionData && MirrorState.mediaProjection == null) {
            val data = pendingData
            val rc = pendingResultCode
            if (data != null) {
                try {
                    val mgr = getSystemService(MediaProjectionManager::class.java)
                    val mp = mgr.getMediaProjection(rc, data)
                    // Android 14(API 34) 硬性要求：createVirtualDisplay 前必须先 registerCallback，
                    // 否则抛 IllegalStateException 且发生在 ServerSocket 线程里会直接崩进程。
                    mp.registerCallback(object : MediaProjection.Callback() {
                        override fun onStop() {
                            Log.i(TAG, "MediaProjection stopped by system -> release")
                            MirrorState.mediaProjection = null
                            stopSender()
                            updateNotification("GLOAI 车机投屏 · 录屏已停止")
                        }
                    }, Handler(Looper.getMainLooper()))
                    MirrorState.mediaProjection = mp
                    Log.i(TAG, "MediaProjection acquired inside foreground service")
                } catch (e: Exception) {
                    Log.e(TAG, "getMediaProjection failed: ${e.message}")
                }
            }
            hasProjectionData = false
            pendingData = null
        }
        isRunning = true
        if (beacon == null) {
            beacon = BeaconSender(Protocol.PORT_DEFAULT).also { it.start() }
        }
        if (server == null) {
            server = MirrorServer(
                port = Protocol.PORT_DEFAULT,
                onConnected = { net -> handleClient(net) },
                onLog = { m -> Log.d(TAG, m) }
            ).also { it.start() }
        }
        registerUsbReceiver()
        updateNotification("GLOAI 车机投屏 · 自动发现中")
        // 若授权已就绪且已连上车机，直接起推流
        startSenderIfReady()
    }

    /** 车机连上：配置回调、握手、视频配置，并视授权情况起推流。 */
    private fun handleClient(net: NetClient) {
        MirrorState.net = net
        net.onTouch = { action, x, y ->
            MirrorAccessibilityService.instance?.injectTouch(x, y, action)
        }
        net.onDisconnect = {
            Log.i(TAG, "car disconnected")
            stopSender()
            MirrorState.net = null
            updateNotification("GLOAI 车机投屏 · 等待连接")
        }
        net.onLog = { m -> Log.d("GLOAI", m) }

        net.sendHandshakePhone(maxW = 800, maxH = 480)
        // V1 车机端只解码 MJPEG（微型 JPEG 解码器），手机默认走 MJPEG 以端到端可解。
        net.sendVideoConfig(Protocol.CODEC_MJPEG, 800, 480, 15, 2_000_000)
        updateNotification("GLOAI 车机投屏 · 已连接，镜像中")
        startSenderIfReady()
    }

    /** 媒体授权 + 已连车机 都满足时，启动 MJPEG 推流（幂等）。 */
    private fun startSenderIfReady() {
        val net = MirrorState.net
        val mp = MirrorState.mediaProjection
        if (net == null || mp == null) {
            if (net != null) updateNotification("GLOAI 车机投屏 · 等待录屏授权")
            return
        }
        if (sender != null) return
        sender = MjpegSender(this, mp, net, 800, 480, 15, 70)
        sender?.start()
        Log.i(TAG, "MjpegSender started")
        updateNotification("GLOAI 车机投屏 · 镜像中")
    }

    private fun stopSender() {
        sender?.stop()
        sender = null
    }

    private fun registerUsbReceiver() {
        if (usbReceiver != null) return
        usbReceiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context?, intent: Intent?) {
                val action = intent?.action ?: return
                val connected = when (action) {
                    UsbManager.ACTION_USB_DEVICE_ATTACHED -> true
                    "android.intent.action.USB_STATE" ->
                        intent.getBooleanExtra("connected", false)
                    else -> false
                }
                if (connected) {
                    Log.i(TAG, "USB device attached -> enable tethering")
                    enableUsbTethering()
                    Toast.makeText(this@MirrorForegroundService,
                        "USB 已连接：已尝试自动开启网络共享", Toast.LENGTH_SHORT).show()
                }
            }
        }
        // "android.intent.action.USB_STATE" 非公开 SDK 常量，用字面量（带 "connected" extra 表示 USB 连线状态）
        val filter = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction("android.intent.action.USB_STATE")
        }
        // Android 13(API 33) 起：动态注册接收器必须显式声明导出标志，否则 IllegalArgumentException 闪退。
        val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            Context.RECEIVER_NOT_EXPORTED else 0
        registerReceiver(usbReceiver, filter, flags)
    }

    /** 尽量自动开启 USB 网络共享（隐藏 API，best-effort，失败则提示用户手动开）。 */
    private fun enableUsbTethering() {
        try {
            val cm = getSystemService(ConnectivityManager::class.java)
            val m = cm.javaClass.getMethod("setUsbTethering", Boolean::class.javaPrimitiveType)
            m.invoke(cm, true)
        } catch (e: Exception) {
            Log.w(TAG, "enableUsbTethering failed: ${e.message}")
            Toast.makeText(this,
                "请手动开启「USB 网络共享」(设置→连接→移动热点/网络共享)", Toast.LENGTH_LONG).show()
        }
    }

    private fun stopEverything() {
        isRunning = false
        stopSender()
        server?.stop(); server = null
        beacon?.stop(); beacon = null
        try { usbReceiver?.let { unregisterReceiver(it) } } catch (_: Exception) { }
        usbReceiver = null
        try { MirrorState.mediaProjection?.stop() } catch (_: Exception) { }
        MirrorState.mediaProjection = null
    }

    override fun onDestroy() {
        stopEverything()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createChannel() {
        val mgr = getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mgr.createNotificationChannel(
                NotificationChannel(CHANNEL, "车机投屏", NotificationManager.IMPORTANCE_LOW)
            )
        }
    }

    private fun buildNotification(text: String): Notification {
        val b = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            Notification.Builder(this, CHANNEL) else Notification.Builder(this)
        @Suppress("DEPRECATION")
        return b.setContentTitle("GLOAI 车机投屏")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .build()
    }

    private fun updateNotification(text: String) {
        val mgr = getSystemService(NotificationManager::class.java)
        mgr.notify(1, buildNotification(text))
    }
}
