#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The address-space fault gate on x86_64: run the `aspacefault` image, in which the kernel maps
# a page into the running space, unmaps it and reads it at ring 0, and assert the exception
# report names that page with the error code a MISSING page gives at ring 0.
#
# CR2 must be the page the kernel announced, so a fault somewhere else cannot stand in, and the
# decoded error code must be present=0 write=0 user=0: a read at ring 0 of a page that is not
# present (Intel SDM Vol 3, 4.7). present=1 would mean the leaf survived the unmap. This port's
# ring-0 exception report ends by halting, not by an exit status, so the gate polls for it.
set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_aspace_fault_x86_64.sh <aspacefault.efi> <dump-marker>"
elf="${1:?$_usage}"
marker="${2:?$_usage}"
require_literal "$marker" "the exception-report marker"

need_qemu_machine
poll_image "$elf" "$marker" "descriptors halting" > /dev/null
if has "ERROR:"; then
    printf '%s\n' "$OUT" | grep 'ERROR:'
    fail "the image reported a failure instead of faulting"
fi
if [ "$POLL_OK" -ne 1 ]; then
    printf '%s\n' "$OUT"
    fail "no exception report reached the wire within ${QEMU_TIMEOUT}s"
fi

page="$(printf '%s\n' "$OUT" | sed -n 's/.*\[aspace\] unmapped 0x\([0-9a-f]*\),.*/\1/p' | tail -n 1)"
if [ -z "$page" ]; then
    fail "the kernel never announced the page it unmapped, so no CR2 comparison is possible"
fi
cr2="$(printf '%s\n' "$OUT" | sed -n 's/.*CR2=0x\([0-9a-f]*\).*/\1/p' | tail -n 1)"
if [ -z "$cr2" ] || [ "$(printf '%d' "0x$cr2")" != "$(printf '%d' "0x$page")" ]; then
    printf '%s\n' "$OUT" | grep -E 'CR2=|unmapped' || :
    fail "the report faults at 0x${cr2:-?}, not at 0x${page}, the page the kernel unmapped"
fi
require_on_wire "pf: present=0 write=0 user=0" \
    "the fault is not a ring-0 read of a page that is not present"
echo "PASS: 0x${page} faulted not-present on a ring-0 read after the unmap"
