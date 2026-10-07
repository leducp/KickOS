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
#   - every row at its widest renders whole into BANNER_IDENTITY_MAX, the buffer a USB device
#     console holds the rows in;
#   - the app stamp kickos/app.h compiles names its image, so two images compiled in the same
#     second carry different stamps.
# tests/static/check_bench_banner.sh holds the verdict to planted captures.
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
    && grep -qE 'app_stamp = g_app_stamp;' <<< "$KBANNER" \
    || bad "kbanner no longer prints its app row from the app's stamp"
[ "$(kbanner_arg KDIAG_F_BANNER_COMMIT)" = kickos_build_commit ] \
    || bad "kbanner no longer prints its commit row from kickos_build_commit"
# The renderer's rows, in order: kbanner's formats over its values.
rows=$(grep -oE 'b\.row\(KDIAG_F_BANNER_[A-Z]+, [A-Za-z_]+' "$RENDER" | sed 's/^b\.row(//' | tr '\n' ' ')
want='KDIAG_F_BANNER_NAME, KICKOS_VERSION KDIAG_F_BANNER_BOARD, KICKOS_BOARD_NAME KDIAG_F_BANNER_BUILD, kickos_identity_build_time KDIAG_F_BANNER_APP, app KDIAG_F_BANNER_COMMIT, kickos_identity_commit '
[ "$rows" = "$want" ] || bad "$RENDER does not print kbanner's rows from its values as [$want]: [$rows]"
grep -qE 'char const\* const volatile app = kickos_app_stamp;' "$RENDER" \
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

# --- the app stamp names its image ---------------------------------------------------------
# One second for both images: SOURCE_DATE_EPOCH fixes __DATE__ and __TIME__, in UTC.
EPOCH=1791264192
# <image>: the object kickos/app.h emits the app stamp into, compiled as the build compiles an
# image's TU, in $TMP/stamp_<image>.o, and the stamp an image linking it carries.
stamp_of() {
    printf 'int kickos_stamp_tu;\n' > "$TMP/stamp_$1.cc"
    SOURCE_DATE_EPOCH=$EPOCH "$CXX" -std=c++17 -include user/include/kickos/app.h \
        -Dmain=kickos_app_main "-DKICKOS_APP_IMAGE=\"$1\"" -DKICKOS_APP_TZ=+0200 \
        -c "$TMP/stamp_$1.cc" -o "$TMP/stamp_$1.o" 2> "$TMP/cc.err" \
        || fail "kickos/app.h does not compile on this host: $(head -n 5 "$TMP/cc.err")"
    printf '%s\n' 'extern "C" char const kickos_app_stamp[];' \
        'int main() { return kickos_app_stamp[0] == 0; }' > "$TMP/probe.cc"
    "$CXX" "$TMP/probe.cc" "$TMP/stamp_$1.o" -o "$TMP/probe_$1" 2> "$TMP/cc.err" \
        || fail "an image of the app stamp does not link: $(head -n 5 "$TMP/cc.err")"
    image_string "$TMP/probe_$1" kickos_app_stamp
}
STAMP=$(stamp_of usbcdcwit)
OTHER=$(stamp_of conreclaim)
STAMP_TIME="$(LC_ALL=C TZ=UTC date -d "@$EPOCH" '+%b %e %Y %H:%M:%S') +0200"
[ "$STAMP" = "usbcdcwit $STAMP_TIME" ] \
    || bad "the app stamp is [$STAMP], not the image and its time [usbcdcwit $STAMP_TIME]"
[ "$OTHER" != "$STAMP" ] || bad "two images compiled in the same second carry one app stamp [$STAMP]"

# --- the renderer prints what the verdict renders --------------------------------------------
BUILT='2026-10-06 05:23:30 +0200'
# <with app 0|1>: a main beside the renderer, defining the stamps an image links.
render_main() {
    {
        printf '%s\n' '#include <kickos/sys/banner_identity.h>' '#include <stdio.h>'
        printf 'extern "C" char const kickos_identity_commit[] = "0123abcd-dirty";\n'
        printf 'extern "C" char const kickos_identity_build_time[] = "%s";\n' "$BUILT"
        printf 'extern "C" char const kickos_build_time[] = "%s";\n' "$BUILT"
        printf '%s\n' 'int main()' '{' '    char out[192];' \
            '    size_t const n = kickos::banner_identity(out, sizeof(out));' \
            '    fwrite(out, 1, n, stdout);' '    return kickos_build_time[0] == 0;' '}'
    } > "$TMP/main$1.cc"
}
for app in 0 1; do
    render_main "$app"
    stamp=""
    stamp_obj=""
    if [ "$app" = 1 ]; then
        stamp=$STAMP
        stamp_obj="$TMP/stamp_usbcdcwit.o"
    fi
    for terse in 0 1; do
        bin="$TMP/render$terse$app"
        "$CXX" -std=c++17 -Iinclude -Iuser/include -Ilib/include -DKICKOS_DIAG_TERSE="$terse" \
            -DKICKOS_LINKER_WEAK_UNDEF=1 '-DKICKOS_VERSION="0.5.1"' '-DKICKOS_BOARD_NAME="picopi"' \
            "$TMP/main$app.cc" "$RENDER" $stamp_obj -o "$bin" 2> "$TMP/cc.err" \
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
    if got=$(image_string "$TMP/render0$app" kickos_app_stamp); then
        [ "$got" = "$stamp" ] || bad "the app stamp read out of the image is [$got], not [$stamp]"
    elif [ -n "$stamp" ]; then
        bad "no app stamp is read out of an image that defines one"
    fi
done

# --- the longest block fits the USB console's buffer ------------------------------------------
# Every value at its row's widest: a 45-character image name stamps the widest app row.
widest() { # <character> <count>
    printf "%${2}s" '' | tr ' ' "$1"
}
LONG_VERSION=$(widest 9 16)
LONG_BOARD=$(widest b 32)
LONG_COMMIT=$(widest c 48)
LONG_STAMP=$(stamp_of "$(widest i 45)")
[ "${#LONG_STAMP}" -eq 72 ] || bad "a 45-character image stamps [$LONG_STAMP], not 72 characters"
{
    printf '%s\n' '#include <kickos/sys/banner_identity.h>' '#include <stdio.h>'
    printf 'extern "C" char const kickos_identity_commit[] = "%s";\n' "$LONG_COMMIT"
    printf 'extern "C" char const kickos_identity_build_time[] = "%s";\n' "$BUILT"
    printf '%s\n' 'int main()' '{' '    char out[kickos::BANNER_IDENTITY_MAX];' \
        '    size_t const n = kickos::banner_identity(out, sizeof(out));' \
        '    fwrite(out, 1, n, stdout);' '    if (n != sizeof(out) - 1u)' '    {' \
        '        return 2;' '    }' '    return 0;' '}'
} > "$TMP/mainlong.cc"
for terse in 0 1; do
    "$CXX" -std=c++17 -Iinclude -Iuser/include -Ilib/include -DKICKOS_DIAG_TERSE="$terse" \
        -DKICKOS_LINKER_WEAK_UNDEF=1 "-DKICKOS_VERSION=\"$LONG_VERSION\"" \
        "-DKICKOS_BOARD_NAME=\"$LONG_BOARD\"" "$TMP/mainlong.cc" "$RENDER" \
        "$TMP/stamp_$(widest i 45).o" -o "$TMP/renderlong$terse" 2> "$TMP/cc.err" \
        || fail "the renderer does not build at the widest rows: $(head -n 5 "$TMP/cc.err")"
    "$TMP/renderlong$terse" > "$TMP/rowslong$terse"
    long_rc=$?
    {
        printf '\n'
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_NAME "$terse" "$LONG_VERSION")"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BOARD "$terse" "$LONG_BOARD")"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BUILD "$terse" "$BUILT")"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_APP "$terse" "$LONG_STAMP")"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_COMMIT "$terse" "$LONG_COMMIT")"
    } > "$TMP/wantlong$terse"
    cmp -s "$TMP/rowslong$terse" "$TMP/wantlong$terse" \
        || bad "column $terse, every row at its widest: the image prints [$(cat "$TMP/rowslong$terse")], the verdict renders [$(cat "$TMP/wantlong$terse")]"
    [ "$long_rc" -eq 0 ] \
        || bad "column $terse: the widest rows do not fill BANNER_IDENTITY_MAX to its NUL, so the buffer is not sized from them"
done

[ "$rc" -eq 0 ] || exit 1
echo "PASS: a USB console's identity rows are kbanner's, rendered as the route's verdict renders them"
