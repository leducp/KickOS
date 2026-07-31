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
. "$(dirname "$0")/lib/gate.sh"

ELF="$1"
NM="$2"
OBJCOPY="$3"
shift 3
# remaining args ($@) are the four kernel-owned archives

[ -f "$ELF" ] || fail "ELF not found: $ELF"

scratch_dir

# --- app-ctor window bounds (the section symbols the split defines) -----------
START="$("$NM" "$ELF" | awk '$3=="__kickos_app_init_array_start"{print $1}')"
END="$(  "$NM" "$ELF" | awk '$3=="__kickos_app_init_array_end"  {print $1}')"
if [ -z "$START" ] || [ -z "$END" ]; then
  fail "ELF has no __kickos_app_init_array_{start,end} -- not an enforced ELF with the ctor split (wrong target?)"
fi
SDEC=$((0x$START))
EDEC=$((0x$END))
[ "$EDEC" -ge "$SDEC" ] || fail "app window end (0x$END) is below start (0x$START) -- corrupt ELF"

# 4 bytes per pointer: registered on armv7m only (see the CMakeLists guard).
APP_ENTRIES=$(((EDEC - SDEC) / 4))
echo "== app-ctor window [0x$START, 0x$END) : $APP_ENTRIES entr(y/ies) =="

# The pointer words a ctor array actually holds, thumb bit cleared, sorted unique.
# The byte count is reconciled against the entry count the window symbols imply:
# objcopy exits 0 and writes ZERO bytes for a section that does not exist, so a linker
# script that renamed the OUTPUT section while keeping the symbols would leave every
# assertion below reading an empty file (i.e. vacuously satisfied) while the entry
# counts printed above still looked healthy.
ctor_targets() { # <section> <entries> <outfile>
  "$OBJCOPY" -O binary --only-section="$1" "$ELF" "$TMP/sect.bin" \
    || fail "objcopy could not extract $1"
  NREAD=$(( $(wc -c < "$TMP/sect.bin") / 4 ))
  [ "$NREAD" -eq "$2" ] \
    || fail "$1: the window symbols span $2 entr(y/ies) but objcopy read $NREAD -- output section renamed or NOBITS"
  od -An -tx4 "$TMP/sect.bin" | tr ' ' '\n' | grep -E '^[0-9a-fA-F]{8}$' | while read -r W; do
    printf '%x\n' $((0x$W & ~1))
  done | sort -u > "$3"
}

# --- kernel-owned ctor NAMES (from the four closed-set archives) --------------
: > "$TMP/kctors.txt"
for A in "$@"; do
  [ -f "$A" ] || fail "kernel archive not found: $A"
  "$NM" --defined-only "$A" 2>/dev/null \
    | awk '$3 ~ /^_GLOBAL__sub_[ID]/ {print $3}' >> "$TMP/kctors.txt"
done
sort -u "$TMP/kctors.txt" -o "$TMP/kctors.txt"
require_nonempty "$TMP/kctors.txt" \
  "collected zero kernel ctors from the archives (wrong archive paths?): the guard below would pass vacuously"
KCOUNT=$(wc -l < "$TMP/kctors.txt")
echo "== $KCOUNT kernel-owned global-ctor name(s) across the closed archive set =="

# --- kernel-owned ctor addresses as they landed in the final ELF --------------
# (only those still present after --gc-sections matter; mask the thumb bit).
"$NM" "$ELF" | awk '$3 ~ /^_GLOBAL__sub_[ID]/ {print $1, $3}' > "$TMP/elf_ctors.txt"
require_nonempty "$TMP/elf_ctors.txt" \
  "the ELF carries no _GLOBAL__sub_I ctor at all: both windows below would be vacuous"
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

PRIV_ENTRIES=$(((PEDEC - PSDEC) / 4))
echo "== privileged-ctor window [0x$PSTART, 0x$PEND) : $PRIV_ENTRIES entr(y/ies) =="

ctor_targets .init_array "$PRIV_ENTRIES" "$TMP/priv_targets.txt"
require_nonempty "$TMP/priv_targets.txt" \
  ".init_array decoded to zero pointer words although it spans $PRIV_ENTRIES entr(y/ies)"

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
# On every board wired to this gate today the app window is EMPTY: --gc-sections drops
# every app/libstdc++ ctor and only the two kernel ones survive, privileged. Skipping
# the leg on that basis is how it stayed dead, so it is not skipped: an empty window is
# extracted and reconciled like any other (0 entries must read 0 bytes), and assertion 3
# below then carries the claim. The day an app ctor survives, this leg engages by itself.
: > "$TMP/app_targets.txt"
if [ "$APP_ENTRIES" -gt 0 ]; then
  ctor_targets .kickos_app_init_array "$APP_ENTRIES" "$TMP/app_targets.txt"
  require_nonempty "$TMP/app_targets.txt" \
    ".kickos_app_init_array decoded to zero pointer words although it spans $APP_ENTRIES entr(y/ies)"
fi

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

echo "PASS: no kernel-owned ctor is in the app-ctor window ($APP_ENTRIES entr(y/ies))"

# =============================================================================
# Assertion 3 (REACHABILITY): every surviving ctor is in one of the two windows.
# =============================================================================
# What makes the empty app window a claim rather than an early-out. --gc-sections keeps
# a _GLOBAL__sub_I only because a KEEP'd array entry roots it, so one that survives in
# the image while appearing in NEITHER array is a ctor that will never run: the linker
# script grew a third bucket, or dropped the entry and kept the code.
ORPHAN=""
while read -r ADDR NAME; do
  EVEN="$(printf '%x\n' $((0x$ADDR & ~1)))"
  if grep -qxF "$EVEN" "$TMP/priv_targets.txt"; then
    continue
  fi
  if grep -qxF "$EVEN" "$TMP/app_targets.txt"; then
    continue
  fi
  ORPHAN="$ORPHAN
  $NAME (0x$ADDR)"
done < "$TMP/elf_ctors.txt"

if [ -n "$ORPHAN" ]; then
  echo "FAIL: global ctor(s) survive in the image but are in NEITHER init array --" >&2
  echo "      nothing will ever run them. A third .init_array-like bucket in the chip" >&2
  echo "      linker script, or an entry dropped while its code was kept.$ORPHAN" >&2
  exit 1
fi

echo "PASS: all $(wc -l < "$TMP/elf_ctors.txt" | tr -d ' ') surviving ctor(s) are reachable from one of the two windows"
