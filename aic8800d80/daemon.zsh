#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
if (( EUID != 0 )) || (( $# != 2 )); then
    print -u2 'This entry point is managed by the system LaunchDaemon.'
    exit 2
fi
aic_dir=${0:A:h}
# Removable-volume TCC requests are handled in a user's audit session. The
# system bootstrap otherwise forwards to tccd/0, which is absent on a normal
# desktop login. asuser adopts the logged-in audit/bootstrap session while
# retaining root credentials for USB capture and the saved Wi-Fi credential.
aic_console_uid=$(/usr/bin/stat -f %u /dev/console)
if (( aic_console_uid == 0 )); then
    print -u2 'AIC: waiting for a logged-in user session for USB privacy authorization.'
    exit 1
fi
if [[ $(/bin/launchctl manageruid) != "$aic_console_uid" ]]; then
    exec /bin/launchctl asuser "$aic_console_uid" /bin/zsh -f "$0" "$@"
fi
source "$aic_dir/prepare.zsh"
aic_executable="$aic_dir/AIC8800.app/Contents/MacOS/aicusb"
print -r -- "$(/bin/date '+%Y-%m-%d %H:%M:%S') AIC: preparing adapter $2 for $1"
# launchd retries startup failures (including a missing USB adapter). Do not
# run the standalone scan or a second copy of connect while this job is up.
aic_prepare_adapter "$aic_executable" "$2" "$aic_dir/firmware"
exec "$aic_executable" connect --ssid "$1" --location "$2" \
    --default-route yes --credential-file "$aic_dir/wifi.credential"
