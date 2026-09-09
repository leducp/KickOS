#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the rule that carries a knob BOTH ways between Kconfig and CMake.
#
# Outbound: every KICKOS_* symbol the tree declares reaches <gen>/kickos_config.cmake, typed
# from its Kconfig type. A symbol that resolves in .config and stops there is silent: CMake
# reads the name as empty, an `if()` on it is false, and where the value feeds a preprocessed
# linker script the symptom is a syntax error inside GENERATED output naming neither the symbol
# nor the omission. The expected line is DERIVED here from kconfiglib's own type and value, in a
# mapping independent of the generator's, so the two must disagree for this leg to fail.
#
# Inbound: a CMake variable becomes a request from the PROMPTED set and the symbol type, so no
# list decides membership. What this leg pins is which candidates are refused BY NAME and which
# are dropped: an asserted name naming a promptless symbol or a choice member is refused, and
# one naming NO symbol is dropped, the KICKOS_ namespace also carrying build-graph switches and
# per-board link knobs that Kconfig does not declare. That last is the residual hole and it is
# asserted so it cannot widen in silence.

set -u

. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 2 ]; then
    fail "usage: check_kconfig_reach.sh <python> <srcdir>"
fi
PY="$1"
SRC="$(cd "$2" && pwd)" || fail "no source tree at $2"
GEN="$SRC/tools/kconfig/genconfig.py"

[ -x "$PY" ] || fail "no python interpreter at $PY"
[ -f "$SRC/Kconfig" ] || fail "no Kconfig at $SRC/Kconfig"
[ -f "$GEN" ] || fail "no generator at $GEN"

export LC_ALL=C
scratch_dir

# The line every declared KICKOS_* symbol must carry in the fragment, one per line as
# "<name>\t<set(...) line>". Reads the RESOLVED .config the generator just wrote, so the
# expectation is this board's and not the fleet defaults.
cat > "$TMP/want.py" <<'PYEOF'
import os
import sys

try:
    import kconfiglib
except ImportError as exc:
    sys.stderr.write("kconfiglib is not importable: %s\n" % exc)
    raise SystemExit(2)

os.chdir(sys.argv[1])
kconf = kconfiglib.Kconfig("Kconfig", warn=False)
kconf.load_config(sys.argv[2])

# CMake's own quoting needs, and nothing else: a quoted set() argument still expands
# ${...}, still ends on an unescaped quote and still splits on a semicolon.
def quoted(text):
    for raw in ("\\", '"', "$", ";"):
        text = text.replace(raw, "\\" + raw)
    return '"' + text + '"'

for sym in kconf.unique_defined_syms:
    if not sym.name.startswith("KICKOS_"):
        continue
    got = sym.str_value
    if sym.type == kconfiglib.STRING:
        shown = quoted(got)
    elif sym.type in (kconfiglib.INT, kconfiglib.HEX):
        # Unmet dependencies leave it empty, and CMake has no #ifndef fallback for that.
        if got == "":
            shown = "0"
        else:
            shown = str(int(got, 0))
    elif got == "y":
        shown = "ON"
    else:
        shown = "OFF"
    sys.stdout.write("%s\tset(%s %s)\n" % (sym.name, sym.name, shown))
PYEOF

gen() { # <defconfig> <gendir> [request...]
    _dc="$1"
    _out="$2"
    shift 2
    "$PY" "$GEN" "$SRC" "$_dc" "$_out" "$@" >"$_out.log" 2>"$_out.err"
}

# --- Leg 1: every declared symbol reaches the fragment, on every defconfig -----------------
# Every variant in the tree, because which symbols a board OFFERS is what its own selects
# decide: a symbol reachable on one board only is exercised by that board's defconfig alone.
mkdir -p "$TMP/all" || fail "cannot create $TMP/all"
boards=0
checked=0
for dc in "$SRC"/boards/*/configs/*/defconfig; do
    [ -f "$dc" ] || continue
    boards=$((boards + 1))
    out="$TMP/all/$boards"
    gen "$dc" "$out" || fail "$dc was refused: $(cat "$out.err")"
    frag="$out/kickos_config.cmake"
    [ -s "$frag" ] || fail "$dc generated no $frag"
    # The separator spelled through $TAB, not as an invisible literal an editor can eat.
    tool_out "$out.want" "^KICKOS_BOARD${TAB}set\(KICKOS_BOARD \"" \
             "$PY" "$TMP/want.py" "$SRC" "$out/.config"
    missing=""
    while IFS="$TAB" read -r name want; do
        checked=$((checked + 1))
        if ! grep -Fxq "$want" "$frag"; then
            have="$(grep "^set($name " "$frag" || echo '(absent)')"
            missing="$missing
        $name: want '$want', fragment has '$have'"
        fi
    done < "$out.want"
    if [ -n "$missing" ]; then
        echo "FAIL: $dc resolves KICKOS_* symbol(s) that do not reach CMake as declared:$missing" >&2
        echo "      Every KICKOS_* symbol is emitted into kickos_config.cmake by rule in" >&2
        echo "      tools/kconfig/genconfig.py (write_cmake_fragment). A name CMake never sees" >&2
        echo "      reads as EMPTY there, so an if() on it is false and a value fed to a" >&2
        echo "      preprocessed linker script collapses to '(() + 0 * ())'." >&2
        exit 1
    fi
done
[ "$boards" -gt 0 ] || fail "no defconfig under $SRC/boards/*/configs/, so leg 1 read nothing"
[ "$checked" -gt 0 ] || fail "no KICKOS_* symbol was compared, so leg 1 passed over no work"

# --- Leg 2: the request syntax is right for EVERY prompted symbol -------------------------
# One run offering every prompted non-choice symbol its OWN resolved value. A type derived
# wrongly makes the request malformed (an int offered as `y`, a string offered bare), and the
# generator's read-back then refuses it, so this leg fails on the syntax and not on the value.
# Symbols left empty by an unmet dependency are out: offering an empty value asks for what the
# board does not offer.
FIX="$SRC/boards/xmc4800-relax/configs/base/defconfig"
[ -f "$FIX" ] || fail "no defconfig at $FIX"
gen "$FIX" "$TMP/fix" || fail "the fixture was refused: $(cat "$TMP/fix.err")"

cat > "$TMP/offers.py" <<'PYEOF'
import os
import sys

import kconfiglib

os.chdir(sys.argv[1])
kconf = kconfiglib.Kconfig("Kconfig", warn=False)
kconf.load_config(sys.argv[2])

BOOLS = (kconfiglib.BOOL, kconfiglib.TRISTATE)
for sym in kconf.unique_defined_syms:
    if not sym.name.startswith("KICKOS_"):
        continue
    if sym.choice is not None:
        continue
    if not any(node.prompt for node in sym.nodes):
        continue
    if sym.str_value == "":
        continue
    value = sym.str_value
    if sym.type in BOOLS:
        # What CMake would hold for it, so the false-constant translation is exercised.
        value = "OFF"
        if sym.str_value == "y":
            value = "ON"
    sys.stdout.write("offer:%s=%s\n" % (sym.name, value))
PYEOF

tool_out "$TMP/offers" '^offer:KICKOS_MAX_THREADS=' \
         "$PY" "$TMP/offers.py" "$SRC" "$TMP/fix/.config"
offers=""
while IFS= read -r line; do
    [ -n "$line" ] || continue
    offers="$offers $line"
done < "$TMP/offers"
offer_n="$(wc -l < "$TMP/offers" | tr -d ' ')"
[ "$offer_n" -gt 10 ] \
    || fail "only $offer_n prompted symbol(s) offered, so leg 2 covers almost nothing"
# Unquoted on purpose: each offer is one word and the list is built above.
# shellcheck disable=SC2086
gen "$FIX" "$TMP/off" $offers \
    || fail "offering every prompted symbol its own resolved value was refused, so a request
    syntax does not match its declared type: $(cat "$TMP/off.err")"

# --- Leg 3: which candidates are refused BY NAME, and which are dropped -------------------
refuse() { # <candidate> <expected reason ERE>
    if gen "$FIX" "$TMP/no" "$1"; then
        fail "'$1' was accepted; expected a refusal by name"
    fi
    grep -q "REFUSED" "$TMP/no.err" \
        || fail "'$1' failed without a refusal: $(cat "$TMP/no.err")"
    grep -qE "$2" "$TMP/no.err" \
        || fail "'$1' refused for the wrong reason: $(cat "$TMP/no.err")"
}
# A promptless symbol is DERIVED, and the four identity strings are promptless, which is what
# keeps a -D from moving board, arch, family or chip behind kickos_kconfig_agree()'s back.
refuse "assert:KICKOS_DOORBELL_CORES=1" "REFUSED -DKICKOS_DOORBELL_CORES=1: no prompt"
refuse "assert:KICKOS_CHIP=nosuchchip" "REFUSED -DKICKOS_CHIP=nosuchchip: no prompt"
# A choice member is selected by a variant defconfig; no -D of its own name reaches one.
refuse "assert:KICKOS_CORES_FOUR=ON" "REFUSED -DKICKOS_CORES_FOUR=ON: a member of a choice"
# THE RESIDUAL, asserted so it cannot widen unnoticed: a name Kconfig does not declare at all
# is DROPPED, because KICKOS_BUILD_TESTS, KICKOS_SMP_TRACE and KICKOS_APPDATA_SIZE live in that
# same namespace and are declared in CMake alone. CMake's own unused-variable notice is what
# still reports one that is neither.
gen "$FIX" "$TMP/drop" "assert:KICKOS_NOSUCHKNOB=1" \
    || fail "an asserted name declaring no Kconfig symbol was refused; it must be dropped so a
    build-graph switch stays passable: $(cat "$TMP/drop.err")"
grep -q "KICKOS_NOSUCHKNOB" "$TMP/drop/kickos_config.cmake" \
    && fail "a name Kconfig does not declare reached the fragment"

# --- Leg 4: a candidate travels in BOTH directions ----------------------------------------
# Sending only the positive one lets a defconfig that enables something outrank a command line
# asking to turn it off. The fixture resolves KICKOS_TLS on, so OFF is the direction to drive.
grep -q '^set(KICKOS_TLS ON)$' "$TMP/fix/kickos_config.cmake" \
    || fail "the fixture no longer resolves KICKOS_TLS on, so leg 4 has nothing to turn off"
gen "$FIX" "$TMP/tlsoff" "offer:KICKOS_TLS=OFF" \
    || fail "turning a flag off from CMake was refused: $(cat "$TMP/tlsoff.err")"
grep -q '^set(KICKOS_TLS OFF)$' "$TMP/tlsoff/kickos_config.cmake" \
    || fail "a candidate holding a CMake false constant did not reach Kconfig as n"
# And an int candidate still meets the read-back refusal, so the int syntax is really parsed.
refuse "offer:KICKOS_MAX_THREADS=999" "outside the declared range"

echo "PASS: $checked KICKOS_* symbol reach(es) checked over $boards defconfig(s);" \
     "$offer_n prompted symbol(s) offered under their declared syntax; 3 candidates refused" \
     "by name, 1 dropped by rule, both flag directions witnessed"
