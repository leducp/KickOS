#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The identity rows a USB device console opens the host's stream with, and the route verdict
# that holds a capture to them (tools/bench/banner.sh identity_verdict):
#   - the rows are kbanner's own: the same formats over the same version and board, and a
#     commit the stamp writes from the label it gives the kernel's;
#   - user/src/banner_identity.cc, compiled here in both diag columns, prints exactly the rows
#     the verdict renders;
#   - over planted captures, a good one is accepted, a missing or wrong row is refused, and a
#     capture of an image that prints no rows, or of another build, is refused.
#
#   check_usb_identity.sh <host c++ compiler>

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 1 ] || { echo "usage: $0 <host c++ compiler>" >&2; exit 2; }
CXX="$1"
require_repo_root
scratch_dir
rc=0
. tools/bench/banner.sh
DIAG=include/kickos/diag.h
RENDER=user/src/banner_identity.cc

# --- the rows are kbanner's --------------------------------------------------------------------
KBANNER=$(awk '/void kbanner\(\)/ { on = 1 }
    on { print; o = gsub(/[{]/, "&"); c = gsub(/[}]/, "&"); d += o - c; if (o > 0) { s = 1 } if (s && d == 0) { exit } }' \
    kernel/init/kmain.cc)
[ -n "$KBANNER" ] || fail "found no kbanner() in kernel/init/kmain.cc"
# <macro>: the argument kbanner prints the row with.
kbanner_arg() {
    grep -oE "kprintf\($1, *[A-Za-z_][A-Za-z0-9_]*\)" <<< "$KBANNER" | sed -E 's/.*, *([A-Za-z0-9_]+)\)/\1/'
}
[ "$(kbanner_arg KDIAG_F_BANNER_NAME)" = KICKOS_VERSION ] \
    || bad "kbanner no longer prints its title from KICKOS_VERSION"
[ "$(kbanner_arg KDIAG_F_BANNER_BOARD)" = KICKOS_BOARD_NAME ] \
    || bad "kbanner no longer prints its board row from KICKOS_BOARD_NAME"
[ "$(kbanner_arg KDIAG_F_BANNER_COMMIT)" = kickos_build_commit ] \
    || bad "kbanner no longer prints its commit row from kickos_build_commit"
# The renderer's one call, joined: its format is the three rows in order, its values the same.
call=$(awk '/ksnprintf\(/ { on = 1 } on { printf "%s ", $0 } on && /;/ { exit }' "$RENDER" | tr -s ' ')
case "$call" in
    *'"\n" KDIAG_F_BANNER_NAME KDIAG_F_BANNER_BOARD KDIAG_F_BANNER_COMMIT, KICKOS_VERSION, KICKOS_BOARD_NAME, kickos_identity_commit);'*) ;;
    *) bad "$RENDER does not print kbanner's title, board and commit rows from its values: $call" ;;
esac
grep -qF 'target_compile_definitions(kickos_kernel PRIVATE ${KICKOS_BANNER_IDENTITY}' kernel/CMakeLists.txt \
    || bad "the kernel does not take its version and board from KICKOS_BANNER_IDENTITY"
grep -qF 'COMPILE_DEFINITIONS "${KICKOS_BANNER_IDENTITY}"' user/CMakeLists.txt \
    || bad "$RENDER does not take its version and board from KICKOS_BANNER_IDENTITY"
grep -qF 'kickos_build_commit[] = \"${_g}\"' cmake/build_stamp.cmake \
    && grep -qF 'kickos_identity_commit[] = \"${_g}\"' cmake/build_stamp.cmake \
    || bad "cmake/build_stamp.cmake does not write both commits from one label"

# --- the renderer prints what the verdict renders --------------------------------------------
cat > "$TMP/main.cc" <<'EOF'
#include <kickos/sys/banner_identity.h>
#include <stdio.h>
extern "C" char const kickos_identity_commit[] = "0123abcd-dirty";
int main()
{
    char out[192];
    size_t const n = kickos::banner_identity(out, sizeof(out));
    fwrite(out, 1, n, stdout);
    return 0;
}
EOF
for terse in 0 1; do
    "$CXX" -std=c++17 -Iinclude -Iuser/include -Ilib/include -DKICKOS_DIAG_TERSE="$terse" \
        '-DKICKOS_VERSION="0.5.1"' '-DKICKOS_BOARD_NAME="picopi"' "$TMP/main.cc" "$RENDER" \
        lib/libc/fmt.cc -o "$TMP/render$terse" 2> "$TMP/cc.err" \
        || fail "the renderer does not build on this host: $(head -n 5 "$TMP/cc.err")"
    "$TMP/render$terse" > "$TMP/rows$terse" || fail "the renderer did not run"
    {
        printf '\n'
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_NAME "$terse" 0.5.1)"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BOARD "$terse" picopi)"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_COMMIT "$terse" 0123abcd-dirty)"
    } > "$TMP/want$terse"
    cmp -s "$TMP/rows$terse" "$TMP/want$terse" \
        || bad "column $terse: the image prints [$(cat "$TMP/rows$terse")], the verdict renders [$(cat "$TMP/want$terse")]"
done

# --- the verdict over planted captures --------------------------------------------------------
# <name> <terse> <log body>: the planted capture, CRLF as the bench records it.
plant() {
    printf '%s' "$3" | sed 's/$/\r/' > "$TMP/$1.log"
}
# <name> <terse>: the verdict over the planted <name> for picopi at 0123abcd-dirty, 0.5.1.
verdict() {
    identity_verdict "$TMP/$1.log" "$DIAG" "$2" 0.5.1 picopi 0123abcd-dirty
}
accepted() { # <name> <terse>
    verdict "$1" "$2" > "$TMP/why" || bad "the good capture '$1' is refused: $(cat "$TMP/why")"
}
refused() { # <name> <terse> <word in the refusal>
    if verdict "$1" "$2" > "$TMP/why"; then
        bad "the capture '$1' is accepted"
    elif ! grep -qF -e "$3" "$TMP/why"; then
        bad "the capture '$1' is refused, but not for '$3': $(cat "$TMP/why")"
    fi
}
APP='[usbcdcwit] accepted=8192 of 8192 err=0 maxzero=3
[usbcdcwit] PASS (sustained output past a full ring)
'
T='   KickOS 0.5.1  -  microkernel RTOS'
B='   board   picopi'
C='   commit  0123abcd-dirty'

plant good 0 "
$T
$B
$C
$APP"
accepted good 0
plant terse 1 "
K 0.5.1
b picopi
c 0123abcd-dirty
$APP"
accepted terse 1
plant stale-then-good 0 "
$T
$B
   commit  77aa77aa
[usbcdcwit] PASS (sustained output past a full ring)

$T
$B
$C
$APP"
accepted stale-then-good 0

plant notitle 0 "
$B
$C
$APP"
refused notitle 0 'no identity rows'
plant noboard 0 "
$T
$C
$APP"
refused noboard 0 'row 2'
plant nocommit 0 "
$T
$B
$APP"
refused nocommit 0 'row 3'
plant clean 0 "
$T
$B
   commit  0123abcd
$APP"
refused clean 0 'row 3'
plant otherboard 0 "
$T
   board   pizero2350
$C
$APP"
refused otherboard 0 'row 2'
plant otherversion 0 "
   KickOS 0.5.0  -  microkernel RTOS
$B
$C
$APP"
refused otherversion 0 'row 1'
plant prose-for-terse 1 "
$T
$B
$C
$APP"
refused prose-for-terse 1 'no identity rows'
plant good-then-stale 0 "
$T
$B
$C
$APP
$T
$B
   commit  77aa77aa
$APP"
refused good-then-stale 0 'row 3'
# What an image printed before the rows existed: the app's own commit line and nothing else.
plant oldimage 0 "
[usbcdcwit] commit 0123abcd-dirty
$APP"
refused oldimage 0 'no identity rows'

[ "$rc" -eq 0 ] || exit 1
echo "PASS: a USB console's identity rows are kbanner's, and the route refuses a capture they do not name"
