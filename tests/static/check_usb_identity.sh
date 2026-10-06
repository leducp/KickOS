#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The identity rows a USB device console opens the host's stream with, and the route verdict
# that holds a capture to them (tools/bench/banner.sh identity_verdict):
#   - the rows are kbanner's own: the same formats over the same version, board and app stamp,
#     and a build time and commit the stamp writes from the values it gives the kernel's;
#   - user/src/banner_identity.cc, compiled here in both diag columns, with and without an app
#     stamp, prints exactly the rows the verdict renders, and the stamps the route reads back
#     out of that image are the ones it printed;
#   - over planted captures, a good one is accepted, a missing or wrong row is refused, and a
#     capture of an image that prints no rows, of another build, or of another image of the
#     same build and commit, is refused.
#
#   check_usb_identity.sh <host c++ compiler>

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 1 ] || { echo "usage: $0 <host c++ compiler>" >&2; exit 2; }
CXX="$1"
require_repo_root
scratch_dir
rc=0
. tools/bench/board-rows.sh
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
[ "$(kbanner_arg KDIAG_F_BANNER_BUILD)" = kickos_build_time ] \
    || bad "kbanner no longer prints its build row from kickos_build_time"
[ "$(kbanner_arg KDIAG_F_BANNER_APP)" = app_stamp ] \
    && grep -qE 'app_stamp = g_app_build_time;' <<< "$KBANNER" \
    || bad "kbanner no longer prints its app row from the app's stamp"
[ "$(kbanner_arg KDIAG_F_BANNER_COMMIT)" = kickos_build_commit ] \
    || bad "kbanner no longer prints its commit row from kickos_build_commit"
# The renderer's two calls, joined: their formats are the rows in kbanner's order, with and
# without the app row, their values the same.
calls=$(awk '/ksnprintf\(/ { on = 1 } on { printf "%s ", $0 } on && /;/ { on = 0; print "" }' "$RENDER" \
    | tr -s ' ')
for want in \
    '"\n" KDIAG_F_BANNER_NAME KDIAG_F_BANNER_BOARD KDIAG_F_BANNER_BUILD KDIAG_F_BANNER_APP KDIAG_F_BANNER_COMMIT, KICKOS_VERSION, KICKOS_BOARD_NAME, kickos_identity_build_time, app, kickos_identity_commit);' \
    '"\n" KDIAG_F_BANNER_NAME KDIAG_F_BANNER_BOARD KDIAG_F_BANNER_BUILD KDIAG_F_BANNER_COMMIT, KICKOS_VERSION, KICKOS_BOARD_NAME, kickos_identity_build_time, kickos_identity_commit);'; do
    grep -qF -e "$want" <<< "$calls" \
        || bad "$RENDER does not print kbanner's rows from its values as [$want]: $calls"
done
grep -qE 'char const\* const volatile app = kickos_app_build_time;' "$RENDER" \
    || bad "$RENDER does not take its app row from the app's stamp"
grep -qF 'target_compile_definitions(kickos_kernel PRIVATE ${KICKOS_BANNER_IDENTITY}' kernel/CMakeLists.txt \
    || bad "the kernel does not take its version and board from KICKOS_BANNER_IDENTITY"
grep -qF 'COMPILE_DEFINITIONS "${KICKOS_BANNER_IDENTITY}"' user/CMakeLists.txt \
    || bad "$RENDER does not take its version and board from KICKOS_BANNER_IDENTITY"
grep -qF 'kickos_build_commit[] = \"${_g}\"' cmake/build_stamp.cmake \
    && grep -qF 'kickos_identity_commit[] = \"${_g}\"' cmake/build_stamp.cmake \
    || bad "cmake/build_stamp.cmake does not write both commits from one label"
grep -qF 'kickos_build_time[] = \"${_t} ${_z}\"' cmake/build_stamp.cmake \
    && grep -qF 'kickos_identity_build_time[] = \"${_t} ${_z}\"' cmake/build_stamp.cmake \
    || bad "cmake/build_stamp.cmake does not write both build times from one time"

# --- the renderer prints what the verdict renders --------------------------------------------
BUILT='2026-10-06 05:23:30 +0200'
STAMP='Oct  6 2026 05:23:12 +0200'
# <with app 0|1>: a main beside the renderer, defining the stamps an image links.
render_main() {
    {
        printf '%s\n' '#include <kickos/sys/banner_identity.h>' '#include <stdio.h>'
        printf 'extern "C" char const kickos_identity_commit[] = "0123abcd-dirty";\n'
        printf 'extern "C" char const kickos_identity_build_time[] = "%s";\n' "$BUILT"
        printf 'extern "C" char const kickos_build_time[] = "%s";\n' "$BUILT"
        if [ "$1" = 1 ]; then
            printf 'extern "C" char const kickos_app_build_time[] = "%s";\n' "$STAMP"
        fi
        printf '%s\n' 'int main()' '{' '    char out[192];' \
            '    size_t const n = kickos::banner_identity(out, sizeof(out));' \
            '    fwrite(out, 1, n, stdout);' '    return kickos_build_time[0] == 0;' '}'
    } > "$TMP/main$1.cc"
}
for app in 0 1; do
    render_main "$app"
    stamp=""
    if [ "$app" = 1 ]; then
        stamp=$STAMP
    fi
    for terse in 0 1; do
        bin="$TMP/render$terse$app"
        "$CXX" -std=c++17 -Iinclude -Iuser/include -Ilib/include -DKICKOS_DIAG_TERSE="$terse" \
            -DKICKOS_LINKER_WEAK_UNDEF=1 '-DKICKOS_VERSION="0.5.1"' '-DKICKOS_BOARD_NAME="picopi"' \
            "$TMP/main$app.cc" "$RENDER" lib/libc/fmt.cc -o "$bin" 2> "$TMP/cc.err" \
            || fail "the renderer does not build on this host: $(head -n 5 "$TMP/cc.err")"
        "$bin" > "$TMP/rows$terse$app" || fail "the renderer did not run"
        {
            printf '\n'
            printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_NAME "$terse" 0.5.1)"
            printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BOARD "$terse" picopi)"
            printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BUILD "$terse" "$BUILT")"
            if [ -n "$stamp" ]; then
                printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_APP "$terse" "$stamp")"
            fi
            printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_COMMIT "$terse" 0123abcd-dirty)"
        } > "$TMP/want$terse$app"
        cmp -s "$TMP/rows$terse$app" "$TMP/want$terse$app" \
            || bad "column $terse, app stamp $app: the image prints [$(cat "$TMP/rows$terse$app")], the verdict renders [$(cat "$TMP/want$terse$app")]"
    done
    # What the route reads back out of the image is what the image printed.
    got=$(image_string "$TMP/render0$app" kickos_build_time)
    [ "$got" = "$BUILT" ] || bad "the build stamp read out of the image is [$got], not [$BUILT]"
    if got=$(image_string "$TMP/render0$app" kickos_app_build_time); then
        [ "$got" = "$stamp" ] || bad "the app stamp read out of the image is [$got], not [$stamp]"
    elif [ -n "$stamp" ]; then
        bad "no app stamp is read out of an image that defines one"
    fi
done

# --- the verdict over planted captures --------------------------------------------------------
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
   app     Oct  6 2026 05:22:47 +0200
$C
[selftest] 1..12
"
refused otherimage 0 'row 4'
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

[ "$rc" -eq 0 ] || exit 1
echo "PASS: a USB console's identity rows are kbanner's, and the route refuses a capture they do not name"
