#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The bench chain's banner reader (tools/bench/banner.sh) over planted logs: the label it reads
# out of the prose and the terse banner, what it recovers out of a damaged one, where it starts
# the last boot, and the identity rows a USB device console opens with (identity_verdict): a
# good capture is accepted, a missing or wrong row is refused, and a capture of an image that
# prints no rows, of another build, or of another image of the same build and commit, the same
# second's included, is refused.

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

# --- the identity verdict over planted captures -------------------------------------------------
DIAG=include/kickos/diag.h
BUILT='2026-10-06 05:23:30 +0200'
STAMP='usbcdcwit Oct  6 2026 05:23:30 +0200'
OTHER='conreclaim Oct  6 2026 05:23:30 +0200'
# <name> <terse> <log body>: the planted capture, CRLF as the bench records it.
plant() {
    printf '%s' "$3" | sed 's/$/\r/' > "$TMP/$1.log"
}
# <name> <terse> [<app stamp>]: the verdict over the planted <name> for picopi at 0123abcd-dirty,
# 0.5.1, built at $BUILT, its app stamped $STAMP unless another is given.
verdict() {
    identity_verdict "$TMP/$1.log" "$DIAG" "$2" 0.5.1 picopi 0123abcd-dirty "$BUILT" "${3-$STAMP}"
}
accepted() { # <name> <terse> [<app stamp>]
    verdict "$@" > "$TMP/why" || bad "the good capture '$1' is refused: $(cat "$TMP/why")"
}
refused() { # <name> <terse> <word in the refusal> [<app stamp>]
    local name=$1 terse=$2 word=$3
    shift 3
    if verdict "$name" "$terse" "$@" > "$TMP/why"; then
        bad "the capture '$name' is accepted"
    elif ! grep -qF -e "$word" "$TMP/why"; then
        bad "the capture '$name' is refused, but not for '$word': $(cat "$TMP/why")"
    fi
}
APP='[usbcdcwit] accepted=8192 of 8192 err=0 maxzero=3
[usbcdcwit] PASS (sustained output past a full ring)
'
T='   KickOS 0.5.1  -  microkernel RTOS'
B='   board   picopi'
U="   build   $BUILT"
P="   app     $STAMP"
C='   commit  0123abcd-dirty'

plant good 0 "
$T
$B
$U
$P
$C
$APP"
accepted good 0
plant terse 1 "
K 0.5.1
b picopi
t $BUILT
p $STAMP
c 0123abcd-dirty
$APP"
accepted terse 1
plant noapp 0 "
$T
$B
$U
$C
$APP"
accepted noapp 0 ''
plant stale-then-good 0 "
$T
$B
$U
$P
   commit  77aa77aa
[usbcdcwit] PASS (sustained output past a full ring)

$T
$B
$U
$P
$C
$APP"
accepted stale-then-good 0

plant notitle 0 "
$B
$U
$P
$C
$APP"
refused notitle 0 'no identity rows'
plant noboard 0 "
$T
$U
$P
$C
$APP"
refused noboard 0 'row 2'
plant nobuild 0 "
$T
$B
$P
$C
$APP"
refused nobuild 0 'row 3'
plant noapprow 0 "
$T
$B
$U
$C
$APP"
refused noapprow 0 'row 4'
plant nocommit 0 "
$T
$B
$U
$P
$APP"
refused nocommit 0 'row 5'
plant clean 0 "
$T
$B
$U
$P
   commit  0123abcd
$APP"
refused clean 0 'row 5'
plant otherboard 0 "
$T
   board   pizero2350
$U
$P
$C
$APP"
refused otherboard 0 'row 2'
plant otherversion 0 "
   KickOS 0.5.0  -  microkernel RTOS
$B
$U
$P
$C
$APP"
refused otherversion 0 'row 1'
# Another build of the same commit: the image flashed before this build's.
plant otherbuild 0 "
$T
$B
   build   2026-10-06 04:58:02 +0200
$P
$C
$APP"
refused otherbuild 0 'row 3'
# Another image of the same build and commit: the app the board ran before this one.
plant otherimage 0 "
$T
$B
$U
   app     conreclaim Oct  6 2026 05:22:47 +0200
$C
[selftest] 1..12
"
refused otherimage 0 'row 4'
# Another image compiled in the same second as the flashed one.
plant sametime 0 "
$T
$B
$U
   app     $OTHER
$C
[conreclaim] PASS
"
refused sametime 0 'row 4'
# An image with an app stamp where the flashed one has none.
plant appless 0 "
$T
$B
$U
$P
$C
$APP"
refused appless 0 'row 4' ''
plant prose-for-terse 1 "
$T
$B
$U
$P
$C
$APP"
refused prose-for-terse 1 'no identity rows'
plant good-then-stale 0 "
$T
$B
$U
$P
$C
$APP
$T
$B
$U
$P
   commit  77aa77aa
$APP"
refused good-then-stale 0 'row 5'
# What an image printed before the rows existed: the app's own commit line and nothing else.
plant oldimage 0 "
[usbcdcwit] commit 0123abcd-dirty
$APP"
refused oldimage 0 'no identity rows'

if [ "$rc" -eq 0 ]; then
    echo "PASS: the banner reader takes the label out of the prose and the terse column,"
    echo "  recovers a damaged row in either as unverified, and holds a USB console's identity"
    echo "  rows to the build they name"
fi
exit "$rc"
