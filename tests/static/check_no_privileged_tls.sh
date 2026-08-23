#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NO PRIVILEGED TU MAY CONTAIN A THREAD-LOCAL ACCESS.
#
# The thread pointer is a property of the UNPRIVILEGED thread that is running. On ARM and RX
# it is derived from SP, so privileged code executing on a per-thread KERNEL stack would mask
# down to a block that is not any thread's TLS; on RISC-V and Xtensa it is a register the
# kernel writes for the thread it is about to resume, so privileged code reads whatever the
# last resume left. Every one of those is a wrong answer that looks like a right one, and
# none of them faults.
#
# So the rule is not "be careful in the kernel", it is that the kernel may not do this at
# all, and this gate is what makes that true rather than intended. It reads RELOCATIONS
# rather than source, because a thread_local reached through an inline function in a header
# leaves no `thread_local` anywhere in the privileged .cc file that emitted it.
#
# What it looks for, per arch:
#   R_ARM_TLS_LE32                              armv7m, armv6m
#   R_RISCV_TPREL_HI20 / _LO12_I / _LO12_S / _ADD   rv32imac
#   R_XTENSA_TLS_TPOFF                          lx6
# plus an UNDEFINED reference to the three ABI entry points a thread_local can reach through
# (__aeabi_read_tp, __emutls_get_address, __tls_get_addr), which catches an access the
# relocation scan would miss because the linker had not resolved it yet.
#
# NOT A SUBSTITUTE FOR THE SAME RULE IN REVIEW: an arch that grows a fifth relocation
# spelling passes here vacuously until its name is added, which is why the per-arch set is
# spelled out above and not globbed.
#
# usage: check_no_privileged_tls.sh <readelf> <nm> <archive>...

set -eu
. "$(dirname "$0")/../lib/gate.sh"

READELF="${1:?usage: check_no_privileged_tls.sh <readelf> <nm> <archive>...}"
shift
NM="${1:?usage: check_no_privileged_tls.sh <readelf> <nm> <archive>...}"
shift

command -v "$READELF" >/dev/null 2>&1 || fail "readelf not found: $READELF"
command -v "$NM" >/dev/null 2>&1 || fail "nm not found: $NM"
[ "$#" -gt 0 ] || fail "no archives given, so every check below would pass vacuously"

TLS_RELOCS='R_ARM_TLS_LE32|R_RISCV_TPREL_HI20|R_RISCV_TPREL_LO12_I|R_RISCV_TPREL_LO12_S|R_RISCV_TPREL_ADD|R_XTENSA_TLS_TPOFF'
TLS_CALLS='__aeabi_read_tp|__emutls_get_address|__tls_get_addr'

FOUND=0
for a in "$@"; do
    [ -f "$a" ] || fail "no archive at $a"
    hits="$("$READELF" -rW "$a" 2>/dev/null | grep -E "$TLS_RELOCS" || true)"
    if [ -n "$hits" ]; then
        echo "$a carries a thread-local relocation:"
        printf '%s\n' "$hits" | head -20 | sed 's/^/    /'
        FOUND=1
    fi
    # `U` in nm's archive listing is an undefined reference from a member of THIS archive.
    calls="$("$NM" "$a" 2>/dev/null | grep -E "^ *U ($TLS_CALLS)$" || true)"
    if [ -n "$calls" ]; then
        echo "$a references a thread-pointer ABI entry point:"
        printf '%s\n' "$calls" | sort -u | sed 's/^/    /'
        FOUND=1
    fi
done

if [ "$FOUND" -ne 0 ]; then
    fail "a privileged archive reaches thread-local storage. The thread pointer belongs to
    the unprivileged thread that is running: on ARM and RX it is derived from SP, so kernel
    code on a per-thread kernel stack masks down to no thread's block, and on RISC-V and
    Xtensa it is whatever the last resume wrote. Move the state to the thread's own memory
    or to a per-slot kernel array indexed by kickos::Kernel::threads.index_of()."
fi

echo "PASS: $# privileged archive(s) carry no thread-local access"
