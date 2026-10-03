package com.gloai.mirror

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.app.PendingIntent
import android.content.pm.ServiceInfo
import android.hardware.usb.UsbManager
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.net.ConnectivityManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
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
 *
 * 触发方式：
 *  - 用户手动从 MainActivity 点「启动」（首次需授权录屏）；
 *  - 插车机线时由 UsbPlugReceiver 发 ACTION_USB_START 自动拉起（日常机轻量：不拔线不常驻）；
 *    若尚缺录屏授权，自动弹/通知引导用户在系统弹窗点一次「开始」（一次性 ADB 固化后无需再进 App）。
 */
class MirrorForegroundService : Service() {

    private val CHANNEL = "gloai_mirror"
    private val TAG = "MirrorFgSvc"

    private var server: MirrorServer? = null
    private var beacon: BeaconSender? = null
    private var sender: MjpegSender? = null
    private var usbReceiver: BroadcastReceiver? = null
    // CPU 常驻锁：镜像期间保持 CPU 唤醒，避免系统息屏/省电把采集与发送线程挂起（与电池白名单配合根治后台冻结）。
    private var wakeLock: PowerManager.WakeLock? = null
    // 是否由“插线”自动拉起：拔线时若为此模式则自动退出，避免在日用机上长期常驻耗电。
    private var startedByUsb = false

    companion object {
        const val ACTION_START = "com.gloai.mirror.START"
        const val ACTION_START_SENDER = "com.gloai.mirror.START_SENDER"
        const val ACTION_USB_START = "com.gloai.mirror.USB_START"
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
            ACTION_USB_START -> {
                // 由 UsbPlugReceiver（插车机线）触发：自动拉起服务。标记由 USB 启动，拔线时自动退出。
                startedByUsb = true
                startCore(); return START_STICKY
            }
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
        // 申请 CPU 常驻锁（PARTIAL_WAKE_LOCK：仅保 CPU 唤醒，不影响系统息屏策略；屏幕常亮由 MainActivity 的 FLAG_KEEP_SCREEN_ON 负责）
        if (wakeLock == null) {
            val pm = getSystemService(PowerManager::class.java)
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "GLOAI::MirrorCpu")
            wakeLock?.setReferenceCounted(false)
        }
        if (wakeLock?.isHeld != true) wakeLock?.acquire()
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
        // 兜底：若尚未获得录屏授权（如插线自动拉起、用户尚未点过“开始”），引导授权。
        // 日常机策略：不自动点弹窗，而是弹授权页 + 常驻通知，用户点一下系统“开始”即可。
        requestConsentIfNeeded()
    }

    /**
     * 若尚未获得录屏授权，引导用户授权一次：尝试直接弹出授权页（前台服务允许），
     * 同时把通知改为“点此授权录屏”，用户点通知也能进入。授权完成后由 ConsentActivity 回灌给本服务。
     * 注意：此处【不】自动点击系统弹窗（用户选择“手动点一次”方案），避免触碰无障碍自动点击。
     */
    private fun requestConsentIfNeeded() {
        if (MirrorState.mediaProjection != null) return
        MirrorState.consentPending = true
        // 尝试直接弹授权页（部分 ROM 在后台启动 Activity 受限，失败也不影响下方通知入口）
        try {
            startActivity(Intent(this, ConsentActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            })
        } catch (_: Exception) { }
        notifyNeedConsent()
    }

    /** 把通知替换为“点此授权录屏以开始车机投屏”，点击进入 ConsentActivity。 */
    private fun notifyNeedConsent() {
        val pi = PendingIntent.getActivity(
            this, 2,
            Intent(this, ConsentActivity::class.java).apply { addFlags(Intent.FLAG_ACTIVITY_NEW_TASK) },
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
            else PendingIntent.FLAG_UPDATE_CURRENT
        )
        val n = Notification.Builder(this, CHANNEL)
            .setContentTitle("GLOAI 车机投屏")
            .setContentText("点此授权录屏以开始车机投屏")
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setContentIntent(pi)
            .setAutoCancel(true)
            .build()
        getSystemService(NotificationManager::class.java).notify(1, n)
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

        net.sendHandshakePhone(maxW = 800, maxH = 480, deviceId = MirrorState.loadDeviceId(this))
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
                val disconnected = when (action) {
                    UsbManager.ACTION_USB_DEVICE_DETACHED -> true
                    "android.intent.action.USB_STATE" ->
                        !intent.getBooleanExtra("connected", true)
                    else -> false
                }
                if (connected) {
                    Log.i(TAG, "USB device attached -> enable tethering")
                    enableUsbTethering()
                    Toast.makeText(this@MirrorForegroundService,
                        "USB 已连接：已尝试自动开启网络共享", Toast.LENGTH_SHORT).show()
                } else if (disconnected) {
                    Log.i(TAG, "USB disconnected")
                    if (startedByUsb) {
                        // 日用机轻量策略：由插线自动拉起的服务，拔线即退出，避免长期常驻耗电。
                        Toast.makeText(this@MirrorForegroundService,
                            "USB 已断开：停止车机投屏", Toast.LENGTH_SHORT).show()
                        stopEverything()
                        stopSelf()
                    }
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
        if (wakeLock?.isHeld == true) wakeLock?.release()
        wakeLock = null
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
