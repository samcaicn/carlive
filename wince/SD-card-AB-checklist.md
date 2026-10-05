# 车机 A/B 对比排障（R28）

**直接进 `wince/` 目录点 exe 运行**（没有 lnk，也不再需要）。
三个 exe **各自写各自的日志**，互不覆盖，可以逐个跑完一起拔卡分析。

## SD 卡现在的内容

```
carlive/
├── AB对比操作清单.md        ← 本文件
└── wince/
    ├── config.txt           ← 唯一配置，三个 exe 共用
    ├── tuptup.exe           ← 旧版基线      sha 01161f41…
    ├── tuptup-full.exe      ← 新版完整      sha f773cc55…
    └── tuptup-usbnet.exe    ← 新版单 usb_net sha 0e93b289…
```

跑起来后 `wince/` 下会多出对应的日志文件（**不是**从 Mac 拷过去的，是车机自己写的）：

| 运行的 exe | 产生的日志 |
|---|---|
| `tuptup.exe` | `tuptup.log`（旧版日志名写死，且没有 crashlog 模块） |
| `tuptup-full.exe` | `tuptup-full.log` + `tuptup-full.crash.log` |
| `tuptup-usbnet.exe` | `tuptup-usbnet.log` + `tuptup-usbnet.crash.log` |

> 标题栏会显示 `[full]` 或 `[usbnet]`，一眼能看出当前跑的是哪个。

## 建议顺序

1. **`tuptup-full.exe`** —— 新版完整。先跑它，正常就不用折腾了。
2. **`tuptup-usbnet.exe`** —— 编译时就把 ADB 整个删掉了。若它正常而 full 闪退，元凶在 ADB 模块。
3. **`tuptup.exe`** —— 旧版基线，用于确认"这台车机到底能不能跑"。

每次：等约 10 秒看结果 → **干净退出**（点窗口右上角 X/OK，别直接断电或拔卡）
→ 等 5 秒让日志落盘 → 再点下一个。

R28 之前三个 exe 共用一个写死的互斥名，上一个没退干净时下一个会**静默不启动**，
现象跟闪退一模一样；现在互斥名按 exe 路径派生，互不干扰，
万一撞上也会弹窗告诉你"已在运行"，不会再当闪退误判。

## 怎么读结果

最直接的办法：卡插回 Mac，在仓库根目录跑

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
- **stage 停在 `LocalIPv4*` / `AdapterBufLen*`**
  → 网卡枚举是元凶，见下面的开关。
- **两个新版都闪退而旧版正常** → 对比 full 与 usbnet 是否一样，缩小到具体模块。

## 关键开关：不用重构建就能再验一次

编辑 `wince/config.txt`，去掉 `#noLocalIP=1` 前面的 `#`，再跑 `tuptup-full.exe`：

- **不再闪退** → 崩溃确认在 `GetAdaptersInfo` / `IP_ADAPTER_INFO` 链表遍历。
- **仍然闪退** → 排除网卡枚举，范围立刻缩小一大圈。

改文件即可反复验证，不必跑 CI。旧版 `tuptup.exe` 不认识这行，会忽略。

## 拔卡前

把 `wince/` 下所有 `.log` / `.crash.log` 直接拷回来即可（连同日渐增长的 `.log.bak` 一起）。
如果哪个版本跑成功过，日志会一直追加，超过 256KB 时旧内容自动滚到 `.bak`，只留一份。
