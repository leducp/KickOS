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
# A BOARD WHOSE PROBE IS SHARED WITH ANOTHER BOARD'S IS NOT DECIDABLE HERE, and is reported
# as undecidable rather than guessed at. A standalone J-Link presents 1366:* exactly as the
# onboard probes do, so a wildcard row for one of those boards reports the other board's
# probe as its own.

# board_probe_rows <board>
#
# Prints one row per USB device the board must present, as
#
#     <vid:pid>|<sn, vid or ->|<what it is>
#
# where `sn` marks the device whose serial the flasher has to be told, and `vid` marks a row
# whose vendor id is all the evidence there is, so its presence says a device of that kind is
# on the bus and nothing about whose. Returns 1 for a board this table does not decode, and 2
# for a board whose probe cannot be told from another's, printing the reason instead of rows.
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
        f302nucleo)
            printf '0483:374b|-|the ST-Link V2.1\n'
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
        bluepill-c8)
            printf 'a standalone J-Link presents 1366:*, which the xmc4800-relax and frdmk64f probes present too\n'
            return 2
            ;;
        blackpill)
            printf 'an ST-Link V2 presents 0483:3748, which the f411disco probe presents too\n'
            return 2
            ;;
        *)
            return 1
            ;;
    esac
    return 0
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
        f302nucleo)             CONSOLE_PATTERN="/dev/serial/by-id/usb-STMicroelectronics_STM32_STLink_*-if02" ;;
        esp32c6-wroom)          CONSOLE_PATTERN="/dev/serial/by-id/usb-1a86_USB_Single_Serial_*" ;;
        esp32-wroom)            CONSOLE_PATTERN="/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0" ;;
        f411disco|rx72m|picopi|pizero2350|teensy41) ;;
        *) return 1 ;;
    esac
    key="$CONSOLE_RIGKEY"
    pin="${!key:-}"
    if [ -n "$pin" ]; then
        CONSOLE_PATTERN="$pin"
        CONSOLE_PORT=""
    fi
    # A _usbcdc service list publishes the console on the board's own USB and blinds the pin
    # UART, so the cable named above goes quiet and resolving it would capture silence.
    # Keyed on our OWN product strings, not on 1209:0001, which is pid.codes' shared test
    # pair (user/include/kickos/sys/usb_cdc.h) and matches anyone's prototype.
    if [ "$cdc" = "1" ]; then
        case $board in
            picopi|pizero2350|teensy41) ;;
            *) return 2 ;;
        esac
        CONSOLE_PATTERN="/dev/serial/by-id/usb-KickOS_KickOS_console_*-if00"
        CONSOLE_PORT=""
    fi
    return 0
}

# The glob expansion, as a script, so a pattern is expanded WHERE THE DEVICES ARE and by the
# same bash on both sides. Each match comes back as `<pattern><tab><device>`; a pattern
# carrying no wildcard comes back as itself whether or not it exists, which is what lets a
# caller's refusal name what it looked for.
#
# THE PATTERNS TRAVEL INSIDE THE SCRIPT, in a quoted heredoc, and not as arguments. `bash -s`
# reads its whole program from stdin, so patterns appended after it would be program text;
# and passed on the command line instead they would be expanded by the remote login shell,
# which is zsh, on the wrong side and by the wrong rules.
CONSOLE_GLOB_SCRIPT='shopt -s nullglob
while IFS= read -r pat; do
  [ -n "$pat" ] || continue
  m=($pat)
  for d in ${m[@]+"${m[@]}"}; do printf "%s\t%s\n" "$pat" "$d"; done
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
