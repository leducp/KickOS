#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The identity rows a USB device console opens the host's stream with, and the route verdict
# that holds a capture to them (tools/bench/banner.sh identity_verdict):
#   - the rows are kbanner's own: the same formats over the same version, board and app stamp,
#     and a build time and commit the stamp writes from the values it gives the kernel's;
#   - user/src/banner_identity.cc, compiled here with and without an app stamp and in either
#     CRLF posture, prints exactly the rows the verdict renders, each newline as the posture
#     sends it, and the stamps the route reads back out of that image are the ones it printed;
#   - every row at its widest renders whole into BANNER_IDENTITY_MAX, the buffer a USB device
#     console holds the rows in;
#   - the app stamp kickos/app.h compiles names its image, so two images compiled in the same
#     second carry different stamps;
#   - of an ELF holding several images, the route reads the app stamp of the image that links
#     banner_identity, none where that image defines none, and refuses an ELF defining a stamp
#     where no image links banner_identity or the one linking it is no image's.
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
grep -qF 'kprintf(KDIAG_F_BANNER_NAME, diag::banner_title_indent(KICKOS_VERSION), KICKOS_VERSION);' \
    <<< "$KBANNER" || bad "kbanner no longer prints its centred title from KICKOS_VERSION"
[ "$(kbanner_arg KDIAG_F_BANNER_BOARD)" = KICKOS_BOARD_NAME ] \
    || bad "kbanner no longer prints its board row from KICKOS_BOARD_NAME"
[ "$(kbanner_arg KDIAG_F_BANNER_BUILD)" = kickos_build_time ] \
    || bad "kbanner no longer prints its build row from kickos_build_time"
[ "$(kbanner_arg KDIAG_F_BANNER_APP)" = app_stamp ] \
    && grep -qE 'image_view\(0u, image_record\(0u\)\.stamp\)\);' \
        <<< "$KBANNER" \
    || bad "kbanner no longer prints its app row from the app's stamp"
[ "$(kbanner_arg KDIAG_F_BANNER_COMMIT)" = kickos_build_commit ] \
    || bad "kbanner no longer prints its commit row from kickos_build_commit"
# The renderer's rows, in order: kbanner's formats over its values.
rows=$(grep -oE 'b\.row\(KDIAG_F_BANNER_[A-Z]+, [A-Za-z_]+' "$RENDER" | sed 's/^b\.row(//' | tr '\n' ' ')
want='KDIAG_F_BANNER_NAME, title KDIAG_F_BANNER_BOARD, KICKOS_BOARD_NAME KDIAG_F_BANNER_BUILD, kickos_identity_build_time KDIAG_F_BANNER_APP, app KDIAG_F_BANNER_COMMIT, kickos_identity_commit '
[ "$rows" = "$want" ] || bad "$RENDER does not print kbanner's rows from its values as [$want]: [$rows]"
grep -qF 'char const* const title[] = {diag::banner_title_indent(KICKOS_VERSION), KICKOS_VERSION};' \
    "$RENDER" || bad "$RENDER does not render kbanner's centred title from KICKOS_VERSION"
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
        "-DKICKOS_APP_IMAGE=\"$1\"" -DKICKOS_APP_TZ=+0200 \
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
# <crlf 0|1>: each newline of stdin as that posture sends it.
posture() {
    if [ "$1" = 1 ]; then
        sed 's/$/\r/'
    else
        cat
    fi
}
for crlf in 0 1; do
for app in 0 1; do
    render_main "$app"
    stamp=""
    stamp_obj=""
    if [ "$app" = 1 ]; then
        stamp=$STAMP
        stamp_obj="$TMP/stamp_usbcdcwit.o"
    fi
    bin="$TMP/render$app"
    "$CXX" -std=c++17 -Iinclude -Iuser/include -Ilib/include "-DKICKOS_CONSOLE_CRLF=$crlf" \
        -DKICKOS_LINKER_WEAK_UNDEF=1 '-DKICKOS_VERSION="0.5.1"' '-DKICKOS_BOARD_NAME="picopi"' \
        "$TMP/main$app.cc" "$RENDER" $stamp_obj -o "$bin" 2> "$TMP/cc.err" \
        || fail "the renderer does not build on this host: $(head -n 5 "$TMP/cc.err")"
    "$bin" > "$TMP/rows$app" || fail "the renderer did not run"
    # The title is held by its name and version; its centring by the probe below.
    title=$(sed -n 2p "$TMP/rows$app" | tr -d '\r')
    [ "$(banner_title_version "$title")" = 0.5.1 ] \
        || bad "app stamp $app, crlf $crlf: the title [$title] does not name version 0.5.1"
    {
        printf '\n'
        printf '%s\n' "$title"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BOARD picopi)"
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BUILD "$BUILT")"
        if [ -n "$stamp" ]; then
            printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_APP "$stamp")"
        fi
        printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_COMMIT 0123abcd-dirty)"
    } | posture "$crlf" > "$TMP/want$app"
    cmp -s "$TMP/rows$app" "$TMP/want$app" \
        || bad "app stamp $app, crlf $crlf: the image prints [$(od -c "$TMP/rows$app" | head -n 4)], the verdict renders [$(od -c "$TMP/want$app" | head -n 4)]"
    # What the route reads back out of the image is what the image printed.
    got=$(image_string "$TMP/render$app" kickos_build_time)
    [ "$got" = "$BUILT" ] || bad "the build stamp read out of the image is [$got], not [$BUILT]"
    if got=$(identity_app_stamp "$TMP/render$app"); then
        [ "$got" = "$stamp" ] || bad "the app stamp read out of the image is [$got], not [$stamp]"
    elif [ -n "$stamp" ]; then
        bad "no app stamp is read out of an image that defines one"
    fi
done
done

# --- of several images, the route reads the stamp of the one printing the rows ---------------
# Each image's stamp a local of its own, as the image link leaves it, the first object linked
# listed first in the symbol table. C, where a local's name is not mangled. The renderer linked
# into image 0, image 1, no image's section, or nowhere.
for n in 0 1; do
    printf 'static char const kickos_app_stamp[] __attribute__((section(".kickos_image%s_rodata"), used)) = "image%s";\n' \
        "$n" "$n" > "$TMP/image$n.c"
    "$CXX" -x c -c "$TMP/image$n.c" -o "$TMP/image$n.o" 2> "$TMP/cc.err" \
        || fail "an image's stamp does not compile: $(head -n 5 "$TMP/cc.err")"
done
for at in image0 image1 plain none; do
    {
        printf '%s\n' '#include <stddef.h>' 'int main() { return 0; }'
        case "$at" in
            image*) printf 'namespace kickos { __attribute__((section(".kickos_%s_text"), used)) size_t banner_identity(char*, size_t) { return 0; } }\n' "$at" ;;
            plain) printf '%s\n' 'namespace kickos { __attribute__((used)) size_t banner_identity(char*, size_t) { return 0; } }' ;;
        esac
    } > "$TMP/anchor_$at.cc"
done
# <name> <anchor> <object>...: links the probe $TMP/<name>.
probe() {
    local name=$1 at=$2
    shift 2
    "$CXX" -std=c++17 "$@" "$TMP/anchor_$at.cc" -o "$TMP/$name" 2> "$TMP/cc.err" \
        || fail "the probe $name does not link: $(head -n 5 "$TMP/cc.err")"
}
first_listed() { # <probe> <stamp>
    [ "$(image_string "$TMP/$1" kickos_app_stamp)" = "$2" ] \
        || fail "the probe $1 does not list [$2]'s stamp first, so it tells no image from another"
}
probe images image1 "$TMP/image0.o" "$TMP/image1.o"
first_listed images image0
got=$(identity_app_stamp "$TMP/images")
[ "$got" = image1 ] \
    || bad "of two images the route reads the app stamp [$got], not banner_identity's image's [image1]"
probe images_first image0 "$TMP/image1.o" "$TMP/image0.o"
first_listed images_first image1
got=$(identity_app_stamp "$TMP/images_first")
[ "$got" = image0 ] \
    || bad "of two images the route reads the app stamp [$got], not banner_identity's image's [image0]"
probe unstamped image1 "$TMP/image0.o"
got=$(identity_app_stamp "$TMP/unstamped")
st=$?
[ "$st" = 1 ] && [ -z "$got" ] \
    || bad "banner_identity's image defines no stamp, yet the route reads [$got] with status $st"
probe unanchored none "$TMP/image0.o" "$TMP/image1.o"
got=$(identity_app_stamp "$TMP/unanchored")
st=$?
[ "$st" = 2 ] && [ -n "$got" ] \
    || bad "no image links banner_identity, yet the route reads [$got] with status $st, not a refusal"
probe unplaced plain "$TMP/image0.o" "$TMP/image1.o"
got=$(identity_app_stamp "$TMP/unplaced")
st=$?
[ "$st" = 2 ] && [ -n "$got" ] \
    || bad "banner_identity lies in no image of two, yet the route reads [$got] with status $st, not a refusal"
probe unplaced_one plain "$TMP/image0.o"
got=$(identity_app_stamp "$TMP/unplaced_one")
st=$?
[ "$st" = 2 ] && [ -n "$got" ] \
    || bad "banner_identity lies in no image, the stamp in one, yet the route reads [$got] with status $st, not a refusal"

# --- the longest block fits the USB console's buffer ------------------------------------------
# Every value at its row's widest: a 45-character image name stamps the widest app row.
widest() { # <character> <count>
    printf "%${2}s" '' | tr ' ' "$1"
}
# A version a valid tag can carry, at its widest.
LONG_VERSION="999.999.999-rc99"
[ "${#LONG_VERSION}" -eq 16 ] || bad "the widest version [$LONG_VERSION] is not 16 characters"
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
for crlf in 0 1; do
"$CXX" -std=c++17 -Iinclude -Iuser/include -Ilib/include "-DKICKOS_CONSOLE_CRLF=$crlf" \
    -DKICKOS_LINKER_WEAK_UNDEF=1 "-DKICKOS_VERSION=\"$LONG_VERSION\"" \
    "-DKICKOS_BOARD_NAME=\"$LONG_BOARD\"" "$TMP/mainlong.cc" "$RENDER" \
    "$TMP/stamp_$(widest i 45).o" -o "$TMP/renderlong" 2> "$TMP/cc.err" \
    || fail "the renderer does not build at the widest rows: $(head -n 5 "$TMP/cc.err")"
"$TMP/renderlong" > "$TMP/rowslong"
long_rc=$?
long_title=$(sed -n 2p "$TMP/rowslong" | tr -d '\r')
[ "$(banner_title_version "$long_title")" = "$LONG_VERSION" ] \
    || bad "crlf $crlf: the widest title [$long_title] does not name version $LONG_VERSION"
{
    printf '\n'
    printf '%s\n' "$long_title"
    printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BOARD "$LONG_BOARD")"
    printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_BUILD "$BUILT")"
    printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_APP "$LONG_STAMP")"
    printf '%s\n' "$(identity_row "$DIAG" KDIAG_F_BANNER_COMMIT "$LONG_COMMIT")"
} | posture "$crlf" > "$TMP/wantlong"
cmp -s "$TMP/rowslong" "$TMP/wantlong" \
    || bad "every row at its widest, crlf $crlf: the image prints [$(cat "$TMP/rowslong")], the verdict renders [$(cat "$TMP/wantlong")]"
[ "$long_rc" -eq 0 ] \
    || bad "crlf $crlf: the widest rows do not fill BANNER_IDENTITY_MAX to its NUL, so the buffer is not sized from them"
done

# --- the title is centred over the rule's '=' for every version length -----------------------
{
    printf '%s\n' '#include <kickos/diag.h>' 'namespace d = kickos::diag;'
    printf '%s\n' 'constexpr bool centred(unsigned len)' '{' \
        '    unsigned const in = d::banner_title_indent_width(len);' \
        '    unsigned const text = d::kBannerTitleText + len;' \
        '    if (text >= d::kBannerRuleWidth)' '    {' '        return in == d::kBannerRuleLead;' '    }' \
        '    unsigned const left = in - d::kBannerRuleLead;' \
        '    unsigned const right = d::kBannerRuleWidth - text - left;' \
        '    return left == right or left + 1u == right;' '}' \
        'constexpr bool all_centred()' '{' '    for (unsigned n = 0; n <= 64u; n++)' '    {' \
        '        if (not centred(n))' '        {' '            return false;' '        }' '    }' \
        '    return true;' '}' 'static_assert(all_centred(), "a title is off centre");' \
        'int main() { return 0; }'
} > "$TMP/centre.cc"
"$CXX" -std=c++17 -Iinclude "$TMP/centre.cc" -o "$TMP/centre" 2> "$TMP/cc.err" \
    || bad "the banner title is not centred over its rule: $(head -n 5 "$TMP/cc.err")"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: a USB console's identity rows are kbanner's, rendered as the route's verdict renders them"
