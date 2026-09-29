# GLOAI 投屏镜像 · 通信协议（v1）

手机端（Android）与车机端（WinCE）通过 **TCP** 在同一条连接上**双向复用**：视频下行、触摸上行、控制、心跳。

- 默认端口：`8686`
- 字节序：**大端（Big-Endian / Network Order）**
- 编码：视频帧为 H.264 Annex-B（SPS/PPS + NALU）或 MJPEG（降级模式）

---

## 1. 通用消息信封

所有消息都包在这个信封里：

```
 0               1 2 3 4       5       6 7 8 9      10 .. (10+LEN-1)
┌───────────────┬───────┬───────────────┬──────────────────────────────┐
│  MAGIC (4B)   │ TYPE  │   LEN (4B)    │         PAYLOAD (LEN B)       │
│  'G''L''O''A' │ 1B    │  uint32 BE    │  取决于 TYPE                  │
└───────────────┴───────┴───────────────┴──────────────────────────────┘
```

- `MAGIC`：`0x47 0x4C 0x4F 0x41`（`GLOA` 四字节），用于从 TCP 流中重新同步。
- `TYPE`：消息类型，见下表。
- `LEN`：PAYLOAD 字节数（不含信封头，不含 MAGIC/TYPE/LEN 自身）。最大 16MB（`0x00FFFFFF`）。

### 消息类型

| TYPE | 名称 | 方向 | 说明 |
|---|---|---|---|
| `0x01` | HANDSHAKE | 双向 | 握手，交换能力 |
| `0x02` | VIDEO_CONFIG | 手机→车机 | 视频参数（编解码、分辨率、帧率） |
| `0x03` | VIDEO_FRAME | 手机→车机 | 一帧编码数据（H264 Annex-B 或 MJPEG） |
| `0x04` | TOUCH_EVENT | 车机→手机 | 触摸事件（按下/移动/抬起 + 归一化坐标） |
| `0x05` | CONTROL | 双向 | 控制指令（如请求关键帧、暂停、退出） |
| `0x06` | HEARTBEAT | 双向 | 心跳（保活） |

---

## 2. HANDSHAKE (`0x01`)

PAYLOAD（JSON，UTF-8，便于扩展；体积很小，仅握手一次）：

```json
{
  "role": "phone" | "headunit",
  "proto_ver": 1,
  "caps": {
    "video_decoders": ["h264", "mjpeg"],   // 车机填写自身能解的格式
    "max_w": 800, "max_h": 480,            // 车机屏分辨率
    "touch": true
  }
}
```

- 车机先发起 TCP 连接，双方各发一个 HANDSHAKE；
- 手机根据车机 `video_decoders` 选择编码格式（优先 h264，不支持则 mjpeg）；
- 握手失败（proto_ver 不匹配 / 无可解格式）则关闭连接。

---

## 3. VIDEO_CONFIG (`0x02`)

视频参数变更时由手机发送（握手后、首帧前，以及分辨率切换时）：

```
 PAYLOAD:
  codec      : 1B    0x00 = H264, 0x01 = MJPEG
  width      : 2B    uint16 BE   (手机投屏分辨率宽)
  height     : 2B    uint16 BE
  fps        : 1B    uint8
  bitrate_k  : 4B    uint32 BE  (kbps)
```

车机据此初始化解码器与渲染位图。

---

## 4. VIDEO_FRAME (`0x03`)

一帧编码数据。为降低头部开销，PAYLOAD 直接是裸编码流：

```
 PAYLOAD:
  flags      : 1B    0x01 = 关键帧(I) / 0x00 = 非关键帧
  timestamp  : 4B    uint32 BE  (ms, 单调递增, 用于音视频同步预留)
  data_len   : 4B    uint32 BE  (= 剩余长度)
  data       : data_len B
```

- **H264 模式**：`data` 为 Annex-B 字节流（以 `00 00 00 01` 起始码分隔 NALU，首帧须含 SPS/PPS）。
- **MJPEG 模式**：`data` 为一个完整 JPEG 文件（含 SOI `FFD8` / EOI `FFD9`）；每帧独立、可随机解码。
- 发送方每 `KEY_FRAME_INTERVAL` 秒强制一个关键帧，便于车机断线重连后快速恢复。

---

## 5. TOUCH_EVENT (`0x04`)

车机触摸 → 手机回注。坐标为**归一化浮点**（0.0~1.0），手机端乘以自身分辨率还原，避免双方分辨率绑定。

```
 PAYLOAD:
  action     : 1B    0x00 = DOWN, 0x01 = MOVE, 0x02 = UP
  x          : 4B    float BE  (0.0 ~ 1.0, 相对车机屏宽)
  y          : 4B    float BE  (0.0 ~ 1.0, 相对车机屏高)
  pointer_id : 1B    (默认 0, 多指扩展用)
```

- DOWN：手指按下；MOVE：移动（可高频）；UP：抬起。
- 车机端按自身屏分辨率把触摸点归一化后填入 x/y。
- 一个滑动 = 一次 DOWN + 多次 MOVE + 一次 UP。

---

## 6. CONTROL (`0x05`)

PAYLOAD（1B 指令码 + 可选参数）：

| code | 名称 | 方向 | 含义 |
|---|---|---|---|
| `0x01` | REQUEST_IDR | 车机→手机 | 请求立即发关键帧（断线恢复/卡顿） |
| `0x02` | PAUSE | 双向 | 暂停镜像 |
| `0x03` | RESUME | 双向 | 恢复镜像 |
| `0x04` | BYE | 双向 | 优雅退出，收到方关闭连接 |

---

## 7. HEARTBEAT (`0x06`)

PAYLOAD 为空。每 **3s** 双向各发一次；**9s** 内未收到对端任何消息（含心跳）则判定断线、关闭并触发重连。

---

## 8. 流示例（H264 模式，成功连接后）

```
车机 ──HANDSHAKE──▶ 手机
手机 ──HANDSHAKE──▶ 车机        (协商 h264, 800x480)
手机 ──VIDEO_CONFIG─▶ 车机       (codec=h264, 1280x720, 30fps)
手机 ──VIDEO_FRAME─▶ 车机        (I 帧, SPS/PPS+slice)
手机 ──VIDEO_FRAME─▶ 车机        (P 帧 ...)
车机 ──TOUCH_EVENT─▶ 手机        (DOWN x=0.5 y=0.3)
车机 ──TOUCH_EVENT─▶ 手机        (MOVE x=0.52 y=0.31)
车机 ──TOUCH_EVENT─▶ 手机        (UP)
手机 ──VIDEO_FRAME─▶ 车机        (...)
双向 ──HEARTBEAT──▶ 双向
车机 ──CONTROL(REQUEST_IDR)─▶ 手机
手机 ──VIDEO_FRAME─▶ 车机        (新 I 帧)
```

---

## 9. 实现注意

- **TCP 粘包/半包**：接收方必须按信封头 `LEN` 循环读满 PAYLOAD 再处理；H264 模式建议每帧一个消息（不要合并）。
- **重同步**：若读到非 `GLOA` MAGIC，丢弃到下一个 `GLOA` 出现为止（防流错位）。
- **大端**：所有多字节整数字段一律 BE；float 用 IEEE754 BE。
- **JSON 仅用于 HANDSHAKE**；其余为二进制，省带宽。
