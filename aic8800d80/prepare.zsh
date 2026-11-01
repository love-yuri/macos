# SPDX-License-Identifier: GPL-2.0-only
# Sourced by connect.command after stopping this repository's old connection.
aic_adapter_identity() {
    local aic_listing
    aic_listing=$("$1" list --location "$2") || return 1
    if [[ -z "$aic_listing" || "$aic_listing" == *$'\n'* ]]; then
        print -u2 '所选 USB 端口未找到唯一的 AIC 网卡，已停止连接。'
        return 1
    fi
    case "$aic_listing" in
        1111:1111[[:space:]]*|a69c:8d80[[:space:]]*|a69c:8d81[[:space:]]*)
            print -r -- "${aic_listing%%[[:space:]]*}" ;;
        *) print -u2 '无法识别所选网卡的 USB 状态，已停止连接。'; return 1 ;;
    esac
}

aic_prepare_adapter() {
    local aic_executable="$1" aic_location="$2" aic_firmware_dir="$3" aic_identity
    aic_identity=$(aic_adapter_identity "$aic_executable" "$aic_location") || return 1
    if [[ "$aic_identity" == 1111:1111 ]]; then
        print '网卡处于安装盘模式，正在切换到 Wi-Fi 模式……'
        "$aic_executable" switch --location "$aic_location" || return 1
        aic_identity=$(aic_adapter_identity "$aic_executable" "$aic_location") || return 1
    fi
    if [[ "$aic_identity" == a69c:8d80 ]]; then
        print '网卡处于 Boot ROM，正在校验并重新加载 RAM 固件……'
        "$aic_executable" load-fw --location "$aic_location" --firmware-dir "$aic_firmware_dir" || return 1
        aic_identity=$(aic_adapter_identity "$aic_executable" "$aic_location") || return 1
    fi
    if [[ "$aic_identity" != a69c:8d81 ]]; then
        print -u2 "网卡尚未进入运行态（$aic_identity），已停止连接。"
        return 1
    fi
    print '网卡运行态 a69c:8d81 已就绪。'
}
