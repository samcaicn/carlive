# 车机运行与排障（R29）

**直接进 `wince/` 目录点 exe 运行**（没有 lnk，也不再需要）。
三个 exe **各自写各自的日志**，互不覆盖，可以逐个跑完一起拔卡分析。

## SD 卡现在的内容

```
carlive/
├── AB对比操作清单.md        ← 本文件
└── wince/
    ├── config.txt           ← 唯一配置，三个 exe 共用；【当前默认 usb_adb】
    ├── tuptup.exe           ← 旧版基线        sha 01161f41…
    ├── tuptup-full.exe      ← 新版完整(R29)    sha 9227896b…
    └── tuptup-usbnet.exe    ← 新版单 usb_net(R29) sha e358a202…
```

跑起来后 `wince/` 下会多出对应的日志文件（**不是**从 Mac 拷过去的，是车机自己写的）：

| 运行的 exe | 产生的日志 |
|---|---|
| `tuptup.exe` | `tuptup.log`（旧版日志名写死，且没有 crashlog 模块） |
| `tuptup-full.exe` | `tuptup-full.log` + `tuptup-full.crash.log` |
| `tuptup-usbnet.exe` | `tuptup-usbnet.log` + `tuptup-usbnet.crash.log` |

> 标题栏会显示 `[full]` 或 `[usbnet]`，一眼能看出当前跑的是哪个。

## R29 修了什么（为什么之前闪退）

真车闪退的根因不是代码逻辑，是**车机这个 CE ROM 上调 `GetAdaptersInfo` 会越界踩堆**：
`StartDiscovery` 在连接线程之前就 spawn 了扫描线程，后者枚举网卡时把缓冲写穿 →
连接线程随后 `readConfig()` 做堆分配时踩到坏块 → 进程消失，crash.log 正好停在
`ConnThread:readConfig`。旧基线 `tuptup.exe`（R13）能过 readConfig 只是当时没触发踩堆。

R29 两道修复：
1. **ADB 模式彻底绕开网卡枚举**——候选 IP 只来自 UDP 8687 信标，不 spawn 扫描线程、
   不调 `GetAdaptersInfo`，是最稳的路径（亿连即走此路）。
2. **usb_net 模式用「2x 缓冲 + 4 次重试」安全封装 `GetAdaptersInfo`**，杜绝越界，
   旧版"枚举崩溃"已消除。

> 顺带修了 crash.log 时间戳越界（原 10 字符的 `"0123456789"` 被 `&0xF` 越界读，日志里那个空格）。

## 建议顺序

1. **`tuptup-full.exe`**（config 默认 `mode=usb_adb`）—— 先跑它。ADB 是最稳路径：
   手机开「USB 调试」+「网络 ADB 调试 / 无线调试」（或手机端 `adb tcpip 5555`），
   车机经 USB 共享联网后，候选 IP 由信标自动拿到，连手机 `adbd:5555 → OPEN tcp:8686`。
   - 连不上时界面会提示「ADB 模式长时间未发现手机」，按提示检查手机 USB 调试 / adbd。
2. **`tuptup-usbnet.exe`** —— 编译时把 ADB 整个删掉，只剩直连（USB 网络共享同网段）。
   若你更习惯 USB 网络共享，把 config 改 `mode=usb_net` 再跑它。
3. **`tuptup.exe`** —— 旧版基线，用于确认"这台车机到底能不能跑"。

每次：等约 10 秒看结果 → **干净退出**（点窗口右上角 X/OK，别直接断电或拔卡）
→ 等 5 秒让日志落盘 → 再点下一个。

R28 起三个 exe 互斥名按 exe 路径派生，各自独立、互不拦截；万一撞上会弹窗告知"已在运行"，
不会再被误判成闪退。

## 怎么读结果

卡插回 Mac，在仓库根目录跑

```
python3 scripts/collect_car_logs.py
```

它会自动抽出构建时间/variant/exe 真实路径/可用内存/noLocalIP/候选数，
并给出每个 crash.log 最后停在哪一步。手工看的话，判据如下：

- **`wince/` 下既没有 .log 也没有 .crash.log**
  → 进程根本没进 WinMain，**不是崩溃，是加载失败**：
  ROM 缺 IPHLPAPI.DLL 或 WS2.DLL，或 SD 卡该目录不可写。
- **日志里有 `already running`**
  → 单实例冲突，上一次没退干净（不应再出现，出现就说明还有别的残留进程）。
- **`.crash.log` 停在某个 `[stage] xxx`**
  → **最后那一行就是崩溃点**：它代表"上一个已完成的步骤"，
    崩溃发生在这行与代码里下一条 `CrashSetStage` 之间的那段区间。
  （R29 后 `LocalIPv4*` / `AdapterBufLen*` 这类网卡枚举 stage 基本不会再出现；
   若仍出现，说明 `GetAdaptersInfo` 在这个 ROM 上有更深的异常，可再开 `noLocalIP=1`。）
- **两个新版都闪退而旧版正常** → 对比 full 与 usbnet 是否一样，缩小到具体模块。

## 关键开关：不用重构建就能再验一次（冗余安全网）

编辑 `wince/config.txt`，去掉 `#noLocalIP=1` 前面的 `#`，再跑 `tuptup-full.exe`：

- **不再闪退** → 崩溃确认在 `GetAdaptersInfo` / `IP_ADAPTER_INFO` 链表遍历。
- **仍然闪退** → 排除网卡枚举，范围立刻缩小一大圈。

改文件即可反复验证，不必跑 CI。旧版 `tuptup.exe` 不认识这行，会忽略。

## 拔卡前

把 `wince/` 下所有 `.log` / `.crash.log` 直接拷回来即可（连同日渐增长的 `.log.bak` 一起）。
如果哪个版本跑成功过，日志会一直追加，超过 256KB 时旧内容自动滚到 `.bak`，只留一份。
