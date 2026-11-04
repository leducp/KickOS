#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An exception nothing catches: the terminate handler names the exception's type and what(),
# then stops the image. A capture carries no exit status, so only the emulator sees the stop.
#
#   check_qemu_cxxterm.sh <cxxterm.elf> [--worker]
#   KOS_CAPTURE=<log> check_qemu_cxxterm.sh <board-build> <kickos-source> <cmake> [--worker]
#
# --worker, on cxxterm_worker: the throw is in a worker of a task main created, beside a sibling
# that keeps running. The terminate handler's abort() ends that whole task, so main reads it dead,
# its sibling stopped, with status 134 (128 + SIGABRT), and main's clean exit ends the system.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=15}"

if judging_capture; then
    shift 3
    judge_capture cxxterm
else
    elf="${1:?usage: check_qemu_cxxterm.sh <cxxterm.elf> [--worker]}"
    shift
    need_qemu_machine
    run_image "$elf"
fi
worker=0
[ "${1:-}" = "--worker" ] && worker=1

assert_no_panic "the image faulted on its uncaught exception"
if [ "$worker" -eq 1 ]; then
    has "^cxxterm: sibling running" || cfail sibling "the peer's sibling never ran"
    has "^cxxterm: worker throwing" || cfail throw "the peer's worker never reached its throw"
    if has "^cxxterm: worker returned"; then
        cfail returned "an uncaught exception returned to its thrower"
    fi
    printf '%s\n' "$OUT" \
        | grep -qxF "terminate: uncaught exception [St13runtime_error]: cxxterm worker uncaught" \
        || cfail terminate "terminate did not report the exception's type and what()"
    if has "^cxxterm: the peer task still runs"; then
        cfail scope "the uncaught throw ended only its thread: the peer task outlived it"
    fi
    printf '%s\n' "$OUT" | grep -qxF "cxxterm: the peer task is dead, status 134" \
        || cfail status "the creator did not read the peer task dead with status 134"
    status_clause "the creator ended the system cleanly" 0
    echo "PASS: a worker's uncaught exception ended its whole task with status 134"
    exit 0
fi
has "^cxxterm: throwing" || cfail throw "the image never reached its throw"
if has "^cxxterm: returned"; then
    cfail returned "an uncaught exception returned to its thrower"
fi
printf '%s\n' "$OUT" | grep -qxF "terminate: uncaught exception [St13runtime_error]: cxxterm uncaught" \
    || cfail terminate "terminate did not report the exception's type and what()"
status_clause "terminate stopped the image" ended
echo "PASS: terminate reported the uncaught exception"
