#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Threads printing and allocating through one C library at once, on one boot of libcrace under
# QEMU.
#
#   check_libcrace.sh <libcrace image> [<runs the dead malloc holder arms, default 1>]
#                     [<one task>] [<unbuffered stdout>]
#   KOS_CAPTURE=<log> check_libcrace.sh <board-build> <kickos-source> <cmake>
#
# Every worker line is one known text: `libcrace w<w> i<i> ` and forty copies of one letter,
# 'a' + (7w + i) mod 26. A line reaches the wire whole and exactly once, and every worker ends
# reporting no block of its own torn and a run that overlapped every other worker of its task;
# every worker says it began before any worker, of either task, is done.
# Before the workers start: a thread killed while it waits
# for the malloc lock ends alone, its task going on, and so does one holding the libc lock, which
# it hands over; a killed waiter whose lock was freed under it asks again at the kernel and ends
# there; a holder killed while it runs releases the malloc lock, and its taker's task lives; getenv
# with a kill pending takes the free libc lock with no kernel entry; a malloc lock taken from a
# holder that died holding it ends the taker's task, whether the taker waited at the death or
# came after it; a printf completes while another thread holds the malloc lock; and main's heap
# runs out at the end of its window.
#
# On the microbit stdout is unbuffered, so the two tasks' lines meet on the console a byte at a
# time. There the lines are not judged whole: every worker still reports no block of its own
# torn and an overlapped run, and each reports. A report is one write, so it reaches the wire whole and ends its line.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# A hang detector, not a speed bound. An SMP board gets a larger one from libcrace.cmake.
: "${QEMU_TIMEOUT:=240}"

dead=1
one_task=0
unbuffered=0
if judging_capture; then
    dead=0
    build="${1:?usage: KOS_CAPTURE=<log> check_libcrace.sh <board-build> <kickos-source> <cmake>}"
    if grep -q '^KICKOS_LIBCRACE_DEAD_ARMS:INTERNAL=1$' "$build/CMakeCache.txt" 2>/dev/null; then
        dead=1
    fi
    one_task="$(sed -n 's/^KICKOS_LIBCRACE_ONE_TASK:INTERNAL=\([0-9]\)$/\1/p' "$build/CMakeCache.txt" 2>/dev/null)"
    one_task="${one_task:-0}"
    if grep -q '^KICKOS_LIBCRACE_UNBUFFERED:INTERNAL=1$' "$build/CMakeCache.txt" 2>/dev/null; then
        unbuffered=1
    fi
    judge_capture libcrace
else
    run_image "${1:?usage: check_libcrace.sh <libcrace.elf> [<dead arms>] [<one task>]}" > /dev/null
    dead="${2:-1}"
    one_task="${3:-0}"
    unbuffered="${4:-0}"
fi

assert_no_panic "panic while the workers raced"
shape="$(printf '%s\n' "$OUT" | sed -n 's/^libcrace: \([0-9][0-9]*\) workers x \([0-9][0-9]*\) rounds$/\1 \2/p')"
if [ -z "$shape" ]; then
    cfail start "main never announced its workers"
fi
if ! has_f "libcrace: a thread killed waiting for malloc ended alone"; then
    cfail malloc-kill "a thread killed waiting for the malloc lock took its task down with it"
fi
if ! has_f "libcrace: a thread killed holding the libc lock handed it over"; then
    cfail libc-kill "a thread killed holding the libc lock did not hand it over"
fi
if ! has_f "libcrace: a killed malloc waiter asked again at the kernel and ended there"; then
    cfail malloc-reask "a killed malloc waiter took a lock freed under it without a kernel entry"
fi
if [ "$one_task" = 1 ]; then
    if ! has_f "libcrace: skip - a holder killed while running released malloc, and its taker's task "\
"lived: this board holds one task"; then
        cfail malloc-release "the arm that needs a peer task did not skip by name on a board that holds one"
    fi
elif ! has_f "libcrace: a holder killed while running released malloc, and its taker's task lived"
then
    cfail malloc-release "a holder killed while running died holding malloc and ended its taker's task"
fi
if ! has_f "libcrace: getenv with a kill pending took the free libc lock with no entry"; then
    cfail getenv-kill "getenv with a kill pending entered the kernel for a free libc lock"
fi
if [ "$dead" = 1 ]; then
    if ! has_f "libcrace: a malloc lock taken from a dead holder ended its task"; then
        cfail malloc-dead "a malloc lock taken from a dead holder left the taker's task running"
    fi
    if ! has_f "libcrace: a malloc lock its holder died in unwatched ended the next taker's task"
    then
        cfail malloc-unwatched "a malloc lock whose holder died unwatched let its next taker run"
    fi
elif ! has_f "libcrace: no task-private heap with a contained fault, so the dead malloc holder "\
"arms did not run"; then
    cfail malloc-dead "the dead malloc holder arms ran where their torn heap would be every task's"
fi
if ! has_f "libcrace: a printf completed while malloc was held"; then
    cfail malloc-wait "a printf waited behind another thread's malloc lock, or never ran beside it"
fi
if ! has_f "libcrace: main's heap ran out at its end"; then
    cfail heap-end "main's heap ran out short of the end of its window"
fi
verdict="$(printf '%s\n' "$OUT" | awk -v shape="$shape" '
    function found(what) { out = what; exit }
    BEGIN { split(shape, s, " "); workers = s[1]; rounds = s[2] }
    /^libcrace w/ {
        if (match($0, /^libcrace w[0-9]+ i[0-9]+ [a-z]+$/)) {
            w = substr($2, 2) + 0; i = substr($3, 2) + 0
            want = sprintf("%c", 97 + (w * 7 + i) % 26)
            text = sprintf("%40s", ""); gsub(/ /, want, text)
            if (w >= workers || i >= rounds || $4 != text) { found("torn " $0) }
            if (seen[w, i]++) { found("duplicated " $0) }
            next
        }
        if (match($0, /^libcrace w[0-9]+ began$/)) {
            next
        }
        if (match($0, /^libcrace w[0-9]+ done torn=[0-9]+ raced=[01]$/)) {
            w = substr($2, 2) + 0
            if ($4 != "torn=0") { found("heap worker " w " found " substr($4, 6) " block(s) torn") }
            if ($5 != "raced=1") { found("raced worker " w " ran no block beside every other racer of its task") }
            done[w] = 1
            next
        }
        found("torn " $0)
    }
    END {
        if (out != "") { print out; exit }
        for (w = 0; w < workers; w++) {
            for (i = 0; i < rounds; i++) {
                if (!((w, i) in seen)) { print "lost worker " w " round " i; exit }
            }
            if (!(w in done)) { print "lost worker " w " never reported its blocks"; exit }
        }
        print "whole"
    }')"
if [ "$unbuffered" = 1 ]; then
    workers="${shape%% *}"
    if printf '%s\n' "$OUT" | grep -q 'done torn=[1-9]'; then
        cfail heap "a heap worker found a block of its own torn"
    fi
    # The other task's bytes can come ahead of a report on its line.
    w=0
    while [ "$w" -lt "$workers" ]; do
        if ! has_e "libcrace w$w done torn=0 raced=[01]\$"; then
            cfail lost "worker $w never reported its blocks whole"
        fi
        if ! has_e "libcrace w$w done torn=0 raced=1\$"; then
            cfail raced "worker $w ran no block beside every other racer of its task"
        fi
        w=$((w + 1))
    done
    verdict=whole
fi
case "$verdict" in
    whole)
        ;;
    torn\ *)
        cfail torn "a worker line reached the wire torn: ${verdict#torn }"
        ;;
    duplicated\ *)
        cfail duplicated "a worker line reached the wire twice: ${verdict#duplicated }"
        ;;
    heap\ *)
        cfail heap "${verdict#heap }"
        ;;
    raced\ *)
        cfail raced "${verdict#raced }"
        ;;
    *)
        cfail lost "${verdict#lost }"
        ;;
esac
# Each began and done line is one write, whole even where the other task's bytes come ahead of it
# on its line, and the wire carries them in the order they were written: a worker's began coming
# after another's done is a run that overlapped nothing of that worker's.
apart="$(printf '%s\n' "$OUT" | awk -v workers="${shape%% *}" '
    match($0, /libcrace w[0-9]+ began$/) {
        w = substr($0, RSTART + 10) + 0
        began[w] = NR
    }
    match($0, /libcrace w[0-9]+ done torn=/) {
        w = substr($0, RSTART + 10) + 0
        done[w] = NR
    }
    END {
        for (a = 0; a < workers; a++) {
            if (!(a in began)) { print "worker " a " never said it began"; exit }
            for (b = 0; b < workers; b++) {
                if (a != b && (b in done) && began[a] > done[b]) {
                    print "worker " a " began after worker " b " was done"; exit
                }
            }
        }
    }')"
if [ -n "$apart" ]; then
    cfail overlap "$apart"
fi
if ! has "libcrace: all workers done"; then
    cfail done "main never saw every worker finish"
fi
status_clause "main returned 0" 0

lines="every worker line reached the wire whole and once"
if [ "$unbuffered" = 1 ]; then
    lines="every worker reported (stdout is unbuffered here, so lines are not judged whole)"
fi
dead_holder="a dead malloc holder's lock ended its taker's task"
if [ "$dead" != 1 ]; then
    dead_holder="the dead malloc holder arms are not built here"
fi
echo "PASS: killed malloc waiters ended alone, a killed waiter re-asked at the kernel, a running killed holder released malloc, getenv took a free lock with a kill pending, $dead_holder, a printf completed beside a held malloc lock, main's heap ran out at its end, $lines, no worker found a block torn, and every worker began before any was done"
exit 0
