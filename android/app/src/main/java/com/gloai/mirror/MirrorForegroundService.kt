package com.gloai.mirror

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log

/**
 * 镜像前台服务（Android 10+ 要求 MediaProjection 必须在前台服务上下文）。
 * 从 MirrorState 取 MediaProjection 与 NetClient，启动 ScreenSender 编码发送。
 */
class MirrorForegroundService : Service() {

    private val CHANNEL = "gloai_mirror"
    private val TAG = "MirrorFgSvc"
    private var sender: ScreenSender? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        startForeground(1, buildNotification("GLOAI 车机投屏 · 镜像中"))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val mp = MirrorState.mediaProjection
        val net = MirrorState.net
        if (mp != null && net != null) {
            sender = ScreenSender(this, mp, net)
            sender?.start()
            Log.i(TAG, "ScreenSender started")
        } else {
            Log.w(TAG, "missing mediaProjection or net")
        }
        return START_STICKY
    }

    override fun onDestroy() {
        sender?.stop()
        sender = null
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
}
