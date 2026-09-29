package com.gloai.mirror

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.graphics.Path
import android.view.accessibility.AccessibilityEvent

/**
 * 触摸回注：车机发来的归一化坐标 → 在手机上模拟点击/滑动。
 *
 * 走 AccessibilityService（canPerformGestures），无需 root、无需 ADB，WiFi 直连即可。
 * 注：若手机已 root / 有系统签名，可改用 InputManager.injectInputEvent 获得更连续的轨迹。
 */
class MirrorAccessibilityService : AccessibilityService() {

    private val path = Path()
    private var isDown = false

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}

    /** @param nx,ny 归一化 0.0~1.0；@param action Protocol.TOUCH_* */
    fun injectTouch(nx: Float, ny: Float, action: Byte) {
        val dm = resources.displayMetrics
        val x = (nx * dm.widthPixels).coerceIn(0f, dm.widthPixels.toFloat())
        val y = (ny * dm.heightPixels).coerceIn(0f, dm.heightPixels.toFloat())
        when (action) {
            Protocol.TOUCH_DOWN -> { path.reset(); path.moveTo(x, y); isDown = true; fire(50) }
            Protocol.TOUCH_MOVE -> { if (isDown) { path.lineTo(x, y); fire(50) } }
            Protocol.TOUCH_UP   -> { if (isDown) { path.lineTo(x, y); fire(50); isDown = false } }
        }
    }

    private fun fire(durationMs: Long) {
        val stroke = GestureDescription.StrokeDescription(path, 0, durationMs)
        dispatchGesture(GestureDescription.Builder().addStroke(stroke).build(), null, null)
    }

    override fun onServiceConnected() {
        instance = this
        super.onServiceConnected()
    }

    override fun onDestroy() {
        instance = null
        super.onDestroy()
    }

    companion object {
        @Volatile var instance: MirrorAccessibilityService? = null
    }
}
