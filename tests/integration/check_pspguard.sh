#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# PSP-BOUNDS gate: boot a `pspguard` image and assert the guarded site REFUSED to write
# through a PSP with no room below it for the {r4-r11, EXC_RETURN} block.
#
# The absence of a refusal is the failure, and the arm's own announce is checked first: a boot
# that never reached the arm otherwise reads the same as one whose PSP was accepted.
#
# <outcome> IS WHAT THE REFUSAL COSTS, and it is carried by the caller because it is a per-
# backend fact and not something to sniff: `contained` says the offending thread alone dies
# and root outlives it, `terminated` says the whole system ends. A backend gaining containment
# changes its call site, so this gate never silently accepts the weaker outcome.
#
# MODE 5 IS THE OPPOSITE CLAIM and has its own clause set below. Its PSP is parked with
# exactly the room the SVC site asks for, which svc_trampoline's transfer onto the caller's
# kernel block makes legal, so what this gate requires there is an ACCEPTANCE plus an intact
# poison band under stack_lo, ordered after the acceptance so a compiled-out readback cannot
# pass. <need>, <site>, <why> and <outcome> are unused on that arm; they stay in the argv so a
# call site cannot become the wrong arm by dropping arguments.
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
# usage: check_pspguard.sh <pspguard.elf> <mode> <need> <site> <why> <banner> <outcome>

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_pspguard.sh <pspguard.elf> <mode> <need-bytes> <site> <why> <banner> <outcome>"
elf="${1:?$_usage}"
mode="${2:?$_usage}"
need="${3:?$_usage}"
site="${4:?$_usage}"
why="${5:?$_usage}"
banner="${6:?$_usage}"
outcome="${7:?$_usage}"

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

# The noun is the outcome: CONTAINED is spelled so it does NOT match tests/lib/panic.ere.
_noun=CONTAINED
if [ "$outcome" = terminated ]; then
    _noun=EXCEPTION
fi
if ! has "=== $banner $_noun (wild PSP: $why) ==="; then
    fail "no $banner refusal classified '$why' reached the wire as $outcome: the PSP was accepted, another bound fired, or the outcome is not the declared one"
fi
if ! has "in $site PSP=" ; then
    fail "the refusal did not come from $site: the other guarded push is the one that fired"
fi
if ! has "need=$need "; then
    fail "the refusal reported a byte count other than need=$need (wrong leg, or the FP block was not measured)"
fi

# MODE 6 IS THE BLOCK LEG'S ROOM BOUND. The worker parks its PSP inside its OWN kernel block,
# deeper than the room the leg reserves, which only a board that carves blocks and does not
# enforce lets a thread reach. BOTH directions are clauses: CORRUPTED names the switcher's
# callee block going through the block's lowest word into the neighbouring slot, and a missing
# verdict means root never reached the readback.
if [ "$mode" = 6 ]; then
    if has "kcanary\] CORRUPTED"; then
        fail "mode=6: the block leg accepted a PSP with no room under it and the switcher saved
    through the block's canary"
    fi
    if ! has "\[pspguard\] \[kcanary\] INTACT"; then
        fail "mode=6: no kernel-canary verdict reached the wire, so nothing here witnessed the
    block's lowest word: the worker never parked, or this is not the mode 6 image"
    fi
fi

# MODE 1 CARRIES A SECOND CLAIM, and it is the one the plain FP arm could not make: that the
# offender's DEFERRED lazy FP save was discarded rather than completed. The app poisons the
# {s0-s15,FPSCR} reservation inside the frame the refused entry allocated, then an innocent
# thread executes FP after containment, which is what fires a save left armed. BOTH directions
# are clauses: CORRUPTED names the write going through the offender's own pointer, and a
# missing verdict means nothing read the reservation at all.
if [ "$mode" = 1 ]; then
    if has "lazyfp\] CORRUPTED"; then
        fail "mode=1: a lazy FP save fired through the frame the guard refused, so FPCCR.LSPACT
    survived containment and the offender still steered a privileged write"
    fi
    if ! has "\[pspguard\] \[lazyfp\] INTACT"; then
        fail "mode=1: no lazy-FP verdict reached the wire, so nothing here witnessed the
    reservation: the checker never ran, or this is not the mode 1 image"
    fi
fi

contained="\[pspguard\] contained: the wild thread was slain and root outlived it"
if [ "$outcome" = contained ]; then
    if ! has "$contained"; then
        fail "the refusal was not contained: root never ran again, so the whole system paid
    for one thread's pointer"
    fi
    if [ "$RC" -ne 0 ]; then
        fail "root outlived the refusal and the image still exited $RC"
    fi
elif [ "$outcome" = terminated ]; then
    if has "$contained"; then
        fail "$banner contained the refusal, which is stronger than the '$outcome' this call
    site declares: the call site is stale, not the backend"
    fi
    if [ "$RC" -eq 0 ]; then
        fail "the refusal ended the system and the image still exited 0"
    fi
else
    fail "unknown outcome '$outcome': expected 'contained' or 'terminated'"
fi

echo "PASS: mode=$mode refused in $site as '$why' with need=$need, outcome $outcome"
exit 0
