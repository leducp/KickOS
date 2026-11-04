#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A thread killed inside printf holding the libc lock, and that lock's next taker, on one boot of
# libc_death under QEMU.
#
#   check_libc_death.sh <libc_death image> <outcome>
#   KOS_CAPTURE=<log> check_libc_death.sh <board-build> <kickos-source> <cmake> <outcome>
#
# The victim's line must never print, since it died before its printf wrote. Main's getenv then
# takes the libc lock from the dead victim, which must end main's task: the line after the take
# is the steal going on. What a trap in main DOES is a property of the backend, so <outcome> is
# passed in: `thread-kill' kills main, whose task the default composition `ends` on, so the init
# ends the system with KOS_EXIT_FAULT; `panic' ends the system at the trap; `image', on a chip
# that holds one task, ends the whole image at the take with KOS_EXIT_FAULT and no fault report.
# A capture carries no exit status, so that status is the emulator's.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_libc_death.sh <libc_death.elf> <outcome: panic|thread-kill|image>"
if judging_capture; then
    shift 3
    outcome="${1:?$_usage}"
else
    elf="${1:?$_usage}"
    outcome="${2:?$_usage}"
fi
case "$outcome" in
    panic|thread-kill|image)
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
if has "libc_death: the victim printed"; then
    cfail printed "the victim's printf completed, so it never parked inside stdio"
fi
if has "libc_death: the victim never died inside printf"; then
    cfail parked "the victim was never seen parked inside its printf, or its kill was refused"
fi
if ! has "libc_death: the victim died inside printf holding the libc lock"; then
    cfail victim "main never reached the take of the dead victim's libc lock"
fi
if has "libc_death: the next taker of the libc lock went on"; then
    cfail steal "the next taker stole the libc lock from its dead holder and went on"
fi
if [ "$outcome" = "thread-kill" ]; then
    if ! has_e "$(thread_fault_re main)"; then
        cfail killed "no thread-kill for 'main' at the take (hang / truncated run?)"
    fi
    assert_no_panic "the take of a dead holder's libc lock panicked the system"
    # KOS_EXIT_FAULT, restated rather than computed from the header the runtime reports.
    status_clause "main's trap ended the system with KOS_EXIT_FAULT (139)" 139
elif [ "$outcome" = "image" ]; then
    if has_e "$KOS_PANIC_RE|=== THREAD FAULT ==="; then
        cfail image "the take ended only the taker's task, where the init shares the image's libc"
    fi
    status_clause "the take ended the image with KOS_EXIT_FAULT (139)" 139
elif ! has_e "$KOS_FAULT_RE"; then
    cfail killed "no fault banner at the take (hang / truncated run?)"
fi

echo "PASS: a thread killed inside printf left the libc lock to a dead holder, and its next taker ended ($outcome)"
exit 0
