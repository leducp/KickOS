#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of k64dspi (user/apps/frdmk64f/k64dspi): the client's verdicts through the SPI
# class, after the packaged k64dspi's up line where the build runs the service, and with no such
# line where it links the local engine (KICKOS_SPI_LOCAL_ENGINE). A build with K64DSPI_LOOPBACK
# owes every loopback case and the loopback verdict, otherwise the LAN9252 BYTE_TEST verdict. Both
# replies come from a bench fitting, a SOUT-to-SIN jumper (`dspi0-loopback`) or the EasyCAT shield
# (`lan9252`): where the rig declares none, the cases and the verdict must still be on the wire and
# their values are owed.
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
if wired "$_peer" && has_e '^\[k64dspi\] .*FAIL'; then
    jfail error "k64dspi reported a failure with its $_peer fitting declared"
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
jafter device-open "$AT" '[k64dspi] device open rc=' "the client's device open" part

if [ "$LOOPBACK" -eq 1 ]; then
    jafter case "$AT" '[k64dspi] device open: PASS' "loopback case device open"
    if wired "$_peer"; then
        for _case in 'single-byte loopback' 'multi-byte (>FIFO) loopback' 'zero-tx loopback' \
                     'two-segment transaction (one CS bracket)'; do
            jafter case "$AT" "[k64dspi] $_case: PASS" "loopback case $_case"
        done
        jafter loopback "$AT" '[k64dspi] loopback PASS (the SPI bus echoes tx == rx)' "loopback verdict"
        echo "PASS: k64dspi's client looped every case back through the bus"
        exit 0
    fi
    for _case in 'single-byte loopback' 'multi-byte (>FIFO) loopback' 'zero-tx loopback' \
                 'two-segment transaction (one CS bracket)'; do
        jafter case "$AT" "[k64dspi] $_case: " "loopback case $_case" part
    done
    _from="$AT"
    after_at "$_from" '[k64dspi] loopback PASS (the SPI bus echoes tx == rx)'
    if [ -z "$AT" ]; then
        jafter loopback "$_from" '[k64dspi] loopback FAIL (see per-case lines above)' "loopback verdict"
    fi
    cnot_evaluated "the bus peer (bench wiring absent)"
    echo "PASS: k64dspi's client ran every loopback case through the bus"
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
    jafter byte-test "$_from" '[k64dspi] LAN9252 BYTE_TEST FAIL: no valid signature' "BYTE_TEST verdict" part
fi
cnot_evaluated "the bus peer (bench wiring absent)"
echo "PASS: k64dspi's client ran BYTE_TEST through the bus"
