#!/bin/bash
# USB 共享网络场景的取证：车机插 USB 线、手机开 USB 网络共享后运行
#
# 前提：车机与手机通过 USB 线连接，手机已开「USB 网络共享」
# 车机会从手机拿到一个 USB 网段地址（常见 192.168.42.x），手机在该网段通常是 192.168.42.129
#
# 用法：./probe_usb.sh [设备serial] [持续秒数]
set -u
DEV="${1:-}"
SECS="${2:-60}"
SDLOG="/Volumes/SD/carlive/wince/tuptup.log"
OUT="/tmp/USBprobe_$(date +%H%M%S)"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

if [ -z "$DEV" ]; then
  DEV=$(adb devices | awk '$2=="device"{print $1; exit}')
fi
if [ -z "$DEV" ]; then
  echo "!! 没有可用设备。先插 USB 线并开启手机『USB 网络共享』，然后重跑。"
  exit 1
fi

echo "=== 设备：$DEV ==="
echo "=== 手机网络接口与IP ==="
adb -s "$DEV" shell ip -4 addr show 2>&1 | grep -oE 'inet [0-9.]+'
echo ""
echo "=== 手机监听端口（8686 = 0x21CE）==="
adb -s "$DEV" shell "ss -tln 2>/dev/null | grep 8686" 2>&1
echo ""

echo "=== 开始取证 ${SECS}秒 → ${OUT} ==="
adb -s "$DEV" logcat -c 2>/dev/null

# 后台抓手机日志
( adb -s "$DEV" logcat -v time \
    MirrorServer:D MirrorForegroundService:D NetClient:D \
    BeaconSender:D ScreenSender:D MjpegSender:D AndroidRuntime:E '*:S' \
    > "${OUT}_phone.log" 2>&1 ) &
LPID=$!

# 车机日志初始行数
BEFORE=0
[ -f "$SDLOG" ] && BEFORE=$(wc -l < "$SDLOG")

# 采样：有连接就记下来
( for i in $(seq 1 "$((SECS/2))"); do
    T=$(date +%H:%M:%S)
    L=$(adb -s "$DEV" shell "ss -tn 2>/dev/null | grep 8686" 2>/dev/null | tr -d '\r')
    if [ -n "$L" ]; then echo "[$T] 8686 已连接:"; echo "$L" | sed 's/^/    /'; fi
    sleep 2
  done > "${OUT}_conn.log" 2>&1 ) &
CPID=$!

sleep "$SECS"
kill $LPID $CPID 2>/dev/null
wait $LPID $CPID 2>/dev/null

echo ""
echo "========== 手机侧日志 =========="
[ -s "${OUT}_phone.log" ] && cat "${OUT}_phone.log" || echo "(空)"
echo ""
echo "========== 车机侧新增日志 =========="
if [ -f "$SDLOG" ]; then
  N=$(( $(wc -l < "$SDLOG") - BEFORE ))
  if [ "$N" -gt 0 ]; then tail -n "$N" "$SDLOG"; else echo "(无新增)"; fi
else
  echo "(找不到 $SDLOG —— 卡不在 Mac 上，或程序还没启动)"
fi
echo ""
echo "========== 8686 连接采样 =========="
[ -s "${OUT}_conn.log" ] && cat "${OUT}_conn.log" || echo "(全程无连接)"
echo ""
echo "产物： ${OUT}_phone.log / ${OUT}_conn.log"
