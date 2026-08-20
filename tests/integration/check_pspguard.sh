#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# PSP-BOUNDS gate: boot a `pspguard` image and assert the switcher REFUSED to save a
# thread through a PSP with no room below it for the {r4-r11, EXC_RETURN} block.
#
# The refusal is a panic, so the absence of one is the failure. That is why the arm's own
# announce is checked first: without it a boot that died in ctors would satisfy nothing and
# a boot that never reached the arm would look the same as one whose PSP was accepted.
#
# <need> is the caller's, not this script's to sniff. Only the reported byte count says
# which figure fired: 36 is the switcher's plain push, 100 is that push with the FP callee
# block under it, and the SVC site reports its whole extent, the push plus the depth
# syscall_dispatch reaches while running privileged in thread mode on the same PSP.
#
# <site> is the same kind of expectation, read from ICSR.VECTACTIVE by the refusal itself:
# both guarded pushes print through one helper, so only the named exception separates "the
# switcher refused" from "the syscall trap refused", and a guard installed in one alone
# leaves the other open.
#
# <why> is the third expectation, and the one that pins WHICH BOUND refused: the helper
# classifies from the values rather than from the branch that fired, so "under stack_lo" and
# "at or above stack_hi" are reachable only by arms that no in-stack PSP can produce.
#
# usage: check_pspguard.sh <pspguard.elf> <mode> <need> <site> <why>

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_pspguard.sh <pspguard.elf> <mode> <need-bytes> <site> <why>"
elf="${1:?$_usage}"
mode="${2:?$_usage}"
need="${3:?$_usage}"
site="${4:?$_usage}"
why="${5:?$_usage}"

need_qemu_machine
run_image "$elf"

if has "\[pspguard\] ERROR"; then
    fail "pspguard reported a failed arm (the PSP was accepted, or the arm never ran)"
fi
# The SP write must have landed at all: a core that ignored it would leave the thread on a
# healthy PSP and every assertion below would be about nothing.
if ! has "\[pspguard\] ok - sp low bits read back as 0"; then
    fail "SP did not read back word-aligned: the SP writes in this image are not landing"
fi
if ! has "\[pspguard\] arm: mode=$mode "; then
    fail "the wild thread never announced arm mode=$mode (faulted earlier?)"
fi
if ! has "=== ARMV7M EXCEPTION (wild PSP: $why) ==="; then
    fail "no refusal classified '$why' reached the wire: the PSP was accepted, or another bound fired"
fi
if ! has "in $site PSP=" ; then
    fail "the refusal did not come from $site: the other guarded push is the one that fired"
fi
if ! has "need=$need "; then
    fail "the refusal reported a byte count other than need=$need (wrong leg, or the FP block was not measured)"
fi

echo "PASS: mode=$mode refused in $site as '$why' with need=$need"
exit 0
