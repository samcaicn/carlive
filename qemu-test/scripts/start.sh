#!/bin/sh
# 一键启动 ARM Windows Embedded Compact 7 模拟器（noVNC 网页访问）
#
#   ./start.sh              默认 VNC 模式 + 数据盘 data.img
#   DISK_IMAGE=/wince/images/prog.img ./start.sh
#   MODE=serial ./start.sh  只跑串口内核日志（无图形，CI 自检用）
set -e
cd "$(dirname "$0")"

IMG="${IMAGE:-wince-ce7-arm:v1}"
DISK="${DISK_IMAGE:-/wince/images/data.img}"
M="${MODE:-vnc}"

docker rm -f wincegui >/dev/null 2>&1 || true
docker build -t "$IMG" . >/dev/null
docker run -d --name wincegui --memory=1g \
  -e MODE="$M" \
  -e QEMU_EXTRA="-audiodev none,id=snd0" \
  -e DISK_IMAGE="$DISK" \
  -p 127.0.0.1:16080:6080 \
  -p 127.0.0.1:15900:5900 \
  -p 127.0.0.1:14444:4444 \
  -p 127.0.0.1:14445:4445 \
  -v "$PWD/images:/wince/images" \
  "$IMG" >/dev/null

echo "[*] 已启动 (MODE=$M)"
if [ "$M" = "vnc" ]; then
  echo "    浏览器打开:  http://127.0.0.1:16080/vnc.html"
  echo "    CE 启动约需 2-3 分钟（TCG 全软件模拟，CPU 密集）"
  echo "    VNC 直连:    127.0.0.1:15900"
  echo "    停止:        docker rm -f wincegui"
fi
