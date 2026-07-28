#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the enforced (KICKOS_HAVE_MPU) boot-ctor split. The chip linker
# scripts route ONLY the closed KickOS-owned archive set (libkickos_kernel.a /
# libkickos_arch_<arch>.a / libkickos_chip_<chip>.a / libkickos_lib.a) into
# .init_array -- Reset_Handler runs those before kmain -- and send every OTHER
# ctor (app / libstdc++ / libsupc++ / newlib / KickCAT) into .kickos_app_init_array,
# which root_entry runs LATER, kernel-live. That set is duplicated across 13 linker
# scripts; a future kernel-side archive whose ctor is NOT added to the set would
# silently fall into .kickos_app_init_array and run too late -- kmain would use an
# unconstructed kernel object. This gate catches exactly that regression.
#
# The gate enforces the partition in BOTH directions, because the two failures are
# different bugs:
#   1. privilege -- a NON-kernel ctor inside .init_array runs from Reset_Handler at
#      full privilege, ahead of kmain, i.e. app code executing inside the TCB. This
#      is what a regression to a monolithic KEEP(*(.init_array)) looks like.
#   2. ordering  -- a KERNEL ctor inside .kickos_app_init_array runs after kmain has
#      already used the object it constructs.
#
# The check works on POINTERS, not symbol addresses: each _GLOBAL__sub_I/_D ctor is
# a function in .text.startup; the two init_array sections hold POINTERS (thumb bit
# set) to those functions. So we read the pointer words inside the app window and
# assert none of them resolves to a ctor that came from a kernel archive.
#
# usage: check_kernel_ctor_placement.sh <elf> <nm> <objcopy> <kernel.a> <arch.a> <chip.a> <lib.a>

set -eu

ELF="$1"
NM="$2"
OBJCOPY="$3"
shift 3
# remaining args ($@) are the four kernel-owned archives

fail() { echo "FAIL: $1" >&2; exit 1; }

[ -f "$ELF" ] || fail "ELF not found: $ELF"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- app-ctor window bounds (the section symbols the split defines) -----------
START="$("$NM" "$ELF" | awk '$3=="__kickos_app_init_array_start"{print $1}')"
END="$(  "$NM" "$ELF" | awk '$3=="__kickos_app_init_array_end"  {print $1}')"
if [ -z "$START" ] || [ -z "$END" ]; then
  fail "ELF has no __kickos_app_init_array_{start,end} -- not an enforced ELF with the ctor split (wrong target?)"
fi
SDEC=$((0x$START))
EDEC=$((0x$END))
[ "$EDEC" -ge "$SDEC" ] || fail "app window end (0x$END) is below start (0x$START) -- corrupt ELF"

echo "== app-ctor window [0x$START, 0x$END) : $(((EDEC - SDEC) / 4)) entr(y/ies) =="

# --- kernel-owned ctor NAMES (from the four closed-set archives) --------------
: > "$TMP/kctors.txt"
for A in "$@"; do
  [ -f "$A" ] || fail "kernel archive not found: $A"
  "$NM" --defined-only "$A" 2>/dev/null \
    | awk '$3 ~ /^_GLOBAL__sub_[ID]/ {print $3}' >> "$TMP/kctors.txt"
done
sort -u "$TMP/kctors.txt" -o "$TMP/kctors.txt"
KCOUNT=$(wc -l < "$TMP/kctors.txt")
[ "$KCOUNT" -gt 0 ] || fail "collected zero kernel ctors from the archives -- wrong archive paths? (guard would pass vacuously)"
echo "== $KCOUNT kernel-owned global-ctor name(s) across the closed archive set =="

# --- kernel-owned ctor addresses as they landed in the final ELF --------------
# (only those still present after --gc-sections matter; mask the thumb bit).
"$NM" "$ELF" | awk '$3 ~ /^_GLOBAL__sub_[ID]/ {print $1, $3}' > "$TMP/elf_ctors.txt"
while read -r ADDR NAME; do
  if grep -qxF "$NAME" "$TMP/kctors.txt"; then
    printf '%x %s\n' $((0x$ADDR & ~1)) "$NAME"
  fi
done < "$TMP/elf_ctors.txt" > "$TMP/kernel_ctor_addrs.txt"

# =============================================================================
# Assertion 1 (PRIVILEGE BOUNDARY): nothing FOREIGN in the privileged window.
# =============================================================================
# The check above guards the "too late" direction. This guards the direction that
# matters for privilege: .init_array runs from Reset_Handler, fully privileged,
# before the kernel is even up -- so anything landing there is inside the TCB by
# construction. The linker partitions by input ARCHIVE, which an app cannot forge
# (naming a section .init_array.00099 still misses every archive selector). But a
# script that regresses to a monolithic `KEEP(*(.init_array))`, or that grows a
# fifth selector, would silently pull app ctors privileged. Assert the privileged
# window contains ONLY ctors from the closed archive set.
PSTART="$("$NM" "$ELF" | awk '$3=="__init_array_start"{print $1}')"
PEND="$(  "$NM" "$ELF" | awk '$3=="__init_array_end"  {print $1}')"
if [ -z "$PSTART" ] || [ -z "$PEND" ]; then
  fail "ELF has no __init_array_{start,end} -- cannot verify the privileged ctor window"
fi
PSDEC=$((0x$PSTART))
PEDEC=$((0x$PEND))
[ "$PEDEC" -ge "$PSDEC" ] || fail "privileged window end (0x$PEND) is below start (0x$PSTART)"

# An EMPTY privileged window means a selector matched nothing (renamed archive):
# every kernel ctor would then fall through to the app bucket and run too late.
# Sound HERE only: this gate runs on armv7m+MPU, where the two kernel ctors always
# survive --gc-sections. It is NOT a universal invariant, so do NOT lift it into the
# linker scripts as an ASSERT -- on the Xtensa esp32 port every ctor is legitimately
# collected and both windows are empty.
[ "$PEDEC" -gt "$PSDEC" ] \
  || fail "privileged .init_array is EMPTY -- an archive selector matched nothing (renamed kernel lib?); kernel ctors would run late"

echo "== privileged-ctor window [0x$PSTART, 0x$PEND) : $(((PEDEC - PSDEC) / 4)) entr(y/ies) =="

"$OBJCOPY" -O binary --only-section=.init_array "$ELF" "$TMP/priv.bin" \
  || fail "objcopy could not extract .init_array"
od -An -tx4 "$TMP/priv.bin" | tr ' ' '\n' | grep -E '^[0-9a-fA-F]{8}$' | while read -r W; do
  printf '%x\n' $((0x$W & ~1))
done | sort -u > "$TMP/priv_targets.txt"

FOREIGN=""
while read -r TGT; do
  if ! awk -v t="$TGT" '$1==t{found=1} END{exit !found}' "$TMP/kernel_ctor_addrs.txt"; then
    # Name it if we can, so the failure is actionable rather than a bare address.
    NAME="$(awk -v t="$TGT" '{a=strtonum("0x" $1); if (and(a, compl(1))==strtonum("0x" t)) print $2}' "$TMP/elf_ctors.txt" | head -1)"
    [ -n "$NAME" ] || NAME="$("$NM" "$ELF" | awk -v t="$TGT" 'tolower($1)==t{print $3}' | head -1)"
    [ -n "$NAME" ] || NAME="<unresolved>"
    FOREIGN="$FOREIGN
  $NAME (0x$TGT)"
  fi
done < "$TMP/priv_targets.txt"

if [ -n "$FOREIGN" ]; then
  echo "FAIL: non-kernel ctor(s) landed in the PRIVILEGED .init_array --" >&2
  echo "      that window runs from Reset_Handler with full privilege, before kmain," >&2
  echo "      so these entries are inside the TCB. Check that the chip linker script" >&2
  echo "      still partitions .init_array by ARCHIVE and has not regressed to a" >&2
  echo "      monolithic KEEP(*(.init_array)).$FOREIGN" >&2
  exit 1
fi
echo "PASS: privileged ctor window holds only closed-set kernel ctors"

# =============================================================================
# Assertion 2 (ORDERING): no kernel ctor in the late app window.
# =============================================================================
# Empty window: nothing can have leaked (a --gc-sections'd minimal app). PASS.
if [ "$SDEC" -eq "$EDEC" ]; then
  echo "PASS: app-ctor window empty -- no kernel ctor can be inside it"
  exit 0
fi

# --- pointer words actually stored in the app window --------------------------
# Extract just that section to a flat binary, read it as host-native (== target
# little-endian) 4-byte words, and clear the thumb bit so each equals the even
# function address nm reports.
"$OBJCOPY" -O binary --only-section=.kickos_app_init_array "$ELF" "$TMP/app.bin" \
  || fail "objcopy could not extract .kickos_app_init_array"
od -An -tx4 "$TMP/app.bin" | tr ' ' '\n' | grep -E '^[0-9a-fA-F]{8}$' | while read -r W; do
  printf '%x\n' $((0x$W & ~1))
done | sort -u > "$TMP/app_targets.txt"

# --- the assertion: no kernel ctor address appears in the app window ----------
LEAK=""
while read -r ADDR NAME; do
  if grep -qxF "$ADDR" "$TMP/app_targets.txt"; then
    LEAK="$LEAK
  $NAME (0x$ADDR)"
  fi
done < "$TMP/kernel_ctor_addrs.txt"

if [ -n "$LEAK" ]; then
  echo "FAIL: kernel-owned global ctor(s) landed in .kickos_app_init_array --" >&2
  echo "      these run in root_entry AFTER kmain, so kmain uses an unconstructed object." >&2
  echo "      Add the offending archive to the .init_array closed set in EVERY chip linker script.$LEAK" >&2
  exit 1
fi

echo "PASS: no kernel-owned ctor is in the app-ctor window (all run early, kernel-side)"
