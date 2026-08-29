#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No privileged TU may contain a thread-local access.
#
# The thread pointer is a property of the UNPRIVILEGED thread that is running. On ARM and RX
# it is derived from SP, so privileged code executing on a per-thread KERNEL stack masks down
# to a block that is not any thread's TLS; on RISC-V and Xtensa it is a register the kernel
# writes for the thread it is about to resume, so privileged code reads whatever the last
# resume left. None of those faults.
#
# It reads RELOCATIONS: a thread_local reached through an inline function in a header leaves no
# `thread_local` anywhere in the privileged .cc that emitted it.
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
# EVERY ARCHIVE IS COUNTED BEFORE IT IS JUDGED. Both tools print a header line per member, so
# the number of members each enumerates is what says the archive was read at all. The two
# counts must agree as well as be nonzero, one tool reading an archive the other could not
# being the same hole one level in.
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

# BOTH TOOLS BELOW HAVE THEIR OUTPUT READ BACK, so the locale is part of the contract. A French
# binutils prints `Fichier:` where slice_readelf keys on `File:`, and the member is then absent
# from a slice that is never empty, so the whole scan is refused as vacuous with a message
# blaming a renamed translation unit. The relocation and symbol columns are untranslated, so
# nothing else here moves.
LC_ALL=C
export LC_ALL

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

# THE DETECTORS, BEFORE THEY ARE ASKED TO REPORT AN ABSENCE. Every leg here reads TEXT and this
# gate is handed no compiler, so the controls are planted tool output: one line per relocation
# spelling and one per ABI entry point, each of which must report, and benign lines of the same
# shapes, which must not.
CTL_RELOC_N=0
for r in $(printf '%s\n' "$TLS_RELOCS" | tr '|' ' '); do
    line="0000000000000018  000000090000019b $r 0000000000000000 kos_ctl_var + 0"
    if ! printf '%s\n' "$line" | grep -qE "$TLS_RELOCS"; then
        fail "the relocation leg does not report a planted $r line, so that spelling is
    listed and unenforced, which reads exactly like a clean archive"
    fi
    CTL_RELOC_N=$((CTL_RELOC_N + 1))
done
# EXACT, not a margin. The loop above draws its cases FROM TLS_RELOCS, so a spelling deleted
# there deletes its own control with it and the pass reads the same; only a pinned count turns
# that back into a refusal. Adding a spelling means moving this number.
[ "$CTL_RELOC_N" -eq 22 ] \
    || fail "$CTL_RELOC_N relocation control(s) ran, expected 22. TLS_RELOCS gained or lost a
    spelling: a lost one takes its own control with it and the scan then passes vacuously on
    that relocation."

CTL_CALL_N=0
for c in __aeabi_read_tp __emutls_get_address __tls_get_addr _bad; do
    for pfx in "" _; do
        line="                 U $pfx$c"
        if [ "$c" = "_bad" ]; then
            if printf '%s\n' "$line" | grep -qE "^ *U ($TLS_CALLS)$"; then
                fail "the call leg reports an undefined reference to $pfx$c, so it would
    report every archive in the tree"
            fi
        else
            if ! printf '%s\n' "$line" | grep -qE "^ *U ($TLS_CALLS)$"; then
                fail "the call leg does not report a planted undefined $pfx$c, so that entry
    point is listed and unenforced"
            fi
            CTL_CALL_N=$((CTL_CALL_N + 1))
        fi
    done
done
[ "$CTL_CALL_N" -eq 6 ] || fail "$CTL_CALL_N call control(s) ran, expected 6"

# The near misses. A DEFINED symbol of the same name is not a reference out of this archive,
# and a relocation of the same family that is not thread-local is not a hit.
for line in "0000000000000000 T __aeabi_read_tp" "                 U memcpy"; do
    if printf '%s\n' "$line" | grep -qE "^ *U ($TLS_CALLS)$"; then
        fail "the call leg reports [$line], so it does not read the symbol class"
    fi
done
for r in R_ARM_ABS32 R_AARCH64_ADR_PREL_PG_HI21 R_RISCV_PCREL_HI20 R_XTENSA_SLOT0_OP; do
    line="0000000000000018  000000090000019b $r 0000000000000000 kos_ctl_var + 0"
    if printf '%s\n' "$line" | grep -qE "$TLS_RELOCS"; then
        fail "the relocation leg reports the ordinary relocation $r, so it would report
    every archive in the tree"
    fi
done

# The two slicers, which are the membership oracle the member path rests on: a present member
# yields its lines, an absent one exits 1 rather than yielding an empty slice.
CTL_RE_TEXT="File: kos_ctl.a(alpha.o)
0000000000000018  000000090000019b R_ARM_ABS32 0000000000000000 a + 0
File: kos_ctl.a(beta.o)
0000000000000020  000000090000019b R_ARM_ABS32 0000000000000000 b + 0"
CTL_NM_TEXT="alpha.o:
                 U kos_ctl_a
beta.o:
                 U kos_ctl_b"

ctl_slice="$(printf '%s\n' "$CTL_RE_TEXT" | slice_readelf beta.o)" \
    || fail "slice_readelf refuses a member that IS in the planted output, so every member
    scan would refuse"
printf '%s\n' "$ctl_slice" | grep -q 'b + 0' \
    || fail "slice_readelf returned [$ctl_slice] for a member whose one line names b, so the
    slice it hands the greps is not that member's"
if printf '%s\n' "$CTL_RE_TEXT" | slice_readelf gamma.o >/dev/null 2>&1; then
    fail "slice_readelf accepts a member that is NOT in the planted output, so a renamed or
    deleted translation unit would scan an empty slice and pass"
fi
ctl_slice="$(printf '%s\n' "$CTL_NM_TEXT" | slice_nm beta.o)" \
    || fail "slice_nm refuses a member that IS in the planted output"
printf '%s\n' "$ctl_slice" | grep -q 'kos_ctl_b' \
    || fail "slice_nm returned [$ctl_slice] for a member whose one symbol is kos_ctl_b"
if printf '%s\n' "$CTL_NM_TEXT" | slice_nm gamma.o >/dev/null 2>&1; then
    fail "slice_nm accepts a member that is NOT in the planted output"
fi

echo "== control: $CTL_RELOC_N relocation spelling(s) and $CTL_CALL_N entry-point spelling(s) report, 6 near misses stay silent, both slicers refuse an absent member =="

FOUND=0
NARCHIVE=0
NMEMBER=0
NREAD=0
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
    # A TOOL THAT FAILED MUST NOT READ AS A CLEAN ARCHIVE: an empty variable matches no
    # pattern below, so swallowing the status passes every board silently.
    if ! relocs="$("$READELF" -rW "$a" 2>/dev/null)"; then
        fail "$READELF -rW failed on $a, so this scan read nothing and would pass vacuously"
    fi
    # `U` in nm's archive listing is an undefined reference from a member of THIS archive.
    if ! syms="$("$NM" "$a" 2>/dev/null)"; then
        fail "$NM failed on $a, so this scan read nothing and would pass vacuously"
    fi
    # A TOOL THAT SUCCEEDED AND PRINTED NOTHING IS THE SAME HOLE. Both tools emit a header per
    # member even for a member with no relocation and no symbol, so the header count is the
    # count of members actually enumerated.
    nre="$(printf '%s\n' "$relocs" | grep -c '^File: ' || true)"
    nnm="$(printf '%s\n' "$syms" | grep -cE '^[^ ]*:$' || true)"
    require_number "$nre" "the member count $READELF enumerated in $a"
    require_number "$nnm" "the member count $NM enumerated in $a"
    if [ "$nre" -eq 0 ] || [ "$nnm" -eq 0 ]; then
        fail "$a: $READELF enumerated $nre member(s) and $NM enumerated $nnm. An archive that
    reaches the scan with no member read is judged clean without being read."
    fi
    if [ "$nre" -ne "$nnm" ]; then
        fail "$a: $READELF enumerated $nre member(s) and $NM enumerated $nnm. One tool read
    an archive the other did not, so whichever leg saw fewer members asserts nothing about
    the rest."
    fi
    NREAD=$((NREAD + nre))
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

echo "PASS: $NARCHIVE privileged archive(s) and $NMEMBER archive member(s), $NREAD member(s) enumerated, carry no thread-local access"
