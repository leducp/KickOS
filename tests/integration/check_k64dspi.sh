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
# (MISMATCH) is owed, and a refused open or a failed transfer (FAIL) still fails. A verdict the
# app cannot print over the cases or attempts above it is refused in every posture.
#
#   KOS_CAPTURE=<log> check_k64dspi.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: KOS_CAPTURE=<log> check_k64dspi.sh <board-build> <kickos-source> <cmake>"
BUILD="${1:?$USAGE}"
SRC="${2:?$USAGE}"
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

_v_pass='[k64dspi] loopback PASS (the SPI bus echoes tx == rx)'
_v_mm='[k64dspi] loopback MISMATCH (every transfer completed, rx != tx)'
_bt_pass='[k64dspi] LAN9252 BYTE_TEST PASS: ESC SPI link OK (read 0x87654321)'
_bt_mm='[k64dspi] LAN9252 BYTE_TEST MISMATCH: no valid signature'

# Each case printed once, and the loopback verdict MISMATCH exactly where a case was.
loopback_consistent() {
    _lc_ifs="$IFS"
    IFS='|'
    _lc_mm=0
    for _lc_case in $_cases; do
        IFS="$_lc_ifs"
        _lc_n="$(printf '%s\n' "$OUT" | grep -cF -- "[k64dspi] $_lc_case: ")"
        if [ "$_lc_n" -gt 1 ]; then
            jfail case "loopback case $_lc_case printed $_lc_n times"
        fi
        if has_f "[k64dspi] $_lc_case: MISMATCH"; then
            _lc_mm=1
        fi
    done
    IFS="$_lc_ifs"
    if has_f "$_v_pass" && has_f "$_v_mm"; then
        jfail verdict "both a PASS and a MISMATCH loopback verdict"
    fi
    if has_f "$_v_pass" && [ "$_lc_mm" -eq 1 ]; then
        jfail verdict "a loopback PASS verdict over a case that read back the wrong bytes"
    fi
    if has_f "$_v_mm" && [ "$_lc_mm" -eq 0 ]; then
        jfail verdict "a loopback MISMATCH verdict with no case that read back the wrong bytes"
    fi
}

# Attempts numbered from 1 up to the app's retry bound, the probe stopping at the first that reads
# the signature: PASS is that attempt last, MISMATCH every attempt spent with none reading it.
byte_test_consistent() {
    _bc_max="$(sed -nE 's/^ *constexpr int PROBE_RETRIES = ([0-9]+);.*/\1/p' \
        "$SRC/user/apps/frdmk64f/k64dspi/main.cc")"
    require_number "$_bc_max" "k64dspi's PROBE_RETRIES"
    _bc_seen="$(printf '%s\n' "$OUT" | awk '
        /^\[k64dspi\] BYTE_TEST attempt [0-9]+: / {
            n++
            sub(/^\[k64dspi\] BYTE_TEST attempt /, "")
            if ($1 != n ":") { print "misnumbered " n; exit }
            if ($2 == "0x87654321") { if (sig) { print "past " sig; exit } sig = n }
        }
        END { print "attempts " n + 0 " " sig + 0 }')"
    case "$_bc_seen" in
        misnumbered*)
            jfail verdict "BYTE_TEST attempt ${_bc_seen#misnumbered } is not numbered in order"
            ;;
        past*)
            jfail verdict "a BYTE_TEST attempt after attempt ${_bc_seen#past }, which read the signature"
            ;;
        *)
            ;;
    esac
    set -- $_bc_seen
    _bc_n=$2
    _bc_sig=$3
    if [ "$_bc_n" -gt "$_bc_max" ]; then
        jfail verdict "$_bc_n BYTE_TEST attempts, past the app's $_bc_max"
    fi
    if has_f "$_bt_pass" && has_f "$_bt_mm"; then
        jfail verdict "both a PASS and a MISMATCH BYTE_TEST verdict"
    fi
    if has_f "$_bt_pass" && { [ "$_bc_sig" -eq 0 ] || [ "$_bc_sig" -ne "$_bc_n" ]; }; then
        jfail verdict "a BYTE_TEST PASS verdict whose last attempt did not read the signature"
    fi
    if has_f "$_bt_mm" && { [ "$_bc_sig" -ne 0 ] || [ "$_bc_n" -ne "$_bc_max" ]; }; then
        jfail verdict "a BYTE_TEST MISMATCH verdict after $_bc_n of $_bc_max attempts, $_bc_sig reading the signature"
    fi
}

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
        jafter loopback "$AT" "$_v_pass" "loopback verdict"
        loopback_consistent
        echo "PASS: k64dspi's client looped every case back through the bus"
        exit 0
    fi
    _from="$AT"
    after_at "$_from" "$_v_pass"
    if [ -z "$AT" ]; then
        jafter loopback "$_from" "$_v_mm" "loopback verdict"
    fi
    loopback_consistent
    cnot_evaluated "the bus peer (bench wiring absent)"
    echo "PASS: k64dspi's client completed every loopback transfer through the bus"
    exit 0
fi
if wired "$_peer"; then
    jafter byte-test "$AT" "$_bt_pass" "BYTE_TEST verdict"
    byte_test_consistent
    echo "PASS: k64dspi's client read the LAN9252 signature through the bus"
    exit 0
fi
jafter byte-test "$AT" '[k64dspi] BYTE_TEST attempt 1: ' "the first BYTE_TEST transfer" part
_from="$AT"
after_at "$_from" "$_bt_pass"
if [ -z "$AT" ]; then
    jafter byte-test "$_from" "$_bt_mm" "BYTE_TEST verdict" part
fi
byte_test_consistent
cnot_evaluated "the bus peer (bench wiring absent)"
echo "PASS: k64dspi's client completed every BYTE_TEST transfer through the bus"
