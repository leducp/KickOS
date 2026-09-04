#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the isolated-core mask's configure-time refusals (cmake/isolated_cores.cmake).
#
# Two refusals, each guaranteeing a different scope, and each PAIRED WITH A CONTROL differing
# in one clause:
#
#   core 0        the boot core may not be isolated, so THE SYSTEM always has somewhere to run.
#                 Without it a mask naming every core produces an image that boots and then
#                 schedules nothing, which is configured rather than programmed.
#   undriven      a bit at or above the kernel's core count names nothing, so it could never be
#                 satisfied by any grant. Refused, never masked off.
#
# The module is driven in SCRIPT MODE against the SAME function the root CMakeLists calls, so a
# refusal that stopped being wired to the build is caught by the whole-tree arm and a refusal
# whose CONDITION rotted is caught here.
#
# Run from anywhere, no build directory:
#   tests/static/check_isolated_cores.sh <src-dir> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 2 ]; then
    fail "usage: check_isolated_cores.sh <src-dir> <cmake>"
fi
SRC="$(cd "$1" && pwd)" || fail "no source tree at $1"
CMAKE="$2"

MODULE="$SRC/cmake/isolated_cores.cmake"
[ -f "$MODULE" ] || fail "no module at $MODULE. It is the one authority this gate drives, and
    without it every arm below would be testing nothing"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"

# The root lists file must actually CALL it. A module nothing invokes refuses nothing, and
# every arm below would still pass.
grep -q 'kickos_isolated_cores_check' "$SRC/CMakeLists.txt" \
    || fail "the root CMakeLists.txt does not call kickos_isolated_cores_check, so the
    refusals this gate exercises are wired to nothing"

scratch_dir

DRIVER="$TMP/drive.cmake"
cat >"$DRIVER" <<EOF
list(APPEND CMAKE_MODULE_PATH "$SRC/cmake")
include(isolated_cores)
kickos_isolated_cores_check(
  ISOLATED     "\${IC_MASK}"
  KERNEL_CORES "\${IC_CORES}"
  ORIGIN       "the fixture")
message(STATUS "ISOLATED-ACCEPTED")
EOF

rc_of() { # <mask> <cores> -> writes $TMP/out, returns cmake's status
    "$CMAKE" "-DIC_MASK=$1" "-DIC_CORES=$2" -P "$DRIVER" >"$TMP/out" 2>&1
}

# <label> <mask> <cores> <phrase the refusal must name>
expect_refusal() {
    if rc_of "$2" "$3"; then
        fail "$1: mask $2 on $3 core(s) was ACCEPTED. $(cat "$TMP/out")"
    fi
    if grep -q 'ISOLATED-ACCEPTED' "$TMP/out"; then
        fail "$1: the module reached its accept marker despite failing"
    fi
    grep -q "$4" "$TMP/out" \
        || fail "$1: refused, but the message does not name '$4', so an operator is told
    that something is wrong without being told what. Got: $(cat "$TMP/out")"
    echo "isolated_cores: REFUSED $1 (mask $2, $3 core(s))"
}

# <label> <mask> <cores>
expect_accept() {
    if ! rc_of "$2" "$3"; then
        fail "$1: mask $2 on $3 core(s) was REFUSED and should not be. $(cat "$TMP/out")"
    fi
    grep -q 'ISOLATED-ACCEPTED' "$TMP/out" \
        || fail "$1: exited 0 without reaching the accept marker, so the module returned
    early and this control asserts nothing"
    echo "isolated_cores: accepted $1 (mask $2, $3 core(s))"
}

# --- The boot core -------------------------------------------------------------------
expect_refusal "core 0 isolated"        0x1 4 "core 0"
expect_refusal "core 0 among others"    0x9 4 "core 0"
# The control: the SAME mask with bit 0 cleared, which is the one clause that differs.
expect_accept  "core 3 alone"           0x8 4

# --- A core the kernel does not schedule ----------------------------------------------
expect_refusal "bit at the core count"  0x10 4 "does not schedule"
expect_refusal "bit above the count"    0x4  2 "does not schedule"
# The control: the same bit on a kernel wide enough to name it.
expect_accept  "bit 4 on eight cores"   0x10 8

# --- Nothing isolated, which is the default and must stay buildable --------------------
expect_accept  "the empty mask"         0x0 1
expect_accept  "the empty mask, wide"   0x0 4

echo "isolated_cores: OK"
