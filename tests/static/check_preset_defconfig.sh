#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the preset-to-defconfig bijection: every visible configure preset resolves to a
# defconfig, and every defconfig is reachable from a preset. Each preset is also its own
# registration key, and the translating ones are exactly the presets
# tests/static/console_reach_roots.txt declares.
#
# THE MAPPING IS NOT NAME EQUALITY, AND IT IS NOT THE PRESET'S NAME AT ALL. A preset's
# defconfig is decided by its BOARD and its resolved KICKOS_CONFIG_VARIANT, so a preset named
# <board>-<variant> whose variant resolves to something else entirely is exactly the defect
# this gate exists to catch and a sed on the name cannot see it. The key comes from
# preset_boards.cmake's third field, the registration key it rebuilds from the RESOLVED
# variant, never from the first.
#
# An own-image AMP partition has ONE defconfig serving one preset per node, and the flattener
# appends -n<N> to that key for those presets alone. A key is therefore tried as it stands
# FIRST and the suffix stripped only as a fallback: a blind strip would refuse a legitimate
# board or variant whose own name ends in -n<digits>.
#
# Source-tree gate: reads the tree through `git ls-files` plus the preset files, builds nothing
# and configures nothing, so it registers on every board.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.
rc=0

if [ "$#" -ne 2 ]; then
    fail "usage: check_preset_defconfig.sh <cmake> <src-dir>"
fi
CMAKE="$1"
SRC="$2"

[ -x "$CMAKE" ] || fail "no cmake at $CMAKE"
[ -d "$SRC" ] || fail "no source directory at $SRC"
cd "$SRC" || fail "cannot enter $SRC"
require_repo_root "$SRC is not the repo root"

FLATTEN="tests/static/preset_boards.cmake"
[ -f "$FLATTEN" ] || fail "no preset flattener at $SRC/$FLATTEN"

scratch_dir

# --- the two sets ------------------------------------------------------------
"$CMAKE" -DSRC="$SRC" -DOUT="$TMP/presets.tsv" -P "$FLATTEN" >"$TMP/flatten.log" 2>&1 \
    || fail "$FLATTEN refused: $(tail -1 "$TMP/flatten.log")"

cut -f1 "$TMP/presets.tsv" | sort -u >"$TMP/preset_names.txt"
require_nonempty "$TMP/preset_names.txt" "$FLATTEN yielded no configure preset at all"

# Field 3, the RESOLVED registration key, not field 1. A preset whose KICKOS_CONFIG_VARIANT
# names a directory that does not exist differs from its own name here and nowhere else.
cut -f3 "$TMP/presets.tsv" | sort -u >"$TMP/preset_keys.txt"
require_nonempty "$TMP/preset_keys.txt" "$FLATTEN yielded no registration key"

corpus "$TMP/defconfigs.txt" "board defconfig" 'boards/*/configs/*/defconfig'
sed -E 's|boards/([^/]+)/configs/([^/]+)/defconfig|\1 \2|' "$TMP/defconfigs.txt" \
    | awk '{ if ($2 == "base") { print $1 } else { print $1 "-" $2 } }' \
    | sort -u >"$TMP/defconfig_keys.txt"
require_nonempty "$TMP/defconfig_keys.txt" "no defconfig resolved to a board and variant"

# --- leg 1: every preset resolves to a defconfig -----------------------------
n_missing=0
: >"$TMP/matched_keys.txt"
while IFS= read -r key; do
    if grep -qxF "$key" "$TMP/defconfig_keys.txt"; then
        echo "$key" >>"$TMP/matched_keys.txt"
        continue
    fi
    # The AMP fallback, tried only after the key itself has failed: a board or variant whose
    # own name ends in -n<digits> must never be mistaken for a node index.
    _stripped="$(printf '%s\n' "$key" | sed -E 's/-n[0-9]+$//')"
    if [ "$_stripped" != "$key" ] && grep -qxF "$_stripped" "$TMP/defconfig_keys.txt"; then
        echo "$_stripped" >>"$TMP/matched_keys.txt"
        continue
    fi
    n_missing=$((n_missing + 1))
    bad "configure preset key '$key' names no defconfig. The key is the board plus the" \
        "RESOLVED KICKOS_CONFIG_VARIANT, so it resolves to" \
        "boards/<board>/configs/<variant>/defconfig and this one is a configure failure" \
        "waiting for somebody to select it."
done <"$TMP/preset_keys.txt"
sort -u "$TMP/matched_keys.txt" -o "$TMP/matched_keys.txt"

# --- leg 2: every defconfig is reachable from a preset -----------------------
n_orphan=0
while IFS= read -r key; do
    # Against what leg 1 actually MATCHED, not against the raw keys: a preset naming a ghost
    # variant must not keep its real defconfig reachable.
    if grep -qxF "$key" "$TMP/matched_keys.txt"; then
        continue
    fi
    n_orphan=$((n_orphan + 1))
    bad "defconfig '$key' is named by no configure preset. Nothing compiles it and no run" \
        "reports it missing. Give it a preset or delete it."
done <"$TMP/defconfig_keys.txt"

# --- leg 3: every preset is its own registration key -------------------------
# trap_redzone rebuilds `cmake --preset <key>`, so a preset keyed otherwise is measured as the
# preset its key names, or as none.
awk -F'\t' '$3 != $1 { print $1, $3 }' "$TMP/presets.tsv" >"$TMP/rekeyed.txt"
while read -r _name _key; do
    bad "configure preset '$_name' registers its per-preset gates under '$_key', so" \
        "trap_redzone measures the preset named '$_key' in its place, or none."
done <"$TMP/rekeyed.txt"

# --- leg 4: the translating presets are console_reach's preset records -------
# A translating arch is one console_reach_roots.txt declares; a preset is of the arch its board
# states.
REACH=tests/static/console_reach_roots.txt
awk '$1 == "preset" { print $2 }' "$REACH" | sort -u >"$TMP/reach_arches.txt"
require_nonempty "$TMP/reach_arches.txt" "$REACH declares no preset"
awk '$1 == "preset" { print $3 }' "$REACH" | sort >"$TMP/reach_presets.txt"
: >"$TMP/translating.txt"
while IFS="$TAB" read -r _name _board _key; do
    _arch="$(sed -n 's/^set(KICKOS_ARCH  *"\([^"]*\)").*/\1/p' "boards/$_board/board.cmake" \
        2>/dev/null)"
    grep -qxF "$_arch" "$TMP/reach_arches.txt" && echo "$_name" >>"$TMP/translating.txt"
done <"$TMP/presets.tsv"
sort "$TMP/translating.txt" | comm -3 - "$TMP/reach_presets.txt" >"$TMP/reach_diff.txt"
while IFS= read -r _line; do
    case "$_line" in
        "$TAB"*) bad "$REACH declares preset ${_line#"$TAB"}, which is no configure preset of" \
                     "an arch it declares, so that record is dead." ;;
        *) bad "translating preset $_line has no preset and floor record in $REACH, so" \
               "its console_reach clause dies where it runs and nothing says so elsewhere." ;;
    esac
done <"$TMP/reach_diff.txt"

n_names=$(wc -l <"$TMP/preset_names.txt")
n_keys=$(wc -l <"$TMP/preset_keys.txt")
n_defconfigs=$(wc -l <"$TMP/defconfig_keys.txt")
echo "== checked $n_names configure preset(s) over $n_keys key(s) against $n_defconfigs" \
     "defconfig(s) =="

if [ "$rc" -ne 0 ]; then
    fail "$n_missing preset(s) with no defconfig, $n_orphan defconfig(s) with no preset," \
         "$(wc -l <"$TMP/rekeyed.txt") preset(s) keyed as another," \
         "$(wc -l <"$TMP/reach_diff.txt") console_reach preset record(s) out of step."
fi

echo "PASS: every configure preset resolves to a defconfig and is its own key, every defconfig is" \
     "reachable, and console_reach declares exactly the translating presets"
