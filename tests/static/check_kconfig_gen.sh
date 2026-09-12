#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate for tools/kconfig/genconfig.py. A host check on the source tree: it opens no
# build directory and is registered on every board.
#
# kconfiglib does not fail on an out-of-range int: it warns and falls back on the symbol's
# DEFAULT, the fleet value rather than the one this defconfig asked for, so an unnoticed
# refusal hands the board a LARGER pool than it declared. Leg 2 asserts the fleet value
# appears in the refusal, so a generator that stopped reading values back cannot pass by
# refusing nothing.

set -u

. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 3 ]; then
    fail "usage: check_kconfig_gen.sh <python> <srcdir> <cmake>"
fi
PY="$1"
# Absolute, because the paths the fragment reports are and leg 1 compares them literally.
SRC="$(cd "$2" && pwd)" || fail "no source tree at $2"
CMK="$3"
GEN="$SRC/tools/kconfig/genconfig.py"
DEFCONFIG="$SRC/boards/xmc4800-relax/configs/base/defconfig"

[ -x "$PY" ] || fail "no python interpreter at $PY"
[ -x "$CMK" ] || fail "no cmake at $CMK"
[ -f "$GEN" ] || fail "no generator at $GEN"
[ -f "$DEFCONFIG" ] || fail "no defconfig at $DEFCONFIG"

scratch_dir

# A run that names no board leaves every leg below measuring the fleet defaults and
# agreeing with itself.
grep -q '^CONFIG_BOARD_XMC4800_RELAX=y' "$DEFCONFIG" \
    || fail "$DEFCONFIG no longer selects a board"

gen() {
    out="$1"
    shift
    gen_with "$DEFCONFIG" "$out" "$@"
}

# The two tallies the PASS line reports, counted at the one place every leg goes through
# rather than written out beside it, so a leg added or dropped cannot leave a number behind.
# A caller that drove gen in a pipeline or `$(...)` would increment a copy of them instead.
ACCEPTED=0
REFUSED=0

gen_with() {
    dc="$1"
    out="$2"
    shift 2
    if ! "$PY" "$GEN" "$SRC" "$dc" "$out" "$@" >"$out.log" 2>"$out.err"; then
        REFUSED=$((REFUSED + 1))
        return 1
    fi
    if [ "$#" -gt 0 ]; then
        ACCEPTED=$((ACCEPTED + 1))
    fi
}

# --- Leg 1: it generates, and a default flows from the selected arch --------
gen "$TMP/ok" || fail "generation failed: $(cat "$TMP/ok.err")"
for artifact in .config include/kickos/board_config.h kickos_config.cmake; do
    [ -s "$TMP/ok/$artifact" ] || fail "generated no $artifact"
done

H="$TMP/ok/include/kickos/board_config.h"
# The NVIC line count is a chip constant, defined unconditionally in the chip's own
# chip_limits.h; a copy here would be a second source for one fact, and this generated
# header shadows the real one.
# Anchored on the whole identifier: KICKOS_MAX_IRQ_HANDLES is a real knob and does belong
# here, and a substring match would call it a leak.
grep -qE '^#define KICKOS_MAX_IRQ ' "$H" \
    && fail "a chip constant reached the generated knob header"
grep -q '^#define KICKOS_MAX_THREADS 8$' "$H" || fail "header lost MAX_THREADS=8"
# Stated in no defconfig: it comes from KICKOS_MIN_STACK_SIZE's `default <n> if
# ARCH_ARMV7M`, which the board reached through its chip. The expected value is READ OUT OF
# Kconfig, since a copy here would not prove the select chain resolved. Scoped to that ONE
# stanza: other knobs carry per-arch defaults too (KICKOS_KERNEL_STACK_SIZE does), so a
# whole-file scrape would find several and have nothing to compare against.
_floor="$(awk '
    /^config KICKOS_MIN_STACK_SIZE[[:space:]]*$/ { inside = 1; next }
    inside && /^config / { inside = 0 }
    inside && $1 == "default" && $3 == "if" && $4 == "ARCH_ARMV7M" && NF == 4 { print $2 }
' "$SRC/Kconfig")"
[ "$(printf '%s\n' "$_floor" | wc -l | tr -d ' ')" = 1 ] && [ -n "$_floor" ] \
    || fail "Kconfig's KICKOS_MIN_STACK_SIZE stanza does not carry exactly one
    'default <n> if ARCH_ARMV7M', so this leg has nothing to compare the generated floor
    against"
grep -q "^#define KICKOS_MIN_STACK_SIZE $_floor\$" "$H" \
    || fail "MIN_STACK_SIZE did not resolve to the armv7m floor ($_floor per Kconfig)"
grep -q '^#define KICKOS_BOARD_CONFIG_H$' "$H" \
    || fail "generated header carries no include guard, so it cannot shadow the board's"

F="$TMP/ok/kickos_config.cmake"
grep -q '^set(KICKOS_BOARD "xmc4800-relax")$' "$F" || fail "fragment lost the board"
grep -q '^set(KICKOS_ARCH "armv7m")$' "$F" || fail "fragment lost the arch"
grep -q '^set(KICKOS_ARCH_FAMILY "arm")$' "$F" || fail "fragment lost the arch family"
grep -q '^set(KICKOS_CHIP "xmc4800")$' "$F" || fail "fragment lost the chip"
grep -q '^set(KICKOS_CONSOLE "both")$' "$F" || fail "fragment lost the console backend"
grep -q '^set(KICKOS_TELEMETRY "off")$' "$F" || fail "fragment lost the telemetry sink"
grep -q "^set(KICKOS_MIN_STACK_SIZE $_floor)\$" "$F" || fail "fragment lost the stack floor"
grep -q '^set(KICKOS_HAVE_MPU 1)$' "$F" \
    || fail "the base variant of an enforcing board did not resolve the enforcing posture"
# Named file by file rather than matched on the list's SHAPE: a shape match still looks
# right with the per-board boards/*/Kconfig entries dropped, and those are where a board
# states its service list and pin map.
SOURCES="$(grep '^set(KICKOS_KCONFIG_SOURCES ' "$F")" \
    || fail "fragment does not report what it read, so a Kconfig edit would not reconfigure"
for want in "$SRC/Kconfig" "$SRC/boards/Kconfig" "$SRC/boards/xmc4800-relax/Kconfig" \
            "$SRC/arch/Kconfig" "$DEFCONFIG"; do
    # First, middle or last element; never a substring of a longer path.
    case "$SOURCES" in
        *"\"$want;"* | *";$want;"* | *";$want\")") ;;
        *) fail "KICKOS_KCONFIG_SOURCES does not name $want, so editing it would not reconfigure" ;;
    esac
done

# --- Leg 2: every requested value is read back, and a refusal is a refusal ---
refuse() {
    request="$1"
    expect="$2"
    if gen "$TMP/no" "$request"; then
        fail "$request was accepted; expected a refusal"
    fi
    grep -q "REFUSED" "$TMP/no.err" \
        || fail "$request failed without a refusal: $(cat "$TMP/no.err")"
    grep -q "$expect" "$TMP/no.err" \
        || fail "$request refused for the wrong reason: $(cat "$TMP/no.err")"
}

refuse "CONFIG_KICKOS_MAX_THREADS=999" \
       "outside the declared range \[2, 64\]: resolved to '16'"
refuse "CONFIG_KICKOS_MAX_HANDLES=12" "no such symbol"
# The same fact from the other side: with no symbol to set, configuring the interrupt-line
# count is refused outright rather than shrinking the kernel table and the startup.S vector
# table and stranding the lines above it.
refuse "CONFIG_KICKOS_MAX_IRQ=64" "no such symbol"
refuse "CONFIG_KICKOS_SHUTDOWN_TO_BOOTLOADER=y" \
       "unmet dependency: KICKOS_ENABLE_SELFTEST"
# The refusal also carries the symbol's OWN help, read out of Kconfig rather than a
# hand-written hint kept in step by hand. Checked on this same fixture instead of a second
# board: what is under test is sym_help() appending the node's help text, one call that
# runs identically for any unmet-dependency refusal, not the dependency it was fed.
grep -q 'arch_reboot is compiled out of a production image' "$TMP/no.err" \
    || fail "the refusal carries no help text, so it does not say how to fix it: $(cat "$TMP/no.err")"
# A knob spelled without the prefix is dropped by the loader and by the read-back
# check alike, so it must be refused on its shape or it reads as honoured.
refuse "KICKOS_MAX_THREADS=4" "not of the form CONFIG_<name>=<value>"

# --- Leg 3: keeps leg 2 from passing by refusing everything ------------------
# Both are legal on this board: the flat posture, which its own flat variant states, and
# RTT telemetry, which it can host because its console carries RTT. The base defconfig
# witnesses the enforcing posture, so the flat direction is the one driven here.
gen "$TMP/flat" "CONFIG_MEMORY_MODEL_FLAT=y" \
    || fail "the flat posture was refused: $(cat "$TMP/flat.err")"
grep -q '^set(KICKOS_HAVE_MPU 0)$' "$TMP/flat/kickos_config.cmake" \
    || fail "the flat posture did not reach the fragment"
gen "$TMP/telem" "CONFIG_TELEMETRY_RTT=y" \
    || fail "RTT telemetry was refused on a board that carries RTT: $(cat "$TMP/telem.err")"

# A STRING request has to round-trip, not just an integer or a bool. Sim gates build one
# board against different service providers, and a knob the fragment sets while the
# translation omits it is one the fragment silently overwrites.
gen "$TMP/svc" 'CONFIG_KICKOS_SERVICE_LIST="kickos_services_sim"' \
    || fail "a service-list override was refused: $(cat "$TMP/svc.err")"
grep -q '^set(KICKOS_SERVICE_LIST "kickos_services_sim")$' "$TMP/svc/kickos_config.cmake" \
    || fail "a service-list override did not reach the fragment"

# --- Leg 7: a string knob's semicolon or dollar reaches the fragment inert ---------
# Neither character needs escaping at the .config level itself (kconfiglib's own quoting
# only cares about a bare backslash or quote), so this value round-trips through
# check_assignments unchanged and the fragment is the only place left to prove it: a
# raw semicolon is CMake's list separator regardless of quoting, and a raw $ expands
# ${...}/$ENV{...} even inside a quoted set() argument.
gen "$TMP/esc" 'CONFIG_KICKOS_SERVICE_LIST="kickos_services_sim;two$LEAK_ME"' \
    || fail "a service-list value carrying ';' and '\$' was refused: $(cat "$TMP/esc.err")"
ESCF="$TMP/esc/kickos_config.cmake"
grep -Fq 'set(KICKOS_SERVICE_LIST "kickos_services_sim\;two\$LEAK_ME")' "$ESCF" \
    || fail "the fragment did not backslash-escape the ';' and '\$' in KICKOS_SERVICE_LIST"

# Read the escaped line back through CMake itself, with a decoy variable in scope, rather
# than trusting the text of the generated line: an unescaped \$ would pull LEAK_ME's value
# in instead of the operator's own, and an unescaped ; would split the value in two.
cat > "$TMP/verify.cmake" <<VEOF
set(LEAK_ME "PWNED")
include("$ESCF")
list(LENGTH KICKOS_SERVICE_LIST _len)
if(NOT _len EQUAL 1)
  message(FATAL_ERROR "KICKOS_SERVICE_LIST split into \${_len} list element(s)")
endif()
list(GET KICKOS_SERVICE_LIST 0 _elem)
file(WRITE "$TMP/esc.got" "\${_elem}")
VEOF
"$CMK" -P "$TMP/verify.cmake" >"$TMP/verify.log" 2>"$TMP/verify.err" \
    || fail "the escaped fragment does not parse back as one CMake string: $(cat "$TMP/verify.err")"
printf '%s' 'kickos_services_sim;two$LEAK_ME' > "$TMP/esc.want"
cmp -s "$TMP/esc.got" "$TMP/esc.want" \
    || fail "KICKOS_SERVICE_LIST round-tripped to '$(cat "$TMP/esc.got")', not \
'kickos_services_sim;two\$LEAK_ME'"

# --- Leg 8: the escaping function itself, for the characters a defconfig cannot carry ---
# A literal quote or backslash needs kconfiglib's OWN backslash to reach sym.str_value at
# all, which then also satisfies check_assignments' plain '"'-stripping read-back, so leg 7's
# route cannot tell an unescaped quote from an escaped one. Checked directly instead. '@' is
# included as a negative case: this fragment is include()'d, never configure_file()'d, so
# CMake never expands @VAR@ here and escaping it would only add noise.
"$PY" - "$SRC" <<'PYEOF' >"$TMP/escape_unit.log" 2>"$TMP/escape_unit.err" \
    || fail "cmake_escape unit cases failed: $(cat "$TMP/escape_unit.err")"
import sys

sys.path.insert(0, sys.argv[1] + "/tools/kconfig")
import genconfig

cases = [
    ("plain", "plain"),
    ('a"b', 'a\\"b'),
    ("a;b", "a\\;b"),
    ("a$b", "a\\$b"),
    ("a\\b", "a\\\\b"),
    ("a@b", "a@b"),
]
for raw, want in cases:
    got = genconfig.cmake_escape(raw)
    if got != want:
        sys.stderr.write("cmake_escape(%r) = %r, want %r\n" % (raw, got, want))
        sys.exit(1)
PYEOF

# --- Leg 9: the stale-directory repair rides the LIVE base and nothing else ---------------
REPAIR='delete that .config to reload from'
GONE='CONFIG_KICKOS_GONE_SYMBOL=y'
gen "$TMP/stale" || fail "generation failed: $(cat "$TMP/stale.err")"
echo "$GONE" >> "$TMP/stale/.config"
if gen "$TMP/stale"; then
    fail "a live .config naming an undeclared symbol was accepted"
fi
grep -q "REFUSED $GONE: no such symbol" "$TMP/stale.err" \
    || fail "the stale live .config refused for the wrong reason: $(cat "$TMP/stale.err")"
grep -q "$REPAIR" "$TMP/stale.err" \
    || fail "the refusal does not say how to repair the build directory: $(cat "$TMP/stale.err")"

# The same undeclared symbol from a DEFCONFIG, into a directory that holds no live state.
cp "$DEFCONFIG" "$TMP/gone-defconfig" || fail "cannot copy $DEFCONFIG"
echo "$GONE" >> "$TMP/gone-defconfig"
if gen_with "$TMP/gone-defconfig" "$TMP/gonedc"; then
    fail "a defconfig naming an undeclared symbol was accepted"
fi
grep -q "REFUSED $GONE: no such symbol" "$TMP/gonedc.err" \
    || fail "the defconfig refused for the wrong reason: $(cat "$TMP/gonedc.err")"
grep -q "$REPAIR" "$TMP/gonedc.err" \
    && fail "a defconfig edit was reported as a stale build directory: $(cat "$TMP/gonedc.err")"

echo "PASS: kconfig generation, $REFUSED refusals, $ACCEPTED accepted overrides," \
     "string-knob escaping verified through CMake and unit cases"
