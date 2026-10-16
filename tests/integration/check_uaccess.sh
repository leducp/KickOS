#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The uaccess witness: ring 0 never reaches a user page except through the kaccess seam, and
# the hardware is what says so. A kernel built with KICKOS_WITNESS_UACCESS answers
# KOS_SYS_WITNESS_UACCESS by touching a word of the caller's own data from ring 0; this gate
# builds that kernel in a scratch tree of the preset under test, boots one `uaccess_<arm>` image
# per access, and requires each to end in the panic dump of a privileged fault at the address the
# image named, with the fault class the guard in force gives.
#
#   read       a read on the fresh entry from user mode
#   write      a write on the fresh entry
#   park       a read after a sleep has parked the caller and resumed it, through the frame its
#              own switch built: where the guard is a bit an exception return restores, a frame
#              the kernel builds without it drops the guard there and nowhere else
#   spaceless  a read under the root a core installs when no space is its own
#   fault      a read in the fault-exit stub, which runs on the frame the kill redirect built
#   fetch      a call into user text on the fresh entry (x86_64)
#   irq        a read in a timer interrupt taken from user mode by a thread that set RFLAGS.AC
#              first, which delivery keeps and which lifts SMAP (x86_64)
#
# On x86_64 a CONTROL follows: the read arm on the same processor model less SMEP and SMAP, where
# the banner must say `none` and the kernel must reach the word. It is what shows the faults
# above are the guard's and not the plant's.
#
# A RETURN IS THE GUARD MISSING. The image then prints an ERROR line with what the kernel read
# or wrote, and that line is refused first.
#
# THE POSTURE IS THE PRESET'S, NOT THE RUN'S. The caller names the guard this machine must run
# under, and the banner's `uaccess` line must name it: a core that silently took a weaker form
# would otherwise pass on the fault class that form gives.
#
# AN ARCH JOINS BY A JUDGE: judge_<arch> below, which reads its own dump. The scratch tree and
# the arms are shared.
#
# Usage: check_uaccess.sh <source-dir> <cmake> <preset> <arch> <posture> <scratch-dir>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_uaccess.sh <source-dir> <cmake> <preset> <arch> <posture> <scratch-dir>"
SRC="${1:?$_usage}"
CMAKE="${2:?$_usage}"
PRESET="${3:?$_usage}"
ARCH="${4:?$_usage}"
POSTURE="${5:?$_usage}"
SCRATCH="${6:?$_usage}"

need_qemu_machine
scratch_dir

ARMS="read write park spaceless fault"
if [ "$ARCH" = x86_64 ]; then
    ARMS="$ARMS fetch irq"
fi

# --- the witness kernel -------------------------------------------------------------------
# Kept under the caller's build directory, so no other checkout or preset shares it. A cache
# that names another source tree, or lacks the knob, is not this witness's kernel.
#
# The knob is refused without the marker only this script passes: such a kernel reads and
# writes user memory on request. The control below holds that refusal.
MARKER=check_uaccess.sh
if "$CMAKE" -S "$SRC" -B "$TMP/refused" --preset "$PRESET" -DKICKOS_WITNESS_UACCESS=ON \
       -DKICKOS_BUILD_TESTS=OFF > "$TMP/refused.log" 2>&1; then
    fail "a configure with KICKOS_WITNESS_UACCESS and no marker was accepted"
fi
if ! grep -q 'WITNESS_UACCESS_REFUSED' "$TMP/refused.log"; then
    sed -n '1,30p' "$TMP/refused.log" >&2
    fail "the unmarked configure failed, but not on the refusal"
fi
_cache="$SCRATCH/CMakeCache.txt"
_reuse=0
if [ -f "$_cache" ] && grep -q '^KICKOS_WITNESS_UACCESS:BOOL=ON$' "$_cache" \
   && grep -qxF -e "KICKOS_WITNESS_UACCESS_SCRATCH:UNINITIALIZED=$MARKER" "$_cache" \
   && grep -qxF -e "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$_cache"; then
    _reuse=1
fi
if [ "$_reuse" -eq 0 ]; then
    rm -rf "$SCRATCH"
    "$CMAKE" -S "$SRC" -B "$SCRATCH" --preset "$PRESET" -DKICKOS_WITNESS_UACCESS=ON \
        -DKICKOS_WITNESS_UACCESS_SCRATCH="$MARKER" -DKICKOS_BUILD_TESTS=OFF > "$TMP/cfg.log" 2>&1 \
        || { sed -n '1,40p' "$TMP/cfg.log" >&2; fail "configure of the witness kernel failed for $PRESET"; }
fi
_targets=""
for _arm in $ARMS; do
    _targets="$_targets uaccess_$_arm"
done
# shellcheck disable=SC2086
"$CMAKE" --build "$SCRATCH" --target $_targets > "$TMP/build.log" 2>&1 \
    || { grep -E 'error' "$TMP/build.log" | sed -n '1,20p' >&2; fail "build of the witness images failed in $SCRATCH"; }

# --- what every arm owes --------------------------------------------------------------------
# Sets ADDR, the word the image named.
arm_common() { # <arm> <verb>
    if has_f "[uaccess] INCONCLUSIVE"; then
        printf '%s\n' "$OUT" | grep -F -e '[uaccess] INCONCLUSIVE'
        fail "$1: inconclusive: the interrupt came before the image set the flag it tests"
    fi
    if has_f "[uaccess] ERROR"; then
        printf '%s\n' "$OUT" | grep -F -e '[uaccess] ERROR'
        _rc="$(printf '%s\n' "$OUT" | sed -n 's/.*\[uaccess\] ERROR: .*: rc \(-[0-9]*\),.*/\1/p' | tail -n 1)"
        if [ -n "$_rc" ]; then
            fail "$1: the kernel refused the witness call (rc $_rc), so nothing was witnessed"
        fi
        fail "$1: ring 0 reached the user page and the image ran on: no guard stopped it"
    fi
    ADDR="$(printf '%s\n' "$OUT" | sed -n 's/.*\[uaccess\] op [0-9]* names 0x\([0-9a-f]*\).*/\1/p' | tail -n 1)"
    if [ -z "$ADDR" ]; then
        printf '%s\n' "$OUT"
        fail "$1: the image named no address"
    fi
    require_on_wire "[uaccess] ring 0 $2 0x$ADDR" \
        "$1: the kernel did not announce its access to 0x$ADDR, so the dump below may be another fault's"
    require_on_wire "   witness uaccess" \
        "$1: the banner does not say this is the witness kernel"
    require_on_wire "   uaccess $POSTURE" \
        "$1: the banner does not name the guard this machine runs under, $POSTURE"
    if [ "$RC" -ne 132 ]; then
        fail "$1: expected the panic status 132, got $RC"
    fi
}

# --- the per-arch judges ----------------------------------------------------------------------
# ESR_EL1 is spelled out, not derived: EC 0x25 (data abort, current EL) and IL, then WnR for a
# write, then the fault status. PAN gives a permission fault at the level-3 leaf (0x0F); the
# empty root gives a translation fault at its first level (0x05), whatever the guard, which is
# why the spaceless arm expects 0x05 on both.
judge_armv8a() { # <arm>
    _verb=reads
    _wnr=0
    if [ "$1" = write ]; then
        _verb=writes
        _wnr=64
    fi
    arm_common "$1" "$_verb"
    _dfsc=5
    if [ "$POSTURE" = pan ] && [ "$1" != spaceless ]; then
        _dfsc=15
    fi
    _esr="$(printf '0x%x' $((0x96000000 + _wnr + _dfsc)))"
    require_single_marker "=== ARMV8A EXCEPTION ===" "$1: the access did not fault"
    require_on_wire "vector=SP_EL1 sync" "$1: the fault was not taken from EL1"
    field_matcher_control
    require_field_on_wire FAR "0x$ADDR" 'FAR=|ESR=' "$1: the fault is at another address than 0x$ADDR"
    require_field_on_wire ESR "$_esr" 'ESR=' "$1: the syndrome is not $_esr"
    echo "PASS: $1: ring 0 $_verb 0x$ADDR faulted at EL1 under $POSTURE, ESR $_esr"
}

# scause 13 or 15 from supervisor mode, and sstatus.SUM (bit 18) clear in the trapped state.
judge_rv64imac() { # <arm>
    _verb=reads
    _cause=0xd
    _what="load page fault"
    if [ "$1" = write ]; then
        _verb=writes
        _cause=0xf
        _what="store page fault"
    fi
    arm_common "$1" "$_verb"
    require_single_marker "=== RISC-V S-TRAP ($_what) ===" "$1: the access did not fault"
    require_on_wire "taken from supervisor-mode" "$1: the fault was not taken from S-mode"
    field_matcher_control
    require_field_on_wire stval "0x$ADDR" 'stval=' "$1: the fault is at another address than 0x$ADDR"
    require_field_on_wire scause "$_cause" 'scause=' "$1: the cause is not $_cause"
    _sstatus="$(printf '%s\n' "$OUT" | sed -n 's/.*stval=0x[0-9a-f]* sstatus=0x\([0-9a-f]*\).*/\1/p' | tail -n 1)"
    [ -n "$_sstatus" ] || fail "$1: the dump carries no sstatus"
    if [ $((0x$_sstatus & 0x40000)) -ne 0 ]; then
        fail "$1: sstatus.SUM was set when ring 0 faulted (sstatus 0x$_sstatus)"
    fi
    echo "PASS: $1: ring 0 $_verb 0x$ADDR faulted in S-mode with SUM clear, scause $_cause"
}

# Vector 14 from ring 0, CR2 the word named, and the error code spelled out (Intel SDM Vol 3,
# section 5.7): present and supervisor (1), plus write (2) or instruction fetch (0x10). The
# spaceless arm runs on the firmware's root, which maps nothing in the user half, so its read
# is NOT present (0) whatever the guard.
judge_x86_64() { # <arm>
    _verb=reads
    _err=1
    case "$1" in
        write) _verb=writes; _err=3 ;;
        fetch) _verb=fetches; _err=17 ;;
        spaceless) _err=0 ;;
    esac
    arm_common "$1" "$_verb"
    require_single_marker "=== X86_64 EXCEPTION ===" "$1: the access did not fault"
    require_on_wire "vector=14 (page fault)" "$1: the fault is not a page fault"
    require_on_wire "  ring=0 " "$1: the fault was not taken at ring 0"
    _cr2="$(printf '%s\n' "$OUT" | sed -n 's/^  CR2=0x\([0-9a-f]*\)$/\1/p' | tail -n 1)"
    _got="$(printf '%s\n' "$OUT" | sed -n 's/^  error=0x\([0-9a-f]*\)$/\1/p' | tail -n 1)"
    [ -n "$_cr2" ] || fail "$1: the dump carries no CR2"
    [ -n "$_got" ] || fail "$1: the dump carries no error code"
    if [ $((0x$_cr2)) -ne $((0x$ADDR)) ]; then
        fail "$1: the fault is at 0x$_cr2, not 0x$ADDR"
    fi
    if [ $((0x$_got)) -ne "$_err" ]; then
        fail "$1: the error code is 0x$_got, not $(printf '0x%x' "$_err")"
    fi
    echo "PASS: $1: ring 0 $_verb 0x$ADDR faulted under $POSTURE, error $(printf '0x%x' "$_err")"
}

case "$ARCH" in
    armv8a|rv64imac|x86_64) ;;
    *) fail "no uaccess judge for $ARCH" ;;
esac

# x86_64 links a PE32+ application, which carries its suffix.
_suffix=""
if [ "$ARCH" = x86_64 ]; then
    _suffix=.efi
fi
for _arm in $ARMS; do
    echo "== uaccess_$_arm =="
    run_image "$SCRATCH/user/apps/common/uaccess/uaccess_$_arm$_suffix"
    "judge_$ARCH" "$_arm"
done

if [ "$ARCH" = x86_64 ]; then
    echo "== uaccess_read, control: the model less SMEP and SMAP =="
    for _f in smep smap; do
        case ",${KICKOS_X86_64_CPU:-}," in
            *",+$_f,"*) ;;
            *) fail "control: the processor model offers no $_f to take away" ;;
        esac
    done
    KICKOS_X86_64_CPU="$(printf '%s' "$KICKOS_X86_64_CPU" | sed 's/,+smep//; s/,+smap//')"
    export KICKOS_X86_64_CPU
    run_image "$SCRATCH/user/apps/common/uaccess/uaccess_read$_suffix"
    require_on_wire "   uaccess none" "control: the banner does not say the guard is absent"
    _reached="$(printf '%s\n' "$OUT" \
        | sed -n 's/.*\[uaccess\] ERROR: ring 0 reached 0x[0-9a-f]*: rc \([0-9][0-9]*\),.*/\1/p' | tail -n 1)"
    if [ -z "$_reached" ]; then
        printf '%s\n' "$OUT" | grep -F -e '[uaccess]' || :
        fail "control: with neither guard the kernel did not reach the word, so the faults above may be the plant's"
    fi
    echo "PASS: control: with neither guard ring 0 reached the word (read 0x$(printf '%x' "$_reached"))"
fi
echo "PASS: every ring-0 access to a user page faulted ($ARMS)"
exit 0
