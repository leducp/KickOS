#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The C library's own exit() on one boot of libc_exit (natively for the sim, on QEMU when
# QEMU_MACHINE is set).
#
#   check_libc_exit.sh <libc_exit image> [--atexit]
#   KOS_CAPTURE=<log> check_libc_exit.sh <board-build> <kickos-source> <cmake> [--atexit]
#
# A capture carries no exit status, so main's exit() ending the system is the emulator's claim.
#
# The WORKER's marker alone would pass on a libc exit() that never reached the kernel, so
# main's survival is the required half: KOS_SYS_EXIT ends only the calling thread unless the
# caller is its task's entry, and nothing else in the image produces a line after a worker's
# exit. Main's own exit() then has to end the SYSTEM carrying its status, its task's end being
# what the default composition `ends` on, so 7 is the witness: 3 means the worker's exit took
# the image down with it, 124 means nobody's did.
#
# --atexit, where the libc's own exit() runs the image's handlers, which the sim's routing onto
# KOS_SYS_EXIT does not: main registers one only after the worker exited, so its line after
# main's own is main's exit() running it, and not the worker's, which would have consumed it.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=8}"
: "${SIM_TIMEOUT:=8}"

if judging_capture; then
    shift 3
    judge_capture libc_exit
else
    elf="${1:?usage: check_libc_exit.sh <libc_exit.elf> [--atexit]}"
    shift
    run_image "$elf"
fi
atexit=0
[ "${1:-}" = "--atexit" ] && atexit=1

assert_no_panic "panic on the exit() path"
if has "worker spawn refused"; then
    cfail spawn "the worker could not be spawned; the thread-exit arm witnessed nothing"
fi
if ! has "worker: exit()"; then
    cfail worker "the worker never reached its exit()"
fi
if ! has "main: survived worker exit()"; then
    cfail survived "the worker's exit() did not reach KOS_SYS_EXIT (it ended the image, or main died)"
fi
if ! has "main: exit()"; then
    cfail main-exit "main never reached its own exit()"
fi
if [ "$atexit" -eq 1 ]; then
    if ! printf '%s\n' "$OUT" | sed -n '/^main: exit()$/,$p' | grep -q '^main: atexit handler$'
    then
        cfail atexit "main's exit() ran no atexit handler registered after the worker's exit, or ran it
  before main reached its own exit()"
    fi
fi
status_clause "main's exit() ended the system with status 7" 7

if judging_capture; then
    echo "PASS: a worker's exit() ended only the worker, and main reached its own exit()"
    exit 0
fi
if [ "$atexit" -eq 1 ]; then
    echo "PASS: exit() reached KOS_SYS_EXIT from a worker and from main, main's after its own atexit handler, carrying its status"
    exit 0
fi
echo "PASS: exit() reached KOS_SYS_EXIT from a worker and from main, carrying its status"
exit 0
