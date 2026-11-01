# 本机硬件

基础信息整理自安装 U 盘原始 `hardware.md`（2026-10-25，Linux 检测）；内存、当前系统和使用状态于 2026-11-01 更新。

| 项目 | 配置 |
| --- | --- |
| CPU | AMD Ryzen 5 5600GT，Cezanne，6 核 12 线程 |
| 主板 | 微星 MSI A520M-A PRO（MS-7C96），版本 1.0，AMD A520 / AM4 |
| BIOS | AMI `1.I0`，2024-07-26（原始硬件记录） |
| 内存 | 32GB DDR4（16GB × 2），当前 macOS 报告 2400MHz |
| 显卡 | CPU 集成 Radeon Vega 核显，无独显；设备 ID `1002:1638`，预留显存 4GB |
| 显示 | HDMI 连接，2K 144Hz，机主实测正常 |
| SSD | 梵想 Fanxiang S500Pro 512GB NVMe，系统容量约 476.9GiB |
| NVMe 控制器 | YEESTOR，设备 ID `1d89:ef25` |
| 有线网卡 | Realtek RTL8111 / RTL8168 系列千兆网卡，`10ec:8168` |
| USB 无线网卡 | AIC 8800D80，USB ID `a69c:8d81`；驱动不包含在本 EFI 内 |
| 板载音频 | Realtek ALC897，Codec ID `10ec:0897`；AMD HDA 控制器 `1022:15e3` |
| HDMI / DP 音频控制器 | AMD Renoir / Cezanne，`1002:1637` |
| 当前系统 | macOS Tahoe 26.7.2（25G332） |
| SMBIOS | MacPro7,1；公开配置已清空机器标识 |

## 已确认的使用状态

以下由机主于 2026-11-01 实测确认：

- 核显驱动与图形加速正常。
- 显示器以 2K 144Hz 正常工作。
- 主板 3.5mm 模拟音频正常，当前使用 AppleALC + SimpleHDA，`alcid=11`。

睡眠、全部 USB 端口、其他显示接口、HDMI / DP 音频及其他功能未逐项记录，不作额外兼容性承诺。

## EFI 对应关系

- 初始引导：安装 U 盘 `/Volumes/TAHOE_USB/EFI`。
- 当前引导：系统 EFI 分区 `/Volumes/EFI/EFI`，在初始版本上增加音频修复。
- 本仓库：当前引导的公开整理版，保留本机启动配置，去除旧配置副本及机器标识。

硬件型号和声卡 Codec 以原始 Linux 检测记录为依据；核显在 macOS 中显示为 `AMD Radeon RX Renoir Graphics`，属于驱动呈现的名称。
