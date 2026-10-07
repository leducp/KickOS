#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Self-test of tests/lib/gate.sh: the boot ledger against the ctest TIMEOUT the root CMakeLists
# hands every test as KOS_CTEST_TIMEOUT_S, and the capture readers the restart witnesses rest on,
# each against planted captures of four cores:
#   boot_bound          this test is handed the TIMEOUT it is registered with; a uefi-pe boot's
#                       bound carries the firmware allowance, a kernel boot's is the image's
#                       alone, each boot is charged on top of the ones before it, a TIMEOUT a
#                       boot would reach is refused before it, and with none handed nothing is;
#   after               finds a line whole, carried within one with `part`, or split by a
#                       foreign line, and refuses a literal ending in a digit, which a foreign
#                       line ending in that digit completes, and one read off the end of a line
#                       and the next line whole;
#   log_holds           holds a split line, so a poll waiting on it ends, and not one read off
#                       two others, which would stop the poll early;
#   require_root_depth  reads the last figure, refuses it unreadable, and passes over an earlier
#                       one a split left unreadable;
#   require_drivers_up  takes each packaged driver's up line, as its source prints it, before a
#                       given line, and refuses a driver whose source prints none.
#
#   check_gate.sh <registered TIMEOUT>

set -u
. "$(dirname "$0")/gate.sh"

[ "$#" -eq 1 ] || fail "usage: check_gate.sh <registered TIMEOUT>"
scratch_dir
rc=0

# --- boot_bound -----------------------------------------------------------------------------
if [ "${KOS_CTEST_TIMEOUT_S:-}" != "$1" ]; then
    bad "this test is registered at a TIMEOUT of $1 and was handed '${KOS_CTEST_TIMEOUT_S:-}'"
fi

# Whether `boot_bound` admits every <seconds> in turn under <boot> and <timeout>, in a subshell,
# since a refusal exits. Prints the last bound.
admits() { # <boot> <timeout> <seconds>...
    (
        KICKOS_BOOT="$1"
        KOS_CTEST_TIMEOUT_S="$2"
        shift 2
        for _s in "$@"; do
            boot_bound "$_s"
        done
        echo "$KOS_BOOT_BOUND_S"
    ) 2>/dev/null
}

stop=$((KOS_STOP_TICKS / 5))
uefi_need=$((20 + KOS_UEFI_FIRMWARE_S + stop))

bound="$(admits uefi-pe $((uefi_need + 1)) 20)" \
    || bad "a uefi-pe boot of 20s is refused under a TIMEOUT above its whole bound"
[ "$bound" = "$((20 + KOS_UEFI_FIRMWARE_S))" ] \
    || bad "a uefi-pe boot of 20s is bounded at '${bound}s', not $((20 + KOS_UEFI_FIRMWARE_S))s"
if admits uefi-pe "$uefi_need" 20 >/dev/null; then
    bad "a uefi-pe boot of 20s is admitted under a TIMEOUT its bound reaches"
fi
if admits uefi-pe 40 20 >/dev/null; then
    bad "a uefi-pe boot of 20s is admitted under a TIMEOUT of 40, which the firmware allowance
  alone takes half of"
fi
bound="$(admits kernel $((20 + stop + 1)) 20)" \
    || bad "a kernel boot of 20s is refused under a TIMEOUT above its bound"
[ "$bound" = "20" ] || bad "a kernel boot of 20s is bounded at '${bound}s'"
if admits kernel $((2 * (20 + stop))) 20 20 >/dev/null; then
    bad "two kernel boots of 20s are admitted under a TIMEOUT only one fits"
fi
admits kernel $((2 * (20 + stop) + 1)) 20 20 >/dev/null \
    || bad "two kernel boots of 20s are refused under a TIMEOUT both fit"
admits uefi-pe "" 400 >/dev/null || bad "a boot is refused where no TIMEOUT was handed"

# --- after and log_holds --------------------------------------------------------------------
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

# --- require_root_depth ---------------------------------------------------------------------
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

# --- require_drivers_up ---------------------------------------------------------------------
SYSTEM="$TMP/system.yaml"
printf 'tasks:\n  - name: console\n    driver: xmcuartirq\n  - name: spi0\n    driver: xmcssc\n  - name: app\n    entry: app_main\n' > "$SYSTEM"
UART_UP='[xmcuartirq] device up (IRQ TX)'
SSC_UP='[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)'
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
if up "[xmcuartirq] device up
$SSC_UP
sensor: 1" 3; then
    bad "require_drivers_up passes an up line cut short of what xmcuartirq's source prints"
fi
printf 'tasks:\n  - name: console\n    driver: k64uartirq\n' > "$TMP/k64.yaml"
up "[k64uartirq] device up (IRQ TX/RX)
sensor: 1" 2 "$TMP/k64.yaml" || bad "require_drivers_up misses k64uartirq's up line"
printf 'tasks:\n  - name: console\n    driver: kos_no_driver\n' > "$TMP/none.yaml"
if up "[kos_no_driver] device up (IRQ TX/RX)
sensor: 1" 2 "$TMP/none.yaml"; then
    bad "require_drivers_up passes a driver whose source prints no up line"
fi

[ "$rc" -eq 0 ] || exit 1
echo "PASS: boot_bound charges every boot to its TIMEOUT, and after, log_holds,
  require_root_depth and require_drivers_up read the planted captures as the witnesses need"
