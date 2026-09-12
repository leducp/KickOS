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
#   R_X86_64_TPOFF / _DTP / _GOTTPOFF / _TLSGD / _TLSLD / _TLSDESC / _GOTPC32_   x86_64
# plus an UNDEFINED reference to the three ABI entry points a thread_local can reach through
# (__aeabi_read_tp, __emutls_get_address, __tls_get_addr), which catches an access the
# relocation scan would miss because the linker had not resolved it yet.
#
# An arch that grows a relocation spelling passes here VACUOUSLY until its name is added, which
# is why the per-arch set is spelled out above and not globbed.
#
# THE x86_64 ENTRIES ARE PREFIXES, NOT NAMES, AND THE GREP IS A SUBSTRING MATCH. A narrow
# `readelf -r` truncates the type column at 17 characters, so R_X86_64_REX_GOTPCRELX prints as
# R_X86_64_REX_GOTP and a full-name pattern misses every long spelling. Each prefix above is
# short enough to survive that cut. `R_X86_64_GOTPC32_` keeps its trailing underscore on
# purpose: it is what separates the TLS R_X86_64_GOTPC32_TLSDESC, truncated or not, from the
# ordinary R_X86_64_GOTPC32.
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

READELF="${1:?usage: check_no_privileged_tls.sh <readelf> <nm> <archive|archive::member>...}"
shift
NM="${1:?usage: check_no_privileged_tls.sh <readelf> <nm> <archive|archive::member>...}"
shift

command -v "$READELF" >/dev/null 2>&1 || fail "readelf not found: $READELF"
command -v "$NM" >/dev/null 2>&1 || fail "nm not found: $NM"
[ "$#" -gt 0 ] || fail "no archives given, so every check below would pass vacuously"

TLS_RELOCS='R_ARM_TLS_LE32|R_RISCV_TPREL_HI20|R_RISCV_TPREL_LO12_I|R_RISCV_TPREL_LO12_S|R_RISCV_TPREL_ADD|R_XTENSA_TLS_TPOFF|R_AARCH64_TLSLE_ADD_TPREL_HI12|R_AARCH64_TLSLE_ADD_TPREL_LO12|R_AARCH64_TLSLE_ADD_TPREL_LO12_NC|R_AARCH64_TLSLE_MOVW_TPREL_G0|R_AARCH64_TLSLE_MOVW_TPREL_G0_NC|R_AARCH64_TLSLE_MOVW_TPREL_G1|R_AARCH64_TLSLE_MOVW_TPREL_G1_NC|R_AARCH64_TLSLE_MOVW_TPREL_G2|R_AARCH64_TLSLE_LDST8_TPREL_LO12|R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC|R_AARCH64_TLSLE_LDST16_TPREL_LO12|R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC|R_AARCH64_TLSLE_LDST32_TPREL_LO12|R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC|R_AARCH64_TLSLE_LDST64_TPREL_LO12|R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC|R_X86_64_TPOFF|R_X86_64_DTP|R_X86_64_GOTTPOFF|R_X86_64_TLSGD|R_X86_64_TLSLD|R_X86_64_TLSDESC|R_X86_64_GOTPC32_'
# The RX psABI prefixes a C identifier with an extra leading underscore, so C's
# __emutls_get_address is asm ___emutls_get_address. Without the optional underscore this gate
# is VACUOUS on rxv3, where the relocation leg can never fire anyway: GNURX emits no TLS
# relocations at all, the emutls fallback being ordinary calls.
TLS_CALLS='_?__aeabi_read_tp|_?__emutls_get_address|_?__tls_get_addr'

# THE TYPE COLUMN ONLY, NOT THE WHOLE LINE. readelf's listing is "Offset Info Type Sym.Value
# Sym. Name [+ Addend]" on every arch this gate reads; the Sym. Name field is caller-chosen and
# can equal a TLS spelling by coincidence (a decoy symbol targeted by an ordinary relocation),
# so matching the raw line flags a relocation that never touches a thread pointer. Field 3 is
# the Type column; scope every match there.
reloc_type_hits() {
    awk -v re="$TLS_RELOCS" '$3 ~ re { print }'
}

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
    if ! printf '%s\n' "$line" | reloc_type_hits | grep -q .; then
        fail "the relocation leg does not report a planted $r line, so that spelling is
    listed and unenforced, which reads exactly like a clean archive"
    fi
    CTL_RELOC_N=$((CTL_RELOC_N + 1))
done
# EXACT, not a margin. The loop above draws its cases FROM TLS_RELOCS, so a spelling deleted
# there deletes its own control with it and the pass reads the same; only a pinned count turns
# that back into a refusal. Adding a spelling means moving this number.
[ "$CTL_RELOC_N" -eq 29 ] \
    || fail "$CTL_RELOC_N relocation control(s) ran, expected 29. TLS_RELOCS gained or lost a
    spelling: a lost one takes its own control with it and the scan then passes vacuously on
    that relocation."

# THE REAL SPELLINGS, AND WHAT A NARROW READER LEAVES OF THEM. The loop above plants the
# pattern's own entries, so it cannot tell a prefix that covers a real relocation from one that
# covers nothing. These are the names binutils actually prints, each also cut to the 17
# characters `readelf -r` leaves, and both forms must report.
CTL_TRUNC_N=0
for r in R_X86_64_TPOFF32 R_X86_64_TPOFF64 R_X86_64_DTPMOD64 R_X86_64_DTPOFF32 \
         R_X86_64_DTPOFF64 R_X86_64_GOTTPOFF R_X86_64_TLSGD R_X86_64_TLSLD \
         R_X86_64_TLSDESC R_X86_64_TLSDESC_CALL R_X86_64_GOTPC32_TLSDESC; do
    for form in "$r" "$(printf '%s' "$r" | cut -c1-17)"; do
        line="0000000000000018  000000090000002a $form 0000000000000000 kos_ctl_var + 0"
        if ! printf '%s\n' "$line" | reloc_type_hits | grep -q .; then
            fail "the relocation leg does not report [$form], the spelling binutils prints for
    $r. x86_64 privileged text would carry a thread-local access and read clean."
        fi
        CTL_TRUNC_N=$((CTL_TRUNC_N + 1))
    done
done
[ "$CTL_TRUNC_N" -eq 22 ] || fail "$CTL_TRUNC_N x86_64 spelling control(s) ran, expected 22"

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
# R_X86_64_GOTPC32 and R_X86_64_REX_GOTP are the two that decide whether the prefixes above
# over-match: the first differs from the TLS R_X86_64_GOTPC32_ by one trailing underscore, and
# the second is what a narrow reader leaves of the ordinary R_X86_64_REX_GOTPCRELX.
for r in R_ARM_ABS32 R_AARCH64_ADR_PREL_PG_HI21 R_RISCV_PCREL_HI20 R_XTENSA_SLOT0_OP \
         R_X86_64_PC32 R_X86_64_GOTPC32 R_X86_64_REX_GOTPCRELX R_X86_64_REX_GOTP \
         R_X86_64_GOTPCRELX R_X86_64_GOTPCREL R_X86_64_PLT32 R_X86_64_64; do
    line="0000000000000018  000000090000019b $r 0000000000000000 kos_ctl_var + 0"
    if printf '%s\n' "$line" | reloc_type_hits | grep -q .; then
        fail "the relocation leg reports the ordinary relocation $r, so it would report
    every archive in the tree"
    fi
done

# THE DECOY. A relocation's TARGET can be named after a TLS spelling without the relocation
# itself being one (readelf's Sym. Name column is caller-chosen); the match has to fail here,
# and its mirror below has to still catch a genuine TLS relocation aimed at a boring name.
line="0000000000000018  000000090000019b R_X86_64_PC32 0000000000000000 R_X86_64_TLSGD_decoy + 0"
if printf '%s\n' "$line" | reloc_type_hits | grep -q .; then
    fail "the relocation leg reports [$line], an ordinary R_X86_64_PC32 relocation whose
    target happens to be named after a TLS spelling. The match reads the whole line instead
    of the Type column, so an ordinary reference is misread as thread-local."
fi
line="0000000000000018  000000090000019b R_X86_64_TLSGD 0000000000000000 kos_ordinary_name + 0"
if ! printf '%s\n' "$line" | reloc_type_hits | grep -q .; then
    fail "the relocation leg does not report [$line], a genuine TLS relocation whose target
    has an unremarkable name. Scoping the match to the Type column must not blind it to a
    real hit."
fi

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

echo "== control: $CTL_RELOC_N relocation spelling(s), $CTL_TRUNC_N x86_64 full and truncated spelling(s) and $CTL_CALL_N entry-point spelling(s) report, 15 near misses stay silent (one a decoy target name), a real hit survives a benign target name, both slicers refuse an absent member =="

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
    hits="$(printf '%s\n' "$relocs" | reloc_type_hits || true)"
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
