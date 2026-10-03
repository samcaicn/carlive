package com.gloai.mirror

import android.content.Context
import android.media.projection.MediaProjection
import java.util.UUID

/** 同进程内共享的镜像状态（Activity 拿到 MediaProjection 后存入，前台服务取出使用） */
object MirrorState {
    var mediaProjection: MediaProjection? = null
    var net: NetClient? = null

    /** 授权引导页是否正处于“等待用户在系统弹窗点开始”的状态（供无障碍自动点击等扩展判断用） */
    var consentPending = false

    /**
     * 手机侧稳定标识：首次运行生成并持久化，之后每次握手都带上，供车机“记住这台手机”。
     * 用 SharedPreferences 存储，重装 App 会变化（可接受；重新配对即可）。
     */
    fun loadDeviceId(ctx: Context): String {
        val prefs = ctx.getSharedPreferences("gloai", Context.MODE_PRIVATE)
        var id = prefs.getString("device_id", null)
        if (id.isNullOrEmpty()) {
            id = UUID.randomUUID().toString()
            prefs.edit().putString("device_id", id).apply()
        }
        return id
    }
}
