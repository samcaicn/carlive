package top.tuptup.mirror

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.graphics.Path
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.accessibility.AccessibilityEvent

/**
 * 触摸回注：车机发来的归一化坐标 → 在手机上模拟点击/滑动。
 *
 * 走 AccessibilityService（canPerformGestures），无需 root、无需 ADB、无需 USB 调试，WiFi 直连即可。
 *
 * 连续拖拽实现：
 *  - API 26+ 用 StrokeDescription 的 willContinue 续接机制，把一连串 MOVE 接成一条连续轨迹（不再每帧从按下点重画，避免回弹/抖）。
 *  - 手指按住不动时不发 MOVE，故本端用 keep-alive 定时器周期性续接同点手势，防止系统误判抬起（支持长按/按住）。
 *  - API 24/25 无 willContinue 构造，降级为「逐段短手势」（每段从上一坐标到当前坐标），仍可用，连续拖拽略弱。
 *
 * 注：若手机已 root / 有系统签名，可改用 InputManager.injectInputEvent 获得更连续的轨迹（需另行适配）。
 */
class MirrorAccessibilityService : AccessibilityService() {

    private val handler = Handler(Looper.getMainLooper())
    private var isDown = false
    private var lastX = 0f
    private var lastY = 0f

    // 每条续接手势的存活时长；keep-alive 周期须明显小于它，确保重叠不断链。
    private val STROKE_MS = 800L
    private val KEEPALIVE_MS = 300L

    private val keepAliveRunnable = object : Runnable {
        override fun run() {
            if (isDown) {
                dispatchStroke(lastX, lastY, lastX, lastY, willContinue = true)
                handler.postDelayed(this, KEEPALIVE_MS)
            }
        }
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {}
    override fun onInterrupt() {}

    /** @param nx,ny 归一化 0.0~1.0；@param action Protocol.TOUCH_* */
    fun injectTouch(nx: Float, ny: Float, action: Byte) {
        val dm = resources.displayMetrics
        val x = (nx * dm.widthPixels).coerceIn(0f, dm.widthPixels.toFloat())
        val y = (ny * dm.heightPixels).coerceIn(0f, dm.heightPixels.toFloat())
        when (action) {
            Protocol.TOUCH_DOWN -> {
                isDown = true
                lastX = x; lastY = y
                dispatchStroke(x, y, x, y, willContinue = true)
                handler.removeCallbacks(keepAliveRunnable)
                handler.postDelayed(keepAliveRunnable, KEEPALIVE_MS)
            }
            Protocol.TOUCH_MOVE -> {
                if (!isDown) return
                dispatchStroke(lastX, lastY, x, y, willContinue = true)
                lastX = x; lastY = y
            }
            Protocol.TOUCH_UP -> {
                if (!isDown) return
                dispatchStroke(lastX, lastY, x, y, willContinue = false)
                isDown = false
                handler.removeCallbacks(keepAliveRunnable)
            }
        }
    }

    /** 发出一段手势轨迹（x0,y0）→（x1,y1）。willContinue 仅 API26+ 生效。 */
    private fun dispatchStroke(x0: Float, y0: Float, x1: Float, y1: Float, willContinue: Boolean) {
        if (Build.VERSION.SDK_INT < 24) return // dispatchGesture 在 API24 才引入
        val path = Path().apply { moveTo(x0, y0); lineTo(x1, y1) }
        val stroke = if (Build.VERSION.SDK_INT >= 26) {
            GestureDescription.StrokeDescription(path, 0, STROKE_MS, willContinue)
        } else {
            GestureDescription.StrokeDescription(path, 0, STROKE_MS)
        }
        dispatchGesture(GestureDescription.Builder().addStroke(stroke).build(), null, null)
    }

    override fun onServiceConnected() {
        instance = this
        super.onServiceConnected()
    }

    override fun onDestroy() {
        handler.removeCallbacks(keepAliveRunnable)
        instance = null
        super.onDestroy()
    }

    companion object {
        @Volatile var instance: MirrorAccessibilityService? = null
    }
}
