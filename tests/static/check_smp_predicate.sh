#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the shared-kernel hardware predicate in the root CMakeLists: more than one core on
# an arch that ships no smp.cmake is refused at configure time. Two child configures of the
# same board, differing in one clause, and neither builds anything.
#
#   refusal   the fixture variant, four cores. cmake must exit non-zero and its diagnostic
#             must carry the predicate's own wording: the resolved core count, the arch, the
#             smp.cmake it looked for, and the six properties that declaration asserts.
#   control   the base variant, one core. cmake must exit zero, so a refusal that is really
#             a broken child configure, a missing kconfiglib or a board that cannot resolve
#             cannot pass as the predicate firing.
#
# Before either arm, the fixture is resolved through the generator and read back: the arch it
# lands on must ship no smp.cmake and its core count must exceed one. A fixture that has
# stopped being ineligible refuses HERE, rather than leaving the refusal arm asserting a
# message the tree no longer produces.
#
# KICKOS_KCONFIG_PY is exported for the child from the interpreter passed in, so the child
# resolves Kconfig with the same one this tree did and needs nothing in the ctest environment.
#
# Run from anywhere, no build directory:
#   tests/static/check_smp_predicate.sh <python> <src-dir> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 3 ]; then
    fail "usage: check_smp_predicate.sh <python> <src-dir> <cmake>"
fi
KCONFIG_PY="$1"
SRC="$(cd "$2" && pwd)" || fail "no source tree at $2"
CMAKE="$3"

# The board whose arch carries no shared-kernel declaration, and the two variants under it.
FIXTURE_BOARD="sim"
FIXTURE_VARIANT="smp-ineligible"
CONTROL_VARIANT="base"

FIXTURE_DC="$SRC/boards/$FIXTURE_BOARD/configs/$FIXTURE_VARIANT/defconfig"
CONTROL_DC="$SRC/boards/$FIXTURE_BOARD/configs/$CONTROL_VARIANT/defconfig"
GEN="$SRC/tools/kconfig/genconfig.py"

[ -x "$KCONFIG_PY" ] || fail "no python interpreter at $KCONFIG_PY"
[ -f "$GEN" ] || fail "no generator at $GEN"
[ -f "$SRC/CMakeLists.txt" ] || fail "no CMakeLists.txt under $SRC"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"

# The fixture is the whole instrument. Missing, it must refuse rather than skip: a run that
# registered no assertion reads exactly like a run that made them all.
[ -f "$FIXTURE_DC" ] || fail "no ineligible fixture at $FIXTURE_DC. It is what asks for more
    than one core on an arch with no smp.cmake, and without it this gate asserts nothing"
[ -f "$CONTROL_DC" ] || fail "no control defconfig at $CONTROL_DC, so the refusal arm below
    has nothing to be compared against"

export KICKOS_KCONFIG_PY="$KCONFIG_PY"
scratch_dir

# --- The fixture is still ineligible, and still asks for more than one core ---
# Read out of the generator rather than restated here: this is the same resolution the child
# configure performs, so a Kconfig edit that quietly folds the fixture to one core, or an
# smp.cmake appearing under this arch, is reported as a dead fixture.
resolve() { # <defconfig> <gendir>
    "$KCONFIG_PY" "$GEN" "$SRC" "$1" "$2" >"$2.log" 2>"$2.err"
}

resolve "$FIXTURE_DC" "$TMP/fix" \
    || fail "the fixture defconfig no longer resolves: $(cat "$TMP/fix.err")"
FIX_FRAG="$TMP/fix/kickos_config.cmake"
[ -s "$FIX_FRAG" ] || fail "the fixture generated no cmake fragment"

frag_value() { # <fragment> <name>
    sed -n "s/^set($2 \"\{0,1\}\([^\")]*\)\"\{0,1\})\$/\1/p" "$1" | tail -n 1
}

FIX_CORES="$(frag_value "$FIX_FRAG" KICKOS_NUM_CORES)"
FIX_ARCH="$(frag_value "$FIX_FRAG" KICKOS_ARCH)"
FIX_FAMILY="$(frag_value "$FIX_FRAG" KICKOS_ARCH_FAMILY)"
require_number "$FIX_CORES" "the core count the fixture resolves to"
require_literal "$FIX_ARCH" "the arch the fixture resolves to"
require_literal "$FIX_FAMILY" "the arch family the fixture resolves to"

if [ "$FIX_CORES" -le 1 ]; then
    fail "the fixture resolves to $FIX_CORES core(s), so it does not reach the predicate at
    all. It exists to ask for more than one core on an ineligible arch"
fi

# The path the root CMakeLists composes, both spellings: a family-less arch sits directly
# under arch/.
OPTIN="$SRC/arch/$FIX_FAMILY/$FIX_ARCH/smp.cmake"
if [ "$FIX_FAMILY" = "$FIX_ARCH" ]; then
    OPTIN="$SRC/arch/$FIX_ARCH/smp.cmake"
fi
if [ -f "$OPTIN" ]; then
    fail "arch '$FIX_ARCH' now ships $OPTIN, so it PASSES the predicate and the fixture is
    no longer ineligible. Point the fixture at an arch that declares no shared-kernel
    properties, or this gate reports a refusal that can never happen"
fi

resolve "$CONTROL_DC" "$TMP/ctl" \
    || fail "the control defconfig does not resolve: $(cat "$TMP/ctl.err")"
CTL_CORES="$(frag_value "$TMP/ctl/kickos_config.cmake" KICKOS_NUM_CORES)"
require_number "$CTL_CORES" "the core count the control resolves to"
if [ "$CTL_CORES" -ne 1 ]; then
    fail "the control variant resolves to $CTL_CORES core(s), not 1, so the two arms differ
    in more than the clause under test"
fi
echo "smp_predicate: fixture armed ($FIXTURE_BOARD/$FIXTURE_VARIANT: arch '$FIX_ARCH',
  $FIX_CORES cores, no $OPTIN); control resolves to $CTL_CORES core"

# --- The two configures --------------------------------------------------------
# Identical but for the variant, and both stop at the configure step: apps and ctest
# registration are off, so the control does not pull the test layer in behind it.
configure() { # <variant> <build-dir> <log>
    "$CMAKE" -S "$SRC" -B "$2" -G Ninja \
        -DKICKOS_BOARD="$FIXTURE_BOARD" \
        -DKICKOS_CONFIG_VARIANT="$1" \
        -DKICKOS_BUILD_TESTS=OFF \
        -DKICKOS_BUILD_APPS=OFF >"$3" 2>&1
}

# --- Control: one core on the same board configures ---------------------------
if ! configure "$CONTROL_VARIANT" "$TMP/bctl" "$TMP/ctl.cfg.log"; then
    sed -n '1,40p' "$TMP/ctl.cfg.log" >&2
    fail "the control configure FAILED at one core, so this box cannot configure a child
    tree at all and the refusal arm below would pass for the wrong reason"
fi
echo "smp_predicate: control configured ($FIXTURE_VARIANT's twin at $CTL_CORES core)"

# --- Refusal: more than one core on the same board is refused -----------------
if configure "$FIXTURE_VARIANT" "$TMP/bfix" "$TMP/fix.cfg.log"; then
    sed -n '1,40p' "$TMP/fix.cfg.log" >&2
    fail "$FIX_CORES cores on arch '$FIX_ARCH' were ACCEPTED. The arch declares none of the
    properties one kernel image across cores requires, so the configure must refuse"
fi

# CMake re-wraps a message() body across lines and doubles the blank after a full stop, so
# every phrase below is matched against the log folded to one space-separated line.
FLAT="$(tr '\n' ' ' <"$TMP/fix.cfg.log" | tr -s ' ')"
[ -n "$FLAT" ] || fail "the refused configure produced no output at all, so there is no
    diagnostic to read and the non-zero exit could be anything"

want() { # <phrase> <what it pins>
    require_literal "$1" "the expected phrase"
    if ! printf '%s\n' "$FLAT" | grep -q -F -e "$1"; then
        sed -n '1,40p' "$TMP/fix.cfg.log" >&2
        fail "the refusal does not state $2. Expected to find: $1"
    fi
}

# The predicate, and nothing that any other FATAL_ERROR in the tree also says. The count and
# the arch come from the fragment above, so a fixture pointed at another board still pins the
# values this run actually resolved.
want "CMake Error at CMakeLists.txt:" "which file refused (the root CMakeLists, not an include)"
want "KICKOS_NUM_CORES=$FIX_CORES on arch '$FIX_ARCH'" "the core count and the arch it refused"
want "arch/$FIX_ARCH/smp.cmake" "the declaration file it looked for"
want "coherent shared memory, an inter-core exclusion primitive, an inter-core interrupt, a per-core identity, symmetric cores and per-line interrupt targeting" \
     "the six properties an smp.cmake asserts"

echo "PASS: $FIX_CORES cores on arch '$FIX_ARCH' refused by the shared-kernel predicate,
  naming the count, the arch, the missing smp.cmake and all six properties; the same board
  at $CTL_CORES core configures"
exit 0
