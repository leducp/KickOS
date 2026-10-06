#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The witnesses that stop a never-ending image on a poll (KOS_POLL_UNTIL, tests/lib/gate.sh) stop
# it only once the judge has every line it requires. Each is replayed (KOS_POLL_REPLAY) over a
# planted passing console cut after every line and halfway through every line, on one core and
# on two: wherever the poll would stop, the judge passes, root's depth figure that follows the end
# included. And the restart witness's console cut halfway through health's end line, past the
# app's, is refused by the judge as a capture.

set -u
. "$(dirname "$0")/../lib/gate.sh"

scratch_dir
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
rc=0
bad() { echo "FAIL: $*" >&2; rc=1; }

SYSTEM="$TMP/system.yaml"
: > "$SYSTEM"

# A build directory holding only the board config the witnesses read.
build_of() { # <cores>
    _bo="$TMP/build$1"
    mkdir -p "$_bo/generated/include/kickos"
    printf '#define KICKOS_KERNEL_CORES %s\n#define KICKOS_ROOT_STACK_SIZE 8192\n#define KICKOS_MIN_STACK_SIZE 1024\n' \
        "$1" > "$_bo/generated/include/kickos/board_config.h"
    printf '%s' "$_bo"
}

restart_capture() { # <cores> <health-last: 0|1>
    printf '   KickOS v0.0.0  -  microkernel RTOS\n'
    printf 'sensor: a thread above its priority was refused\n'
    printf 'root: stack high water 2048 of 8192, 5000 free above the thread-local block\n'
    if [ "$1" -gt 1 ]; then
        printf 'sensor: a thread on another core was refused\n'
    fi
    _rc_k=1
    for _rc_end in 'sensor restarted (1 deaths, 1 restarts left)' \
                   'sensor restarted (2 deaths, 0 restarts left)' ''; do
        _rc_last=$((_rc_k + 4))
        while [ "$_rc_k" -le "$_rc_last" ]; do
            printf 'sensor: %s from /svc/sensor\n' "$_rc_k"
            _rc_k=$((_rc_k + 1))
        done
        printf 'health: sensor died, last reading %s in /shm/history\n' "$_rc_last"
        if [ -n "$_rc_end" ]; then
            printf 'health: %s\nsensor: restarting\n' "$_rc_end"
        fi
    done
    if [ "$2" -eq 1 ]; then
        printf 'sensor: gone for good\nhealth: sensor is down for good, running degraded\n'
    else
        printf 'health: sensor is down for good, running degraded\nsensor: gone for good\n'
    fi
    printf 'root: stack high water 2304 of 8192, 4744 free above the thread-local block\n'
}

driver_capture() {
    _dc_i=1
    while [ "$_dc_i" -le 2 ]; do
        for _dc_k in 1 2 3 4; do
            printf 'exits: served %s by /svc/exits\ntraps: served %s by /svc/traps\n' "$_dc_k" "$_dc_k"
        done
        printf "=== THREAD FAULT === thread 'worker' killed, system continues\n"
        printf 'exits: served 5 by /svc/exits\ntraps: served 5 by /svc/traps\n'
        if [ "$_dc_i" -eq 1 ]; then
            printf 'watch: exits deaths 1 left 0 alive\nwatch: traps deaths 1 left 0 alive\n'
            printf 'watch: fails deaths 1 left 0 alive\n'
        else
            printf 'watch: exits deaths 2 left 0 gone\nwatch: traps deaths 2 left 0 gone\n'
            printf 'watch: fails deaths 2 left 0 gone\nwatch: fails_client deaths 0 left 0 down\n'
        fi
        _dc_i=$((_dc_i + 1))
    done
    printf 'exits: refused\ntraps: refused\n'
}

# Exit status of <witness> replaying <log>, its output kept in $TMP/last.
replay() { # <log> <cores> <witness>
    _rp_b="$(build_of "$2")"
    case "$3" in
        restart)
            KOS_POLL_REPLAY="$1" "$SRC/tests/integration/check_composition_witness.sh" \
                "$_rp_b" "$SRC" cmake restart "$SYSTEM" depth > "$TMP/last" 2>&1 ;;
        driver)
            KOS_POLL_REPLAY="$1" "$SRC/tests/integration/check_driver_restart.sh" \
                "$_rp_b" "$SRC" cmake "$SYSTEM" > "$TMP/last" 2>&1 ;;
    esac
}

# Every cut of <full>: the poll stops nowhere the judge refuses, and does stop on the whole.
sweep() { # <full> <cores> <witness> <what>
    _sw_n="$(wc -l < "$1")"
    replay "$1" "$2" "$3"
    _sw_rc=$?
    [ "$_sw_rc" -eq 0 ] || { cat "$TMP/last" >&2; bad "$4: the whole console is not passed (exit $_sw_rc)"; }
    _sw_i=1
    while [ "$_sw_i" -le "$_sw_n" ]; do
        head -n "$_sw_i" "$1" > "$TMP/cut"
        replay "$TMP/cut" "$2" "$3"
        _sw_rc=$?
        if [ "$_sw_rc" -ne 0 ] && [ "$_sw_rc" -ne 3 ]; then
            bad "$4: the poll stops after line $_sw_i and the judge refuses it: $(tail -n 1 "$TMP/last")"
        fi
        head -n "$((_sw_i - 1))" "$1" > "$TMP/cut"
        _sw_line="$(sed -n "${_sw_i}p" "$1")"
        printf '%s' "$_sw_line" | cut -c "1-$(( (${#_sw_line} + 1) / 2 ))" | tr -d '\n' >> "$TMP/cut"
        replay "$TMP/cut" "$2" "$3"
        _sw_rc=$?
        if [ "$_sw_rc" -ne 0 ] && [ "$_sw_rc" -ne 3 ]; then
            bad "$4: the poll stops halfway through line $_sw_i and the judge refuses it: $(tail -n 1 "$TMP/last")"
        fi
        _sw_i=$((_sw_i + 1))
    done
}

for cores in 1 2; do
    for order in 0 1; do
        restart_capture "$cores" "$order" > "$TMP/restart"
        sweep "$TMP/restart" "$cores" restart "restart witness, $cores core(s), health last $order"
    done
    driver_capture > "$TMP/driver"
    sweep "$TMP/driver" "$cores" driver "driver restart witness, $cores core(s)"
done

restart_capture 1 1 > "$TMP/restart"
head -n "$(($(wc -l < "$TMP/restart") - 2))" "$TMP/restart" > "$TMP/cut"
printf 'health: sensor is down fo' >> "$TMP/cut"
replay "$TMP/cut" 1 restart
[ $? -eq 3 ] || bad "the restart witness's poll stops on a console cut halfway through health's end line"
if KOS_CAPTURE="$TMP/cut" "$SRC/tests/integration/check_composition_witness.sh" "$(build_of 1)" \
    "$SRC" cmake restart "$SYSTEM" > "$TMP/last" 2>&1; then
    bad "the restart witness passes a capture cut halfway through health's end line"
fi

if [ "$rc" -eq 0 ]; then
    echo "PASS: each witness's poll stops only where its judge has every line it requires"
fi
exit "$rc"
