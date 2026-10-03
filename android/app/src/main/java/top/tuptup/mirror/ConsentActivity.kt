package top.tuptup.mirror

import android.app.Activity
import android.content.Intent
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle

/**
 * 透明授权页：把系统的「屏幕录制/投屏」授权弹窗转交给用户点一次。
 *  - 由 MirrorForegroundService 在服务缺录屏授权时拉起（或用户点通知进入）；
 *  - 用户点系统弹窗的「开始/立即开始」后，把 resultCode + data 回灌给前台服务（ACTION_START），
 *    服务即可获取 MediaProjection 并开始推流。
 * 日常机策略：此处不自动点击，仅呈现并等待用户一次手动确认（重启后重复一次即可）。
 */
class ConsentActivity : Activity() {

    private val REQ_MEDIA = 2001

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        MirrorState.consentPending = true
        try {
            val mgr = getSystemService(MediaProjectionManager::class.java)
            startActivityForResult(mgr.createScreenCaptureIntent(), REQ_MEDIA)
        } catch (e: Exception) {
            finish()
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        MirrorState.consentPending = false
        if (requestCode == REQ_MEDIA && resultCode == RESULT_OK && data != null) {
            // 把授权结果交给前台服务，由其在前台化之后获取 MediaProjection。
            val intent = Intent(this, MirrorForegroundService::class.java).apply {
                action = MirrorForegroundService.ACTION_START
                putExtra(MirrorForegroundService.EXTRA_MP_RESULT, resultCode)
                putExtra(MirrorForegroundService.EXTRA_MP_DATA, data)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(intent)
            } else {
                @Suppress("DEPRECATION")
                startService(intent)
            }
        }
        finish()
    }
}
