# AIC8800D80 macOS 无线网卡驱动

AIC8800D80 USB 无线网卡的实验性用户态驱动，已在 macOS Tahoe 26.7.2 / x86_64 上实现 Wi-Fi 连接和 IPv4 上网。

**目前能连接 Wi-Fi，但尚未适配 macOS 原生 Wi-Fi 接口和菜单。** 驱动通过 IOKit 控制 USB 网卡，以 `utun` 隧道接口接入系统网络，需要用这里的工具管理连接。

## 支持情况

| 功能 | 状态 |
| --- | --- |
| USB 安装盘模式切换、RAM 固件加载 | 已实现 |
| 2.4GHz / 5GHz 被动扫描 | 已实现 |
| WPA2-PSK / AES-CCMP 连接 | 已验证；连接目前限制在 5GHz 信道 36–48、非强制 PMF |
| DHCP、ARP、IPv4、系统 DNS、普通应用 HTTPS | 已验证 |
| 后台服务、失败后重新启动连接 | 已实现；冷启动顺序和休眠恢复仍需验证 |
| Clash Verge TUN 共存 | 已验证基础联网；可选辅助工具见下文 |
| macOS 原生 Wi-Fi 接口、菜单和网络选择 | 尚未适配 |
| WPA3、IPv6、HT/VHT/HE、完整休眠恢复 | 尚未实现 |

已验证设备：安装盘模式 `1111:1111`（产品名 `88M80`）、Boot ROM `a69c:8d80`、运行态 `a69c:8d81`，D80 `chip_id=7 / chip_mcu_id=0`。其他芯片或 MCU 版本会被拒绝。尚未做长期大流量稳定性测试，USB 收包边界处理仍有优化空间。

## 目录

| 路径 | 内容 |
| --- | --- |
| `*.c`、`*.h` | USB、固件加载、扫描、WPA2、网络和凭据处理源码 |
| `connect.command` / `disconnect.command` | 连接 / 断开工具 |
| `install-autostart.command` / `uninstall-autostart.command` | 安装 / 卸载后台自动联网服务 |
| `autostart.py`、`daemon.zsh`、`prepare.zsh` | 服务安装与网卡状态准备 |
| `clash-tun.py` | 可选的 Clash Verge TUN 辅助工具 |
| `firmware/` | 固件加载器校验并使用的 5 个 D80 MCU0 固件文件 |
| `vendor/aic8800/` | 编译所需的 Linux 驱动协议头文件 |
| `vendor/wpa_supplicant/` | WPA2 状态机和内部密码算法源码 |
| `linux/` | 完整 Linux 驱动项目：驱动源码、全部固件、构建安装脚本、测试与文档，供后续原生 Wi-Fi 移植参考 |
| `test_*` | 无需连接真实网卡的自动检查 |

## 编译

需要 Apple Command Line Tools（或 Xcode）和 Python 3。首次使用可运行 `xcode-select --install` 安装命令行工具。

从本仓库根目录执行：

```sh
cd aic8800d80
make all test
./build/aicusb list
```

后续示例均在 `aic8800d80/` 目录中执行。固件和协议头文件已包含在目录内，编译不依赖其他源码仓库，也不需要安装 kext 或修改 EFI。

## 手动连接

连接脚本保留原版配置：Wi-Fi 名称为 `2-1501-5G`，USB 位置为 `0x00b00000`。使用其他网络或网卡端口时，自行修改 `connect.command` 中对应的值；USB 位置可通过 `./build/aicusb list` 查看。

```sh
./connect.command
```

也可以直接双击脚本。脚本根据自身位置查找程序和固件，可以从其他工作目录启动。它会编译程序，在需要时请求管理员认证，停止旧连接，然后按网卡状态切换 USB 模式、恢复 RAM 固件或复用运行态固件，再连接网络。

密码从对应网络的钥匙串获取，缺少凭据时隐藏输入或弹出安全输入框。密码不放在命令参数中。前台连接需要保留窗口；按 **Ctrl-C** 正常退出，或在另一个终端执行：

```sh
./disconnect.command
```

驱动会临时发布 IPv4、路由和 DNS；退出时清理这些临时状态。如果后台自动联网服务已经安装，连接 / 断开脚本会控制该服务；更换网络请重新安装服务并替换凭据。

## 后台自动联网

直接双击 `install-autostart.command`，或执行：

```sh
./install-autostart.command
```

安装脚本同样保留原版默认配置：`2-1501-5G` / `0x00b00000`。其他使用者自行修改 `autostart.py` 中的默认值，也可使用原版已支持的 `--ssid`、`--location` 参数覆盖。

首次安装需要管理员认证，并获取一次 Wi-Fi 密码。程序、固件和凭据复制到 `/Library/Application Support/AIC8800/`，服务不依赖仓库路径。凭据保存在该目录的 `wifi.credential`，仅 root 可读；这是一份受文件权限保护的凭据文件。

所有副本使用同一个系统服务和安装目录。从旧目录运行卸载脚本也会移除已安装的服务及凭据，随后从此目录重新安装即可。

更换网络或密码时重新执行安装命令并加上 `--replace-credential`。服务在用户登录后建立连接，启动失败或进程退出时约每 15 秒重试；尚未登录时会等待可用的用户授权会话。

查看状态和日志：

```sh
/usr/bin/python3 ./autostart.py status
tail -n 50 /Library/Logs/AIC8800/service.log
```

暂停 / 恢复服务：

```sh
sudo /usr/bin/python3 ./autostart.py stop
sudo /usr/bin/python3 ./autostart.py start
```

卸载服务并删除保存的凭据（日志保留）：

```sh
./uninstall-autostart.command
```

如果日志出现 `Open installation-disk interface: 0xe00002be` 或可移动宗卷访问被拒绝，在“系统设置 → 隐私与安全性”中为 **AIC8800** 开启“文件与文件夹 → 可移动宗卷”权限。若系统未提供该开关，可在“完全磁盘访问权限”中添加已安装的 `/Library/Application Support/AIC8800/AIC8800.app`。后台程序需要自己的授权，终端的权限不能替代它。

## 诊断与 Clash

固件仅加载到 RAM，不写入闪存；拔插或断电后需要重新加载。连接脚本会自动处理，也可以手动执行：

```sh
sudo ./build/aicusb switch --location 0x00b00000
./build/aicusb check-fw --firmware-dir ./firmware
./build/aicusb load-fw --firmware-dir ./firmware --location 0x00b00000
./build/aicusb runtime-info --location 0x00b00000
./build/aicusb scan --location 0x00b00000
```

仅在安装盘模式时使用 `switch`，仅在 Boot ROM 状态时使用 `load-fw`。`check-fw` 只校验固件文件，不访问 USB；其余命令遇到 USB 权限不足时可使用 `sudo`。

首次连接建议关闭 Clash TUN，待 AIC 报告系统 DNS 和普通 HTTPS 验证成功后再启用。需要辅助启用 TUN 或清理旧驱动残留路由时，可运行：

```sh
sudo /usr/bin/python3 ./clash-tun.py
```

脚本自动查找 AIC 接口和当前用户的 Clash 控制 socket，可通过 `--socket` 指定自定义接口。它会调整 Clash 的运行时 TUN 状态并验证 HTTPS，不修改订阅文件。

## Linux 源码

`linux/` 包含完整 Linux 驱动项目，保留 `drivers/`、全部 `fw/` 固件、`usb_modeswitch/`、DKMS、安装脚本、测试、文档和许可证。macOS 工具仍使用自己的源码与打包依赖；Linux 源码用于后续协议对照及原生 Wi-Fi 接口适配，不代表这些内核模块可以在 macOS 中加载。Linux 项目说明见 [linux/README.md](linux/README.md)。

## 检查与许可

`make test` 覆盖协议编码、固件校验和模拟加载、扫描、WPA2 握手、收发重组、IPv4/DNS、路由、服务安装及凭据文件策略；不连接真实网卡，不修改系统网络配置。自动检查不能替代硬件兼容性和长期稳定性测试。

驱动源码沿用 GPL-2.0-only，见 [LICENSE](LICENSE)。协议头文件和固件来自 [shenmintao/aic8800d80](https://github.com/shenmintao/aic8800d80)，保留原文件与版权声明。`vendor/wpa_supplicant` 是上游 2.11 的未修改源码子集，遵循其 BSD 许可，见该目录的 `COPYING`、`README` 和 `SOURCES`。
