#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the preset-to-defconfig bijection: every visible configure preset resolves to a
# defconfig, and every defconfig is reachable from a preset.
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

# A defconfig with no preset, consumed BY PATH by some gate. Each entry is <board>/<variant>
# and each must be named by a file under tests/.
FIXTURES="sim/smp-ineligible"

scratch_dir

# --- fixture consumption, and the bound that keeps one entry from covering another ---------
# The needle is the bare VARIANT and cannot be board-qualified: check_smp_predicate.sh
# composes its path out of shell variables, so `sim/configs/smp-ineligible` appears nowhere in
# the tree as a literal. A distinctive variant name is therefore part of what makes an entry
# checkable; keep the list short and its names specific.
#
# BOUNDED, never a bare substring. `-` is an ordinary character in a variant name, so a plain
# match lets any entry whose name is a piece of a LIVE one ride on the live one's mentions,
# and the dead entry then sits in the list forever with the gate green. The bound is any
# character a variant name cannot hold.
VARIANT_EDGE='[^A-Za-z0-9_-]'

fixture_consumed() { # <variant> <root>; 0 when a file under <root> besides this gate names it
    # The alphabet VARIANT_EDGE assumes. Anything else reaches the match as a pattern, where a
    # dot stands for a character the name does not hold and the entry rides on a near neighbour.
    case "$1" in
        *[!A-Za-z0-9_-]*) fail "FIXTURES names the variant '$1', which holds a character no
      board variant may: it would reach the consumption match as a regular expression" ;;
    esac
    grep -rlE "(^|$VARIANT_EDGE)$1($VARIANT_EDGE|\$)" "$2" 2>/dev/null \
        | grep -qvxF "tests/static/$(basename "$0")"
}

# The match as it was before the bound, so the control below is a near miss and not a needle
# that never matched anything. Self-test only; the legs never call it.
fixture_consumed_unbounded() { # <variant> <root>
    grep -rl -e "$1" "$2" 2>/dev/null | grep -qvxF "tests/static/$(basename "$0")"
}

# One consumer naming one variant, and a second variant whose name is a piece of the first.
mkdir -p "$TMP/st"
printf 'KOS_FIXTURE_VARIANT="kos-probe-live"\n' >"$TMP/st/consumer.sh"
fixture_consumed "kos-probe-live" "$TMP/st" \
    || fail "the consumption match does not see a variant a file under tests/ names outright,
      so every entry would read as dead and this leg would cry wolf"
if fixture_consumed "kos-probe" "$TMP/st"; then
    fail "a variant whose name is a piece of a live one reads as consumed, so a dead entry
      hides behind the live one's mentions and stays in the list forever"
fi
fixture_consumed_unbounded "kos-probe" "$TMP/st" \
    || fail "the unbounded match does not accept the alias either, so the bound above is not
      what refuses it and the control proves nothing"
# The same alias spelled with a dot, which matches the live name as a pattern and nothing as a
# literal, so the refusal above is what keeps it out and not a needle that misses.
if ( fixture_consumed "kos.probe-live" "$TMP/st" ) 2>/dev/null; then
    fail "a variant holding a regular-expression character is matched as a pattern, so a dot
      in the list stands for a character no variant name holds"
fi

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
    if grep -qxF "$key" "$TMP/fixture_keys.txt"; then
        continue
    fi
    n_orphan=$((n_orphan + 1))
    bad "defconfig '$key' is named by no configure preset. Nothing compiles it and no run" \
        "reports it missing. Give it a preset, delete it, or declare it in this gate's" \
        "FIXTURES if a test consumes it by path."
done <"$TMP/defconfig_keys.txt"

# --- leg 3: the exception list is not a hiding place -------------------------
# A fixture must BE a tracked defconfig and must be named by a file under tests/.
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
    # every entry matches its own declaration and this leg can never fire at all.
    if ! fixture_consumed "$_v" tests/; then
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
