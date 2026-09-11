#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the preset-to-defconfig bijection: every visible configure preset resolves to a
# defconfig, and every defconfig is reachable from a preset.
#
# Neither direction was checked anywhere. tests/static/preset_boards.cmake TOLERATES a missing
# defconfig, and check_kconfig_gen.sh walks defconfigs and never reads presets, so a preset
# naming a variant directory that does not exist fails only when somebody configures it, and a
# defconfig nothing selects is compiled by nothing and reported by nothing.
#
# THE MAPPING IS NOT NAME EQUALITY, AND IT IS NOT THE PRESET'S NAME AT ALL. A preset's
# defconfig is decided by its BOARD and its resolved KICKOS_CONFIG_VARIANT, so a preset named
# <board>-<variant> whose variant resolves to something else entirely is exactly the defect
# this gate exists to catch and a sed on the name cannot see it. The key therefore comes from
# preset_boards.cmake's third field, the registration key it rebuilds from the RESOLVED
# variant, never from the first.
#
# An own-image AMP partition has ONE defconfig serving one preset per node, and the flattener
# appends -n<N> to that key for those presets alone. So a key is tried as it stands FIRST and
# the suffix is stripped only as a fallback, which a blind strip gets wrong twice: it would
# refuse a legitimate board or variant whose own name ends in -n<digits>.
#
# THE EXCEPTION LIST IS ITSELF CHECKED, or it would be a place to hide a real gap: a variant
# declared here as consumed-by-path must be named by something under tests/ OTHER THAN THIS
# FILE, and it is named as a path rather than as a bare word. Grepping for the bare variant
# from a gate that lists it in its own source matches ITSELF, so every entry would prove its
# own consumption and the leg could never fire.
#
# Source-tree gate: reads the tree through `git ls-files` plus the preset files, builds nothing
# and configures nothing, so it registers on every board.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off. bad() sets rc.
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

# A defconfig with no preset, because a gate consumes it BY PATH rather than by configuring it.
# Each entry is <board>/<variant> and each must be named by a file under tests/.
FIXTURES="sim/smp-ineligible"

scratch_dir

# --- the two sets ------------------------------------------------------------
# Through the tree's own preset reader, so a preset file it cannot parse refuses here rather
# than silently shrinking the set being compared.
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

# The exception list in the same key shape as the two sets above.
: >"$TMP/fixture_keys.txt"
for _f in $FIXTURES; do
    _b="${_f%%/*}"
    _v="${_f#*/}"
    if [ "$_v" = "base" ]; then
        echo "$_b" >>"$TMP/fixture_keys.txt"
    else
        echo "$_b-$_v" >>"$TMP/fixture_keys.txt"
    fi
done
sort -u "$TMP/fixture_keys.txt" -o "$TMP/fixture_keys.txt"

# --- leg 1: every preset resolves to a defconfig -----------------------------
n_missing=0
: >"$TMP/matched_keys.txt"
while IFS= read -r key; do
    if grep -qxF "$key" "$TMP/defconfig_keys.txt"; then
        echo "$key" >>"$TMP/matched_keys.txt"
        continue
    fi
    # The AMP fallback, tried only after the key itself has failed, so a board or variant whose
    # own name ends in -n<digits> is never mistaken for a node index.
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
    if grep -qxF "$key" "$TMP/fixture_keys.txt"; then
        continue
    fi
    n_orphan=$((n_orphan + 1))
    bad "defconfig '$key' is named by no configure preset. Nothing compiles it and no run" \
        "reports it missing. Give it a preset, delete it, or declare it in this gate's" \
        "FIXTURES if a test consumes it by path."
done <"$TMP/defconfig_keys.txt"

# --- leg 3: the exception list is not a hiding place -------------------------
# A fixture must BE a tracked defconfig and must be named by a file under tests/. Without this
# leg the list would launder exactly the defect leg 2 exists to find.
n_dead=0
for _f in $FIXTURES; do
    _b="${_f%%/*}"
    _v="${_f#*/}"
    if ! grep -qxF "boards/$_b/configs/$_v/defconfig" "$TMP/defconfigs.txt"; then
        n_dead=$((n_dead + 1))
        bad "FIXTURES names '$_f' and boards/$_b/configs/$_v/defconfig is not tracked."
        continue
    fi
    # EXCLUDING THIS FILE, which lists the entry two dozen lines up: without that exclusion
    # every entry matches its own declaration, every entry proves its own consumption, and
    # this leg can never fire at all.
    #
    # The needle is the bare VARIANT and cannot be board-qualified: check_smp_predicate.sh
    # composes its path out of shell variables, so `sim/configs/smp-ineligible` appears
    # nowhere in the tree as a literal. A distinctive variant name is therefore part of what
    # makes an entry checkable, and a short one (`st`, `flat`) would satisfy this on an
    # unrelated hit. Keep the list short and its names specific.
    if ! grep -rl -e "$_v" tests/ 2>/dev/null \
         | grep -qvxF "tests/static/$(basename "$0")"; then
        n_dead=$((n_dead + 1))
        bad "FIXTURES names '$_f' and nothing under tests/ besides this gate names the" \
            "variant '$_v'. A declared exception nothing consumes is an orphan defconfig" \
            "wearing a licence."
    fi
done

n_names=$(wc -l <"$TMP/preset_names.txt")
n_keys=$(wc -l <"$TMP/preset_keys.txt")
n_defconfigs=$(wc -l <"$TMP/defconfig_keys.txt")
n_fixtures=$(wc -l <"$TMP/fixture_keys.txt")
echo "== checked $n_names configure preset(s) over $n_keys key(s) against $n_defconfigs" \
     "defconfig(s), $n_fixtures declared fixture(s) =="

if [ "$rc" -ne 0 ]; then
    fail "$n_missing preset(s) with no defconfig, $n_orphan defconfig(s) with no preset," \
         "$n_dead dead fixture declaration(s)."
fi

echo "PASS: every configure preset resolves to a defconfig and every defconfig is reachable"
