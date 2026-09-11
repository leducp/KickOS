#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tests/integration/oot_arch_boards.txt says which board carries the out-of-tree package gate
# for each KICKOS_ARCH. It is a LIST, and a list is only worth what holds it whole: an arch
# that gains a board and no row here gets no gate, and nothing else in the tree would say so.
#
# What this gate asserts about that file:
#   - every KICKOS_ARCH named by a tracked boards/*/board.cmake has exactly one row;
#   - no row names an arch no board states;
#   - a `covers` board is tracked, states that arch, and has a configure preset;
#   - a `declines` row carries a reason;
#   - no tab anywhere, because the CMake side matches a literal space (CMake's regex has no
#     tab escape and [ \t] there means "space or the letter t").
#
# Run from the repo root, no arguments: tests/static/check_oot_arch_cover.sh

set -u
. "$(dirname "$0")/../lib/gate.sh"

scratch_dir

MAP=tests/integration/oot_arch_boards.txt
[ -f "$MAP" ] || fail "$MAP is missing; the out-of-tree package gate registers on no board"

# --- the arches the tree actually has ---------------------------------------
# git ls-files, not a glob: an untracked board.cmake is not in the build either, and a glob
# would also pick a stale copy out of a build directory.
corpus "$TMP/descriptors" "board descriptor" 'boards/*/board.cmake'
require_nonempty "$TMP/descriptors" "no tracked boards/*/board.cmake, so every check below
      would pass over an empty fleet"
BOARDS="$(wc -l < "$TMP/descriptors" | tr -d ' ')"

board_arch() { # <descriptor path> -> the KICKOS_ARCH it states
    sed -n 's/^set(KICKOS_ARCH *"\{0,1\}\([A-Za-z0-9_]*\)"\{0,1\}).*/\1/p' "$1" | head -1
}

: > "$TMP/tree_arches"
while IFS= read -r d; do
    a="$(board_arch "$d")"
    [ -n "$a" ] || fail "$d states no KICKOS_ARCH, so the arch it belongs to cannot be derived"
    printf '%s\n' "$a" >> "$TMP/tree_arches"
done < "$TMP/descriptors"
sort -u "$TMP/tree_arches" > "$TMP/tree_arches.u"
ARCHES="$(wc -l < "$TMP/tree_arches.u" | tr -d ' ')"

# --- the presets, so a covered board is one CI can configure ----------------
corpus "$TMP/presetfiles" "preset file" 'CMakePresets.json' 'cmake/presets/*.json'
require_nonempty "$TMP/presetfiles" "no tracked preset file, so the preset clause below would
      pass vacuously"
: > "$TMP/presetnames"
while IFS= read -r f; do
    sed -n 's/.*"name"[[:space:]]*:[[:space:]]*"\([A-Za-z0-9_-]*\)".*/\1/p' "$f" \
        >> "$TMP/presetnames"
done < "$TMP/presetfiles"
sort -u "$TMP/presetnames" > "$TMP/presetnames.u"
require_nonempty "$TMP/presetnames.u" "no preset name parsed out of the preset files, so the
      preset clause below would pass vacuously"

# --- the map, read the way the CMake side reads it --------------------------
# One verdict per line so a planted control can be read against a map it does not live at.
map_findings() { # <map file> <arch list> <preset list> -> one finding per line
    _m="$1"
    _arches="$2"
    _presets="$3"
    if grep -q "$TAB" "$_m"; then
        printf 'tab in the map, which the CMake side cannot match\n'
    fi
    : > "$TMP/map_arches"
    while IFS= read -r line; do
        case "$line" in
            ''|'#'*) continue ;;
        esac
        _verb="${line%% *}"
        _rest="${line#* }"
        # Leading spaces of the column alignment, dropped so $1 below is the arch.
        while :; do
            case "$_rest" in
                ' '*) _rest="${_rest# }" ;;
                *) break ;;
            esac
        done
        _arch="${_rest%% *}"
        _tail="${_rest#* }"
        case "$_verb" in
            covers)
                while :; do
                    case "$_tail" in
                        ' '*) _tail="${_tail# }" ;;
                        *) break ;;
                    esac
                done
                _board="${_tail%% *}"
                if [ -z "$_board" ]; then
                    printf 'covers %s names no board\n' "$_arch"
                elif ! grep -Fxq "boards/$_board/board.cmake" "$TMP/descriptors"; then
                    printf 'covers %s names board %s, which is not tracked\n' "$_arch" "$_board"
                elif [ "$(board_arch "boards/$_board/board.cmake")" != "$_arch" ]; then
                    printf 'covers %s names board %s, whose descriptor states %s\n' \
                        "$_arch" "$_board" "$(board_arch "boards/$_board/board.cmake")"
                elif ! grep -Fxq "$_board" "$_presets"; then
                    printf 'covers %s names board %s, which has no configure preset\n' \
                        "$_arch" "$_board"
                fi
                ;;
            declines)
                if [ "$_tail" = "$_rest" ] || [ -z "$_tail" ]; then
                    printf 'declines %s carries no reason\n' "$_arch"
                fi
                ;;
            *)
                printf 'unknown verb %s\n' "$_verb"
                continue
                ;;
        esac
        printf '%s\n' "$_arch" >> "$TMP/map_arches"
    done < "$_m"

    sort "$TMP/map_arches" > "$TMP/map_arches.s"
    sort -u "$TMP/map_arches" > "$TMP/map_arches.u"
    if [ "$(wc -l < "$TMP/map_arches.s")" != "$(wc -l < "$TMP/map_arches.u")" ]; then
        printf 'an arch has more than one row: %s\n' \
            "$(uniq -d "$TMP/map_arches.s" | tr '\n' ' ')"
    fi
    comm -23 "$_arches" "$TMP/map_arches.u" | while IFS= read -r a; do
        printf 'arch %s is stated by a board and has no row\n' "$a"
    done
    comm -13 "$_arches" "$TMP/map_arches.u" | while IFS= read -r a; do
        printf 'the map names arch %s, which no board states\n' "$a"
    done
}

# --- the instrument, before the verdict -------------------------------------
# Each control is a MINIMAL PAIR against the real map: one property changed, and the finding
# it must produce named exactly. A control that produces the RIGHT COUNT of the WRONG
# findings would pass a bare count, so each is matched on its own text.
assert_finding() { # <tag> <expected finding substring> <findings file>
    grep -q "$2" "$3" || {
        sed 's/^/      /' "$3" >&2
        fail "the $1 control did not produce a finding matching /$2/, so this gate is blind
      to it and the map it passes is unproven"
    }
}

control() { # <tag> <expected finding substring> <sed program applied to the map>
    sed "$3" "$MAP" > "$TMP/ctl.txt"
    map_findings "$TMP/ctl.txt" "$TMP/tree_arches.u" "$TMP/presetnames.u" > "$TMP/ctl.out"
    assert_finding "$1" "$2" "$TMP/ctl.out"
}
control missing-row   'has no row'                 '/^covers  *armv7m /d'
control wrong-arch    'whose descriptor states'    's|^covers  *armv7m  *qemu$|covers   armv7m   rx72m|'
control unknown-board 'is not tracked'             's|^covers  *armv7m  *qemu$|covers   armv7m   nosuchboard|'
control phantom-arch  'which no board states'      's|^covers  *armv7m  *qemu$|covers   armv9z   qemu|'
# NO ARCH DECLINES TODAY, so both declines arms are proven on a PLANTED row and neither on a
# real one. The reason-less half below; the well-formed half is the block after the controls,
# because a clause that rejected EVERY decline would otherwise sit unnoticed until an arch
# needed one, and no minimal pair whose expected finding is a NAMED string can say that.
control no-reason     'carries no reason'          's|^covers  *armv7m  *qemu$|declines armv7m|'
control unknown-verb  'unknown verb'               's|^covers  *armv7m |mentions armv7m |'
control duplicate-row 'more than one row'          's|^covers  *armv7m  *qemu$|covers   armv7m   qemu\ncovers   armv7m   qemu-m3|'
control tab-separated 'tab in the map'             "s|^covers  *armv7m  *qemu\$|covers${TAB}armv7m${TAB}qemu|"

# The other half of the declines verb: a well-formed row must produce NO finding. Planted over
# armv7m's row so the arch still has exactly one, which keeps every other clause silent and
# makes an empty findings file the whole assertion.
# DIFFERENTIAL and not "no finding at all": the map's own findings are whatever they are, and
# an emptiness test here would report any other defect in it under this control's name.
map_findings "$MAP" "$TMP/tree_arches.u" "$TMP/presetnames.u" > "$TMP/ctl.base"
sed 's|^covers  *armv7m  *qemu$|declines armv7m no package can be consumed on it yet|' "$MAP" \
    > "$TMP/ctl.txt"
map_findings "$TMP/ctl.txt" "$TMP/tree_arches.u" "$TMP/presetnames.u" > "$TMP/ctl.out"
if ! diff "$TMP/ctl.base" "$TMP/ctl.out" > "$TMP/ctl.diff"; then
    sed 's/^/      /' "$TMP/ctl.diff" >&2
    fail "a well-formed declines row changed this gate's findings, so it would refuse an arch
      no package can be consumed on yet"
fi

# The preset clause moves the OTHER input: every board in the map has a preset today, so a
# board without one cannot be planted in the map without also tripping the not-tracked
# clause and proving that one instead.
grep -vFx qemu "$TMP/presetnames.u" > "$TMP/presets_noqemu"
[ "$(wc -l < "$TMP/presets_noqemu")" -lt "$(wc -l < "$TMP/presetnames.u")" ] \
    || fail "qemu is not among the parsed preset names, so the preset clause is being proven
      against a list that never held it"
map_findings "$MAP" "$TMP/tree_arches.u" "$TMP/presets_noqemu" > "$TMP/ctl.out"
assert_finding no-preset 'has no configure preset' "$TMP/ctl.out"

# --- the verdict ------------------------------------------------------------
map_findings "$MAP" "$TMP/tree_arches.u" "$TMP/presetnames.u" > "$TMP/findings"
if [ -s "$TMP/findings" ]; then
    sed 's/^/      /' "$TMP/findings" >&2
    fail "$MAP does not cover the fleet. Every arch a board states needs one row: covers,
      with the board whose preset runs the out-of-tree package gate, or declines, with the
      reason no package can be consumed on it yet."
fi

COVERS="$(grep -c '^covers ' "$MAP")"
DECLINES="$(grep -c '^declines ' "$MAP")"
echo "PASS: $ARCHES arch(es) over $BOARDS tracked board descriptor(s), each with one row in"
echo "      $MAP: $COVERS covered, $DECLINES declined"
