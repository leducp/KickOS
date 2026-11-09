#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# WHAT EACH BOARD PUTS ON THE USB BUS: the probe row that decides presence, and the console
# row that decides which device a capture reads. One table, read by bench-fleet.sh,
# bench-capture.sh and bench-present.sh, so those three can never disagree about what a
# board looks like.
#
# Sourced, never executed. Requires bench-host.sh, for bench_run.
#
# A PROBE ID NAMES A BOARD KIND AND NEVER A PARTICULAR UNIT. More than one XMC and more
# than one K64F are in rotation, so the row says which idProduct to look for and the serial
# is resolved live from the bus, never quoted from a note.
#
# A VENDOR ID IS NOT PROOF THE DEVICE IS OURS. This bench carries other people's FTDIs and
# CP210x cables, so a row keyed on 0403:6001 alone can be satisfied by a cable that is not
# ours. That is why every FTDI console is keyed on the CABLE'S OWN SERIAL through
# RIG_CONSOLE_<BOARD> and there is deliberately no vendor-pattern fallback.
#
# A ROW TWO BOARDS NAME DECIDES NEITHER. A standalone probe's presence says nothing about which
# board its lead is on, so bench-present.sh reports such a row as SHARED, naming the other
# boards, rather than as present.

# The by-id glob of a KickOS USB CDC console. Every board's image presents the same strings and
# serial, so a match says some board runs one and never which.
KICKOS_CDC_CONSOLE="/dev/serial/by-id/usb-KickOS_KickOS_console_*-if00"

# board_probe_rows <board>
#
# Prints one row per USB device the board presents, as
#
#     <vid:pid or by-id glob>|<sn, vid, seen or ->|<what it is>
#
# where `sn` marks the device whose serial the flasher has to be told, `vid` a row whose vendor
# id is all the evidence there is, so its presence says a device of that kind is on the bus and
# nothing about whose, and `seen` a device the board may present and need not, which never
# decides presence. Every other row must be present for the board to be. Returns 1 for a board
# this table does not decode.
board_probe_rows() {
    case "$1" in
        frdmk64f)
            printf '1366:1015|sn|the J-Link OB on the OpenSDA header\n'
            ;;
        xmc4800-relax)
            printf '1366:1024|sn|the onboard J-Link\n'
            ;;
        rx72m)
            printf '045b:82a0|-|the E2 Lite\n'
            printf '0403:6001|vid|an FTDI for the SCI6 console\n'
            ;;
        f411disco)
            printf '0483:3748|-|the ST-Link V2, which carries no VCP\n'
            ;;
        esp32-wroom)
            printf '1a86:7523|-|the CH340\n'
            ;;
        esp32c6-wroom)
            # The C6 also presents 303a:1001, its native USB JTAG. NEVER DRIVE THAT ONE: it
            # wedges the part on reflash. Other Espressif parts present it too, so it names
            # no chip on its own and is no evidence about this board.
            printf '1a86:55d3|-|the CH343P\n'
            ;;
        blackpill)
            printf '1366:1020|sn|the J-Link Pro on its SWD header\n'
            ;;
        picopi)
            printf '2e8a:0003|sn|the RP2040 in BOOTSEL, which picotool loads\n'
            printf '%s|seen|a KickOS USB CDC console\n' "$KICKOS_CDC_CONSOLE"
            printf '1366:1020|seen|the J-Link Pro, its SWD probe, which this chain never drives\n'
            ;;
        pizero2350)
            printf '2e8a:000f|sn|the RP2350 in BOOTSEL, which picotool loads\n'
            printf '%s|seen|a KickOS USB CDC console\n' "$KICKOS_CDC_CONSOLE"
            ;;
        teensy41)
            printf '16c0:0478|-|HalfKay, the bootloader teensy_loader_cli loads\n'
            printf '%s|seen|a KickOS USB CDC console\n' "$KICKOS_CDC_CONSOLE"
            ;;
        *)
            return 1
            ;;
    esac
    return 0
}

# board_unseen <board>  what a board whose required rows are absent may still be doing, for a
# board that leaves the bus when it runs. Prints nothing for any other board.
# tests/integration/check_reboot.sh witnesses a loader only on a board this prints for.
board_unseen() {
    case "$1" in
        picopi|pizero2350)
            printf 'not in BOOTSEL: running an image, unpowered or not attached, and only BOOTSEL brings a running one back. A console cable is not a board.\n'
            ;;
        teensy41)
            printf 'not in HalfKay: running an image, unpowered or not attached, and only its button brings a running one back. A console cable is not a board.\n'
            ;;
        *)
            ;;
    esac
}

# console_row <board> <probe-serial> <usb-cdc 0|1>
#
# Sets CONSOLE_PORT to a device derived outright, or CONSOLE_PATTERN to a glob still to be
# resolved, and CONSOLE_RIGKEY to the rig.conf key that pins the row. Returns 1 where the
# board has no row and 2 where a USB CDC console is asked of a board with no device
# controller backend.
#
# WHERE THE DEVICE COMES FROM, PER BOARD CLASS. A J-Link VCOM carries the probe serial the
# caller already resolved live, so the row derives the path outright. An ST-Link or CH34x
# has a by-id prefix naming the probe on its own. An FTDI cable is rig data and has no
# tracked pattern at all. A pin in rig.conf always wins: set one the day a second ST-Link
# or a second CH34x joins the bus.
console_row() {
    local board="$1" sn="${2:-}" cdc="${3:-0}" key pin
    CONSOLE_PORT=""
    CONSOLE_PATTERN=""
    CONSOLE_RIGKEY="RIG_CONSOLE_$(printf '%s' "$board" | tr 'a-z-' 'A-Z_')"
    case $board in
        xmc4800-relax|frdmk64f) CONSOLE_PORT="/dev/serial/by-id/usb-SEGGER_J-Link_${sn}-if00" ;;
        esp32c6-wroom)          CONSOLE_PATTERN="/dev/serial/by-id/usb-1a86_USB_Single_Serial_*" ;;
        esp32-wroom)            CONSOLE_PATTERN="/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0" ;;
        f411disco|rx72m|picopi|pizero2350|teensy41|blackpill) ;;
        *) return 1 ;;
    esac
    key="$CONSOLE_RIGKEY"
    pin="${!key:-}"
    if [ -n "$pin" ]; then
        CONSOLE_PATTERN="$pin"
        CONSOLE_PORT=""
    fi
    # A USB device console publishes on the board's own USB and blinds the pin UART, so the
    # cable named above goes quiet and resolving it would capture silence.
    # Keyed on our OWN product strings, not on 1209:0001, which is pid.codes' shared test
    # pair (user/include/kickos/sys/usb_cdc.h) and matches anyone's prototype.
    if [ "$cdc" = "1" ]; then
        case $board in
            picopi|pizero2350|teensy41) ;;
            *) return 2 ;;
        esac
        CONSOLE_PATTERN="$KICKOS_CDC_CONSOLE"
        CONSOLE_PORT=""
    fi
    return 0
}

# board_resources <board> <probe-serial>
#
# Prints what a capture on <board> holds while it runs, one line each: every probe row's device,
# and every console its images can be read on, pin UART or USB device. Two boards printing a
# common line share a probe or a cable, so their captures cannot overlap.
board_resources() {
    local rows cdc
    if rows=$(board_probe_rows "$1" 2>/dev/null); then
        printf '%s\n' "$rows" | awk -F '|' 'NF { print "probe " $1 }'
    fi
    for cdc in 0 1; do
        if console_row "$1" "${2:-}" "$cdc" && [ -n "$CONSOLE_PORT$CONSOLE_PATTERN" ]; then
            printf 'console %s\n' "${CONSOLE_PORT:-$CONSOLE_PATTERN}"
        fi
    done
}

# elf_symbol_at <elf> <symbol>
#
# Prints the file offset and the size of <symbol>'s bytes in <elf>, read through readelf alone,
# which takes every target's ELF. Returns 1 where the symbol is undefined or holds no file bytes.
elf_symbol_at() {
    local sym sec at
    sym=$(LC_ALL=C readelf -sW "$1" 2>/dev/null \
        | awk -v name="$2" '$8 == name && $7 != "UND" { print $2, $3, $7; exit }')
    [ -n "$sym" ] || return 1
    # shellcheck disable=SC2086
    set -- "$1" $sym
    sec=$(LC_ALL=C readelf -SW "$1" 2>/dev/null \
        | sed -n "s/^ *\[ *$4\] *//p" | awk '{ print $2, $3, $4; exit }')
    [ -n "$sec" ] || return 1
    # shellcheck disable=SC2086
    set -- "$2" "$3" $sec
    [ "$3" != NOBITS ] || return 1
    at=$(( 0x$5 + 0x$1 - 0x$4 ))
    printf '%s %s\n' "$at" "$(( $2 ))"
}

# usb_console_image <elf>
#
# Succeeds where the image's kickos_usb_device_console (<kickos/usb_console.h>) is 1: its stdout
# is a USB device console, so its console is the device's own ACM.
usb_console_image() {
    local span byte
    span=$(elf_symbol_at "$1" kickos_usb_device_console) || return 1
    byte=$(dd if="$1" bs=1 skip="${span% *}" count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
    [ "$byte" = 1 ]
}

# image_string <elf> <symbol>
#
# Prints the string <symbol> holds in <elf>, up to its NUL. Returns 1 where it holds none.
image_string() {
    local span
    span=$(elf_symbol_at "$1" "$2") || return 1
    dd if="$1" bs=1 skip="${span% *}" count="${span#* }" 2>/dev/null | tr -d '\000'
}

# The glob expansion, as a script, so a pattern is expanded WHERE THE DEVICES ARE and by the
# same bash on both sides. Each match comes back as `<pattern><tab><device>`; only devices
# that exist come back, a pattern without a wildcard included.
#
# THE PATTERNS TRAVEL INSIDE THE SCRIPT, in a quoted heredoc, and not as arguments. `bash -s`
# reads its whole program from stdin, so patterns appended after it would be program text;
# and passed on the command line instead they would be expanded by the remote login shell,
# which is zsh, on the wrong side and by the wrong rules.
CONSOLE_GLOB_SCRIPT='shopt -s nullglob
while IFS= read -r pat; do
  [ -n "$pat" ] || continue
  m=($pat)
  for d in ${m[@]+"${m[@]}"}; do
    [ -e "$d" ] || continue
    printf "%s\t%s\n" "$pat" "$d"
  done
done <<\KICKOS_PATTERNS'

# console_expand_all  patterns on stdin, `<pattern><tab><device>` out. ONE round trip
# however many patterns it is handed.
console_expand_all() {
    {
        printf '%s\n' "$CONSOLE_GLOB_SCRIPT"
        cat
        printf 'KICKOS_PATTERNS\nexit 0\n'
    } | bench_run
}

# console_expand_one <pattern>  the matching devices, one per line.
console_expand_one() {
    printf '%s\n' "$1" | console_expand_all | cut -f2-
}
