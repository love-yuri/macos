#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
aic_dir=${0:A:h}
DEVELOPER_DIR=/Library/Developer/CommandLineTools /usr/bin/make -C "$aic_dir" all
print '将安装开机联网服务。安装需要一次管理员认证；Wi-Fi 凭据仅保存到 root 可读的系统目录。'
exec /usr/bin/sudo /usr/bin/python3 "$aic_dir/autostart.py" install "$@"
