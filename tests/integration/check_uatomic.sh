#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# One boot of uatomic (on QEMU when QEMU_MACHINE is set): round-robin workers race fetch_add on a
# std::atomic<int> and a std::atomic<uint8_t>, and fetch_sub on the std::atomic<uint16_t> beside
# the byte. Each total is the exact one the announced workers and rounds make, and every worker
# saw another's add land between two of its own.
#
#   check_uatomic.sh <uatomic image>
#   KOS_CAPTURE=<log> check_uatomic.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

if judging_capture; then
    judge_capture uatomic
else
    run_image "${1:?usage: check_uatomic.sh <uatomic image>}"
fi

assert_no_panic "panic while the workers raced"
if has "^uatomic: FAIL"; then
    cfail arm "$(printf '%s\n' "$OUT" | grep '^uatomic: FAIL' | head -n 3)"
fi
workers="$(printf '%s\n' "$OUT" | sed -n 's/^uatomic: \([0-9][0-9]*\) workers, quantum [0-9][0-9]* ns$/\1/p')"
[ -n "$workers" ] || cfail start "main never announced its workers"
total=0
w=0
while [ "$w" -lt "$workers" ]; do
    rounds="$(printf '%s\n' "$OUT" \
        | sed -n "s/^uatomic: worker $w ran \\([0-9][0-9]*\\) rounds, interleaved [0-9][0-9]* times\$/\\1/p")"
    [ -n "$rounds" ] || cfail rounds "worker $w never reported its rounds"
    has "^uatomic: worker $w ran $rounds rounds, interleaved [1-9][0-9]* times\$" \
        || cfail interleaved "worker $w saw no other worker's add between two of its own"
    total=$((total + rounds))
    w=$((w + 1))
done
has "^uatomic: ok int $total\$" || cfail int "the std::atomic<int> does not hold the $total rounds run"
has "^uatomic: ok byte $((total % 256))\$" \
    || cfail byte "the std::atomic<uint8_t> does not hold $total modulo 256"
has "^uatomic: ok half $(((65536 - total % 65536) % 65536))\$" \
    || cfail half "the std::atomic<uint16_t> beside the byte does not hold minus $total modulo 65536"
has "^uatomic: done\$" || cfail done "main never reached its end"
status_clause "main's return ended the system with status 0" 0
echo "PASS: $workers workers ran $total rounds of std::atomic read-modify-writes and lost no update"
exit 0
