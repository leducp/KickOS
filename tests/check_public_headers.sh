#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every INSTALLED header must compile standalone at the language level the package
# ADVERTISES, not the one the kernel is built with: the kernel compiles at C++20, the
# exported targets carry cxx_std_17. A C++20 construct in a public header compiles under
# the kernel's own flags here and fails only in the consumer's build.
#
# Usage: check_public_headers.sh <install-prefix> <c++-compiler> <std>
#
# Standalone also means self-contained: a header that only compiles after some other
# header has been included is a header whose include list is wrong.

set -u
. "$(dirname "$0")/lib/gate.sh"

PREFIX="${1:?usage: check_public_headers.sh <prefix> <cxx> <std>}"
CXX="${2:?}"
STD="${3:?}"

scratch_dir

INC="$PREFIX/include"
[ -d "$INC" ] || fail "no include directory in the package at $PREFIX"

# The usage requirements a consumer inherits from the target, minus the generator
# expressions CMake resolves per configuration. An ARRAY, not a joined string: a shell
# that does not word-split would pass the whole thing as one unrecognised argument,
# dropping every define and firing board.h's missing-chip-header #error instead.
DEFS=(-Dmain=kickos_app_main -D__KickOS__=1
      -DKICKOS_TELEMETRY=0 -DKICKOS_TELEMETRY_RTT=0 -DKICKOS_TRACE_ARCH=0
      -DKICKOS_HAVE_MPU=1 -DKICKOS_DEBUG=0)
# A sim package ships no chip_limits.h, and config/board.h refuses to guess an IRQ count
# without one. The real consumer gets this from the target's INTERFACE definitions.
if [ ! -f "$INC/kickos/chip_limits.h" ]; then
    DEFS+=(-DKICKOS_ARCH_SIM=1)
fi

n=0
bad=0
for h in $(cd "$INC" && find kickos -name '*.h' | sort); do
    n=$((n + 1))
    if ! echo "#include <$h>" | "$CXX" -std="$STD" -fsyntax-only "${DEFS[@]}" \
         -I"$INC" -x c++ - 2>"$TMP/hdr.err"; then
        bad=$((bad + 1))
        echo "FAIL $h"
        head -4 "$TMP/hdr.err"
    fi
done

[ "$n" -gt 0 ] || fail "no headers found under $INC, so this gate proved nothing"
[ "$bad" -eq 0 ] || fail "$bad of $n installed header(s) do not compile at $STD"
echo "PASS: $n installed headers compile standalone at $STD"
