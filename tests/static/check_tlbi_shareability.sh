#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# TLB maintenance shareability on AArch64, read out of the LINKED IMAGE. The oracle is the
# image's own instruction stream.
#
# THE SHAREABILITY IS IN THE ENCODING, NOT IN THE MNEMONIC, so this gate DECODES it out of the
# raw 32-bit word and never greps the disassembler's text. ARM ARM DDI 0487 M.b, C5.1.2: a
# system instruction is bits[31:22]=0b1101010100, bit[21]=L, bits[20:19]=op0, bits[18:16]=op1,
# bits[15:12]=CRn, bits[11:8]=CRm, bits[7:5]=op2, bits[4:0]=Rt. D8.17.5 (rule IDMCXY) gives an
# operation the name {R}<type><regime><shareability>{NXS}: `IS` applies to all the TLBs of the
# Inner Shareable domain, and NO shareability component applies only to the TLBs of the PE that
# executes it. C5.5 puts both halves of that pair in CRm alone: TLBI VAAE1 is op0=0b01 op1=0b000
# CRn=0b1000 CRm=0b0111 op2=0b011 and TLBI VAAE1IS is the same with CRm=0b0011; VMALLE1 and
# VMALLE1IS pair the same way at op2=0b000. So CRm=7 is LOCAL and CRm=3 is BROADCAST. Any other
# CRm is a shareability this gate was not written to judge, and is refused rather than passed.
#
# The oracle is the image because QEMU does not model a stale TLB entry: a runtime arm stays
# green with the maintenance loop bounds wrong, and green with the operation left local on a
# multi-core image.
#
# arch_aspace_activate is asserted LOCAL at EVERY core count: a root change concerns the PE whose
# register changed, so a uniform edit that turned the whole file broadcast must redden this gate.
#
# AN EMPTY CORPUS IS A FAILURE, not a pass. Zero TLBI decoded image-wide, or zero reached from
# any one of the four roots, says the parse or the encoding matcher moved, and that is UNKNOWN.
# The per-root count and every decoded CRm are printed on success so a reader sees the corpus.
#
# usage: check_tlbi_shareability.sh <elf> <expect-kernel-cores> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_tlbi_shareability.sh <elf> <expect-kernel-cores> <nm> <objdump>"
elf="${1:?$_usage}"
cores="${2:?$_usage}"
nm="${3:?$_usage}"
objdump="${4:?$_usage}"

# The arch seam's four entry points. Named as symbols, resolved out of the image.
ROOTS="arch_aspace_map arch_aspace_unmap arch_aspace_destroy arch_aspace_activate"
# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100
# The transitive walk's bound, per root.
WALK_CAP=100000

require_number "$cores" "the expected kernel core count"
if [ "$cores" -lt 1 ]; then
    fail "the expected kernel core count is $cores; an image is built at one core or more"
fi
[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the symbol table, and the four roots in it -------------------------------
tool_out "$TMP/nm" "[0-9a-fA-F]" "$nm" -S --defined-only "$elf"
require_nonempty "$TMP/nm" "$nm printed no symbol at all for $elf, so the corpus is UNKNOWN
  rather than empty and every verdict below it would be vacuous"
syms="$(wc -l < "$TMP/nm" | tr -d ' ')"
require_number "$syms" "the defined-symbol count"
if [ "$syms" -lt "$SYM_FLOOR" ]; then
    fail "$nm reports $syms defined symbol(s) in $elf, below the floor of $SYM_FLOOR. A table
  that short is a misread, not a small image, and the corpus is UNKNOWN"
fi

for root in $ROOTS; do
    kind="$(awk -v s="$root" '$NF == s { print NF; exit }' "$TMP/nm")"
    if [ -z "$kind" ]; then
        fail "no defined symbol '$root' in $elf. The seam's entry point was renamed, made
  static or inlined away, so this gate has nothing to walk from and reports an absence it
  cannot tell apart from a failure to read"
    fi
    size="$(awk -v s="$root" 'NF == 4 && $4 == s { print $2; exit }' "$TMP/nm")"
    if [ -z "$size" ]; then
        fail "'$root' is defined in $elf but carries no size, so its body cannot be bounded"
    fi
    case "$size" in
        *[!0]*) ;;
        *) fail "'$root' has size 0 in $elf: the symbol survived as a label but its body is
  gone, so no TLBI can be reached from it" ;;
    esac
done

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

# Records out of one pass: TOTAL <n>, BAD <crm> <word> <sym>, MISSING <root>, OVERRUN <root>,
# and ROOT <root> <sym> <crm> <word> for every TLBI the walk from <root> reaches.
awk -F'\t' -v roots="$ROOTS" -v cap="$WALK_CAP" '
    function hex2num(s,   i, c, v, n) {
        n = 0
        for (i = 1; i <= length(s); i++) {
            c = substr(s, i, 1)
            v = index("0123456789abcdef", c) - 1
            if (v < 0) {
                return -1
            }
            n = n * 16 + v
        }
        return n
    }
    NF == 1 && $1 ~ /^[0-9a-f]+ <.*>:$/ {
        cur = $1
        sub(/^[0-9a-f]+ </, "", cur)
        sub(/>:$/, "", cur)
        issym[cur] = 1
        next
    }
    cur == "" { next }
    $1 !~ /^[ ]*[0-9a-f]+:$/ { next }
    {
        word = $2
        gsub(/ /, "", word)
        if (length(word) != 8) {
            next
        }
        mn = $3
        # A data word inside a text symbol is printed as a directive, and its bits are not an
        # opcode.
        if (mn ~ /^\./) {
            next
        }
        w = hex2num(word)
        if (w < 0) {
            next
        }
        # DDI 0487 M.b C5.1.2, field by field: the SYS class, L, op0, then CRn.
        if (int(w / 4194304) == 852 && int(w / 2097152) % 2 == 0 \
            && int(w / 524288) % 4 == 1 && int(w / 4096) % 16 == 8) {
            crm = int(w / 256) % 16
            total++
            tlbis[cur] = tlbis[cur] " " crm " " word
            if (crm != 3 && crm != 7) {
                print "BAD " crm " " word " " cur
            }
        }
        # A branch whose target names the enclosing symbol is internal; objdump spells those
        # <sym+0xNN>, and following one would walk nothing new.
        if (mn == "b" || mn == "bl" || mn ~ /^b\./) {
            tgt = $4
            if (tgt ~ /</) {
                sub(/^[^<]*</, "", tgt)
                sub(/>.*$/, "", tgt)
                sub(/\+0x[0-9a-f]+$/, "", tgt)
                if (tgt != "" && tgt != cur) {
                    edges[cur] = edges[cur] " " tgt
                }
            }
        }
    }
    END {
        print "TOTAL " total + 0
        nr = split(roots, root, " ")
        for (i = 1; i <= nr; i++) {
            r = root[i]
            if (!(r in issym)) {
                print "MISSING " r
                continue
            }
            delete seen
            head = 0
            tail = 1
            queue[1] = r
            seen[r] = 1
            while (head < tail) {
                head++
                if (head > cap) {
                    print "OVERRUN " r
                    break
                }
                s = queue[head]
                if (s in tlbis) {
                    n = split(tlbis[s], part, " ")
                    for (j = 1; j <= n; j += 2) {
                        print "ROOT " r " " s " " part[j] " " part[j + 1]
                    }
                }
                if (s in edges) {
                    n = split(edges[s], part, " ")
                    for (j = 1; j <= n; j++) {
                        if (!(part[j] in seen)) {
                            seen[part[j]] = 1
                            tail++
                            queue[tail] = part[j]
                        }
                    }
                }
            }
        }
    }
' "$TMP/dis" > "$TMP/rec" || fail "the disassembly walk failed; a scanner that dies emits no
  record and this gate would judge an empty corpus"

echo "== TLBI shareability in $elf, built at $cores kernel core(s) =="

# --- the corpus, before any verdict -------------------------------------------
total="$(sed -n 's/^TOTAL //p' "$TMP/rec")"
require_number "$total" "the image-wide TLBI count"
if [ "$total" -eq 0 ]; then
    sed -n '1,5p' "$TMP/dis"
    fail "not one TLBI was decoded anywhere in $elf. Every AArch64 image executes TLB
  maintenance on its way up, so this is the disassembly parse or the encoding matcher having
  moved, and not an image without maintenance. The corpus is UNKNOWN"
fi
echo "   corpus: $total TLBI decoded image-wide"

if grep -q '^BAD ' "$TMP/rec"; then
    awk '$1 == "BAD" { printf "      CRm %s  %s  in %s\n", $2, $3, $4 }' "$TMP/rec" >&2
    fail "a TLBI carries a CRm this gate does not model. DDI 0487 M.b C5.5 gives CRm=0b0011
  for the Inner Shareable form and CRm=0b0111 for the local one; anything else is a
  shareability whose verdict is UNKNOWN, so it is refused rather than judged"
fi

# --- the verdict, per root ----------------------------------------------------
for root in $ROOTS; do
    if grep -q "^MISSING $root\$" "$TMP/rec"; then
        fail "'$root' is in the symbol table but the disassembly carries no body for it, so
  the walk starts nowhere. The disassembler's output shape has moved"
    fi
    if grep -q "^OVERRUN $root\$" "$TMP/rec"; then
        fail "the walk from '$root' passed $WALK_CAP symbols; the call graph read out of the
  disassembly is not the shape this gate models"
    fi

    grep "^ROOT $root " "$TMP/rec" > "$TMP/root" || true
    n="$(wc -l < "$TMP/root" | tr -d ' ')"
    require_number "$n" "the TLBI count under $root"
    if [ "$n" -eq 0 ]; then
        fail "the walk from '$root' reaches no TLBI at all, out of $total in the image. Its
  symbol range moved, its maintenance was inlined into a body this walk does not reach, or the
  disassembler's output shape changed. That is UNKNOWN, not a root without maintenance, and it
  is neither a skip nor a pass"
    fi

    want=3
    label=broadcast
    if [ "$root" = arch_aspace_activate ] || [ "$cores" -eq 1 ]; then
        want=7
        label=local
    fi

    wrong="$(awk -v w="$want" '$4 != w { printf "      CRm %s  %s  in %s\n", $4, $5, $3 }' \
        "$TMP/root")"
    if [ -n "$wrong" ]; then
        printf '%s\n' "$wrong" >&2
        if [ "$root" = arch_aspace_activate ]; then
            fail "'$root' reaches a BROADCAST TLBI. A root change concerns the PE whose
  register changed, so this operation is deliberately LOCAL at every core count: it is the one
  site a uniform edit that made the file broadcast must not carry with it"
        fi
        if [ "$want" -eq 3 ]; then
            fail "'$root' reaches a LOCAL TLBI in an image built at $cores kernel cores. A
  mapping change every core can see must be maintained on every core's TLB, so each of these
  sites has to be the Inner Shareable form (DDI 0487 M.b C5.5, CRm=0b0011)"
        fi
        fail "'$root' reaches a BROADCAST TLBI in an image built at one kernel core. There is
  no peer TLB to reach, so the local form (CRm=0b0111) is what this posture spends"
    fi

    echo "   $root: $n TLBI, every one CRm $want ($label)"
    awk '{ printf "      CRm %s  %s  in %s\n", $4, $5, $3 }' "$TMP/root"
done

echo "PASS: every TLBI reached from the four arch_aspace roots carries the shareability this
  posture requires, decoded from CRm and not from a mnemonic"
exit 0
