#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The C library's exit() and abort() in a worker, on one boot of libc_exit or libc_exit_abort
# (natively for the sim, on QEMU when QEMU_MACHINE is set).
#
#   check_libc_exit.sh <image> [--atexit | --abort]
#   KOS_CAPTURE=<log> check_libc_exit.sh <board-build> <kickos-source> <cmake> [--atexit | --abort]
#
# A capture carries no exit status, so the system's end status is the emulator's claim.
#
# The worker's exit() ends its whole task, so main never prints past it, and the default
# composition ends the system with the task's status: 3, the worker's code. 7 means main survived
# and exited itself, 124 that nobody's exit ended anything. The worker leaves text in stdout's
# buffer first, which exit() flushes.
#
# --atexit, where the libc's own exit() runs the task's handlers, which the sim's routing onto
# KOS_SYS_EXIT does not: the handler main registered before the spawn runs in the worker's exit().
#
# --abort, on libc_exit_abort: the worker's abort() ends the task with 134 (128 + SIGABRT),
# running no exit handler and flushing no stream.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=8}"
: "${SIM_TIMEOUT:=8}"

if judging_capture; then
    shift 3
    judge_capture libc_exit
else
    elf="${1:?usage: check_libc_exit.sh <image> [--atexit | --abort]}"
    shift
    run_image "$elf"
fi
atexit=0
[ "${1:-}" = "--atexit" ] && atexit=1
abort=0
[ "${1:-}" = "--abort" ] && abort=1

assert_no_panic "panic on the exit() path"
if has "worker spawn refused"; then
    cfail spawn "the worker could not be spawned; the exit arm witnessed nothing"
fi
if has "main: survived the worker"; then
    cfail survived "the worker's end left main running: it ended only the worker"
fi
if [ "$abort" -eq 1 ]; then
    has "worker: abort()" || cfail worker "the worker never reached its abort()"
    if has "main: atexit handler"; then
        cfail handlers "the worker's end ran the task's exit handlers"
    fi
    if has "worker: buffered"; then
        cfail flushed "the worker's end flushed the task's streams"
    fi
    status_clause "the worker's abort() ended the task, and the system, with status 134" 134
    echo "PASS: a worker's abort() ended its task with status 134, with no exit handler and no flush"
    exit 0
fi
has "worker: exit()" || cfail worker "the worker never reached its exit()"
if ! has "worker: buffered"; then
    cfail unflushed "the worker's exit() did not flush the text it left in stdout's buffer"
fi
if [ "$atexit" -eq 1 ]; then
    if ! printf '%s\n' "$OUT" | sed -n '/^worker: exit()$/,$p' | grep -q '^main: atexit handler$'
    then
        cfail atexit "the worker's exit() ran no atexit handler main registered, or ran it before
  the worker reached its exit()"
    fi
fi
status_clause "the worker's exit() ended the task, and the system, with status 3" 3
if [ "$atexit" -eq 1 ]; then
    echo "PASS: a worker's exit() ran the task's handler, flushed its streams and ended the task with its status"
    exit 0
fi
echo "PASS: a worker's exit() flushed the task's streams and ended the task with its status"
exit 0
