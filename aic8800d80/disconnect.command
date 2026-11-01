#!/bin/zsh
set -eu
aic_dir=${0:A:h}
if (( EUID != 0 )); then
    exec /usr/bin/sudo /bin/zsh "$0"
fi
# A KeepAlive daemon must be unloaded before stopping, or launchd reconnects it.
if /bin/launchctl print system/com.aic8800.wifi >/dev/null 2>&1; then
    /bin/launchctl bootout system/com.aic8800.wifi
    print '后台联网服务已停止；下次开机仍会自动启动。'
fi
# Match this repository's exact executable path. Do not signal other network
# tools, other repositories, or a PID read from a user-writable file.
/bin/ps -axo pid=,comm= | while read -r aic_pid aic_executable; do
    if [[ "$aic_executable" == "$aic_dir/build/aicusb" ]]; then
        /bin/kill -INT "$aic_pid"
        for aic_wait in {1..50}; do
            if ! /bin/kill -0 "$aic_pid" 2>/dev/null; then break; fi
            /bin/sleep 0.1
        done
        if /bin/kill -0 "$aic_pid" 2>/dev/null; then
            print -u2 'AIC 程序仍在清理，请等待后再连接。'
            exit 1
        fi
        print 'AIC 连接已停止，临时路由和 DNS 已清理。'
    fi
done
