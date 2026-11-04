#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# mutexbench's rows against the load it states: every row main's THREADS budget seats ran, and
# each loaded row and the crowd stood at that load. The figures themselves are read, not judged.
#
#   check_mutexbench.sh <mutexbench image> [aspace] [coarse-clock]
#   KOS_CAPTURE=<log> check_mutexbench.sh <board-build> <kickos-source> <cmake> [aspace] [coarse-clock]
#
# `aspace`: the board gives each task an address space, so the window row runs. `coarse-clock`:
# the board's clock may step too coarsely to time the crowd, which may then skip.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=90}"
: "${SIM_TIMEOUT:=90}"

if judging_capture; then
    shift 3
    judge_capture mutexbench
else
    run_image "${1:?usage: check_mutexbench.sh <mutexbench image> [aspace] [coarse-clock]}"
    shift
fi
aspace=0
coarse=0
for posture in "$@"; do
    case $posture in
    aspace) aspace=1 ;;
    coarse-clock) coarse=1 ;;
    *) fail "unknown posture '$posture', expected aspace or coarse-clock" ;;
    esac
done

assert_no_panic "the bench faulted"
load="$(printf '%s\n' "$OUT" | sed -n 's/^mutexbench: load: \([0-9]*\) threads, \([0-9]*\) load pairs, a crowd of \([0-9]*\)$/\1 \2 \3/p' | tail -n 1)"
[ -n "$load" ] || cfail load "the bench stated no load"
set -- $load
threads=$1
pairs=$2
crowd=$3
has "^mutexbench: done$" || cfail done "the bench did not finish"
if has_e "^  [a-z-]+: FAIL "; then
    cfail refused "$(printf '%s\n' "$OUT" | grep -E '^  [a-z-]+: FAIL ' | head -n 1)"
fi

# The row's round line, or the SKIP that stands for it.
row() { # <row>
    printf '%s\n' "$OUT" | grep -E "^  $1(-round)?: " | head -n 1
}

if [ "$threads" -lt 3 ]; then
    has_f "  mutex-handoff: SKIP (main's threads seat no handoff pair)" \
        || cfail handoff "$threads threads seat no handoff pair, and the row did not say so"
else
    for r in mutex-handoff sem-handoff; do
        has "^  $r-round: " || cfail "$r" "$threads threads seat $r, and it did not run: $(row "$r")"
    done
    for r in mutex-handoff-loaded mutex-handoff-window; do
        line="$(row "$r")"
        case $line in
        *"SKIP (main's threads seat no load pair)")
            [ "$pairs" -eq 0 ] || cfail "$r" "$pairs load pairs stated, and $r ran none"
            ;;
        *"SKIP (no address spaces)")
            [ "$r" = mutex-handoff-window ] && [ "$aspace" -eq 0 ] || cfail "$r" "$r skipped: $line"
            ;;
        *"-round: "*)
            printf '%s\n' "$line" | grep -qF "; $pairs pairs parked" \
                || cfail "$r" "$pairs load pairs stated, and $r ran beside another load: $line"
            ;;
        *)
            cfail "$r" "$r did not run its load: $line"
            ;;
        esac
    done
    line="$(row sem-wake-crowd)"
    case $line in
    *"SKIP (main's threads seat no crowd)")
        [ "$crowd" -eq 0 ] || cfail crowd "a crowd of $crowd stated, and none stood"
        ;;
    *"SKIP (the clock steps "*)
        [ "$coarse" -eq 1 ] || cfail crowd "the crowd skipped on a board that declares no coarse clock: $line"
        ;;
    *"[$crowd waiters parked, one woken]")
        ;;
    *)
        cfail crowd "a crowd of $crowd stated, and the row reads: $line"
        ;;
    esac
fi
status_clause "the bench ended" 0
echo "PASS: mutexbench ran $pairs load pairs and a crowd of $crowd on $threads threads"
