#!/bin/bash
# 真车实测取证：同时抓手机侧 logcat 与车机侧 tuptup.log
# 用法：./probe.sh <持续秒数>   默认 60秒
set -u
SECS="${1:-60}"
DEV="192.168.10.5:5555"
SDLOG="/Volumes/SD/carlive/wince/tuptup.log"
OUT="/tmp/probe_$(date +%H%M%S)"

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

echo "=== 开始取证 ${SECS}秒 → ${OUT} ==="

# 后台持续抓手机日志
( adb -s "$DEV" logcat -v time \
    MirrorServer:D MirrorForegroundService:D NetClient:D \
    BeaconSender:D ScreenSender:D MjpegSender:D AndroidRuntime:E '*:S' \
    > "${OUT}_phone.log" 2>&1 ) &
LPID=$!

# 车机日志初始行数（用于只取新增部分）
BEFORE=0
[ -f "$SDLOG" ] && BEFORE=$(wc -l < "$SDLOG")

# 端口/连接采样
( for i in $(seq 1 "$SECS"); do
    T=$(date +%H:%M:%S)
    L=$(adb -s "$DEV" shell "ss -tn 2>/dev/null | grep 8686" 2>/dev/null | tr -d '\r')
    if [ -n "$L" ]; then
        echo "[$T] 8686 有连接:"
        echo "$L" | sed 's/^/    /'
    fi
    sleep 2
  done > "${OUT}_conn.log" 2>&1 ) &
CPID=$!

sleep "$SECS"
kill $LPID $CPID 2>/dev/null
wait $LPID $CPID 2>/dev/null

echo ""
echo "========== 手机侧日志 =========="
if [ -s "${OUT}_phone.log" ]; then
  cat "${OUT}_phone.log"
else
  echo "(空 —— 手机侧没有任何 TAG 匹配的输出)"
fi

echo ""
echo "========== 车机侧新增日志 =========="
if [ -f "$SDLOG" ]; then
  TAILN=$(( $(wc -l < "$SDLOG") - BEFORE ))
  if [ "$TAILN" -gt 0 ]; then
    tail -n "$TAILN" "$SDLOG"
  else
    echo "(无新增 —— 车机程序可能没启动，或 SD 卡没插好)"
  fi
else
  echo "(找不到 $SDLOG —— 车机还没写日志)"
fi

echo ""
echo "========== 8686 连接采样 =========="
if [ -s "${OUT}_conn.log" ]; then
  cat "${OUT}_conn.log"
else
  echo "(全程无连接)"
fi
echo ""
echo "产物： ${OUT}_phone.log / ${OUT}_conn.log"
