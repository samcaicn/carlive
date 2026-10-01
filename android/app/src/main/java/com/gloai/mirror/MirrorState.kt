package com.gloai.mirror

import android.media.projection.MediaProjection

/** 同进程内共享的镜像状态（Activity 拿到 MediaProjection 后存入，前台服务取出使用） */
object MirrorState {
    var mediaProjection: MediaProjection? = null
    var net: NetClient? = null
}
