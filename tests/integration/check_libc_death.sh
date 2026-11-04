#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A thread killed inside libc holding one of its locks, and that lock's next taker, on one boot of
# libc_death (inside printf, holding stdout's lock) or libc_death_env (inside setenv, holding the
# environment's lock) under QEMU.
#
#   check_libc_death.sh <libc_death or libc_death_env image> <outcome>
#   KOS_CAPTURE=<log> check_libc_death.sh <board-build> <kickos-source> <cmake> <outcome>
#
# The victim's line must never print, since it died before its call returned. Main then takes
# the lock from the dead victim, which must end main's task: the line after the take is the
# steal going on. What a trap in main DOES is a property of the backend, so <outcome> is
# passed in: `thread-kill' kills main, whose task the default composition `ends` on, so the init
# ends the system with KOS_EXIT_FAULT; `panic' ends the system at the trap.
# A capture carries no exit status, so that status is the emulator's.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_libc_death.sh <libc_death.elf> <outcome: panic|thread-kill>"
if judging_capture; then
    shift 3
    outcome="${1:?$_usage}"
else
    elf="${1:?$_usage}"
    outcome="${2:?$_usage}"
fi
case "$outcome" in
    panic|thread-kill)
        ;;
    *)
        fail "$_usage"
        ;;
esac
if judging_capture; then
    judge_capture libc_death
else
    run_image "$elf" > /dev/null
fi

if ! has "libc_death: start"; then
    cfail start "libc_death never started"
fi
if has "libc_death: the victim was refused"; then
    cfail spawn "the victim could not be spawned"
fi
if has_e "libc_death: the victim (printed|set the environment)"; then
    cfail printed "the victim's call completed, so it never parked inside libc"
fi
if has_e "libc_death: the victim never died inside (printf|setenv)"; then
    cfail parked "the victim was never seen parked inside its call, or its kill was refused"
fi
_held="(printf holding stdout's|setenv holding the environment's) lock"
if ! has_e "libc_death: the victim died inside $_held"; then
    cfail victim "main never reached the take of the dead victim's lock"
fi
if has_e "libc_death: the next taker of (stdout's|the environment's) lock went on"; then
    cfail steal "the next taker stole the lock from its dead holder and went on"
fi
if [ "$outcome" = "thread-kill" ]; then
    if ! has_e "$(thread_fault_re main)"; then
        cfail killed "no thread-kill for 'main' at the take (hang / truncated run?)"
    fi
    assert_no_panic "the take of a dead holder's lock panicked the system"
    # KOS_EXIT_FAULT, restated rather than computed from the header the runtime reports.
    status_clause "main's trap ended the system with KOS_EXIT_FAULT (139)" 139
elif ! has_e "$KOS_FAULT_RE"; then
    cfail killed "no fault banner at the take (hang / truncated run?)"
fi

echo "PASS: a thread killed inside libc left its lock to a dead holder, and its next taker" \
    "ended ($outcome)"
exit 0
