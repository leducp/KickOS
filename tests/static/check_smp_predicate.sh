#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the shared-kernel hardware predicate (cmake/smp_predicate.cmake): six properties
# with TWO owners, an arch declaring the three an ISA gives every part and a part declaring the
# three its die decides, and a shared-kernel selection missing any of them refused at configure
# time.
#
# THE PREDICATE IS DRIVEN TWICE AND THIS GATE IS THE SECOND CALLER, which is what buys the part
# case a control at all. A whole-tree configure can only refuse what some board in the tree
# actually fails, and no board fails the PART's three while passing the arch's: every chip that
# ships a declaration declares all three of them. So the arms below split:
#
#   whole tree   one child configure of a real board, four cores on an arch that declares
#                nothing, plus its one-core twin as the control. This is what proves the
#                refusal is wired into the build at all rather than only into a function.
#   script mode  cmake -P over SYNTHETIC declaration trees this gate writes, one per case,
#                against the SAME module the build calls. Each refusal arm is paired with a
#                control differing in one clause.
#
# KICKOS_KCONFIG_PY is exported for the whole-tree child from the interpreter passed in, so it
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

MODULE="$SRC/cmake/smp_predicate.cmake"

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
[ -f "$MODULE" ] || fail "no predicate module at $MODULE. It is the one authority this gate
    drives, and without it every arm below would be testing nothing"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"

# The fixture is the whole instrument. Missing, it must refuse rather than skip: a run that
# registered no assertion reads exactly like a run that made them all.
[ -f "$FIXTURE_DC" ] || fail "no ineligible fixture at $FIXTURE_DC. It is what asks for more
    than one core on an arch with no smp.cmake, and without it this gate asserts nothing"
[ -f "$CONTROL_DC" ] || fail "no control defconfig at $CONTROL_DC, so the refusal arm below
    has nothing to be compared against"

export KICKOS_KCONFIG_PY="$KCONFIG_PY"
scratch_dir

# --- The property names the module owns, read out of it rather than restated ------
# The refusal's wording is the module's, so a renamed property must break HERE and not leave
# an arm grepping for a phrase the tree no longer prints.
decl_list() { # <ARCH|CHIP>
    sed -n "s/^set(KICKOS_SMP_PROPS_$1 \(.*\))\$/\1/p" "$MODULE"
}
ARCH_PROPS="$(decl_list ARCH)"
CHIP_PROPS="$(decl_list CHIP)"
[ -n "$ARCH_PROPS" ] || fail "$MODULE declares no KICKOS_SMP_PROPS_ARCH list, so this gate
    cannot tell which properties an arch is supposed to own"
[ -n "$CHIP_PROPS" ] || fail "$MODULE declares no KICKOS_SMP_PROPS_CHIP list, so this gate
    cannot tell which properties a part is supposed to own"

n_words() { echo "$1" | wc -w | tr -d ' '; }
N_ARCH="$(n_words "$ARCH_PROPS")"
N_CHIP="$(n_words "$CHIP_PROPS")"
if [ "$N_ARCH" -eq 0 ] || [ "$N_CHIP" -eq 0 ]; then
    fail "the predicate splits $N_ARCH arch and $N_CHIP chip properties; a side with none
    means the two-owner split has collapsed and one file certifies everything again"
fi
echo "smp_predicate: module splits $N_ARCH arch-owned ($ARCH_PROPS) and $N_CHIP part-owned
  ($CHIP_PROPS) properties"

# --- Script-mode driver ---------------------------------------------------------
# Calls the SAME function the root CMakeLists calls, over a tree this gate composes. The
# accepted marker is printed only past the call, so a refusal cannot reach it.
DRIVER="$TMP/drive.cmake"
cat >"$DRIVER" <<EOF
list(APPEND CMAKE_MODULE_PATH "$SRC/cmake")
include(smp_predicate)
kickos_smp_predicate(
  SOURCE_DIR   "\${SP_TREE}"
  ARCH         "\${SP_ARCH}"
  ARCH_FAMILY  "\${SP_FAMILY}"
  CHIP         "\${SP_CHIP}"
  BOARD        "fixture-board"
  NUM_CORES    "\${SP_CORES}"
  MODEL_SHARED "\${SP_MODEL}")
message(STATUS "PREDICATE-ACCEPTED")
EOF

FAM=testfam
ARCH=testarch
CHIP=testchip

# <case> <arch-decl-body> <chip-decl-body-or-NONE>
compose() {
    _case="$1"
    _t="$TMP/tree-$_case"
    rm -rf "$_t"
    mkdir -p "$_t/arch/$FAM/$ARCH" "$_t/arch/$FAM/chip/$CHIP"
    printf '%s\n' "$2" >"$_t/arch/$FAM/$ARCH/smp.cmake"
    if [ "$3" != NONE ]; then
        printf '%s\n' "$3" >"$_t/arch/$FAM/chip/$CHIP/smp.cmake"
    fi
    echo "$_t"
}

decls() { # <prefix> <space-separated property names>
    for _p in $2; do
        echo "set(KICKOS_${1}_SMP_${_p} 1)"
    done
}

ARCH_ALL="$(decls ARCH "$ARCH_PROPS")"
CHIP_ALL="$(decls CHIP "$CHIP_PROPS")"
# The overreach, one direction each. The arch's is one part-owned line on top of its own three;
# the chip's is a file declaring ALL SIX, which is the shape that satisfies the arch's half from a
# die's file while the arch declares nothing. Each refusal names the FIRST property of the set it
# is reaching into, the module walking the list in order.
FIRST_CHIP_PROP="$(echo "$CHIP_PROPS" | awk '{print $1}')"
FIRST_ARCH_PROP="$(echo "$ARCH_PROPS" | awk '{print $1}')"
ARCH_OVERREACH="$ARCH_ALL
set(KICKOS_CHIP_SMP_${FIRST_CHIP_PROP} 1)"
CHIP_OVERREACH="$CHIP_ALL
$ARCH_ALL"
# The same reach where the ARCH already declares the property, so the line changes no value and
# only the scope the chip include is given can tell it apart from silence.
CHIP_RESTATE="$CHIP_ALL
set(KICKOS_ARCH_SMP_${FIRST_ARCH_PROP} 1)"

# <case> <tree> <cores> <model> ; prints the run's combined output to $TMP/<case>.log
drive() {
    "$CMAKE" -DSP_TREE="$2" -DSP_ARCH="$ARCH" -DSP_FAMILY="$FAM" -DSP_CHIP="$CHIP" \
        -DSP_CORES="$3" -DSP_MODEL="$4" -P "$DRIVER" >"$TMP/$1.log" 2>&1
}

flat() { tr '\n' ' ' <"$1" | tr -s ' '; }

# <case> <phrase> <what it pins>
want_in() {
    _log="$TMP/$1.log"
    if ! flat "$_log" | grep -q -F -e "$2"; then
        sed -n '1,40p' "$_log" >&2
        fail "[$1] the refusal does not state $3. Expected to find: $2"
    fi
}

deny_in() {
    _log="$TMP/$1.log"
    if flat "$_log" | grep -q -F -e "$2"; then
        sed -n '1,40p' "$_log" >&2
        fail "[$1] the refusal states $3, which this case must not report. Found: $2"
    fi
}

accepted() { # <case>
    if ! grep -q "PREDICATE-ACCEPTED" "$TMP/$1.log"; then
        sed -n '1,40p' "$TMP/$1.log" >&2
        fail "[$1] the predicate did NOT accept, so this control cannot show the refusal arms
    fire for the reason they claim"
    fi
}

# --- all-declared: the control every refusal arm is measured against ------------
T="$(compose alldecl "$ARCH_ALL" "$CHIP_ALL")"
if ! drive alldecl "$T" 4 1; then
    sed -n '1,40p' "$TMP/alldecl.log" >&2
    fail "[alldecl] both owners declare every property at four cores and the predicate still
    REFUSED, so every refusal below would pass for the wrong reason"
fi
accepted alldecl
echo "smp_predicate: [alldecl] both owners declaring, four cores, shared model: accepted"

# --- part-undeclared: the case a whole-tree configure cannot reach --------------
T="$(compose partundecl "$ARCH_ALL" NONE)"
if drive partundecl "$T" 4 1; then
    sed -n '1,40p' "$TMP/partundecl.log" >&2
    fail "[partundecl] four cores were ACCEPTED with the arch declaring its $N_ARCH properties
    and the chip declaring none. GIC version, routing and topology are the PART's, so a chip
    that declares nothing must refuse however complete its arch is"
fi
# Every part-owned property named, and named as the PART's.
for p in $CHIP_PROPS; do
    want_in partundecl "KICKOS_CHIP_SMP_${p}" "which part-owned property is undeclared ($p)"
done
want_in partundecl "the PART's" "that the undeclared properties belong to the part"
want_in partundecl "arch/$FAM/chip/$CHIP/smp.cmake" "the declaration file the part owes"
# And NOT the arch's, which this arch declared in full.
for p in $ARCH_PROPS; do
    deny_in partundecl "KICKOS_ARCH_SMP_${p}" "an arch-owned property as missing ($p)"
done
deny_in partundecl "the ARCH's" "that an arch-owned property is undeclared"
echo "smp_predicate: [partundecl] arch complete, part silent: refused, naming all $N_CHIP
  part-owned properties and none of the $N_ARCH arch-owned ones"

# --- arch-overreach: an arch certifying a part's property -----------------------
T="$(compose overreach "$ARCH_OVERREACH" "$CHIP_ALL")"
if drive overreach "$T" 4 1; then
    sed -n '1,40p' "$TMP/overreach.log" >&2
    fail "[overreach] an ARCH declaration set KICKOS_CHIP_SMP_${FIRST_CHIP_PROP} and was
    ACCEPTED. Nothing else stops the per-arch declaration growing back: the include shares
    scope, so that line would certify a part property for every part of the arch"
fi
want_in overreach "KICKOS_CHIP_SMP_${FIRST_CHIP_PROP}" "which part-owned property the arch set"
want_in overreach "arch/$FAM/$ARCH/smp.cmake" "the arch file that overreached"
want_in overreach "property of a PART" "why the line is refused"
echo "smp_predicate: [overreach] arch setting a part-owned property: refused, naming the
  property and the file"

# --- chip-overreach: a part certifying the ISA's half ---------------------------
# The mirror of the arm above, and the two are not one case: the arch include and the chip
# include are separate scopes to hold open, so a module that refuses one direction can accept the
# other outright. THE ARCH FILE HERE DECLARES NOTHING, so nothing but this refusal stands between
# a chip file and a four-core shared kernel on an ISA that promises none of it.
T="$(compose chipoverreach "" "$CHIP_OVERREACH")"
if drive chipoverreach "$T" 4 1; then
    sed -n '1,40p' "$TMP/chipoverreach.log" >&2
    fail "[chipoverreach] a CHIP declaration set all $((N_ARCH + N_CHIP)) properties, its arch
    declaring none, and four cores under the shared model were ACCEPTED. The include shares scope,
    so those $N_ARCH lines stood in for an ISA-wide claim made by no file, and the ownership split
    this module exists to enforce holds in one direction only"
fi
want_in chipoverreach "KICKOS_ARCH_SMP_${FIRST_ARCH_PROP}" \
    "which arch-owned property the chip set"
want_in chipoverreach "arch/$FAM/chip/$CHIP/smp.cmake" "the chip file that overreached"
want_in chipoverreach "arch/$FAM/$ARCH/smp.cmake" "the arch file that owes the property"
want_in chipoverreach "property of an ISA" "why the line is refused"
echo "smp_predicate: [chipoverreach] chip setting an arch-owned property: refused, naming the
  property, the file that set it and the file that owes it"

# --- chip-restatement: the reach that changes no value --------------------------
# The other half of the isolation, and the arm that separates HOLDING THE ARCH'S VALUES OUT OF
# THE CHIP INCLUDE from comparing them across it: here the arch declares the property and the
# chip file sets it to the same 1, so a refusal keyed on the value sees nothing to report and the
# part goes on owning a line that is the ISA's. The control is [alldecl], which differs in one
# clause: which file that line sits in.
T="$(compose chiprestate "$ARCH_ALL" "$CHIP_RESTATE")"
if drive chiprestate "$T" 4 1; then
    sed -n '1,40p' "$TMP/chiprestate.log" >&2
    fail "[chiprestate] a CHIP declaration restated KICKOS_ARCH_SMP_${FIRST_ARCH_PROP}, which
    its arch already declares, and was ACCEPTED. The value is unchanged, so nothing but the scope
    the chip include runs in can refuse this, and a module that compares values across that
    include instead of holding them out of it reads the line as absent"
fi
want_in chiprestate "KICKOS_ARCH_SMP_${FIRST_ARCH_PROP}" \
    "which arch-owned property the chip restated"
want_in chiprestate "arch/$FAM/chip/$CHIP/smp.cmake" "the chip file that overreached"
want_in chiprestate "property of an ISA" "why the line is refused"
echo "smp_predicate: [chiprestate] chip restating an arch-owned property its arch already
  declares: refused, so the boundary is the include's SCOPE and not a value comparison"

# --- one-core: the predicate says nothing below two cores ----------------------
T="$(compose onecore "" NONE)"
if ! drive onecore "$T" 1 1; then
    sed -n '1,40p' "$TMP/onecore.log" >&2
    fail "[onecore] ONE core with nothing declared was refused. The predicate is about one
    kernel image spanning cores, so at one core a declaration is owed by nobody and every
    single-core board in the fleet would stop configuring"
fi
accepted onecore
echo "smp_predicate: [onecore] nothing declared, one core: accepted"

# --- amp-model: the refusal is the MODEL's and not the count's ------------------
T="$(compose ampmodel "" NONE)"
if ! drive ampmodel "$T" 4 0; then
    sed -n '1,40p' "$TMP/ampmodel.log" >&2
    fail "[ampmodel] FOUR cores under the AMP model with nothing declared was refused. An AMP
    image raises the count to have a core identity at all and satisfies none of the six, so
    this refusal keys on the count somewhere and locks the parts the predicate sends to AMP
    out of the count AMP needs"
fi
accepted ampmodel
echo "smp_predicate: [ampmodel] nothing declared, four cores, AMP model: accepted, so the
  refusal keys on the model and not the count"

# --- The whole tree: the refusal is wired into the build ------------------------
# Read out of the generator rather than restated here: this is the same resolution the child
# configure performs, so a Kconfig edit that quietly folds the fixture to one core, or an
# smp.cmake appearing under this arch, is reported as a dead fixture.
resolve() { # <defconfig> <gendir>
    "$KCONFIG_PY" "$GEN" "$SRC" "$1" "$2" >"$2.log" 2>&1
}

resolve "$FIXTURE_DC" "$TMP/fix" \
    || fail "the fixture defconfig no longer resolves: $(cat "$TMP/fix.err" 2>/dev/null)"
FIX_FRAG="$TMP/fix/kickos_config.cmake"
[ -s "$FIX_FRAG" ] || fail "the fixture generated no cmake fragment"

frag_value() { # <fragment> <name>
    sed -n "s/^set($2 \"\{0,1\}\([^\")]*\)\"\{0,1\})\$/\1/p" "$1" | tail -n 1
}

FIX_CORES="$(frag_value "$FIX_FRAG" KICKOS_NUM_CORES)"
FIX_ARCH="$(frag_value "$FIX_FRAG" KICKOS_ARCH)"
FIX_FAMILY="$(frag_value "$FIX_FRAG" KICKOS_ARCH_FAMILY)"
FIX_MODEL="$(frag_value "$FIX_FRAG" KICKOS_MULTICORE_MODEL_SHARED)"
require_number "$FIX_CORES" "the core count the fixture resolves to"
require_literal "$FIX_ARCH" "the arch the fixture resolves to"
require_literal "$FIX_FAMILY" "the arch family the fixture resolves to"

if [ "$FIX_CORES" -le 1 ]; then
    fail "the fixture resolves to $FIX_CORES core(s), so it does not reach the predicate at
    all. It exists to ask for more than one core on an ineligible arch"
fi
if [ "$FIX_MODEL" != 1 ]; then
    fail "the fixture resolves KICKOS_MULTICORE_MODEL_SHARED=$FIX_MODEL, so it selects no
    shared kernel and the refusal it exists to trigger is keyed on something it does not ask
    for"
fi

# The path the module composes, both spellings: a family-less arch sits directly under arch/.
OPTIN="$SRC/arch/$FIX_FAMILY/$FIX_ARCH/smp.cmake"
if [ "$FIX_FAMILY" = "$FIX_ARCH" ]; then
    OPTIN="$SRC/arch/$FIX_ARCH/smp.cmake"
fi
if [ -f "$OPTIN" ]; then
    fail "arch '$FIX_ARCH' now ships $OPTIN, so it declares its own share of the predicate and
    the fixture is no longer ineligible. Point the fixture at an arch that declares nothing, or
    this gate reports a refusal that can never happen"
fi

resolve "$CONTROL_DC" "$TMP/ctl" \
    || fail "the control defconfig does not resolve: $(cat "$TMP/ctl.err" 2>/dev/null)"
CTL_CORES="$(frag_value "$TMP/ctl/kickos_config.cmake" KICKOS_NUM_CORES)"
require_number "$CTL_CORES" "the core count the control resolves to"
if [ "$CTL_CORES" -ne 1 ]; then
    fail "the control variant resolves to $CTL_CORES core(s), not 1, so the two arms differ
    in more than the clause under test"
fi
echo "smp_predicate: fixture armed ($FIXTURE_BOARD/$FIXTURE_VARIANT: arch '$FIX_ARCH',
  $FIX_CORES cores, shared model, no $OPTIN); control resolves to $CTL_CORES core"

# Identical but for the variant, and both stop at the configure step: apps and ctest
# registration are off, so the control does not pull the test layer in behind it.
configure() { # <variant> <build-dir> <log>
    "$CMAKE" -S "$SRC" -B "$2" -G Ninja \
        -DKICKOS_BOARD="$FIXTURE_BOARD" \
        -DKICKOS_CONFIG_VARIANT="$1" \
        -DKICKOS_BUILD_TESTS=OFF \
        -DKICKOS_BUILD_APPS=OFF >"$3" 2>&1
}

if ! configure "$CONTROL_VARIANT" "$TMP/bctl" "$TMP/ctl.cfg.log"; then
    sed -n '1,40p' "$TMP/ctl.cfg.log" >&2
    fail "the control configure FAILED at one core, so this box cannot configure a child
    tree at all and the refusal arm below would pass for the wrong reason"
fi
echo "smp_predicate: control configured ($FIXTURE_VARIANT's twin at $CTL_CORES core)"

if configure "$FIXTURE_VARIANT" "$TMP/bfix" "$TMP/fix.cfg.log"; then
    sed -n '1,40p' "$TMP/fix.cfg.log" >&2
    fail "$FIX_CORES cores on arch '$FIX_ARCH' were ACCEPTED. The arch declares none of the
    properties one kernel image across cores requires, so the configure must refuse"
fi

# CMake re-wraps a message() body across lines and doubles the blank after a full stop, so
# every phrase is matched against the log folded to one space-separated line.
cp "$TMP/fix.cfg.log" "$TMP/tree.log"
FLAT="$(flat "$TMP/tree.log")"
[ -n "$FLAT" ] || fail "the refused configure produced no output at all, so there is no
    diagnostic to read and the non-zero exit could be anything"

# The count, the arch, and every one of the six with its owner. Nothing here is a phrase
# another FATAL_ERROR in the tree also prints.
want_in tree "KICKOS_NUM_CORES=$FIX_CORES under the SHARED-kernel model on arch '$FIX_ARCH'" \
    "the core count, the model and the arch it refused"
for p in $ARCH_PROPS; do
    want_in tree "KICKOS_ARCH_SMP_${p}" "the arch-owned property $p as undeclared"
done
for p in $CHIP_PROPS; do
    want_in tree "KICKOS_CHIP_SMP_${p}" "the part-owned property $p as undeclared"
done
want_in tree "the ARCH's" "that the arch-owned properties are the arch's"
want_in tree "the PART's" "that the part-owned properties are the part's"
want_in tree "arch/$FIX_ARCH/smp.cmake" "the arch declaration file it looked for"

echo "PASS: $FIX_CORES cores on arch '$FIX_ARCH' refused in a whole-tree configure naming the
  count, the model, the arch and all $((N_ARCH + N_CHIP)) properties with their owners, while
  the same board at $CTL_CORES core configures; and over synthetic trees the part case refuses
  on its own, BOTH directions of overreach refuse, and one core and the AMP model are both
  accepted"
exit 0
