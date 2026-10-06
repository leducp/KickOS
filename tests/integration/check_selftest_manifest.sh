#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every arm a row of kickos-selftest-manifest.txt permits to skip or to report partial is an arm
# that row's image registers. A split suite spreads its arms over several images, and a name
# left on the wrong image's row permits nothing there while the image that does run the arm is
# judged without it. Which arms an image carries is read off the LINKED image: each registration
# a region compiles leaves an absolute symbol `kickos_tap_arm.<name>` (TAP_REGISTER,
# user/apps/common/selftest/selftest.h). A name no image of the build registers permits nothing
# anywhere, so it is refused as stale.
#
#   check_selftest_manifest.sh <manifest> <image list> <nm>
#   check_selftest_manifest.sh --self-test
#
# The image list holds one `<image>|<linked file>` row per selftest image.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# <image list> <nm> <out>: `<image> <arm>` per arm each image registers. Fails on an image whose
# symbol table names no arm, which is a registration this read no longer sees.
image_arms() {
    : > "$3"
    while IFS='|' read -r _ia_img _ia_file; do
        [ -n "$_ia_img" ] || continue
        [ -f "$_ia_file" ] || fail "no linked image at $_ia_file for $_ia_img"
        "$2" "$_ia_file" > "$TMP/nm.out" 2> "$TMP/nm.err" \
            || fail "$2 could not read $_ia_file: $(head -n 1 "$TMP/nm.err")"
        sed -n 's/^.* kickos_tap_arm\.\([A-Za-z0-9_]*\)[[:space:]]*$/\1/p' "$TMP/nm.out" \
            | sort -u | sed "s/^/$_ia_img /" > "$TMP/one"
        [ -s "$TMP/one" ] || fail "$_ia_img ($_ia_file) carries no kickos_tap_arm symbol, so
  which arms it registers cannot be read"
        cat "$TMP/one" >> "$3"
    done < "$1"
    [ -s "$3" ] || fail "$1 names no selftest image"
}

# <manifest> <arms>: every skip and partial name of each row against the arms of that row's
# image. Sets rc.
judge() {
    while IFS='|' read -r _j_img _j_plan _j_skips _j_partials _j_faults; do
        [ -n "$_j_img" ] || continue
        if ! grep -q "^$_j_img " "$2"; then
            bad "the manifest has a row for $_j_img, which is not a linked selftest image"
            continue
        fi
        for _j_kind in skip partial; do
            _j_set="$_j_skips"
            if [ "$_j_kind" = partial ]; then
                _j_set="$_j_partials"
            fi
            for _j_name in $(printf '%s\n' "$_j_set" | tr ',' ' '); do
                if grep -qx "$_j_img $_j_name" "$2"; then
                    continue
                fi
                _j_where="$(awk -v n="$_j_name" '$2 == n { print $1 }' "$2" | tr '\n' ' ')"
                if [ -n "$_j_where" ]; then
                    bad "$_j_name runs in ${_j_where% }, not in $_j_img, whose row permits it to
  $_j_kind: the image running it is judged without the permission and this row permits nothing"
                else
                    bad "$_j_img's row names $_j_name ($_j_kind), which no image of this build registers"
                fi
            done
        done
    done < "$1"
}

if [ "${1:-}" = --self-test ]; then
    scratch_dir
    # A stand-in nm: prints the planted table named after the file it is given.
    cat > "$TMP/fake-nm" <<'EOF'
#!/bin/sh
cat "$1.nm"
EOF
    chmod +x "$TMP/fake-nm"
    printf '%s\n' '00000001 a kickos_tap_arm.alpha' '00000001 a kickos_tap_arm.beta' \
        '08000100 T selftest_main' '00000001 a kickos_tap_arm_lookalike' > "$TMP/p1.nm"
    printf '%s\n' '00000001 a kickos_tap_arm.gamma' '00000001 a kickos_tap_arm.delta' \
        > "$TMP/p2.nm"
    printf '%s\n' '08000100 T selftest_main' > "$TMP/none.nm"
    : > "$TMP/p1"
    : > "$TMP/p2"
    : > "$TMP/none"
    printf '%s\n' "p1|$TMP/p1" "p2|$TMP/p2" > "$TMP/images"
    # <name> <manifest rows>...: the verdict on those rows against p1 and p2.
    verdict() {
        _v_name="$1"
        shift
        printf '%s\n' "$@" > "$TMP/$_v_name.manifest"
        sh "$0" "$TMP/$_v_name.manifest" "$TMP/images" "$TMP/fake-nm" > "$TMP/$_v_name.out" 2>&1
    }
    verdict good 'p1|2|alpha|beta|' 'p2|2||delta|kw'
    [ "$?" -eq 0 ] || fail "a manifest naming each arm on its own image's row is refused:
  $(cat "$TMP/good.out")"
    verdict moved 'p1|2|alpha||' 'p2|2|beta||'
    [ "$?" -ne 0 ] || fail "a row permitting an arm another image runs passes"
    grep -q 'beta runs in p1, not in p2' "$TMP/moved.out" \
        || fail "the refusal of a moved skip does not name the image the arm runs in:
  $(cat "$TMP/moved.out")"
    verdict movedpartial 'p1|2||gamma|' 'p2|2|||'
    [ "$?" -ne 0 ] || fail "a row permitting a partial another image runs passes"
    verdict absent 'p1|2|zeta||' 'p2|2|||'
    [ "$?" -ne 0 ] || fail "a name no image registers passes"
    grep -q "p1's row names zeta" "$TMP/absent.out" \
        || fail "a name no image registers is refused for another reason: $(cat "$TMP/absent.out")"
    verdict lookalike 'p1|2|lookalike||' 'p2|2|||'
    grep -q "p1's row names lookalike" "$TMP/lookalike.out" \
        || fail "a symbol that is not a registration was read as one: $(cat "$TMP/lookalike.out")"
    verdict stray 'p3|2|||'
    [ "$?" -ne 0 ] || fail "a row for an image the list does not carry passes"
    printf '%s\n' "p1|$TMP/p1" "none|$TMP/none" > "$TMP/images"
    verdict blind 'p1|2|||'
    [ "$?" -ne 0 ] || fail "an image carrying no registration symbol passes"
    grep -q 'carries no kickos_tap_arm symbol' "$TMP/blind.out" \
        || fail "an image carrying no registration symbol is refused for another reason:
  $(cat "$TMP/blind.out")"
    echo "PASS: a skip or partial on the wrong image's row is refused naming the image that runs"
    echo "  the arm, a name no image registers is refused, and an image whose registrations"
    echo "  cannot be read is refused"
    exit 0
fi

[ "$#" -eq 3 ] || fail "usage: check_selftest_manifest.sh <manifest> <image list> <nm>"
[ -f "$1" ] || fail "no manifest at $1"
[ -f "$2" ] || fail "no image list at $2"
scratch_dir
image_arms "$2" "$3" "$TMP/arms"
rc=0
judge "$1" "$TMP/arms"
if [ "$rc" -ne 0 ]; then
    exit "$rc"
fi
echo "PASS: every skip and partial of $(grep -c . "$1") manifest row(s) names an arm its own image registers"
