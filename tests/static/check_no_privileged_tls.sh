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
# It reads RELOCATIONS rather than source: a thread_local reached through an inline function in
# a header leaves no `thread_local` anywhere in the privileged .cc that emitted it.
#
# What it looks for, per arch:
#   R_ARM_TLS_LE32                              armv7m, armv6m
#   R_RISCV_TPREL_HI20 / _LO12_I / _LO12_S / _ADD   rv32imac
#   R_XTENSA_TLS_TPOFF                          lx6
# plus an UNDEFINED reference to the three ABI entry points a thread_local can reach through
# (__aeabi_read_tp, __emutls_get_address, __tls_get_addr), which catches an access the
# relocation scan would miss because the linker had not resolved it yet.
#
# An arch that grows a fifth relocation spelling passes here VACUOUSLY until its name is added,
# which is why the per-arch set is spelled out above and not globbed.
#
# An argument spelled <archive>::<member> scans ONE member of that archive. It exists for an
# archive whose OTHER members are legitimately thread-local (libkickos_user.a: ordinary user
# runtime TLS is correct there) but which carries one privileged-adjacent TU. A member named
# here and absent from the archive is a HARD FAILURE, never a quiet pass: a renamed or dropped
# TU has to break the gate rather than empty it.
#
# usage: check_no_privileged_tls.sh <readelf> <nm> <archive|archive::member>...

set -eu
. "$(dirname "$0")/../lib/gate.sh"

READELF="${1:?usage: check_no_privileged_tls.sh <readelf> <nm> <archive|archive::member>...}"
shift
NM="${1:?usage: check_no_privileged_tls.sh <readelf> <nm> <archive|archive::member>...}"
shift

command -v "$READELF" >/dev/null 2>&1 || fail "readelf not found: $READELF"
command -v "$NM" >/dev/null 2>&1 || fail "nm not found: $NM"
[ "$#" -gt 0 ] || fail "no archives given, so every check below would pass vacuously"

TLS_RELOCS='R_ARM_TLS_LE32|R_RISCV_TPREL_HI20|R_RISCV_TPREL_LO12_I|R_RISCV_TPREL_LO12_S|R_RISCV_TPREL_ADD|R_XTENSA_TLS_TPOFF|R_AARCH64_TLSLE_ADD_TPREL_HI12|R_AARCH64_TLSLE_ADD_TPREL_LO12|R_AARCH64_TLSLE_ADD_TPREL_LO12_NC|R_AARCH64_TLSLE_MOVW_TPREL_G0|R_AARCH64_TLSLE_MOVW_TPREL_G0_NC|R_AARCH64_TLSLE_MOVW_TPREL_G1|R_AARCH64_TLSLE_MOVW_TPREL_G1_NC|R_AARCH64_TLSLE_MOVW_TPREL_G2|R_AARCH64_TLSLE_LDST8_TPREL_LO12|R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC|R_AARCH64_TLSLE_LDST16_TPREL_LO12|R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC|R_AARCH64_TLSLE_LDST32_TPREL_LO12|R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC|R_AARCH64_TLSLE_LDST64_TPREL_LO12|R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC'
# The RX psABI prefixes a C identifier with an extra leading underscore, so C's
# __emutls_get_address is asm ___emutls_get_address. Without the optional underscore this gate
# is VACUOUS on rxv3, where the relocation leg can never fire anyway: GNURX emits no TLS
# relocations at all, the emutls fallback being ordinary calls.
TLS_CALLS='_?__aeabi_read_tp|_?__emutls_get_address|_?__tls_get_addr'

# Both binutils emit a header line per member EVEN when the member has no relocation section
# and no symbol at all, so a member absent from that output is a member absent from the
# archive; that is what makes these two the membership oracle and keeps `ar` out of the
# argument list. Exit 1 when the header never appeared, so the caller can tell "member is
# clean" from "member is not there" (the two produce the same empty slice).
#
# readelf: "File: <archive>(<member>)". Compared as a fixed suffix, not as an ERE, because
# every member name here holds dots.
slice_readelf() {
    awk -v m="$1" '
        index($0, "File: ") == 1 {
            inmem = (substr($0, length($0) - length(m) - 1) == "(" m ")")
            if (inmem) { seen = 1 }
            next
        }
        inmem { print }
        END { if (! seen) { exit 1 } }'
}
# nm: a bare "<member>:" line. A symbol line always holds a space, so a space-free line
# ending in a colon is a header and nothing else.
slice_nm() {
    awk -v m="$1" '
        /^[^ ]*:$/ {
            inmem = ($0 == m ":")
            if (inmem) { seen = 1 }
            next
        }
        inmem { print }
        END { if (! seen) { exit 1 } }'
}

FOUND=0
NARCHIVE=0
NMEMBER=0
for spec in "$@"; do
    case "$spec" in
        *::*)
            a="${spec%::*}"
            member="${spec##*::}"
            what="$a($member)"
            NMEMBER=$((NMEMBER + 1))
            ;;
        *)
            a="$spec"
            member=""
            what="$a"
            NARCHIVE=$((NARCHIVE + 1))
            ;;
    esac
    [ -f "$a" ] || fail "no archive at $a"
    # A TOOL THAT FAILED MUST NOT READ AS A CLEAN ARCHIVE. Swallowing the status here left
    # both variables empty, which no pattern below matches, so a missing or broken readelf
    # passed every board silently.
    if ! relocs="$("$READELF" -rW "$a" 2>/dev/null)"; then
        fail "$READELF -rW failed on $a, so this scan read nothing and would pass vacuously"
    fi
    # `U` in nm's archive listing is an undefined reference from a member of THIS archive.
    if ! syms="$("$NM" "$a" 2>/dev/null)"; then
        fail "$NM failed on $a, so this scan read nothing and would pass vacuously"
    fi
    if [ -n "$member" ]; then
        missing="$a has no member $member, so this scan would be vacuous. A renamed or
    deleted translation unit must update the no_privileged_tls argument in arch/CMakeLists.txt."
        # `|| fail` puts the assignment in a condition, so set -e does not abort ahead of it.
        relocs="$(printf '%s\n' "$relocs" | slice_readelf "$member")" || fail "$missing"
        syms="$(printf '%s\n' "$syms" | slice_nm "$member")" || fail "$missing"
    fi
    hits="$(printf '%s\n' "$relocs" | grep -E "$TLS_RELOCS" || true)"
    if [ -n "$hits" ]; then
        echo "$what carries a thread-local relocation:"
        printf '%s\n' "$hits" | head -20 | sed 's/^/    /'
        FOUND=1
    fi
    calls="$(printf '%s\n' "$syms" | grep -E "^ *U ($TLS_CALLS)$" || true)"
    if [ -n "$calls" ]; then
        echo "$what references a thread-pointer ABI entry point:"
        printf '%s\n' "$calls" | sort -u | sed 's/^/    /'
        FOUND=1
    fi
done

if [ "$FOUND" -ne 0 ]; then
    fail "privileged code reaches thread-local storage. The thread pointer belongs to
    the unprivileged thread that is running: on ARM and RX it is derived from SP, so kernel
    code on a per-thread kernel stack masks down to no thread's block, and on RISC-V and
    Xtensa it is whatever the last resume wrote. Move the state to the thread's own memory
    or to a per-slot kernel array indexed by kickos::Kernel::threads.index_of()."
fi

echo "PASS: $NARCHIVE privileged archive(s) and $NMEMBER archive member(s) carry no thread-local access"
