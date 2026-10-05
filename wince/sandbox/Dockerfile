# Windows CE / WEC7 on QEMU (ARM vexpress-a15) — noVNC sandbox
#
# 目的：在浏览器里跑一台 ARM Windows Embedded Compact 7 虚拟机，
#       用来在电脑上预验证 ARM WinCE 程序（布局/依赖/配置），避免反复拔插 SD 卡上车验证。
#
# 镜像来源（真实存在的唯一可用 ARM CE QEMU 端口）：
#   https://github.com/Milek7/wince-qemu  Release 2022-04-17
#
# 注意：这是 Windows Embedded Compact 7（CE 7），不是车机的 CE 6.0。
#       ARM EABI 二进制向下兼容性较好，但第三方程序的依赖 DLL 必须存在于 NK 镜像里。
FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      qemu-system-arm \
      qemu-utils \
      novnc \
      websockify \
      tigervnc-common \
      supervisor \
      mtools \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /wince

COPY run.sh /wince/run.sh
RUN chmod +x /wince/run.sh

EXPOSE 6080

CMD ["/wince/run.sh"]
