#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The bench chain's banner reader (tools/bench/banner.sh) over planted logs: the label it reads
# out of the prose and the terse banner, what it recovers out of a damaged one, and where it
# starts the last boot.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
. tools/bench/banner.sh

# <file> <title> <commit row>: a boot banner as the console prints it, then one app line.
boot() {
    {
        printf '\r\n'
        printf '%s\r\n' "$2"
        printf 'b f302nucleo\r\na armv7m\r\nu cortex-m4\r\nm off\r\ns tickless\r\n'
        printf '%s\r\n' "$3"
        printf 'h 1\r\n\r\n[reclaimwit] console reclaim + terminate drain witness\r\n'
    } >> "$1"
}
PROSE='   KickOS 0.5.1  -  microkernel RTOS'
TERSE='K 0.5.1'

# <name> <expected label> [expected-commit argument]: banner_label's answer over the planted log.
label_is() {
    _li=$(banner_label "$TMP/$1" "${3:-}")
    if [ "$_li" != "$2" ]; then
        bad "the banner reader reads [$_li] out of the planted '$1' log, not [$2]"
    fi
}

boot "$TMP/prose" "$PROSE" '   commit  6c3ce8c7'
label_is prose 6c3ce8c7
boot "$TMP/terse" "$TERSE" 'c 6c3ce8c7'
label_is terse 6c3ce8c7
boot "$TMP/tersedirty" "$TERSE" 'c 6c3ce8c7-dirty'
label_is tersedirty 6c3ce8c7-dirty
boot "$TMP/tersetag" "$TERSE" 'c v0.5.1-3-g6c3ce8c7'
label_is tersetag v0.5.1-3-g6c3ce8c7
# Damage: a prose row that lost its word, the short row under a prose title, and a short row
# that lost its own `c `. Each is recovered as a bare hash whose -dirty cannot be confirmed.
boot "$TMP/damaged" "$PROSE" '   c 6c3ce8c7'
label_is damaged 6c3ce8c7-UNVERIFIED
boot "$TMP/mixed" "$PROSE" 'c 6c3ce8c7'
label_is mixed 6c3ce8c7-UNVERIFIED
boot "$TMP/bare" "$TERSE" '6c3ce8c7'
label_is bare 6c3ce8c7-UNVERIFIED
boot "$TMP/nolabel" "$TERSE" 'c '
label_is nolabel ''
# A last boot that lost its commit row is not credited with the boot before it.
boot "$TMP/rowlessterse" "$TERSE" 'c 6c3ce8c7'
boot "$TMP/rowlessterse" "$TERSE" ''
label_is rowlessterse 6c3ce8c7-UNVERIFIED
boot "$TMP/rowlessprose" "$PROSE" '   commit  6c3ce8c7'
boot "$TMP/rowlessprose" "$PROSE" ''
label_is rowlessprose 6c3ce8c7-UNVERIFIED

# The last boot starts at the later of its title and its commit row, so either one lost leaves
# the other as the anchor; an earlier boot's rows never stand in.
# <name> <pattern of the line it must start at>
starts_at() {
    _bs=$(bench_boot_start "$TMP/$1")
    _want=$(grep -an "$2" "$TMP/$1" | tail -n 1 | cut -d: -f1)
    if [ "$_bs" != "$_want" ]; then
        bad "the last boot of the planted '$1' log is read to start at line [$_bs], not at $_want"
    fi
}
boot "$TMP/twoboots" "$TERSE" 'c 6c3ce8c7'
boot "$TMP/twoboots" "$TERSE" 'c 6c3ce8c7'
starts_at twoboots '^c 6'
boot "$TMP/rowless" "$TERSE" 'c 6c3ce8c7'
boot "$TMP/rowless" "$TERSE" ''
starts_at rowless '^K 0'
boot "$TMP/titleless" "$TERSE" 'c 6c3ce8c7'
boot "$TMP/titleless" '' 'c 6c3ce8c7'
starts_at titleless '^c 6'

if [ "$rc" -eq 0 ]; then
    echo "PASS: the banner reader takes the label out of the prose and the terse column, and"
    echo "  recovers a damaged row in either as unverified"
fi
exit "$rc"
