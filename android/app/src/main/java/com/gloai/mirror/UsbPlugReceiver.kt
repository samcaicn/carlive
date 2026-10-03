package com.gloai.mirror

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.hardware.usb.UsbManager
import android.os.Build

/**
 * 插线/拔线自启（manifest 注册，无需 App 已在运行）：
 *  - 车机 USB 线插入 → 发 ACTION_USB_START 拉起 MirrorForegroundService（自动开网络共享 + 等服务）；
 *  - 车机 USB 线拔出 → 发 ACTION_STOP（日用机拔线即退，避免长期常驻耗电）。
 * 覆盖 android.intent.action.USB_STATE（connected extra）与 USB 设备插拔动作，最大化各 ROM 兼容性。
 */
class UsbPlugReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context?, intent: Intent?) {
        val c = context ?: return
        // 注意：外层局部变量不能叫 action，否则会遮蔽 Intent.apply{} 里的 action 属性赋值
        // （Kotlin 局部变量优先于隐式接收者成员），导致编译期 "Val cannot be reassigned"。
        val usbAction = intent?.action ?: return
        val connected = when (usbAction) {
            UsbManager.ACTION_USB_DEVICE_ATTACHED -> true
            "android.intent.action.USB_STATE" -> intent.getBooleanExtra("connected", false)
            else -> false
        }
        val disconnected = when (usbAction) {
            UsbManager.ACTION_USB_DEVICE_DETACHED -> true
            "android.intent.action.USB_STATE" -> !intent.getBooleanExtra("connected", true)
            else -> false
        }
        when {
            connected -> {
                val i = Intent(c, MirrorForegroundService::class.java).apply {
                    action = MirrorForegroundService.ACTION_USB_START
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) c.startForegroundService(i)
                else c.startService(i)
            }
            disconnected -> {
                val i = Intent(c, MirrorForegroundService::class.java).apply {
                    action = MirrorForegroundService.ACTION_STOP
                }
                // 停止前台服务允许用普通 startService
                c.startService(i)
            }
        }
    }
}
