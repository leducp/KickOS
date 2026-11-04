#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The bench chain's banner reader (tools/bench/banner.sh) over planted logs: the label it reads
# out of the banner, what it recovers out of a damaged one, where it starts
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
        printf '   board   f411disco\r\n   arch    armv7m\r\n   cpu     cortex-m4\r\n'
        printf '   mpu     off\r\n   sched   tickless\r\n'
        printf '%s\r\n' "$3"
        printf '   heap    1 KiB available\r\n\r\n[reclaimwit] console reclaim + terminate drain witness\r\n'
    } >> "$1"
}
PROSE='   KickOS 0.5.1  -  microkernel RTOS'

# <name> <expected label> [expected-commit argument]: banner_label's answer over the planted log.
label_is() {
    _li=$(banner_label "$TMP/$1" "${3:-}")
    if [ "$_li" != "$2" ]; then
        bad "the banner reader reads [$_li] out of the planted '$1' log, not [$2]"
    fi
}

boot "$TMP/prose" "$PROSE" '   commit  6c3ce8c7'
label_is prose 6c3ce8c7
boot "$TMP/prosedirty" "$PROSE" '   commit  6c3ce8c7-dirty'
label_is prosedirty 6c3ce8c7-dirty
boot "$TMP/prosetag" "$PROSE" '   commit  v0.5.1-3-g6c3ce8c7'
label_is prosetag v0.5.1-3-g6c3ce8c7
# Damage: a row that lost its word, and a row that lost all but its hash. Each is recovered as a
# bare hash whose -dirty cannot be confirmed.
boot "$TMP/damaged" "$PROSE" '   c 6c3ce8c7'
label_is damaged 6c3ce8c7-UNVERIFIED
boot "$TMP/bare" "$PROSE" '6c3ce8c7'
label_is bare 6c3ce8c7-UNVERIFIED
boot "$TMP/nolabel" "$PROSE" '   commit  '
label_is nolabel ''
# A last boot that lost its commit row is not credited with the boot before it.
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
boot "$TMP/twoboots" "$PROSE" '   commit  6c3ce8c7'
boot "$TMP/twoboots" "$PROSE" '   commit  6c3ce8c7'
starts_at twoboots '^   commit  6'
boot "$TMP/rowless" "$PROSE" '   commit  6c3ce8c7'
boot "$TMP/rowless" "$PROSE" ''
starts_at rowless '^   KickOS 0'
boot "$TMP/titleless" "$PROSE" '   commit  6c3ce8c7'
boot "$TMP/titleless" '' '   commit  6c3ce8c7'
starts_at titleless '^   commit  6'

# --- the identity verdict over planted captures -------------------------------------------------
DIAG=include/kickos/diag.h
BUILT='2026-10-06 05:23:30 +0200'
STAMP='usbcdcwit Oct  6 2026 05:23:30 +0200'
OTHER='conreclaim Oct  6 2026 05:23:30 +0200'
# <name> <log body>: the planted capture, CRLF as the bench records it.
plant() {
    printf '%s' "$2" | sed 's/$/\r/' > "$TMP/$1.log"
}
# <name> [<app stamp>]: the verdict over the planted <name> for picopi at 0123abcd-dirty,
# 0.5.1, built at $BUILT, its app stamped $STAMP unless another is given.
verdict() {
    identity_verdict "$TMP/$1.log" "$DIAG" 0.5.1 picopi 0123abcd-dirty "$BUILT" "${2-$STAMP}"
}
accepted() { # <name> [<app stamp>]
    verdict "$@" > "$TMP/why" || bad "the good capture '$1' is refused: $(cat "$TMP/why")"
}
refused() { # <name> <word in the refusal> [<app stamp>]
    local name=$1 word=$2
    shift 2
    if verdict "$name" "$@" > "$TMP/why"; then
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

plant good "
$T
$B
$U
$P
$C
$APP"
accepted good
plant noapp "
$T
$B
$U
$C
$APP"
accepted noapp ''
plant stale-then-good "
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
accepted stale-then-good

plant notitle "
$B
$U
$P
$C
$APP"
refused notitle 'no identity rows'
plant noboard "
$T
$U
$P
$C
$APP"
refused noboard 'row 2'
plant nobuild "
$T
$B
$P
$C
$APP"
refused nobuild 'row 3'
plant noapprow "
$T
$B
$U
$C
$APP"
refused noapprow 'row 4'
plant nocommit "
$T
$B
$U
$P
$APP"
refused nocommit 'row 5'
plant clean "
$T
$B
$U
$P
   commit  0123abcd
$APP"
refused clean 'row 5'
plant otherboard "
$T
   board   pizero2350
$U
$P
$C
$APP"
refused otherboard 'row 2'
plant otherversion "
   KickOS 0.5.0  -  microkernel RTOS
$B
$U
$P
$C
$APP"
refused otherversion 'row 1'
# Another build of the same commit: the image flashed before this build's.
plant otherbuild "
$T
$B
   build   2026-10-06 04:58:02 +0200
$P
$C
$APP"
refused otherbuild 'row 3'
# Another image of the same build and commit: the app the board ran before this one.
plant otherimage "
$T
$B
$U
   app     conreclaim Oct  6 2026 05:22:47 +0200
$C
[selftest] 1..12
"
refused otherimage 'row 4'
# Another image compiled in the same second as the flashed one.
plant sametime "
$T
$B
$U
   app     $OTHER
$C
[conreclaim] PASS
"
refused sametime 'row 4'
# An image with an app stamp where the flashed one has none.
plant appless "
$T
$B
$U
$P
$C
$APP"
refused appless 'row 4' ''
plant good-then-stale "
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
refused good-then-stale 'row 5'
# What an image printed before the rows existed: the app's own commit line and nothing else.
plant oldimage "
[usbcdcwit] commit 0123abcd-dirty
$APP"
refused oldimage 'no identity rows'

if [ "$rc" -eq 0 ]; then
    echo "PASS: the banner reader takes the label out of the banner, recovers a damaged row as"
    echo "  unverified, and holds a USB console's identity rows to the build they name"
fi
exit "$rc"
