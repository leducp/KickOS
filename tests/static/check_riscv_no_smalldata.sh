#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Security CI gate for RISC-V full-C++ under PMP (docs/design-riscv-gp-split.md,
# Option 4). The gp small-data window (__global_pointer$ +/- 0x800) is anchored
# INSIDE the .appdata grant, so it is reachable by an unprivileged thread. That is
# only safe if the window holds NOTHING kernel-owned: the KickOS libs are built
# -msmall-data-limit=0 so they emit no .sdata/.sbss and their globals land in the
# kernel-side .data/.bss instead. One stray KickOS global left in .sdata/.sbss would
# sit in the app-granted window: a privilege-escalation vector (an unprivileged
# thread could overwrite g_arch_current / g_clint_msip). This gate fails loud on any
# such leak, per-archive, before it can ship.
#
# Section-level via objdump -h: with -msmall-data-limit=0 no KickOS object may carry
# a .sdata/.sdata2/.sbss section. (nm is NOT used: this toolchain's nm reports .sdata
# symbols with the same 'D'/'B' type as .data/.bss, so a symbol-type check would pass
# vacuously. Verified: section headers are the reliable signal.)
#
# usage: check_riscv_no_smalldata.sh <objdump> <kernel.a> <arch.a> <chip.a> <lib.a>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

OBJDUMP="$1"
shift
# remaining args ($@) are the KickOS-owned archives

command -v "$OBJDUMP" >/dev/null 2>&1 || fail "objdump not found: $OBJDUMP"
[ "$#" -gt 0 ] || fail "no archives given (guard would pass vacuously)"

scratch_dir
leaks=""
for A in "$@"; do
  [ -f "$A" ] || fail "archive not found: $A"
  # Positive control: objdump reads a text file named *.a with a diagnostic and no
  # section table at all, and the absence-assertion below would call that clean. Every
  # KickOS archive carries code, so a run that saw no .text saw nothing.
  tool_out "$TMP/sect" '[[:space:]]\.text' "$OBJDUMP" -h "$A"
  # objdump -h prints a section table per archive member. The section name is field 2
  # on the numbered rows; match anything beginning .sdata (covers .sdata/.sdata.*/
  # .sdata2*) or .sbss (covers .sbss/.sbss.*). ^\. anchors so .data/.bss never match.
  hits="$(awk '$2 ~ /^\.sdata/ || $2 ~ /^\.sbss/ { print $2 }' "$TMP/sect" | sort -u)"
  if [ -n "$hits" ]; then
    leaks="$leaks
  $(basename "$A"): $(echo "$hits" | tr '\n' ' ')"
  fi
done

if [ -n "$leaks" ]; then
  echo "FAIL: KickOS RISC-V archive(s) emit gp small-data (.sdata/.sbss):" >&2
  echo "      these land in the app-granted gp window (privilege-escalation vector)." >&2
  echo "      Ensure every KickOS RISC-V lib is built -msmall-data-limit=0.$leaks" >&2
  exit 1
fi

echo "PASS: $# KickOS RISC-V archive(s) carry zero .sdata/.sbss small-data"
