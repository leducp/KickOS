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

if [ "$#" -ne 2 ]; then
    fail "usage: check_kconfig_gen.sh <python> <srcdir>"
fi
PY="$1"
# Absolute, because the paths the fragment reports are and leg 1 compares them literally.
SRC="$(cd "$2" && pwd)" || fail "no source tree at $2"
GEN="$SRC/tools/kconfig/genconfig.py"
DEFCONFIG="$SRC/boards/xmc4800-relax/configs/base/defconfig"

[ -x "$PY" ] || fail "no python interpreter at $PY"
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

gen_with() {
    dc="$1"
    out="$2"
    shift 2
    "$PY" "$GEN" "$SRC" "$dc" "$out" "$@" >"$out.log" 2>"$out.err"
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
# Kconfig, since a copy here would not prove the select chain resolved.
_floor="$(sed -n 's/^[[:space:]]*default[[:space:]]\{1,\}\([0-9]\{1,\}\)[[:space:]]\{1,\}if[[:space:]]\{1,\}ARCH_ARMV7M[[:space:]]*$/\1/p' \
          "$SRC/Kconfig")"
[ "$(printf '%s\n' "$_floor" | wc -l | tr -d ' ')" = 1 ] && [ -n "$_floor" ] \
    || fail "Kconfig does not carry exactly one 'default <n> if ARCH_ARMV7M', so this leg
    has nothing to compare the generated floor against"
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
# These five reach C from CMake rather than through the header, so a missing line here
# lets option() supply its own default and .config stops describing the build.
for flag in KICKOS_DEBUG KICKOS_ENABLE_SELFTEST KICKOS_BENCH \
            KICKOS_SHUTDOWN_TO_BOOTLOADER CONFIG_SCHED_PERIODIC_TICK; do
    grep -q "^set($flag \(ON\|OFF\))$" "$F" || fail "fragment does not carry $flag"
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

# --- Leg 4: the same posture on a board whose chip declares no MPU ------------
# The one refusal that needs a second board, and the one whose diagnostic a user acts on,
# so it must carry the DECLARATION's own help.
NOMPU="$SRC/boards/microbit/configs/base/defconfig"
[ -f "$NOMPU" ] || fail "no defconfig at $NOMPU"
if gen_with "$NOMPU" "$TMP/nompu" "CONFIG_MEMORY_MODEL_MPU=y"; then
    fail "the enforcing posture was accepted on a board with no MPU backend"
fi
grep -q 'unmet dependency: HAS_MPU' "$TMP/nompu.err" \
    || fail "posture refused for the wrong reason: $(cat "$TMP/nompu.err")"
grep -q 'selecting HAS_MPU in arch/Kconfig' "$TMP/nompu.err" \
    || fail "the refusal carries no help text, so it does not say how to fix it"

# --- Leg 5: telemetry needs the RTT TRANSPORT, and the console does not ------
# microbit's console is `chip`, so RTT telemetry has nowhere to go and is refused on the
# transport, not on the trace clock: its chip ships arch_trace_now.
#
# The console arm checks CONSOLE_RTT is selectable, and its scope stops at the transport:
# every chip in the fleet has a trace clock, so a trace-clock dependency re-added to
# CONSOLE_RTT would still select here, while lib/rtt.cc is a RAM ring that reads no clock.
if gen_with "$NOMPU" "$TMP/notelem" "CONFIG_TELEMETRY_RTT=y"; then
    fail "RTT telemetry was accepted on a board whose console carries no RTT"
fi
grep -q 'CONSOLE_RTT || CONSOLE_BOTH' "$TMP/notelem.err" \
    || fail "telemetry refused for the wrong reason: $(cat "$TMP/notelem.err")"
gen_with "$NOMPU" "$TMP/mbrtt" "CONFIG_CONSOLE_RTT=y" \
    || fail "the RTT console was refused, so an arm gained a dependency it does not have"

# --- Leg 6: EVERY defconfig in the tree resolves -----------------------------
# The legs above read two fixtures. Every other variant, including every `st` one on the
# silicon bench path, is resolved by nothing until somebody configures that board, and a
# defconfig the declarations refuse is a board that cannot be built at all. Each gets its
# OWN gendir: the generator refuses a variant change inside a directory that already holds
# a live .config.
mkdir -p "$TMP/all" || fail "cannot create $TMP/all"
n=0
for dc in "$SRC"/boards/*/configs/*/defconfig; do
    [ -f "$dc" ] || continue
    n=$((n + 1))
    out="$TMP/all/$n"
    gen_with "$dc" "$out" || fail "$dc was refused: $(cat "$out.err")"
    [ -s "$out/include/kickos/board_config.h" ] || fail "$dc generated no board_config.h"
    [ -s "$out/kickos_config.cmake" ] || fail "$dc generated no cmake fragment"
done
# A glob that matched nothing leaves every assertion above unexecuted and this leg
# reporting success over zero work.
[ "$n" -gt 0 ] || fail "no defconfig under $SRC/boards/*/configs/"

echo "PASS: kconfig generation, 7 refusals, 4 accepted overrides, $n defconfigs resolved"
