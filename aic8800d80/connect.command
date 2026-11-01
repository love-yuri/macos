#!/bin/zsh
set -eu
aic_dir=${0:A:h}
aic_ssid=''
aic_location=''
aic_usage() {
    print -r -- "用法：$0 --ssid '你的 Wi-Fi 名称' --location USB位置"
    print 'USB 位置可先编译后运行 build/aicusb list 查看。'
}
while (( $# )); do
    case "$1" in
        --ssid|--location)
            if (( $# < 2 )); then aic_usage >&2; exit 2; fi
            if [[ "$1" == --ssid ]]; then aic_ssid="$2"; else aic_location="$2"; fi
            shift 2 ;;
        --help|-h) aic_usage; exit 0 ;;
        *) aic_usage >&2; exit 2 ;;
    esac
done
if [[ -f /Library/LaunchDaemons/com.aic8800.wifi.plist ]] &&
    /bin/launchctl print system/com.aic8800.wifi >/dev/null 2>&1; then
    print '开机联网服务已在后台运行，无需重复启动。日志：/Library/Logs/AIC8800/service.log'
    exit 0
fi
if [[ ! -f /Library/LaunchDaemons/com.aic8800.wifi.plist ]]; then
    if [[ -t 0 ]]; then
        if [[ -z "$aic_ssid" ]]; then read -r 'aic_ssid?Wi-Fi 名称（SSID）：'; fi
        if [[ -z "$aic_location" ]]; then read -r 'aic_location?USB 位置（见 build/aicusb list）：'; fi
    fi
    if [[ -z "$aic_ssid" || ! "$aic_location" =~ '^0[xX][0-9a-fA-F]{1,8}$' ]]; then
        aic_usage >&2
        exit 2
    fi
    if (( aic_location == 0 )); then aic_usage >&2; exit 2; fi
fi
if (( EUID != 0 )); then
    /usr/bin/make -C "$aic_dir" all
    exec /usr/bin/sudo /bin/zsh "$0" --ssid "$aic_ssid" --location "$aic_location"
fi
if [[ -f /Library/LaunchDaemons/com.aic8800.wifi.plist ]]; then
    /bin/launchctl enable system/com.aic8800.wifi
    /bin/launchctl bootstrap system /Library/LaunchDaemons/com.aic8800.wifi.plist
    print '已启动后台联网服务。日志：/Library/Logs/AIC8800/service.log'
    exit 0
fi
/bin/zsh "$aic_dir/disconnect.command"
source "$aic_dir/prepare.zsh"
aic_prepare_adapter "$aic_dir/build/aicusb" "$aic_location" "$aic_dir/firmware"
print -r -- "连接 $aic_ssid，将校验系统 DNS 和 HTTPS。首次连接时请先关闭 Clash TUN，连接完成后可启用。"
print '驱动需持续运行；前台启动时请保留窗口，按 Ctrl-C 断开。后台运行方式见 README，可用 disconnect.command 停止。'
exec "$aic_dir/build/aicusb" connect --ssid "$aic_ssid" --location "$aic_location" --default-route yes
