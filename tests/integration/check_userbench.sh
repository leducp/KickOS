#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# userbench's rows against the posture it states: every row that posture owes printed a figure in
# the shape its clock owes, nothing refused, the run reached its end in one boot. The figures
# themselves are read, not judged.
#
#   check_userbench.sh <userbench image>
#   KOS_CAPTURE=<log> check_userbench.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=180}"
: "${SIM_TIMEOUT:=180}"

if judging_capture; then
    judge_capture userbench
else
    run_image "${1:?usage: check_userbench.sh <userbench image>}"
fi

assert_no_panic "the bench faulted"
n="$(boot_starts | grep -c .)"
[ "$n" -eq 1 ] || cfail reboot "the image booted $n times"
has_f "microbenchmark: switch, IPC, spawn and wake costs, each timed from user space" \
    || cfail header "the bench printed no header"
has "^cpu clock: [0-9]* Hz" || cfail clock "the bench stated no CPU clock"
posture="$(printf '%s\n' "$OUT" | sed -n 's/^bench: posture: cores \([0-9][0-9]*\), clock \([a-z]*\), vector \([a-z]*\), aspace \([a-z]*\), flash \([a-z]*\), threads \([0-9][0-9]*\)$/\1 \2 \3 \4 \5 \6/p' | tail -n 1)"
[ -n "$posture" ] || cfail posture "the bench stated no posture"
set -- $posture
cores=$1
clock=$2
vector=$3
aspace=$4
flash=$5
threads=$6
case "$clock $vector $aspace $flash" in
    fine\ *|average\ *) ;;
    *) cfail posture "the bench stated a clock that is neither fine nor average: $clock" ;;
esac
for f in "$vector" "$aspace" "$flash"; do
    case $f in
        yes|no) ;;
        *) cfail posture "the bench stated a posture field that is neither yes nor no: $f" ;;
    esac
done

# The posture against the image's own statements: the kernel's scheduled core count, and the
# banner's address-space and arch lines.
kcores="$(printf '%s\n' "$OUT" | sed -n 's/^# smp sched: \([0-9][0-9]*\) core(s) in the scheduler$/\1/p' | tail -n 1)"
if [ -z "$kcores" ]; then
    kcores=1
fi
[ "$cores" -eq "$kcores" ] || cfail posture "the bench stated $cores cores, the kernel scheduled $kcores"
kaspace=no
if has "^   mmu     "; then
    kaspace=yes
fi
[ "$aspace" = "$kaspace" ] || cfail posture "the bench stated aspace $aspace, the banner $kaspace"
arch="$(printf '%s\n' "$OUT" | sed -n 's/^   arch    \([a-z0-9_]*\)$/\1/p' | tail -n 1)"
[ -n "$arch" ] || cfail posture "the image's banner names no arch"
case $arch in
    x86_64|armv8a) kvector=yes ;;
    sim) kvector=$vector ;;
    *) kvector=no ;;
esac
[ "$vector" = "$kvector" ] || cfail posture "the bench stated vector $vector on $arch"
kflash=no
if [ "$arch" = lx6 ]; then
    kflash=yes
fi
[ "$flash" = "$kflash" ] || cfail posture "the bench stated flash $flash on $arch"
has "^bench: done$" || cfail done "the bench did not finish"
if has_e "^  [a-z0-9 /-]+: (FAIL|STALL)" || has_e "^bench: FAILED"; then
    cfail refused "$(printf '%s\n' "$OUT" | grep -E '^  [a-z0-9 /-]+: (FAIL|STALL)|^bench: FAILED' | head -n 1)"
fi

# A per-sample row's figure in its clock's shape: p50/p99/max on a fine clock, a mean elsewhere.
N='[0-9][0-9]*'
if [ "$clock" = fine ]; then
    SAMPLED="$N/$N/$N ns  (p50\/p99\/max, n=$N)"
    WALKFIG=median
else
    SAMPLED="$N ns avg  (n=$N)"
    WALKFIG=mean
fi

# <landmark>: a literal whose first line the refusal quotes.
need() { # <token> <basic regex> <what> [landmark]
    if ! has "$2"; then
        if [ -n "${4:-}" ]; then
            cfail "$1" "$3: $(printf '%s\n' "$OUT" | grep -F -- "$4" | head -n 1)"
        fi
        cfail "$1" "$3"
    fi
}

for sz in 8 16 32 64 128 256; do
    need callreply "^  call/reply: $sz B  $N ns/round-trip  ($N round-trips/s over $N calls / $N ms)\$" \
        "no $sz B call/reply row"
    need callreply "^  call/reply: $sz B  $N ns/round-trip  .* \[generic path\]\$" \
        "no $sz B generic call/reply row"
done
need callreply "^  call/reply: 32 B  $N ns/round-trip  .* \[caller outranks server, D1 donates\]\$" \
    "no donating call/reply row"
t="$(printf '%s\n' "$OUT" | grep -c "^  throughput: $N ctx-sw/s  ($N ns/sw avg over $N switches / $N ms)\$")"
[ "$t" -eq 3 ] || cfail throughput "$t throughput rows, not 3"

walks="word-wake owner-exit"
if [ "$aspace" = yes ]; then
    walks="$walks reach-lost"
fi
for w in $walks; do
    need walk "^  $w: -*$N ns/waiter  (k=0 $N ns, k=$N $N ns, k=[1-9][0-9]* $N ns; $WALKFIG of $N)\$" \
        "no $w slope over a waiter in the $clock clock's shape" "  $w:"
done
need e2e "^  e2e-local: $SAMPLED \[tare $N ns\]\$" "no e2e-local row in the $clock clock's shape" \
    "  e2e-local:"

if [ "$cores" -gt 1 ]; then
    c=1
    while [ "$c" -lt "$cores" ]; do
        need xcore "^  xcore-wake 0-$c: $SAMPLED \[round trip \/ 2\]\$" \
            "no core 0 to $c wake in the $clock clock's shape" "  xcore-wake 0-$c:"
        c=$((c + 1))
    done
    for w in w2 w3; do
        need workload "^  $w: $N switches in $N us\$" "no $w workload row"
    done
    for w in w4-push w4-reseat; do
        need workload "^  $w: $N rounds in $N us\$" "no $w workload row"
    done
fi
if [ "$vector" = yes ]; then
    need vec "^  switch-vec: -*$N ns/sw  (dirty $N - clean $N ns/sw, $N switches)\$" \
        "no switch-vec row"
fi
if [ "$flash" = yes ]; then
    need flash "^  flash text: hit $N ns, miss $N ns per call" "no flash text row"
fi
need xtask "^  xtask ping-pong: $N ns/switch" "no xtask ping-pong row" "  xtask ping-pong:"
need xtask "^  xtask call/reply: 16 B  $N ns/round-trip" "no xtask call/reply row" \
    "  xtask call/reply:"
need spawn "^  spawn/exit: thread $N ns" "no thread spawn/exit row" "  spawn/exit: thread"
need spawn "^  spawn/exit: task $N ns" "no task spawn/exit row" "  spawn/exit: task"

status_clause "the bench ended" 0
echo "PASS: userbench printed every row its posture owes ($cores cores, $clock clock)"
