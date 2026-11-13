#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# codewindow_below and codewindow_past (user/apps/common/codewindow): the RV32 PMP code window
# is the TOR pair the script bounds. The window's first and last words read; the fetch of the
# word below it, the kernel's text, or of the word past it kills the thread at the address
# fetched, and nothing panicked. Boots the image, or reads a silicon capture with KOS_CAPTURE.
#
# With <plant>, the boot runs under <gdb> on the emulator's stub, which writes PMP entry 12 at
# reset as a boot stage before the kernel could leave it. `stale' is an unlocked entry granting
# U-mode everything: the seat must clear it, so every claim above holds unchanged. `locked' is an
# entry the seat cannot clear: the boot must refuse with the kernel's reason and run no app.
#
#   check_codewindow.sh <codewindow image> below|past [stale|locked <gdb>]
#   KOS_CAPTURE=<log> check_codewindow.sh <board-build> <kickos-source> <cmake> below|past

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_codewindow.sh <codewindow image> below|past [stale|locked <gdb>]"
plant=""
if judging_capture; then
    side="${4:?$_usage}"
    judge_capture "codewindow_$side"
else
    elf="${1:?$_usage}"
    side="${2:?$_usage}"
    plant="${3:-}"
fi
if [ -n "$plant" ]; then
    GDB="${4:?$_usage}"
    case "$plant" in
        stale) cfg=0x1f ;;
        locked) cfg=0x80 ;;
        *) fail "$_usage" ;;
    esac
    need_qemu
    need_qemu_machine
    if ! command -v "$GDB" >/dev/null 2>&1; then
        echo "SKIP: $GDB not found"
        exit 77
    fi
    scratch_dir
    boot_args "$elf"
    gdb_boot "$TMP/gdb" "$elf" '' 0 -ex 'set $pmpaddr12 = 0xffffffff' \
        -ex "set \$pmpcfg3 = $cfg" -ex 'p/x $pmpcfg3' -ex 'continue'
    if ! grep -q "^\$1 = $cfg\$" "$TMP/gdb"; then
        fail "the stub did not plant pmpcfg3 = $cfg: $(tail -n 5 "$TMP/gdb")"
    fi
    # Semihosting carries the console through the stub while gdb is attached.
    OUT="$(cat "$TMP/console" "$TMP/gdb" | tr -d '\r')"
    printf '%s\n' "$OUT"
    if [ "$plant" = locked ]; then
        if ! has "KERNEL PANIC: rv32imac: a boot stage left a PMP entry the kernel cannot clear"; then
            fail "the boot did not refuse the locked entry 12"
        fi
        if has '^\[codewindow\]'; then
            fail "an app ran under a PMP entry the kernel could not clear"
        fi
        echo "PASS: the boot refused a locked PMP entry 12"
        exit 0
    fi
elif ! judging_capture; then
    run_image "$elf"
fi

if has '^\[codewindow\] ERROR'; then
    cfail error "the fetch outside the window was permitted"
fi
if ! has '^\[codewindow\] first and last words read'; then
    cfail words "the window's own words were not readable"
fi
lo="$(printf '%s\n' "$OUT" | sed -n 's/^\[codewindow\] window 0x\([0-9a-fA-F]*\)\.\..*/\1/p' | head -n1)"
hi="$(printf '%s\n' "$OUT" | sed -n 's/^\[codewindow\] window 0x[0-9a-fA-F]*\.\.0x\([0-9a-fA-F]*\)$/\1/p' | head -n1)"
at="$(printf '%s\n' "$OUT" | sed -n 's/^\[codewindow\] fetching 0x\([0-9a-fA-F]*\)$/\1/p' | head -n1)"
if [ -z "$lo" ] || [ -z "$hi" ] || [ -z "$at" ]; then
    cfail announce "the app announced no window or no fetch"
fi
case "$side" in
    below) want="$((0x$lo - 4))" ;;
    past) want="$((0x$hi))" ;;
    *) fail "$_usage" ;;
esac
if [ "$((0x$at))" -ne "$want" ]; then
    cfail fetch "the app fetched 0x$at, not the word $side the window 0x$lo..0x$hi"
fi
require_killed_at main "0x$at"
echo "PASS: the fetch $side the window 0x$lo..0x$hi faulted at 0x$at"
exit 0
