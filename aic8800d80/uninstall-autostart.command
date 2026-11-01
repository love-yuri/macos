#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
exec /usr/bin/sudo /usr/bin/python3 "${0:A:h}/autostart.py" uninstall
