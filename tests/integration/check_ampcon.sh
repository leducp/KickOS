#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The user's console contract on one console, judged the same wherever the writer runs.
#
#   check_ampcon.sh <ampcon.elf> local
#   check_ampcon.sh <unused.elf> partition <cmake> <build-dir> <artefact>
#
# `local` boots ampcon on one kernel. `partition` boots the qemu-arm64 amp3 partition: node 1's
# ampcon writes through node 0's relay onto serial 0, and node 2's on its own UART, serial 1. On
# each console, ampcon's four writers each see every line whole and in their order, every
# non-blocking write goes whole or is answered EAGAIN, a write of two chunks arrives whole, and a
# writer killed in its write leaves the console usable.
#
# A change to this gate or its image owes ten green partition runs, as check_amp_partition.sh's.

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"

# <capture> <what>: fails naming <what> on the first broken clause.
judge() {
    _jc="$1"
    _jw="$2"
    printf '%s\n' "$_jc" | grep -qE '^ampcon: 4 writers x 32 lines$' \
        || fail "$_jw: ampcon never announced its writers"
    _verdict="$(printf '%s\n' "$_jc" | awk '
        function found(what) { out = what; exit }
        /ampcon (w[0-9]|nb|k) / {
            if (!match($0, /^ampcon (w[0-3]|nb|k) i[0-9]+ [a-z]+$/)) { found("torn " $0) }
            tag = $2; i = substr($3, 2) + 0; key = 0
            if (tag ~ /^w/) { key = substr(tag, 2) + 0 }
            want = sprintf("%c", 97 + (key * 7 + i) % 26)
            text = sprintf("%40s", ""); gsub(/ /, want, text)
            if ($4 != text) { found("torn " $0) }
            if (i < next_i[tag] + 0 || (tag != "nb" && i != next_i[tag] + 0)) {
                found("out of order " $0)
            }
            next_i[tag] = i + 1
            if (tag == "nb") { nb++ }
            next
        }
        END {
            if (out != "") { print out; exit }
            for (w = 0; w < 4; w++) {
                if (next_i["w" w] + 0 != 32) { print "lost w" w " after " next_i["w" w] + 0 " line(s)"; exit }
            }
            print "whole " nb + 0
        }')"
    case "$_verdict" in
        whole\ *) ;;
        *) fail "$_jw: $_verdict" ;;
    esac
    _nb="${_verdict#whole }"
    printf '%s\n' "$_jc" | grep -qE '^ampcon: every blocking write returned whole$' \
        || fail "$_jw: a blocking write returned short"
    _tally="$(printf '%s\n' "$_jc" \
        | sed -n 's/^ampcon: nonblocking \([0-9]*\) whole, \([0-9]*\) EAGAIN, \([0-9]*\) other$/\1 \2 \3/p')"
    [ -n "$_tally" ] || fail "$_jw: ampcon never reported its non-blocking writes"
    _taken="$(echo "$_tally" | cut -d' ' -f1)"
    _again="$(echo "$_tally" | cut -d' ' -f2)"
    _other="$(echo "$_tally" | cut -d' ' -f3)"
    [ "$_other" = 0 ] || fail "$_jw: $_other non-blocking write(s) answered neither whole nor EAGAIN"
    [ "$((_taken + _again))" = 64 ] \
        || fail "$_jw: $_taken non-blocking write(s) whole and $_again EAGAIN, of 64"
    [ "$_taken" = "$_nb" ] || fail "$_jw: $_taken non-blocking write(s) went whole and $_nb reached the wire"
    printf '%s\n' "$_jc" | grep -qE '^ampcon long q{187}$' \
        || fail "$_jw: the write of two chunks did not arrive whole"
    printf '%s\n' "$_jc" | grep -qE '^ampcon: a writer killed in its write ended$' \
        || fail "$_jw: a writer killed in its write did not end"
    printf '%s\n' "$_jc" | grep -qE '^ampcon: end$' \
        || fail "$_jw: the console took nothing after the killed writer"
    echo "== $_jw: every line whole and in order, $_taken non-blocking write(s) whole and $_again EAGAIN"
}

MODE="${2:?usage: check_ampcon.sh <elf> local | <elf> partition <cmake> <build> <artefact>}"
if [ "$MODE" = local ]; then
    run_image "$1" > /dev/null
    assert_no_panic "panic while ampcon wrote"
    status_clause "ampcon returned 0" 0
    judge "$OUT" "one kernel"
    echo "PASS: one kernel's console kept the contract"
    exit 0
fi

CMAKE="${3:?}"
BUILD="${4:?}"
ART="${5:?}"
: "${QEMU_TIMEOUT:=300}"
need_qemu_machine
echo "== building the ampcon partition artefact =="
"$CMAKE" --build "$BUILD" --target amp_partition_ampcon >/dev/null 2>&1 \
    || fail "the amp_partition_ampcon target did not build"
[ -f "$ART" ] || fail "no artefact at $ART"

scratch_dir
serial1_capture
serial1_ended() { # <console log>
    grep -q '^ampcon: end' "$TMP/serial1" 2>/dev/null
}
KOS_POLL_UNTIL=serial1_ended
poll_image "$ART" '^ampcon: end' > /dev/null
[ "$POLL_OK" = 1 ] && [ "$POLL_UNTIL_OK" = 1 ] \
    || fail "the partition did not end both writers within ${QEMU_TIMEOUT}s"
assert_no_panic "panic while the nodes wrote"
OUT1="$(serial1_out)"
judge "$OUT" "node 1 through node 0's relay"
judge "$OUT1" "node 2 on its own UART"
echo "PASS: node 1 through node 0's relay and node 2 on its own UART kept the one-kernel contract"
