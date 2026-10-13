#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Threads printing and allocating through one C library at once, on one boot of libcrace under
# QEMU.
#
#   check_libcrace.sh <libcrace image>
#   KOS_CAPTURE=<log> check_libcrace.sh <board-build> <kickos-source> <cmake>
#
# Every worker line is one known text: `libcrace w<w> i<i> ` and forty copies of one letter,
# 'a' + (7w + i) mod 26. A line reaches the wire whole and exactly once, and every worker ends
# reporting no block of its own torn.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

if judging_capture; then
    judge_capture libcrace
else
    run_image "${1:?usage: check_libcrace.sh <libcrace.elf>}" > /dev/null
fi

assert_no_panic "panic while the workers raced"
shape="$(printf '%s\n' "$OUT" | sed -n 's/^libcrace: \([0-9][0-9]*\) workers x \([0-9][0-9]*\) rounds$/\1 \2/p')"
if [ -z "$shape" ]; then
    cfail start "main never announced its workers"
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
        if (match($0, /^libcrace w[0-9]+ done torn=[0-9]+$/)) {
            w = substr($2, 2) + 0
            if ($4 != "torn=0") { found("heap worker " w " found " substr($4, 6) " block(s) torn") }
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
    *)
        cfail lost "${verdict#lost }"
        ;;
esac
if ! has "libcrace: all workers done"; then
    cfail done "main never saw every worker finish"
fi
status_clause "main returned 0" 0

echo "PASS: every worker line reached the wire whole and once, and no worker found a block torn"
exit 0
