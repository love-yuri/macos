#!/bin/zsh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
source "${0:A:h}/prepare.zsh"
aic_test_dir=$(mktemp -d "${TMPDIR:-/tmp}/aic-prepare.XXXXXX")
trap '/bin/rm -rf -- "$aic_test_dir"' EXIT

fake_aicusb() {
    local aic_action="$1"
    shift
    [[ "$1" == --location && "$2" == 0x00b00000 ]] || return 90
    print -r -- "$aic_action" >> "$aic_test_dir/calls"
    case "$aic_action" in
        list)
            [[ ! -f "$aic_test_dir/list-fail" ]] || return 1
            local aic_state=$(<"$aic_test_dir/state")
            [[ "$aic_state" != missing ]] || return 1
            print -r -- "$aic_state location=0x00b00000 AIC"
            if [[ -f "$aic_test_dir/duplicate" ]]; then
                print 'a69c:8d81 location=0x00b00000 AIC'
            fi ;;
        switch)
            [[ ! -f "$aic_test_dir/switch-fail" ]] || return 1
            print 'a69c:8d80' > "$aic_test_dir/state" ;;
        load-fw)
            [[ "$3" == --firmware-dir && "$4" == '/firmware with spaces' ]] || return 91
            [[ ! -f "$aic_test_dir/load-fail" ]] || return 1
            [[ -f "$aic_test_dir/not-ready" ]] || print 'a69c:8d81' > "$aic_test_dir/state" ;;
        *) return 92 ;;
    esac
}

expect_calls() {
    local aic_actual=$(<"$aic_test_dir/calls")
    if [[ "$aic_actual" != "$1" ]]; then
        print -u2 "Unexpected initialization commands: $aic_actual"
        exit 1
    fi
}
reset_case() {
    : > "$aic_test_dir/calls"
    print -r -- "$1" > "$aic_test_dir/state"
}
prepare() { aic_prepare_adapter fake_aicusb 0x00b00000 '/firmware with spaces'; }

reset_case a69c:8d81
prepare
expect_calls list

# Reproduces the reported restart: firmware was lost but the wrapper must
# restore it before connect, without asking for a separate manual load-fw.
reset_case a69c:8d80
prepare
expect_calls $'list\nload-fw\nlist'

reset_case 1111:1111
prepare
expect_calls $'list\nswitch\nlist\nload-fw\nlist'

reset_case a69c:8d80
: > "$aic_test_dir/load-fail"
if prepare; then exit 1; fi
expect_calls $'list\nload-fw'
/bin/rm "$aic_test_dir/load-fail"

reset_case a69c:8d80
: > "$aic_test_dir/not-ready"
if prepare; then exit 1; fi
expect_calls $'list\nload-fw\nlist'
/bin/rm "$aic_test_dir/not-ready"

reset_case 1111:1111
: > "$aic_test_dir/switch-fail"
if prepare; then exit 1; fi
expect_calls $'list\nswitch'
/bin/rm "$aic_test_dir/switch-fail"

for aic_state in missing unexpected; do
    reset_case "$aic_state"
    if prepare; then exit 1; fi
    expect_calls list
done
reset_case a69c:8d81
: > "$aic_test_dir/duplicate"
if prepare; then exit 1; fi
expect_calls list
print 'Startup tests passed: runtime reuse, Boot ROM recovery, mode switch, selected USB port, firmware path quoting, failure stops, duplicate refusal (mock USB only).'
