#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The C library's own exit() on one boot of libc_exit (natively for the sim, on QEMU when
# QEMU_MACHINE is set).
#
# The WORKER's marker alone would pass on a libc exit() that never reached the kernel, so
# root's survival is the required half: KOS_SYS_EXIT ends only the calling thread unless
# the caller is root, and nothing else in the image produces a line after a worker's exit.
# Root's own exit() then has to end the SYSTEM carrying its status, so 7 is the witness:
# 3 means the worker's exit took the image down with it, 124 means nobody's did.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=8}"
: "${SIM_TIMEOUT:=8}"

elf="${1:?usage: check_libc_exit.sh <libc_exit.elf>}"

run_image "$elf"

assert_no_panic "panic on the exit() path"
if has "worker spawn refused"; then
    fail "the worker could not be spawned; the thread-exit arm witnessed nothing"
fi
if ! has "worker: exit()"; then
    fail "the worker never reached its exit()"
fi
if ! has "root: survived worker exit()"; then
    fail "the worker's exit() did not reach KOS_SYS_EXIT (it ended the image, or root died)"
fi
if ! has "root: exit()"; then
    fail "root never reached its own exit()"
fi
if [ "$RC" -eq 124 ]; then
    fail "root's exit() left the system running (timed out)"
fi
if [ "$RC" -ne 7 ]; then
    fail "root's exit() shut down with status $RC, not the 7 it passed"
fi

echo "PASS: exit() reached KOS_SYS_EXIT from a worker and from root, carrying its status"
exit 0
