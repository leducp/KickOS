#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# PSP-BOUNDS gate: boot a `pspguard` image and assert the guarded site REFUSED to write
# through a PSP with no room below it for the {r4-r11, EXC_RETURN} block.
#
# The refusal is a panic, so the absence of one is the failure, and the arm's own announce is
# checked first: a boot that never reached the arm otherwise reads the same as one whose PSP
# was accepted.
#
# MODE 5 IS THE OPPOSITE CLAIM and has its own clause set below. Its PSP is parked with
# exactly the room the SVC site asks for, which svc_trampoline's transfer onto the caller's
# kernel block makes legal, so what this gate requires there is an ACCEPTANCE plus an intact
# poison band under stack_lo, ordered after the acceptance so a compiled-out readback cannot
# pass. <need>, <site> and <why> are unused on that arm; they stay in the argv so a call site
# cannot become the wrong arm by dropping arguments.
#
# <need> is passed in, because only the reported byte count says which figure fired: 36 is the
# switcher's plain push, 100 is that push with the FP callee block under it, and the SVC site
# reports its whole extent, the trampoline's scratch push plus the exception pair that can
# preempt the window it spends on this PSP before it reaches the caller's kernel block.
#
# <site> comes from ICSR.VECTACTIVE, read by the refusal itself. Both guarded pushes print
# through one helper, so only the named exception separates "the switcher refused" from "the
# syscall trap refused", and a guard installed in one alone leaves the other open.
#
# <why> pins WHICH BOUND refused: the helper classifies from the values rather than from the
# branch that fired, so "under stack_lo" and "at or above stack_hi" are reachable only by arms
# that no in-stack PSP can produce.
#
# <banner> names the backend whose fault reporter must print the refusal: each spells its own
# arch into the panic line (kickos_armv7m_bad_psp -> ARMV7M, kickos_armv6m_bad_psp ->
# ARMV6M). Carried by the caller and not matched loosely, so an image whose refusal came out
# of another backend's reporter fails here instead of passing on the shared wording.
#
# usage: check_pspguard.sh <pspguard.elf> <mode> <need> <site> <why> <banner>

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_pspguard.sh <pspguard.elf> <mode> <need-bytes> <site> <why> <banner>"
elf="${1:?$_usage}"
mode="${2:?$_usage}"
need="${3:?$_usage}"
site="${4:?$_usage}"
why="${5:?$_usage}"
banner="${6:?$_usage}"

need_qemu_machine
run_image "$elf"

if has "\[pspguard\] ERROR"; then
    fail "pspguard reported a failed arm (the PSP was accepted, or the arm never ran)"
fi
# The SP write must have landed: a core that ignored it leaves the thread on a healthy PSP
# and every assertion below is about nothing.
if ! has "\[pspguard\] ok - sp low bits read back as 0"; then
    fail "SP did not read back word-aligned: the SP writes in this image are not landing"
fi
if ! has "\[pspguard\] arm: mode=$mode "; then
    fail "the wild thread never announced arm mode=$mode (faulted earlier?)"
fi

# First matching line number, so the two mode-5 clauses can be ordered against each other.
line_of() { printf '%s\n' "$OUT" | grep -nE "$1" | head -n1 | cut -d: -f1; }

if [ "$mode" = 5 ]; then
    accepted="$(line_of "\\[pspguard\\] accepted: the syscall trap ran on the low-edge sp")"
    if [ -z "$accepted" ]; then
        fail "mode=5: the low-edge sp was never accepted, so the SVC site refused a legal sp"
    fi
    assert_no_panic "the syscall ran on the low-edge sp AND the system panicked"
    # The band, and BOTH directions are clauses. Corrupted names the privileged writes that
    # went under the parked sp; a missing verdict line means root reached the readback and
    # printed neither, which is the silent arm this pair exists to refuse.
    if has "lowband\] CORRUPTED"; then
        fail "mode=5: the syscall dispatch ran below the parked sp, through a PSP a thread chose"
    fi
    intact="$(line_of "\\[pspguard\\] \\[lowband\\] INTACT")"
    if [ -z "$intact" ]; then
        fail "mode=5: root printed no band verdict, so nothing here witnessed the band at all:
    the readback is compiled out, or this is not the mode 5 image"
    fi
    if [ "$intact" -le "$accepted" ]; then
        fail "mode=5: the band verdict at $intact is not after the acceptance at $accepted,
    so it was read before the syscall it is meant to judge"
    fi
    if [ "$RC" -ne 0 ]; then
        fail "mode=5: expected a clean exit 0 once root printed the verdict, got $RC"
    fi
    echo "PASS: mode=5 accepted the low-edge sp at line $accepted and the band was intact at $intact"
    exit 0
fi

if ! has "=== $banner EXCEPTION (wild PSP: $why) ==="; then
    fail "no $banner refusal classified '$why' reached the wire: the PSP was accepted, or another bound fired"
fi
if ! has "in $site PSP=" ; then
    fail "the refusal did not come from $site: the other guarded push is the one that fired"
fi
if ! has "need=$need "; then
    fail "the refusal reported a byte count other than need=$need (wrong leg, or the FP block was not measured)"
fi

echo "PASS: mode=$mode refused in $site as '$why' with need=$need"
exit 0
