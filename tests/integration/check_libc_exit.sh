#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The C library's own exit() on one boot of libc_exit (natively for the sim, on QEMU when
# QEMU_MACHINE is set).
#
#   check_libc_exit.sh <libc_exit image> [--atexit]
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

elf="${1:?usage: check_libc_exit.sh <libc_exit.elf> [--atexit]}"
atexit=0
[ "${2:-}" = "--atexit" ] && atexit=1

run_image "$elf"

assert_no_panic "panic on the exit() path"
if has "worker spawn refused"; then
    fail "the worker could not be spawned; the thread-exit arm witnessed nothing"
fi
if ! has "worker: exit()"; then
    fail "the worker never reached its exit()"
fi
if ! has "main: survived worker exit()"; then
    fail "the worker's exit() did not reach KOS_SYS_EXIT (it ended the image, or main died)"
fi
if ! has "main: exit()"; then
    fail "main never reached its own exit()"
fi
if [ "$atexit" -eq 1 ]; then
    if ! printf '%s\n' "$OUT" | sed -n '/^main: exit()$/,$p' | grep -q '^main: atexit handler$'
    then
        fail "main's exit() ran no atexit handler registered after the worker's exit, or ran it
  before main reached its own exit()"
    fi
fi
if [ "$RC" -eq 124 ]; then
    fail "main's exit() left the system running (timed out)"
fi
if [ "$RC" -ne 7 ]; then
    fail "main's exit() shut down with status $RC, not the 7 it passed"
fi

if [ "$atexit" -eq 1 ]; then
    echo "PASS: exit() reached KOS_SYS_EXIT from a worker and from main, main's after its own atexit handler, carrying its status"
    exit 0
fi
echo "PASS: exit() reached KOS_SYS_EXIT from a worker and from main, carrying its status"
exit 0
