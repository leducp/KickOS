#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# PSP-BOUNDS gate: boot a `pspguard` image and assert the switcher REFUSED to save a
# thread through a PSP with no room below it for the {r4-r11, EXC_RETURN} block.
#
# The refusal is a panic, so the absence of one is the failure, and the arm's own announce is
# checked first: a boot that never reached the arm otherwise reads the same as one whose PSP
# was accepted.
#
# <need> is passed in, because only the reported byte count says which figure fired: 36 is the
# switcher's plain push, 100 is that push with the FP callee block under it, and the SVC site
# reports its whole extent, the push plus the depth syscall_dispatch reaches while running
# privileged in thread mode on the same PSP.
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
