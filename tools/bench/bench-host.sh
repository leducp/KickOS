#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# WHICH MACHINE THE BOARDS ARE ON, how to run a command there, and the one enumeration
# of its USB bus. Sourced by every script in tools/bench/ that has to ask the bus a
# question, so the local answer and the remote answer come from one implementation
# instead of two that can drift.
#
# Sourced, never executed.
#
# THE HOST IS AN ARGUMENT, NOT A GUESS. bench_host_select takes the host the caller
# resolved and nothing else: bench.sh and bench-fleet.sh select remote mode on BENCH_HOST
# alone, bench-present.sh falls back to RIG_BENCH_HOST.
#
# HOW TO REACH THE HOST IS ssh_config's QUESTION, NOT OURS. RIG_BENCH_HOST may be a bare
# ssh alias carrying its own HostName, Port and User, so a port is passed ONLY when one is
# configured here and there is no fallback to 22: a guessed 22 overrides the alias and the
# connection goes to a different machine, or nowhere. A user is never synthesised either.

BENCH_REMOTE=0
BENCH_SSH=()
BENCH_RSH=""
BENCH_WHERE=""
BENCH_PORT_FROM=""

# bench_host_select <host>  empty host means the boards are on this box.
bench_host_select() {
    local host="${1:-}" port
    port="${BENCH_PORT:-${RIG_BENCH_PORT:-}}"
    if [ -z "$host" ]; then
        BENCH_REMOTE=0
        BENCH_SSH=()
        BENCH_RSH=""
        BENCH_PORT_FROM=""
        BENCH_WHERE="this box ($(uname -n))"
        return 0
    fi
    BENCH_REMOTE=1
    BENCH_SSH=(ssh)
    BENCH_RSH="ssh"
    BENCH_WHERE="bench host $host"
    BENCH_PORT_FROM=""
    if [ -n "$port" ]; then
        BENCH_SSH+=(-p "$port")
        BENCH_RSH="$BENCH_RSH -p $port"
        if [ -n "${BENCH_PORT:-}" ]; then
            BENCH_PORT_FROM="BENCH_PORT"
        else
            BENCH_PORT_FROM="RIG_BENCH_PORT"
        fi
        BENCH_WHERE="$BENCH_WHERE:$port"
    fi
    BENCH_SSH+=(-o BatchMode=yes "$host")
    BENCH_RSH="$BENCH_RSH -o BatchMode=yes"
}

# bench_run  reads a script on stdin and runs it WHERE THE BOARDS ARE.
#
# Always a script on stdin and never a command line: the remote login shell is zsh, which
# does not word-split and ABORTS on an unmatched glob, so a `/dev/...*` or
# `/sys/bus/usb/devices/*/` written into an ssh command line is expanded by the wrong shell
# on the wrong box, or kills the command outright. Nothing on stdin is seen by that shell.
bench_run() {
    if [ "$BENCH_REMOTE" = "1" ]; then
        "${BENCH_SSH[@]}" bash -s
        return $?
    fi
    bash -s
}

# One enumeration of the bus. Every presence and serial question reads what this leaves in
# USB_BUS, so a board is never asked about twice and never asked about on the wrong box.
USB_ENUM_SCRIPT='
for d in /sys/bus/usb/devices/*/; do
  [ -r "$d/idVendor" ] && [ -r "$d/idProduct" ] || continue
  s=""
  [ -r "$d/serial" ] && s=$(cat "$d/serial")
  printf "%s:%s %s\n" "$(cat "$d/idVendor")" "$(cat "$d/idProduct")" "$s"
done
'
USB_BUS=""

# bench_bus_read  fills USB_BUS. Returns 1 when the host could not be asked and 2 when it
# answered nothing, which are different failures: the first is the link, the second a box
# with no bus at all.
bench_bus_read() {
    USB_BUS=$(printf '%s\n' "$USB_ENUM_SCRIPT" | bench_run) || return 1
    [ -n "$USB_BUS" ] || return 2
    return 0
}

usb_present() { # <vid:pid>
    printf '%s\n' "$USB_BUS" | grep -q "^$1 "
}

# The /dev/serial/by-id directory, read lazily: only usb_serial_of's ambiguity refusal
# needs it.
USB_BYID=""
USB_BYID_SCRIPT='shopt -s nullglob
for f in /dev/serial/by-id/*; do
  printf "%s\n" "$f"
done'
bench_byid_read() {
    USB_BYID=$(printf '%s\n' "$USB_BYID_SCRIPT" | bench_run)
}

# usb_serial_of_list <rows>  one line per row, in the shape the refusal below prints:
# the serial (or the fact that there is none) and any by-id path that names it. A by-id
# name conventionally carries the serial verbatim (SEGGER, STMicroelectronics, 1a86 all
# do), so grepping the live directory for it is what "where they have them" means here.
usb_serial_of_list() {
    local line sn byid
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        sn=$(printf '%s\n' "$line" | awk '{ sub(/^[^ ]+[ ]+/, ""); sub(/[ ]+$/, ""); print }')
        if [ -z "$sn" ]; then
            printf '  no serial descriptor\n'
            continue
        fi
        byid=$(printf '%s\n' "$USB_BYID" | grep -F -- "$sn" | paste -sd ', ' -)
        if [ -n "$byid" ]; then
            printf '  serial %s  %s\n' "$sn" "$byid"
        else
            printf '  serial %s  (no by-id path)\n' "$sn"
        fi
    done <<EOF
$1
EOF
}

# Echo the serial of the device matching vid:pid. Everything past the vid:pid is the
# serial: the E2 Lite spells its own as `E2L: OBE110014`, and a field-two read of that
# hands back `E2L:`.
#
# MORE THAN ONE ENUMERATED DEVICE AT ONE VID:PID IS REFUSED, NEVER RESOLVED BY TAKING THE
# FIRST: every unit of a kind presents the same vid:pid, and more than one physical XMC and
# more than one K64F are in rotation on this bench. Returns 1 when nothing matches and 2 when
# the match is refused (ambiguous, or a stale pin); either way nothing is echoed.
#
# A pin always wins, exactly as a console row's does: RIG_PROBE_<VID>_<PID> in the rig
# config (vid:pid upper-cased, ':' turned into '_', e.g. RIG_PROBE_1366_1024) names the one
# serial this vid:pid means here. It is checked against what is actually enumerated, the
# same way a derived console VCOM is checked against the bus, so a stale pin is refused
# rather than believed.
usb_serial_of() { # <vid> <pid>
    local key="$1:$2" pinkey pin rows n line sn found=0
    pinkey="RIG_PROBE_$(printf '%s' "$key" | tr 'a-z:' 'A-Z_')"
    pin="${!pinkey:-}"
    rows=$(printf '%s\n' "$USB_BUS" | awk -v k="$key" '$1 == k')
    n=$(printf '%s\n' "$USB_BUS" | awk -v k="$key" '$1 == k' | grep -c '^')
    [ "$n" -gt 0 ] || return 1

    if [ -n "$pin" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            sn=$(printf '%s\n' "$line" | awk '{ sub(/^[^ ]+[ ]+/, ""); sub(/[ ]+$/, ""); print }')
            [ "$sn" = "$pin" ] && found=1
        done <<EOF
$rows
EOF
        if [ "$found" = "1" ]; then
            echo "$pin"
            return 0
        fi
        bench_byid_read
        {
            printf 'REFUSING: %s pins serial %s, and %s carries no %s device with that serial:\n' \
                   "$pinkey" "$pin" "${BENCH_WHERE:-the bus}" "$key"
            usb_serial_of_list "$rows"
            printf '  Fix %s in %s, or clear it if the pin is stale.\n' \
                   "$pinkey" "${RIG_CONF:-the rig config}"
            printf '  tools/bench/bench-present.sh bus names every serial and by-id path this bus carries right now.\n'
        } >&2
        return 2
    fi

    if [ "$n" -gt 1 ]; then
        bench_byid_read
        {
            printf 'REFUSING: %s matches %d devices on %s, so which one this is is a guess:\n' \
                   "$key" "$n" "${BENCH_WHERE:-the bus}"
            usb_serial_of_list "$rows"
            printf '  Pin %s in %s to the one this is (see tools/bench/rig.conf.example).\n' \
                   "$pinkey" "${RIG_CONF:-the rig config}"
            printf '  tools/bench/bench-present.sh bus names every serial and by-id path this bus carries right now.\n'
        } >&2
        return 2
    fi

    sn=$(printf '%s\n' "$rows" | awk '{ sub(/^[^ ]+[ ]+/, ""); sub(/[ ]+$/, ""); print }')
    [ -n "$sn" ] || return 1
    echo "$sn"
}
