#pragma once

#include <stddef.h>   // size_t：本头文件刻意不依赖平台头，缺它在 macOS/clang 下
                      // 直接 error: unknown type name 'size_t'（RsaSha1 声明处）

// rsa.h - 极简 RSA-2048 签名：仅供车机端完成 adbd 的 AUTH 鉴权。
//
// 为什么需要它：现代 Android 的 adbd 在连接建立前会下发一个随机 20 字节 token，
// 要求客户端用 RSA 私钥对 SHA1(token) 做 PKCS#1 v1.5 签名来证明身份；
// 首次连接时手机上会弹「允许 USB 调试吗」，用户允许后该公钥被永久信任，
// 之后每次连接仍要走同样的签名流程。
//
// 为什么这么实现：WinCE 车机无法现场生成 RSA 密钥（找素数太慢），
// 故在开发机上预生成一对并把参数以常量编进 exe（见 scripts/gen_adbkey.py）。
// 只实现“签名”所需的运算，不做验签/加密：
//   · CRT 模式幂（p/q/dp/dq/qinv）——比直接 mod n 快约 4 倍
//   · Barrett 约减（mu 预计算）——避免实现多精度除法(Knuth D)
// 本文件刻意不依赖 windows.h，便于在宿主机上编译做交叉验证。

// 用私钥对 20 字节 token 签名，产出 256 字节签名写入 sig。成功返回 true。
bool RsaSignToken(const unsigned char token[20], unsigned char sig[256]);

// 返回 mincrypt 格式的公钥 blob（AUTH 时提交给 adbd，触发"允许调试"弹窗）
// outLen 接收其长度（2048 位时为 524 字节）。
const unsigned char* RsaPublicBlob(int* outLen);

// 签名长度（字节），恒为 256
int RsaSignatureSize();

// SHA1 摘要（供 adb.cpp 对 AUTH token 做摘要；与 RsaSignToken 内部共用实现）
void RsaSha1(const unsigned char* data, size_t len, unsigned char out[20]);
