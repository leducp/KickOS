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
# THE SELF-TEST DRIVES THE WHOLE JUDGE THROUGH STUB TOOLS. There is no assembler here, so the
# controls below hand it stubs that print a forged symbol table and a forged disassembly.
# Every encoding planted there is one the real aarch64 disassembler spells `tlbi vaae1is` or
# `tlbi vaae1`, so the CRm decode runs on real words. What is NOT proven is the
# disassembler's output SHAPE: if a
# future binutils changes the column layout, the controls keep passing and only the tool_out
# landmark and the zero-TLBI refusal stand in the way.
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
# The two CRm values C5.5 gives the pair, handed to the scanner and to the verdict from ONE
# place, so a control cannot be judged by a different reading from the tree's.
CRM_BCAST=3
CRM_LOCAL=7
# What the disassembler prints in the mnemonic column for a DATA word inside a text symbol. Its
# bits are not an opcode, and one that happened to match the SYS class would otherwise be
# decoded as a TLB operation.
# Bracketed and not `^\.`: awk processes escape sequences in a -v assignment, so a backslash
# before the dot arrives as a bare dot and the pattern would match EVERY mnemonic.
DIRECTIVE='^[.]'

scratch_dir

judge() { # <elf> <cores> <nm> <objdump>
    _elf="$1"
    _cores="$2"
    _nm="$3"
    _objdump="$4"

    require_number "$_cores" "the expected kernel core count"
    if [ "$_cores" -lt 1 ]; then
        fail "the expected kernel core count is $_cores; an image is built at one core or more"
    fi
    [ -f "$_elf" ] || fail "no image at $_elf"
    [ -x "$_nm" ] || fail "no nm at $_nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
    [ -x "$_objdump" ] || fail "no objdump at $_objdump; there is no instruction stream to decode"

    # A fresh subdirectory per call, so no file a previous control left behind can be read as
    # this one's corpus.
    _t="$TMP/judge"
    rm -rf "$_t"
    mkdir -p "$_t"

    # --- the symbol table, and the four roots in it ---------------------------
    tool_out "$_t/nm" "[0-9a-fA-F]" "$_nm" -S --defined-only "$_elf"
    require_nonempty "$_t/nm" "$_nm printed no symbol at all for $_elf, so the corpus is UNKNOWN
  rather than empty and every verdict below it would be vacuous"
    syms="$(wc -l < "$_t/nm" | tr -d ' ')"
    require_number "$syms" "the defined-symbol count"
    if [ "$syms" -lt "$SYM_FLOOR" ]; then
        fail "$_nm reports $syms defined symbol(s) in $_elf, below the floor of $SYM_FLOOR. A table
  that short is a misread, not a small image, and the corpus is UNKNOWN"
    fi

    for root in $ROOTS; do
        kind="$(awk -v s="$root" '$NF == s { print NF; exit }' "$_t/nm")"
        if [ -z "$kind" ]; then
            fail "no defined symbol '$root' in $_elf. The seam's entry point was renamed, made
  static or inlined away, so this gate has nothing to walk from and reports an absence it
  cannot tell apart from a failure to read"
        fi
        size="$(awk -v s="$root" 'NF == 4 && $4 == s { print $2; exit }' "$_t/nm")"
        if [ -z "$size" ]; then
            fail "'$root' is defined in $_elf but carries no size, so its body cannot be bounded"
        fi
        case "$size" in
            *[!0]*) ;;
            *) fail "'$root' has size 0 in $_elf: the symbol survived as a label but its body is
  gone, so no TLBI can be reached from it" ;;
        esac
    done

    # --- the instruction stream ----------------------------------------------
    tool_out "$_t/dis" "^[0-9a-f]+ <.*>:\$" "$_objdump" -d "$_elf"
    require_nonempty "$_t/dis" "$_objdump printed no disassembly for $_elf"

    # Records out of one pass: TOTAL <n>, BAD <crm> <word> <sym>, MISSING <root>, OVERRUN <root>,
    # and ROOT <root> <sym> <crm> <word> for every TLBI the walk from <root> reaches.
    awk -F'\t' -v roots="$ROOTS" -v cap="$WALK_CAP" -v bcast="$CRM_BCAST" \
        -v local_crm="$CRM_LOCAL" -v directive="$DIRECTIVE" '
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
            if (mn ~ directive) {
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
                if (crm != bcast + 0 && crm != local_crm + 0) {
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
    ' "$_t/dis" > "$_t/rec" || fail "the disassembly walk failed; a scanner that dies emits no
  record and this gate would judge an empty corpus"

    echo "== TLBI shareability in $_elf, built at $_cores kernel core(s) =="

    # --- the corpus, before any verdict --------------------------------------
    total="$(sed -n 's/^TOTAL //p' "$_t/rec")"
    require_number "$total" "the image-wide TLBI count"
    if [ "$total" -eq 0 ]; then
        sed -n '1,5p' "$_t/dis"
        fail "not one TLBI was decoded anywhere in $_elf. Every AArch64 image executes TLB
  maintenance on its way up, so this is the disassembly parse or the encoding matcher having
  moved, and not an image without maintenance. The corpus is UNKNOWN"
    fi
    echo "   corpus: $total TLBI decoded image-wide"

    if grep -q '^BAD ' "$_t/rec"; then
        awk '$1 == "BAD" { printf "      CRm %s  %s  in %s\n", $2, $3, $4 }' "$_t/rec" >&2
        fail "a TLBI carries a CRm this gate does not model. DDI 0487 M.b C5.5 gives CRm=0b0011
  for the Inner Shareable form and CRm=0b0111 for the local one; anything else is a
  shareability whose verdict is UNKNOWN, so it is refused rather than judged"
    fi

    # --- the verdict, per root ------------------------------------------------
    for root in $ROOTS; do
        if grep -q "^MISSING $root\$" "$_t/rec"; then
            fail "'$root' is in the symbol table but the disassembly carries no body for it, so
  the walk starts nowhere. The disassembler's output shape has moved"
        fi
        if grep -q "^OVERRUN $root\$" "$_t/rec"; then
            fail "the walk from '$root' passed $WALK_CAP symbols; the call graph read out of the
  disassembly is not the shape this gate models"
        fi

        grep "^ROOT $root " "$_t/rec" > "$_t/root" || true
        n="$(wc -l < "$_t/root" | tr -d ' ')"
        require_number "$n" "the TLBI count under $root"
        if [ "$n" -eq 0 ]; then
            fail "the walk from '$root' reaches no TLBI at all, out of $total in the image. Its
  symbol range moved, its maintenance was inlined into a body this walk does not reach, or the
  disassembler's output shape changed. That is UNKNOWN, not a root without maintenance, and it
  is neither a skip nor a pass"
        fi

        want="$CRM_BCAST"
        label=broadcast
        if [ "$root" = arch_aspace_activate ] || [ "$_cores" -eq 1 ]; then
            want="$CRM_LOCAL"
            label=local
        fi

        wrong="$(awk -v w="$want" '$4 != w { printf "      CRm %s  %s  in %s\n", $4, $5, $3 }' \
            "$_t/root")"
        if [ -n "$wrong" ]; then
            printf '%s\n' "$wrong" >&2
            if [ "$root" = arch_aspace_activate ]; then
                fail "'$root' reaches a BROADCAST TLBI. A root change concerns the PE whose
  register changed, so this operation is deliberately LOCAL at every core count: it is the one
  site a uniform edit that made the file broadcast must not carry with it"
            fi
            if [ "$want" = "$CRM_BCAST" ]; then
                fail "'$root' reaches a LOCAL TLBI in an image built at $_cores kernel cores. A
  mapping change every core can see must be maintained on every core's TLB, so each of these
  sites has to be the Inner Shareable form (DDI 0487 M.b C5.5, CRm=0b0011)"
            fi
            fail "'$root' reaches a BROADCAST TLBI in an image built at one kernel core. There is
  no peer TLB to reach, so the local form (CRm=0b0111) is what this posture spends"
        fi

        echo "   $root: $n TLBI, every one CRm $want ($label)"
        awk '{ printf "      CRm %s  %s  in %s\n", $4, $5, $3 }' "$_t/root"
    done

    echo "PASS: every TLBI reached from the four arch_aspace roots carries the shareability this
  posture requires, decoded from CRm and not from a mnemonic"
}

# --- self-test: one control per clause, each a minimal pair -------------------
# The stubs answer for the real tools: `nm` prints <elf>.nm and `objdump` prints <elf>.dis.
# Every planted world is one edit away from a clean image, so no other clause can be what
# reddens it.
CTL="$TMP/ctl"
mkdir -p "$CTL/bin"
cat > "$CTL/bin/nm" <<'STUB'
#!/bin/sh
f=""
for a in "$@"; do
    case "$a" in
        -*) ;;
        *) f="$a" ;;
    esac
done
cat "$f.nm"
STUB
cat > "$CTL/bin/objdump" <<'STUB'
#!/bin/sh
f=""
for a in "$@"; do
    case "$a" in
        -*) ;;
        *) f="$a" ;;
    esac
done
cat "$f.dis"
STUB
printf '#!/bin/sh\nexit 5\n' > "$CTL/bin/dead"
chmod +x "$CTL/bin/nm" "$CTL/bin/objdump" "$CTL/bin/dead"

# Words the real aarch64 disassembler calls, in this order: tlbi vaae1is (CRm=3, broadcast),
# tlbi vaae1 (CRm=7, local), tlbi vmalle1is, tlbi vmalle1, a SYS with an unmodelled CRm=0,
# dc civac (CRn=7, not a TLB operation at all), and a sysl, which is the same word as
# vaae1is with the L bit set.
W_BCAST=d5088360
W_LOCAL=d5088760
W_BCAST_ALL=d508831f
W_LOCAL_ALL=d508871f
W_BAD_CRM=d5088060
W_NOT_TLBI=d50b7e20
W_READ=d5288360
W_RET=d65f03c0
W_NOP=d503201f

insn() { # <addr> <word> <mnemonic> <operands>
    printf '  %s:\t%s \t%s\t%s\n' "$1" "$2" "$3" "$4"
}
sym() { # <addr> <name>
    printf '\n%016x <%s>:\n' "0x$1" "$2"
}
# The mnemonic and operands the real disassembler prints for each planted word, so a forged
# line is the line objdump would have written for those bits. A word with no spelling here is
# refused rather than printed with a guessed mnemonic.
tlbi_line() { # <addr> <word>
    case "$2" in
        "$W_BCAST")     insn "$1" "$2" tlbi 'vaae1is, x0' ;;
        "$W_LOCAL")     insn "$1" "$2" tlbi 'vaae1, x0' ;;
        "$W_BCAST_ALL") insn "$1" "$2" tlbi 'vmalle1is' ;;
        "$W_LOCAL_ALL") insn "$1" "$2" tlbi 'vmalle1' ;;
        "$W_BAD_CRM")   insn "$1" "$2" sys '#0, C8, C0, #3, x0' ;;
        "$W_RET")       insn "$1" "$2" ret '' ;;
        *) fail "no disassembly spelling for the planted word $2" ;;
    esac
}

# The six TLBI slots of the clean image, in the order world() takes them: arch_aspace_map,
# the callee under it, arch_aspace_unmap, arch_aspace_destroy, arch_aspace_activate, and the
# symbol no root reaches.
CLEAN="$W_BCAST $W_BCAST_ALL $W_BCAST $W_BCAST_ALL $W_LOCAL_ALL $W_LOCAL"

CW="$TMP/world"
# The clean image at four cores: the three map editors reach the broadcast form, activate
# reaches the local one, and one editor reaches its TLBI through a call so the walk is
# exercised and not just the enclosing body. kos_unreached holds a TLBI no root reaches, so it
# counts image-wide and must reach no verdict.
#
# The optional argument is ONE instruction planted inside arch_aspace_activate's body, which
# is where a word has to sit to reach a verdict at all: a word in a symbol no root reaches is
# silent whatever it decodes to, so a control planted there would be silent for the wrong
# reason.
world() { # <map> <flush> <unmap> <destroy> <activate> <unreached> [<word> <mn> <operands>]
    _wmap="$1"
    _wflush="$2"
    _wunmap="$3"
    _wdestroy="$4"
    _wact="$5"
    _wfar="$6"
    _mw="${7:-$W_NOP}"
    _mm="${8:-nop}"
    _mo="${9:-}"
    rm -rf "$CW"
    mkdir -p "$CW"
    : > "$CW/elf"
    {
        printf '0000000000400000 0000000000000010 T kos_entry\n'
        printf '0000000000400080 0000000000000040 T arch_aspace_map\n'
        printf '00000000004000c0 0000000000000020 T kos_flush_range\n'
        printf '0000000000400100 0000000000000020 T arch_aspace_unmap\n'
        printf '0000000000400140 0000000000000020 T arch_aspace_destroy\n'
        printf '0000000000400180 0000000000000020 T arch_aspace_activate\n'
        printf '00000000004001c0 0000000000000010 T kos_unreached\n'
        _i=0
        while [ "$_i" -lt 120 ]; do
            printf '0000000000%06x 0000000000000004 T kos_filler_%d\n' \
                "$((0x500000 + _i * 4))" "$_i"
            _i=$((_i + 1))
        done
    } > "$CW/elf.nm"
    {
        sym 400000 kos_entry
        insn 400000 "$W_RET" ret ''
        sym 400080 arch_aspace_map
        tlbi_line 400080 "$_wmap"
        insn 400084 14000010 b '4000c0 <kos_flush_range>'
        sym 4000c0 kos_flush_range
        tlbi_line 4000c0 "$_wflush"
        insn 4000c4 "$W_RET" ret ''
        sym 400100 arch_aspace_unmap
        tlbi_line 400100 "$_wunmap"
        insn 400104 "$W_RET" ret ''
        sym 400140 arch_aspace_destroy
        tlbi_line 400140 "$_wdestroy"
        insn 400144 "$W_RET" ret ''
        sym 400180 arch_aspace_activate
        tlbi_line 400180 "$_wact"
        insn 400184 "$_mw" "$_mm" "$_mo"
        insn 400188 "$W_RET" ret ''
        sym 4001c0 kos_unreached
        tlbi_line 4001c0 "$_wfar"
        insn 4001c4 "$W_RET" ret ''
    } > "$CW/elf.dis"
}
edit_dis() { # <sed-script>
    sed "$1" "$CW/elf.dis" > "$CW/elf.dis.new"
    mv "$CW/elf.dis.new" "$CW/elf.dis"
}
edit_nm() { # <sed-script>
    sed "$1" "$CW/elf.nm" > "$CW/elf.nm.new"
    mv "$CW/elf.nm.new" "$CW/elf.nm"
}

CORES=4
call() {
    ( judge "$CW/elf" "$CORES" "$CTL/bin/nm" "$CTL/bin/objdump" ) > "$CTL/out" 2>&1
}
FIRED=0
ctl() { # <label> <expect-ere>
    if call; then
        cat "$CTL/out"
        fail "positive control '$1' passed, so the clause it plants for fires on nothing"
    fi
    grep -qE "$2" "$CTL/out" || {
        cat "$CTL/out" >&2
        fail "positive control '$1' reddened for the wrong reason, expected /$2/. A control
      another clause catches proves nothing about the clause it plants for"
    }
    FIRED=$((FIRED + 1))
}
QUIET=0
neg() { # <label>
    if call; then
        QUIET=$((QUIET + 1))
    else
        cat "$CTL/out" >&2
        fail "negative control '$1' reports; the gate would redden a correct image"
    fi
}

# The clean image at four cores, and the same shape at one core with every root local.
CORES=4; world $CLEAN; neg clean_smp
CORES=1; world "$W_LOCAL" "$W_LOCAL_ALL" "$W_LOCAL" "$W_LOCAL_ALL" "$W_LOCAL_ALL" "$W_LOCAL"
neg clean_one_core
# A data word inside arch_aspace_activate whose bits ARE the broadcast encoding. The
# disassembler prints it in the mnemonic column as a directive, and its bits are not an opcode.
CORES=4; world $CLEAN "$W_BCAST" .word "0x$W_BCAST"
neg data_word_in_text
# A system instruction that is not a TLB operation (CRn=7), and the read form of one that is
# (the L bit set, which is what a sysl of the same word is). Neither is a TLBI and neither may
# reach a verdict, both sitting in a root's body where a TLBI would.
CORES=4; world $CLEAN "$W_NOT_TLBI" dc 'civac, x0'
neg not_a_tlb_operation
CORES=4; world $CLEAN "$W_READ" sysl 'x0, #0, C8, C3, #3'
neg system_register_read
# An internal branch, which objdump spells <sym+0xNN>: following it would walk nothing new, and
# the walk must not read it as a call to another symbol. No withdrawal of a reading moves this
# one, an unknown branch target reaching nothing either way, so it stands as a plain refusal to
# cry wolf.
CORES=4; world $CLEAN 14000010 b '400180 <arch_aspace_activate+0x0>'
neg internal_branch
[ "$QUIET" -eq 6 ] || fail "$QUIET of 6 negative controls ran silent"

# The three shareability verdicts, one per posture.
CORES=4; world "$W_BCAST" "$W_BCAST_ALL" "$W_LOCAL" "$W_BCAST_ALL" "$W_LOCAL_ALL" "$W_LOCAL"
ctl editor_left_local 'reaches a LOCAL TLBI in an image built at 4 kernel cores'
CORES=4; world "$W_BCAST" "$W_BCAST_ALL" "$W_BCAST" "$W_BCAST_ALL" "$W_BCAST_ALL" "$W_LOCAL"
ctl activate_made_broadcast "'arch_aspace_activate' reaches a BROADCAST TLBI\."
CORES=1; world $CLEAN
ctl one_core_broadcast 'reaches a BROADCAST TLBI in an image built at one kernel core'
# The same edit reached THROUGH THE CALL, so the walk is load-bearing and not just the
# enclosing body.
CORES=4; world "$W_BCAST" "$W_LOCAL_ALL" "$W_BCAST" "$W_BCAST_ALL" "$W_LOCAL_ALL" "$W_LOCAL"
ctl callee_left_local 'reaches a LOCAL TLBI in an image built at 4 kernel cores'

# A CRm this gate does not model, which is refused rather than judged.
CORES=4; world "$W_BCAST" "$W_BCAST_ALL" "$W_BAD_CRM" "$W_BCAST_ALL" "$W_LOCAL_ALL" "$W_LOCAL"
ctl unmodelled_crm 'carries a CRm this gate does not model'

# The corpus refusals: nothing decoded image-wide, and a root the walk reaches nothing from.
CORES=4; world "$W_RET" "$W_RET" "$W_RET" "$W_RET" "$W_RET" "$W_RET"
ctl nothing_decoded 'not one TLBI was decoded anywhere'
CORES=4; world "$W_BCAST" "$W_BCAST_ALL" "$W_BCAST" "$W_RET" "$W_LOCAL_ALL" "$W_LOCAL"
ctl root_reaches_nothing "the walk from 'arch_aspace_destroy' reaches no TLBI at all"

# The symbol table: a root that is gone, one with no size, one whose body was discarded, one
# the disassembly has no body for, and a table too short to be one.
CORES=4; world $CLEAN; edit_nm '/ T arch_aspace_map$/d'
ctl root_absent "no defined symbol 'arch_aspace_map'"
CORES=4; world $CLEAN
edit_nm 's|^0000000000400080 0000000000000040 T arch_aspace_map$|0000000000400080 T arch_aspace_map|'
ctl root_unsized 'carries no size, so its body cannot be bounded'
CORES=4; world $CLEAN
edit_nm 's|^0000000000400080 0000000000000040 T arch_aspace_map$|0000000000400080 0000000000000000 T arch_aspace_map|'
ctl root_size_zero 'has size 0 in'
CORES=4; world $CLEAN; edit_dis '/^0000000000400080 <arch_aspace_map>:$/d'
ctl root_no_body 'the disassembly carries no body for it'
CORES=4; world $CLEAN; head -20 "$CW/elf.nm" > "$CW/elf.nm.new"; mv "$CW/elf.nm.new" "$CW/elf.nm"
ctl symbol_floor "below the floor of $SYM_FLOOR"

# The core count, which is an argument and not a corpus.
CORES=0; world $CLEAN
ctl cores_zero 'an image is built at one core or more'
CORES=two; world $CLEAN
ctl cores_not_a_number 'the expected kernel core count'
CORES=4

# The tools and the inputs.
world $CLEAN
if ( judge "$CW/elf" 4 "$CTL/bin/dead" "$CTL/bin/objdump" ) > "$CTL/out" 2>&1; then
    fail "an nm that exits nonzero read the whole image and said nothing"
fi
grep -q '^FAIL: exit 5 from: ' "$CTL/out" || {
    cat "$CTL/out" >&2
    fail "a dead nm reddened the gate without naming the tool failure"
}
FIRED=$((FIRED + 1))
if ( judge "$CW/elf" 4 "$CTL/bin/nm" "$CTL/bin/dead" ) > "$CTL/out" 2>&1; then
    fail "an objdump that exits nonzero read the whole image and said nothing"
fi
grep -q '^FAIL: exit 5 from: ' "$CTL/out" || {
    cat "$CTL/out" >&2
    fail "a dead objdump reddened the gate without naming the tool failure"
}
FIRED=$((FIRED + 1))
world $CLEAN; printf 'no symbol header here\n' > "$CW/elf.dis"
ctl no_disassembly_landmark 'nothing matching .* came out of'
world $CLEAN; rm -f "$CW/elf"
ctl image_absent 'no image at'
[ "$FIRED" -eq 18 ] || fail "$FIRED of 18 positive controls reddened"

# The reading withdrawn: SWAP the two CRm values and the clean image must report on every
# root, so the negatives distinguish the Inner Shareable form from the local one rather than
# accepting whatever the image holds.
CORES=4; world $CLEAN
if ( CRM_BCAST=7
     CRM_LOCAL=3
     judge "$CW/elf" 4 "$CTL/bin/nm" "$CTL/bin/objdump" ) > "$CTL/out" 2>&1; then
    fail "with the two CRm values swapped the clean image still passed, so no verdict below
      depends on which of the pair a site carries"
fi
grep -qE 'reaches a LOCAL TLBI|reaches a BROADCAST TLBI' "$CTL/out" || {
    cat "$CTL/out" >&2
    fail "swapping the two CRm values reddened something other than a shareability verdict"
}
# And the data-word skip withdrawn: the planted `.word` must then be decoded as a TLBI and
# reach a verdict, so that control is a near miss.
CORES=4; world $CLEAN "$W_BCAST" .word "0x$W_BCAST"
if ( DIRECTIVE='^KICKOS_THIS_ERE_MATCHES_NOTHING'
     judge "$CW/elf" 4 "$CTL/bin/nm" "$CTL/bin/objdump" ) > "$CTL/out" 2>&1; then
    fail "with the data-word skip withdrawn the planted .word still reached no verdict, so
      that control is not a near miss and proves nothing"
fi
grep -q "'arch_aspace_activate' reaches a BROADCAST TLBI" "$CTL/out" || {
    cat "$CTL/out" >&2
    fail "withdrawing the data-word skip reddened something other than the verdict under the
      root the planted word sits in"
}

# --- the image ----------------------------------------------------------------
judge "$elf" "$cores" "$nm" "$objdump"
exit 0
