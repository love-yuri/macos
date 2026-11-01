# macOS Tahoe · MSI A520M-A PRO / Ryzen 5 5600GT

适用于 **Ryzen 5 5600GT + 微星 A520M-A PRO + Vega 核显** 的 OpenCore EFI，已在 **macOS Tahoe 26.7.2（25G332）** 上使用。完整硬件信息见 [hardware.md](hardware.md)。

## 已验证功能

以下功能于 2026-11-01 实测正常：

| 功能 | 状态 |
| --- | --- |
| Vega 核显驱动与图形加速 | 正常 |
| 2K 144Hz 显示输出 | 正常 |
| ALC897 板载 3.5mm 模拟音频 | 正常 |

## 目录

```text
.
├── EFI/
│   ├── BOOT/
│   └── OC/
├── aic8800d80/       # AIC8800D80 USB 无线网卡驱动与工具
├── hardware.md
├── README.md
└── .gitignore
```

[aic8800d80/](aic8800d80/) 提供无线网卡驱动，目前能连接 Wi-Fi 并上网，尚未适配 macOS 原生 Wi-Fi 接口及菜单；编译和使用方法见该目录的 README。

## 配置要点

- **引导与核显**：OpenCore 1.0.8、AMD Vanilla 内核补丁（6 核）、NootedRed 0.8.10。
- **音频修复**：加入 AppleALC 1.9.8 和 SimpleHDA 1.0.0，启用范围为 Darwin 25（Tahoe）。SimpleHDA 恢复传统 HDA 音频栈，启动参数 `alcid=11` 用于 ALC897。
- **加载顺序**：Lilu → NootedRed → AppleALC → SimpleHDA，避免 NootedRed 与音频插件的加载冲突。
- **基础配置**：包含 EC-USBX、CPUR、USB Reset ACPI，以及 VirtualSMC、RealtekRTL8111、RestrictEvents 等驱动。

## 使用

1. 先备份自己的 EFI，并保留可启动的安装 U 盘。
2. 在 `EFI/OC/config.plist` 的 `PlatformInfo → Generic` 中填写自己生成的 **SystemSerialNumber、MLB、SystemUUID、ROM**。这些字段需要自行填写，使用 [GenSMBIOS](https://github.com/corpnewt/GenSMBIOS) 为 **MacPro7,1** 生成独立标识，并按[官方说明](https://dortania.github.io/OpenCore-Install-Guide/AMD/zen.html#platforminfo)填写 ROM。
3. 用 **OpenCore 1.0.8** 随附的 `ocvalidate` 检查配置，将整个 `EFI/` 文件夹复制到目标 EFI 分区。先从 U 盘测试，再替换硬盘引导。

AMD 内核补丁的物理核心数为 **6**；更换 CPU 时需要调整。`NVMeFix` 当前关闭，`HoRNDIS` 保持启用（Android USB 网络共享，Tahoe 下兼容性需自行确认）。睡眠、全部 USB 端口和其他接口未逐项验证；AIC8800D80 无线驱动需要在 macOS 中单独运行，安装时建议使用有线网络。

## 镜像下载

使用 OpenCore 官方 [macrecovery](https://github.com/acidanthera/OpenCorePkg/tree/1.0.8/Utilities/macrecovery) 从苹果服务器下载恢复镜像。需要 Python 3；下载工具和镜像放在仓库外，下面命令可直接使用：

```sh
mkdir -p ../macos-recovery
cd ../macos-recovery
curl -fL https://raw.githubusercontent.com/acidanthera/OpenCorePkg/1.0.8/Utilities/macrecovery/macrecovery.py -o macrecovery.py
python3 macrecovery.py -b Mac-27AD2F918AE68F61 -m 00000000000000000 -os latest download
```

将生成的 `com.apple.recovery.boot/` 和本仓库的 `EFI/` 放在 FAT32 安装 U 盘根目录。下载的是**联网恢复镜像**，安装仍需网络；`latest` 随苹果服务器更新，请在安装前确认恢复环境显示的是 **macOS Tahoe**。仓库不包含苹果系统镜像。

## 主要组件与来源

| 组件 | 当前版本 / 来源 |
| --- | --- |
| OpenCore | [1.0.8](https://github.com/acidanthera/OpenCorePkg/releases/tag/1.0.8) |
| Lilu | [1.7.2](https://github.com/acidanthera/Lilu/releases/tag/1.7.2) |
| VirtualSMC | [1.3.8](https://github.com/acidanthera/VirtualSMC/releases/tag/1.3.8) |
| NootedRed | [0.8.10](https://github.com/ChefKissInc/NootedRed/releases/tag/v0.8.10) |
| AppleALC | [1.9.8](https://github.com/acidanthera/AppleALC/releases/tag/1.9.8) |
| SimpleHDA | [1.0.0](https://github.com/laobamac/SimpleHDA/releases/tag/v1.0.0) · [加载顺序参考](https://github.com/laobamac/SimpleHDA/issues/3) |
| RealtekRTL8111 | [3.0.0（CFBundleVersion 3.0.4）](https://github.com/Mieze/RTL8111_driver_for_OS_X/releases/tag/v3.0.0) |
| RestrictEvents | [1.1.6](https://github.com/acidanthera/RestrictEvents/releases/tag/1.1.6) |
| NVMeFix | [1.1.3](https://github.com/acidanthera/NVMeFix/releases/tag/1.1.3)，未启用 |
| HoRNDIS | [9.2](https://github.com/jwise/HoRNDIS) |
| AMD 内核补丁 | [AMD-OSX/AMD_Vanilla](https://github.com/AMD-OSX/AMD_Vanilla)，本机 6 核配置 |
| ACPI / HfsPlus | [Dortania ACPI](https://github.com/dortania/Getting-Started-With-ACPI/tree/master/extra-files/compiled) · [同主板 MCE / USB Reset 来源](https://github.com/Sanqing678/MSI-A520M-A-PRO-Hackintosh/releases/tag/v1.0.0) · [OcBinaryData](https://github.com/acidanthera/OcBinaryData) |

第三方组件遵循各自许可。SimpleHDA 随附的许可与声明保存在 `EFI/OC/Licenses/SimpleHDA/`；其内嵌 AppleHDA 组件仍受苹果相应条款约束。
