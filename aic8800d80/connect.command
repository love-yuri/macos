#!/bin/zsh
set -eu
aic_dir=${0:A:h}
if [[ -f /Library/LaunchDaemons/com.aic8800.wifi.plist ]] &&
    /bin/launchctl print system/com.aic8800.wifi >/dev/null 2>&1; then
    print '开机联网服务已在后台运行，无需重复启动。日志：/Library/Logs/AIC8800/service.log'
    exit 0
fi
if (( EUID != 0 )); then
    DEVELOPER_DIR=/Library/Developer/CommandLineTools /usr/bin/make -C "$aic_dir" all
    exec /usr/bin/sudo /bin/zsh "$0"
fi
if [[ -f /Library/LaunchDaemons/com.aic8800.wifi.plist ]]; then
    /bin/launchctl enable system/com.aic8800.wifi
    /bin/launchctl bootstrap system /Library/LaunchDaemons/com.aic8800.wifi.plist
    print '已启动后台联网服务。日志：/Library/Logs/AIC8800/service.log'
    exit 0
fi
/bin/zsh "$aic_dir/disconnect.command"
source "$aic_dir/prepare.zsh"
aic_prepare_adapter "$aic_dir/build/aicusb" 0x00b00000 "$aic_dir/firmware"
print '连接 2-1501-5G，将校验系统 DNS 和 HTTPS。首次连接时请先关闭 Clash TUN，连接完成后可启用。'
print '驱动需持续运行；前台启动时请保留窗口，按 Ctrl-C 断开。后台运行方式见 README，可用 disconnect.command 停止。'
exec "$aic_dir/build/aicusb" connect --ssid 2-1501-5G --location 0x00b00000 --default-route yes
