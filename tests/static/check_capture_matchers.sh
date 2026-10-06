#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The capture readers of tests/lib/gate.sh the restart witnesses rest on, each against planted
# captures of four cores:
#   after               finds a line whole, carried within one with `part`, or split by a
#                       foreign line, and refuses a literal ending in a digit, which a foreign
#                       line ending in that digit completes, and one read off the end of a line
#                       and the next line whole;
#   log_holds           holds a split line, so a poll waiting on it ends, and not one read off
#                       two others, which would stop the poll early;
#   require_root_depth  reads the last figure, refuses it unreadable, and passes over an earlier
#                       one a split left unreadable;
#   require_drivers_up  takes each packaged driver's up line, as its source prints it, before a
#                       given line, and refuses a driver it knows no up line for.

set -u
. "$(dirname "$0")/../lib/gate.sh"

scratch_dir
rc=0
CORES=4

# Whether `after <from> <literal>` finds a line in <capture>, in a subshell, since a miss exits.
found() { # <capture> <from> <literal> [part]
    (
        OUT="$1"
        after "$2" "$3" "the planted line" "${4:-}"
    ) >/dev/null 2>&1
}

CAPTURE="exits: served 1 by /svc/exits
exits: served 2 by /svc/exits
traps: served 3 by /svc/traps
=== THREAD FAULT === thread 'worker' killed, system continues
exits: served 3 by watch: traps deaths 1 left 0 alive
/svc/exits"
found "$CAPTURE" 1 'exits: served 2 by /svc/exits' || bad "after misses a whole line"
found "$CAPTURE" 1 "=== THREAD FAULT === thread 'worker' killed" part \
    || bad "after misses a literal a line carries, given part"
if (CORES=1; found "$CAPTURE" 1 "=== THREAD FAULT === thread 'worker' killed"); then
    bad "on one core after takes a line carrying the literal for the literal whole, without part"
fi
found "$CAPTURE" 3 'exits: served 3 by /svc/exits' || bad "after misses a line split by a foreign one"
if found "exits: served 2 by /svc/exits
traps: served 3 by /svc/traps" 1 'exits: served 3 by /svc/exits'; then
    bad "after takes exits' second answer and traps' third for exits' third"
fi
if found "exits: served 2
traps: served 3" 1 'exits: served 3'; then
    bad "after accepts a literal ending in a digit, which a foreign line completes across a split"
fi
STITCH="exits: served 5 by /svc/exits
traps: refused"
if found "$STITCH" 1 'exits: refused'; then
    bad "after reads exits' refusal off its last answer and traps' refusal"
fi
found "exits: refuse
d" 1 'exits: refused' || bad "after misses a line whose second half is one byte"
if found "exits: re
futraps: xsed" 1 'exits: refused'; then
    bad "after takes a line whose tail a third writer's bytes interrupt"
fi
found "exits: rtraps: xef
used" 1 'exits: refused' || bad "after misses a line broken by foreign bytes ahead of its split"

LOG="$TMP/console.log"
printf 'exits: refused\ntraps: rewatch: fails deaths 2 left 0 gone\nfused\n' > "$LOG"
log_holds "$LOG" 'exits: refused' 'traps: refused' || bad "log_holds misses a line split by a foreign one"
if log_holds "$LOG" 'exits: refused' 'fails: refused'; then
    bad "log_holds holds a line the log does not carry"
fi
printf '%s\n' "$STITCH" > "$LOG"
if log_holds "$LOG" 'exits: refused'; then
    bad "log_holds reads exits' refusal off its last answer and traps' refusal"
fi

BOARD_CFG="$TMP/board_config.h"
printf '#define KICKOS_ROOT_STACK_SIZE 8192\n#define KICKOS_MIN_STACK_SIZE 1024\n' > "$BOARD_CFG"
# The depth in a subshell, its PASS line on stdout.
depth() { # <capture>
    (
        OUT="$1"
        require_root_depth
    ) 2>/dev/null
}
FIGURE="root: stack high water 2000 of 8192, 1500 free above the thread-local block"
DEEPER="root: stack high water 3000 of 8192, 1200 free above the thread-local block"
case "$(depth "$FIGURE
watch: exits deaths 1 left 0 alive
$DEEPER")" in
    *"reached 3000 of root's 8192 bytes, leaving 1200 free"*) ;;
    *) bad "require_root_depth does not read the last figure" ;;
esac
if depth "$DEEPER
root: stack high water 3000 of 8192, 1200 frexits: served 2 by /svc/exits
ee above the thread-local block" >/dev/null; then
    bad "require_root_depth reads an earlier figure when the last reached the wire unreadable"
fi
case "$(depth "root: stack high water 1000 of 8192, 1700 frwatch: x
ee above the thread-local block
$DEEPER")" in
    *"reached 3000 of root's 8192 bytes"*) ;;
    *) bad "require_root_depth refuses a run whose earlier figure a split left unreadable" ;;
esac
if depth "root: stack high water 7900 of 8192, 100 free above the thread-local block" >/dev/null; then
    bad "require_root_depth passes a figure under KICKOS_MIN_STACK_SIZE"
fi

SYSTEM="$TMP/system.yaml"
printf 'tasks:\n  - name: console\n    driver: xmcuartirq\n  - name: spi0\n    driver: xmcssc\n  - name: app\n    entry: app_main\n' > "$SYSTEM"
UART_UP='[xmcuartirq] device up (IRQ TX)'
SSC_UP='[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)'
grep -qF -- "$UART_UP" system/driver/xmc4800/xmcuartirq/xmcuartirq.cc \
    || bad "xmcuartirq's source no longer prints the up line planted here"
grep -qF -- "$SSC_UP" system/driver/xmc4800/xmcssc/xmcssc.cc \
    || bad "xmcssc's source no longer prints the up line planted here"
# Whether `require_drivers_up <system> <before>` passes over <capture>, in a subshell.
up() { # <capture> <before> [<system>]
    (
        OUT="$1"
        require_drivers_up "${3:-$SYSTEM}" "$2"
    ) >/dev/null 2>&1
}
up "$UART_UP
$SSC_UP
sensor: 1" 3 || bad "require_drivers_up misses both drivers' up lines ahead of the first reading"
if up "$UART_UP
sensor: 1" 2; then
    bad "require_drivers_up passes a capture missing xmcssc's up line"
fi
if up "$UART_UP
sensor: 1
$SSC_UP" 2; then
    bad "require_drivers_up passes an up line after the first reading"
fi
printf 'tasks:\n  - name: console\n    driver: k64uartirq\n' > "$TMP/other.yaml"
if up "[k64uartirq] device up (IRQ TX/RX)
sensor: 1" 2 "$TMP/other.yaml"; then
    bad "require_drivers_up passes a driver it knows no up line for"
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "PASS: after, log_holds, require_root_depth and require_drivers_up read the planted captures as the witnesses need"
