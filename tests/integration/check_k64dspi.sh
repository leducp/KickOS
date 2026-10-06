#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of k64dspi (user/apps/frdmk64f/k64dspi): the client's verdicts through the SPI
# class, after the packaged k64dspi's up line where the build runs the service, and with no such
# line where it links the local engine (KICKOS_SPI_LOCAL_ENGINE). A build with K64DSPI_LOOPBACK
# owes every loopback case and the loopback verdict, otherwise the LAN9252 BYTE_TEST verdict. Both
# replies come from a bench fitting, a SOUT-to-SIN jumper (`dspi0-loopback`) or the EasyCAT shield
# (`lan9252`): where the rig declares none, a completed transfer reading back the wrong bytes
# (MISMATCH) is owed, and a refused open or a failed transfer (FAIL) still fails.
#
#   KOS_CAPTURE=<log> check_k64dspi.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: KOS_CAPTURE=<log> check_k64dspi.sh <board-build> <kickos-source> <cmake>"
BUILD="${1:?$USAGE}"
[ -f "$BUILD/CMakeCache.txt" ] || jfail no-cache "no $BUILD/CMakeCache.txt to read the build's mode from"
judge_capture k64dspi

LOOPBACK=0
_peer=lan9252
if grep -q '^K64DSPI_LOOPBACK:BOOL=ON' "$BUILD/CMakeCache.txt"; then
    LOOPBACK=1
    _peer=dspi0-loopback
fi
if has_e '^\[k64dspi\] ERROR'; then
    jfail error "k64dspi reported a failure"
fi
if has_e '^\[k64dspi\] .*FAIL'; then
    jfail error "k64dspi reported a refused open or a failed transfer"
fi
if has_e '^\[k64dspi\] BYTE_TEST attempt [0-9]+: .*\(xfer ERR'; then
    jfail xfer "a BYTE_TEST transfer failed"
fi
jno_panic "a panic in k64dspi"
_up='[k64dspi] SPI service up (DSPI0, polled FIFO, GPIO CS)'
if grep -q '^KICKOS_SPI_LOCAL_ENGINE:BOOL=ON' "$BUILD/CMakeCache.txt"; then
    if has_f "$_up"; then
        jfail posture "k64dspi's service came up in a build that links the local engine"
    fi
    AT=1
else
    jafter driver-up 1 "$_up" "k64dspi's up line"
fi
jafter device-open "$AT" '[k64dspi] device open rc=0 achieved=' "the client's successful device open" part

_cases="single-byte loopback|multi-byte (>FIFO) loopback|zero-tx loopback|two-segment transaction (one CS bracket)"
if [ "$LOOPBACK" -eq 1 ]; then
    jafter case "$AT" '[k64dspi] device open: PASS' "loopback case device open"
    _ifs="$IFS"
    IFS='|'
    for _case in $_cases; do
        IFS="$_ifs"
        if wired "$_peer"; then
            jafter case "$AT" "[k64dspi] $_case: PASS" "loopback case $_case"
        else
            _from="$AT"
            after_at "$_from" "[k64dspi] $_case: PASS"
            if [ -z "$AT" ]; then
                jafter case "$_from" "[k64dspi] $_case: MISMATCH" "loopback case $_case"
            fi
        fi
    done
    IFS="$_ifs"
    if wired "$_peer"; then
        jafter loopback "$AT" '[k64dspi] loopback PASS (the SPI bus echoes tx == rx)' "loopback verdict"
        echo "PASS: k64dspi's client looped every case back through the bus"
        exit 0
    fi
    _from="$AT"
    after_at "$_from" '[k64dspi] loopback PASS (the SPI bus echoes tx == rx)'
    if [ -z "$AT" ]; then
        jafter loopback "$_from" '[k64dspi] loopback MISMATCH (every transfer completed, rx != tx)' "loopback verdict"
    fi
    cnot_evaluated "the bus peer (bench wiring absent)"
    echo "PASS: k64dspi's client completed every loopback transfer through the bus"
    exit 0
fi
if wired "$_peer"; then
    jafter byte-test "$AT" '[k64dspi] LAN9252 BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)' "BYTE_TEST verdict"
    echo "PASS: k64dspi's client read the LAN9252 signature through the bus"
    exit 0
fi
jafter byte-test "$AT" '[k64dspi] BYTE_TEST attempt 1: ' "the first BYTE_TEST transfer" part
_from="$AT"
after_at "$_from" '[k64dspi] LAN9252 BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)'
if [ -z "$AT" ]; then
    jafter byte-test "$_from" '[k64dspi] LAN9252 BYTE_TEST MISMATCH: no valid signature' "BYTE_TEST verdict" part
fi
cnot_evaluated "the bus peer (bench wiring absent)"
echo "PASS: k64dspi's client completed every BYTE_TEST transfer through the bus"
