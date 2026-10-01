#!/bin/sh
# Windows Embedded Compact 7 (ARM) on QEMU vexpress-a15
# MODE=serial  -> 无图形，串口内核日志打到 stdout（用于自检 / CI）
# MODE=vnc     -> VNC + noVNC（默认），浏览器开 http://<host>:6080/vnc.html
set -e

NK="${NK_IMAGE:-/wince/images/NK.bin.load-0x800010c0.raw}"
DISK="${DISK_IMAGE:-/wince/images/disk.img}"
RAM="${RAM_MB:-256}"
MODE="${MODE:-vnc}"
# 注意：必须监听 0.0.0.0，监听 127.0.0.1 时 docker 端口映射转发不到（容器内 loopback 不可达）
VNC_OPTS="${VNC_OPTS:-0.0.0.0:0,share=force-shared,connections=8}"

if [ ! -f "$NK" ]; then
  echo "FATAL: NK image not found: $NK" >&2
  exit 1
fi
if [ ! -f "$DISK" ]; then
  echo "[*] creating blank virtio disk: $DISK (512MB)"
  qemu-img create -f raw "$DISK" 512M >/dev/null
fi

COMMON="-M vexpress-a15 -m $RAM \
 -global virtio-mmio.force-legacy=false \
 -device loader,addr=0x80000000,file=$NK \
 -device loader,addr=0x800010c0,cpu-num=0 \
 -device virtio-tablet-device \
 -drive file=$DISK,if=none,id=d0,format=raw \
 -device virtio-blk-device,drive=d0 \
 -monitor tcp:0.0.0.0:4444,server,nowait \
 -qmp tcp:0.0.0.0:4445,server,nowait $QEMU_EXTRA"

case "$MODE" in
  serial)
    echo "[*] MODE=serial  (headless, kernel log -> stdout)"
    exec qemu-system-arm $COMMON -display none -serial mon:stdio
    ;;
  vnc)
    echo "[*] MODE=vnc  noVNC on :6080  (VNC 0.0.0.0:5900, no password)"
    rm -f /tmp/serial.log
    # 注意：必须监听 0.0.0.0，监听 127.0.0.1 时 docker 端口映射转发不到（容器内 loopback 不可达）
    qemu-system-arm $COMMON -display none -vnc "$VNC_OPTS" -serial file:/tmp/serial.log &
    QPID=$!
    sleep 3
    websockify --web=/usr/share/novnc 6080 127.0.0.1:5900 &
    WPID=$!
    trap "kill -TERM $QPID $WPID 2>/dev/null; exit 0" TERM INT
    wait
    ;;
  *)
    echo "unknown MODE=$MODE (serial|vnc)" >&2
    exit 1
    ;;
esac
