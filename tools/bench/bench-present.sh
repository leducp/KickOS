#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# WHAT IS ON THE BENCH RIGHT NOW. The rig is a command, never a note: which machine holds
# the boards, which boards answer on its bus, what serial each probe carries and which
# device each console is. Read only, safe to run at any time, and it flashes nothing.
#
#   tools/bench/bench-present.sh                  every board this chain can flash
#   tools/bench/bench-present.sh rx72m f411disco  named boards, nonzero if one is absent
#   tools/bench/bench-present.sh reach            does the bench host answer at all
#   tools/bench/bench-present.sh bus              the raw enumeration and the by-id devices
#   tools/bench/bench-present.sh consoles         every console row, resolved
#   tools/bench/bench-present.sh holders          which process holds each console device
#
# From a git worktree, which has no .session/ of its own:
#
#   KICKOS_RIG=/path/to/main/checkout/.session/rig.conf tools/bench/bench-present.sh
#
# THE MACHINE IT ASKED IS IN ITS OWN OUTPUT, always, and so is what ssh resolves that name
# to. A bench reached through an alias and a laptop with an empty bus otherwise produce the
# same board table.
#
# AN UNREACHABLE BENCH AND A BENCH WITH NO BOARDS ARE DIFFERENT ANSWERS. The first refuses
# and names the ssh error; the second is a clean report whose every board says ABSENT.
#
# A BOARD IS NEVER SILENTLY SKIPPED. Every board the tree can flash gets a line: present,
# absent, undecidable because another board presents the same probe, or lacking a row in
# tools/bench/board-rows.sh, which is a thing to add rather than to guess at.
set -u

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
. "$HERE/rig.sh"
. "$HERE/bench-host.sh"
. "$HERE/board-rows.sh"
# Sourced SOFT: with no rig config at all this still enumerates a local bus and says so.
# Every value it would have taken from one is named where it is missing.
rig_find "$ROOT" || true

MODE="report"
NAMED=0
WANT=()
case "${1:-}" in
    reach|bus|consoles|holders)
        MODE="$1"
        ;;
    "")
        ;;
    -h|--help)
        sed -n '5,14p' "$0" | sed 's/^#\{1,\} \{0,1\}//'
        exit 0
        ;;
    *)
        NAMED=1
        WANT=("$@")
        ;;
esac

# The host, and where the name came from. BENCH_HOST wins, because that is what bench.sh and
# bench-fleet.sh select remote mode on; RIG_BENCH_HOST is the rig's own answer to which box
# is the bench, so this script never has to be told a hostname.
HOST=""
HOST_FROM=""
if [ -n "${BENCH_HOST:-}" ]; then
    HOST="$BENCH_HOST"
    HOST_FROM="BENCH_HOST"
elif [ -n "${RIG_BENCH_HOST:-}" ]; then
    HOST="$RIG_BENCH_HOST"
    HOST_FROM="RIG_BENCH_HOST in $RIG_CONF"
fi
bench_host_select "$HOST"

say_where() {
    echo "=== rig     ${RIG_CONF:-none}"
    if [ -z "$HOST" ]; then
        echo "=== bench   THIS BOX ($(uname -n)): no bench host is named"
        echo "===         If the boards are on another machine, set RIG_BENCH_HOST in"
        echo "===         ${RIG_CONF:-a rig config}. See tools/bench/rig.conf.example."
        return 0
    fi
    echo "=== bench   $HOST (from $HOST_FROM)"
    # What ssh will do with that name. `ssh -G` resolves the config and connects to nothing,
    # and it is the only thing that tells an alias pointing at the bench from an alias
    # pointing at some other box that also answers.
    local g args=()
    if [ -n "$BENCH_PORT_FROM" ]; then
        args=(-p "${BENCH_PORT:-${RIG_BENCH_PORT:-}}")
    fi
    if g=$(ssh -G "${args[@]+${args[@]}}" "$HOST" 2>/dev/null); then
        echo "===         ssh resolves it to $(printf '%s\n' "$g" | awk '$1 == "user" { u = $2 } $1 == "hostname" { h = $2 } $1 == "port" { p = $2 } END { printf "%s@%s port %s", u, h, p }')"
    else
        echo "===         ssh cannot resolve $HOST: it names neither an alias nor a host"
    fi
    if [ -n "$BENCH_PORT_FROM" ]; then
        echo "===         port from $BENCH_PORT_FROM, passed as -p"
    else
        echo "===         no -p passed: the port is ssh_config's to answer"
    fi
    if [ -z "${BENCH_HOST:-}" ]; then
        echo "===         BENCH_HOST is unset, so bench.sh and bench-fleet.sh would run"
        echo "===         against THIS BOX. Export BENCH_HOST=$HOST for a flashing run."
    fi
}

# Reachability, which is not presence: a bench that did not answer must never read as a
# bench with nothing plugged into it.
reach() {
    local out
    if ! out=$(printf 'uname -n\nuname -sr\n' | bench_run 2>&1); then
        {
            echo "REFUSING: $BENCH_WHERE did not answer."
            printf '%s\n' "$out" | sed 's/^/  /'
            echo "  Nothing about the boards can be read until it does. Retry with"
            echo "  tools/bench/bench-present.sh reach."
        } >&2
        return 1
    fi
    echo "=== answer  $(printf '%s' "$out" | tr '\n' ' ')"
    return 0
}

read_bus() {
    local rc
    bench_bus_read
    rc=$?
    if [ "$rc" = "1" ]; then
        echo "REFUSING: could not enumerate the bus on $BENCH_WHERE. Run tools/bench/bench-present.sh reach." >&2
        exit 2
    fi
    if [ "$rc" = "2" ]; then
        echo "REFUSING: $BENCH_WHERE answered, and reported no USB bus at all." >&2
        exit 2
    fi
    echo "=== bus     $(printf '%s\n' "$USB_BUS" | wc -l | tr -d ' ') USB devices on $BENCH_WHERE"
}

# A probe serial is a USB string descriptor and is not always text: an ST-Link V2 carries
# twelve RAW BYTES there, which print as mojibake. sysfs has already widened each of those
# bytes into UTF-8, so the hex printed beside it is narrowed back through iso-8859-1 first;
# that is the spelling st-info and RIG_STLINK_<BOARD> use, and the raw sysfs bytes are not.
pretty_serial() {
    local s="$1" clean
    clean=$(printf '%s' "$s" | LC_ALL=C tr -c '[:print:]' '?')
    if [ "$clean" = "$s" ]; then
        printf '%s' "$s"
        return 0
    fi
    printf '%s (hex %s)' "$clean" \
        "$(printf '%s' "$s" | iconv -f UTF-8 -t ISO-8859-1 2>/dev/null | LC_ALL=C od -A n -t x1 | tr -d ' \n' | tr 'a-f' 'A-F')"
}

# Every board the tree can flash, derived from the tree rather than listed here, so a board
# added tomorrow is reported tomorrow. tools/flash.sh spells an emulated or host target with
# a parenthesis, and those are the ones with no bus presence to have.
hardware_boards() {
    "$ROOT/tools/flash.sh" --list | awk 'NR > 1 && $0 !~ /not flashed\)/ { print $1 }'
}

BYID="/dev/serial/by-id"
DEVS=""

# Expand every console pattern, and the whole by-id directory, in ONE round trip. The
# directory listing is what says whether a derived J-Link VCOM path is really there: a
# pattern holding no wildcard comes back as itself whether or not it exists.
resolve_consoles() { # <board>...
    local b pats=""
    for b in "$@"; do
        if console_row "$b" "" 0; then
            if [ -n "$CONSOLE_PATTERN" ]; then
                pats="$pats$CONSOLE_PATTERN
"
            fi
        fi
    done
    DEVS=$(printf '%s%s/*\n' "$pats" "$BYID" | console_expand_all)
}

matches_of() { # <pattern>
    printf '%s\n' "$DEVS" | awk -F '\t' -v p="$1" '$1 == p { print $2 }'
}

byid_has() { # <device>
    printf '%s\n' "$DEVS" | awk -F '\t' -v d="$1" -v p="$BYID/*" '$1 == p && $2 == d { f = 1 } END { exit !f }'
}

INDENT="                          "

# WHO HOLDS A CONSOLE. An orphaned reader from an earlier run writes at its own offset and
# the two logs interleave into something that still looks complete, so a capture refuses a
# held port and names the PID to kill.
HOLDERS_SCRIPT='
while IFS= read -r d; do
  [ -n "$d" ] || continue
  p=$(fuser "$d" 2>/dev/null | tr -s " " " ")
  if [ -z "$p" ]; then
    printf "%s\tfree\n" "$d"
    continue
  fi
  printf "%s\theld by %s\n" "$d" "$(ps -o pid=,comm= -p $p 2>/dev/null | tr -s " " | sed "s/^ //" | paste -sd, -)"
done <<\KICKOS_DEVS'

# console_state <board> <probe serial>  one line, plus more where something needs saying.
console_state() {
    local board="$1" sn="$2" rc n m
    console_row "$board" "$sn" 0
    rc=$?
    if [ "$rc" = "1" ]; then
        echo "no console row in tools/bench/board-rows.sh"
        return 0
    fi
    if [ -n "$CONSOLE_PORT" ]; then
        if byid_has "$CONSOLE_PORT"; then
            echo "$CONSOLE_PORT"
        else
            echo "$CONSOLE_PORT is not on the bus: the probe carries no VCOM there"
        fi
        return 0
    fi
    if [ -z "$CONSOLE_PATTERN" ]; then
        echo "unnamed: set $CONSOLE_RIGKEY in ${RIG_CONF:-the rig config}, see tools/bench/rig.conf.example"
        return 0
    fi
    n=$(matches_of "$CONSOLE_PATTERN" | grep -c .)
    if [ "$n" = "0" ]; then
        echo "nothing matches $CONSOLE_PATTERN"
        return 0
    fi
    if [ "$n" = "1" ]; then
        m=$(matches_of "$CONSOLE_PATTERN")
        if [ -n "${!CONSOLE_RIGKEY:-}" ]; then
            echo "$m (pinned by $CONSOLE_RIGKEY)"
        else
            echo "$m"
        fi
        return 0
    fi
    echo "AMBIGUOUS: $CONSOLE_PATTERN matches $n devices, so which one is this board is a guess"
    matches_of "$CONSOLE_PATTERN" | sed "s|^|$INDENT  |"
    echo "$INDENT""Pin $CONSOLE_RIGKEY in ${RIG_CONF:-the rig config} to the one that is the console."
}

console_line() { # <board> <probe serial>
    local line first=1
    console_state "$1" "$2" | while IFS= read -r line; do
        if [ "$first" = "1" ]; then
            printf '%-16s %-9s %s\n' "" "console" "$line"
            first=0
        else
            printf '%s\n' "$line"
        fi
    done
}

# The serial the flasher has to be told, which is the row marked `sn`. Empty where the board
# needs none, or where the probe is not on the bus.
# rc 2 is a REFUSAL and not an absent serial, so it is carried out rather than flattened to the
# empty string, which reads as a board that carries no serial descriptor. stderr is dropped
# because board_state's row pass has already printed the same refusal.
flash_serial() { # <rows>
    local id
    id=$(printf '%s\n' "$1" | awk -F '|' '$2 == "sn" { print $1; exit }')
    [ -n "$id" ] || return 0
    usb_serial_of "${id%%:*}" "${id##*:}" 2>/dev/null
}

# board_state <board>  the block for one board. Returns 1 when it is not fully present and 2
# when the bus cannot decide it, which is NO ROW, SHARED, or a probe serial that is a guess.
board_state() {
    local board="$1" rows rc id flag what sn snrc missing=0 shown=0 state undecided=0
    rows=$(board_probe_rows "$board")
    rc=$?
    if [ "$rc" = "1" ]; then
        printf '%-16s %-9s %s\n' "$board" "NO ROW" "presence is not decidable; add a row to tools/bench/board-rows.sh"
        console_line "$board" ""
        return 2
    fi
    if [ "$rc" = "2" ]; then
        printf '%-16s %-9s %s\n' "$board" "SHARED" "$rows"
        console_line "$board" ""
        return 2
    fi
    while IFS='|' read -r id flag what; do
        [ -n "$id" ] || continue
        if usb_present "$id"; then
            sn=""
            # A `vid` row is presence only, never identification (see board-rows.sh): the
            # display below never reads $sn for one. Asking usb_serial_of anyway means every
            # vid row that shares its vid:pid with more than one enumerated device prints
            # that function's ambiguity refusal on stderr, on every run, whether or not
            # anything here needed to know WHICH device it was.
            snrc=0
            if [ "$flag" != "vid" ]; then
                sn=$(usb_serial_of "${id%%:*}" "${id##*:}")
                snrc=$?
                if [ "$snrc" != "0" ]; then
                    sn=""
                fi
            fi
            state="present"
        else
            sn=""
            state="ABSENT"
            missing=1
        fi
        if [ "$shown" = "0" ]; then
            printf '%-16s ' "$board"
            shown=1
        else
            printf '%-16s ' ""
        fi
        if [ "$state" = "ABSENT" ]; then
            printf '%-9s %s %s\n' "ABSENT" "$id" "$what"
        elif [ "$flag" = "vid" ]; then
            printf '%-9s %s %s: a device of that kind is here, which is no evidence it is this one\n' "present" "$id" "$what"
        elif [ -n "$sn" ]; then
            printf '%-9s %s %s, serial %s\n' "present" "$id" "$what" "$(pretty_serial "$sn")"
        elif [ "$snrc" = "2" ]; then
            # The flashing path refuses this bus, so the read-only one may not answer it.
            undecided=1
            printf '%-9s %s %s, SERIAL REFUSED: which device this is is a guess (the refusal is above)\n' \
                   "present" "$id" "$what"
        else
            printf '%-9s %s %s, no serial descriptor\n' "present" "$id" "$what"
        fi
    done <<EOF
$rows
EOF
    sn=$(flash_serial "$rows")
    snrc=$?
    # A console derived from a probe serial cannot be derived when the probe is absent, and
    # the path it would build carries an empty serial and looks like a device that once was.
    console_row "$board" "$sn" 0
    if [ "$snrc" = "2" ] && [ -n "$CONSOLE_PORT" ]; then
        printf '%-16s %-9s %s\n' "" "console" "not derived: this board's probe serial is a guess"
    elif [ "$missing" = "1" ] && [ -z "$sn" ] && [ -n "$CONSOLE_PORT" ]; then
        printf '%-16s %-9s %s\n' "" "console" "the probe's own VCOM, and that probe is not on this bus"
    else
        console_line "$board" "$sn"
    fi
    if [ "$snrc" = "2" ] || [ "$undecided" = "1" ]; then
        return 2
    fi
    return "$missing"
}

# holders <device>...  one round trip, one line per device.
holders() {
    {
        printf '%s\n' "$HOLDERS_SCRIPT"
        printf '%s\n' "$@"
        printf 'KICKOS_DEVS\nexit 0\n'
    } | bench_run
}

case "$MODE" in
    reach)
        say_where
        reach || exit 2
        exit 0
        ;;
    bus)
        say_where
        reach || exit 2
        read_bus
        printf '%s\n' "$USB_BUS" | sort | sed 's/^/  /'
        echo "=== $BYID"
        printf '%s/*\n' "$BYID" | console_expand_all | cut -f2- | sort | sed 's/^/  /'
        exit 0
        ;;
esac

if [ "$NAMED" = "0" ]; then
    while IFS= read -r b; do
        WANT+=("$b")
    done < <(hardware_boards)
fi
[ "${#WANT[@]}" -gt 0 ] || { echo "REFUSING: no board to report on; tools/flash.sh --list names none" >&2; exit 2; }

say_where
reach || exit 2
read_bus
resolve_consoles "${WANT[@]}"
echo

if [ "$MODE" = "holders" ]; then
    DEVS_SEEN=()
    for b in "${WANT[@]}"; do
        bsn=$(flash_serial "$(board_probe_rows "$b" || true)")
        if ! console_row "$b" "$bsn" 0; then
            continue
        fi
        if [ -n "$CONSOLE_PORT" ]; then
            # A VCOM path built from an empty serial names no device, and asking who holds
            # it would report a board that is not here as a free port.
            if [ -n "$bsn" ]; then
                DEVS_SEEN+=("$CONSOLE_PORT")
            fi
            continue
        fi
        [ -n "$CONSOLE_PATTERN" ] || continue
        while IFS= read -r d; do
            [ -n "$d" ] && DEVS_SEEN+=("$d")
        done < <(matches_of "$CONSOLE_PATTERN")
    done
    [ "${#DEVS_SEEN[@]}" -gt 0 ] || { echo "no console device resolved, so there is nothing to hold"; exit 0; }
    holders "${DEVS_SEEN[@]}" | sort -u | sed 's/\t/  /' | sed 's/^/  /'
    exit 0
fi

PRESENT=0
ABSENT=0
UNDECIDED=0
for b in "${WANT[@]}"; do
    if [ "$MODE" = "consoles" ]; then
        printf '%-16s ' "$b"
        console_state "$b" "$(flash_serial "$(board_probe_rows "$b" || true)")"
        continue
    fi
    board_state "$b"
    case $? in
        0) PRESENT=$((PRESENT + 1)) ;;
        1) ABSENT=$((ABSENT + 1)) ;;
        *) UNDECIDED=$((UNDECIDED + 1)) ;;
    esac
done
if [ "$MODE" = "consoles" ]; then
    exit 0
fi

echo
echo "=== $PRESENT present, $ABSENT absent, $UNDECIDED not decidable, on $BENCH_WHERE"
if [ "$ABSENT" -gt 0 ]; then
    echo "    An absent board is absent from THAT bus. A board sitting on another machine is"
    echo "    not absent, it is elsewhere, and the machine asked is named at the top."
fi
if [ "$UNDECIDED" -gt 0 ]; then
    echo "    NO ROW and SHARED are neither present nor absent: this chain cannot tell from"
    echo "    the bus, which is a row to add to tools/bench/board-rows.sh and never a thing"
    echo "    to assume. A SERIAL REFUSED row is the same verdict one level in: the device is"
    echo "    here and which unit it is is a guess, which is a RIG_PROBE_<VID>_<PID> pin to"
    echo "    set in the rig config and never a board to flash."
fi
if [ "$NAMED" = "1" ] && [ "$ABSENT" -gt 0 ]; then
    exit 1
fi
exit 0
