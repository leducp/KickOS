#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The store->load fence the rv64imac interrupt-controller handshake owes, read out of the
# linked image, and its position relative to the word each side publishes.
#
# arch_irq_unmask and arch_irq_inject are Dekker-shaped against each other: each writes its own
# word and then reads the peer's. Unmask writes g_irq_unmasked then takes from g_irq_pending;
# inject writes g_irq_pending then re-reads g_irq_unmasked. RVWMO preserves no order between a
# store and a later load to a different address (RISC-V Unprivileged ISA, Version 20260120,
# section 18.1.3), and an AMO with .aq and .rl both clear imposes no additional ordering
# (section 13.1); with no fence on both sides, both writes may sit behind both reads: unmask's
# take finds nothing pending, inject's re-read finds the line still masked, the bit stays set,
# the line stays unmasked, and nothing raises sip.SSIP. The driver sleeps for good. The take
# settles which side delivers, never whether the peer's write is visible.
#
# Refused: a body with no fence between its publish and its next memory access; a fence that
# does not order stores before loads (FENCE.TSO omits exactly this edge); a fence with nothing
# after it; a body whose publish this reader cannot resolve to a named word; a listing this
# reader cannot decode.
#
# Asserted structurally because no run can witness it: QEMU's TCG gives stronger ordering than
# RVWMO, so an image carrying no fence at all passes every arm in this tree, and there is no
# RVWMO silicon on this bench. The ordering rests on the specification and on this gate, and is
# recorded as unwitnessed in STATE.md.
#
# The reader resolves which word an access names rather than guessing from position: objdump
# annotates the `addi` that forms a global's address with that global's symbol, and a base
# register is tracked from the annotation to the access that uses it. Where the low half is 0
# objdump prints that `addi` as an unannotated `mv`, so an `auipc` value is also carried and
# resolved against the image's symbol table by exact address. Bindings are dropped at
# every branch target and at every instruction this reader does not model, because the
# disassembly is in address order and a basic-block boundary is where a linear walk would
# otherwise carry in a binding from a branch it never took. An unresolved publish fails; an
# unresolved peer read is reported as unresolved, and the ordering assertion then stands on the
# publish side alone.
#
# An empty corpus is a failure, not a pass: a body with no publish, or no memory access at all,
# means the symbol moved or the disassembly shape changed.
#
# Above one kernel core the handshake has another shape. The three words become one row per
# hart, written by that hart alone, so unmask and inject no longer race; the race moves to the
# post that carries a raise to the hart a line is routed to: the injector reads the owner's
# g_acked word after its caller's stores, and the owner's dispatch writes g_acked and then reads
# what the raise published. Both sides owe the same store->load fence for the same reason. The
# owner's write is the first release store of each body that takes, the dispatch and the clear:
# a `fence rw,w` directly ahead of the store, because its base register is formed ahead of a
# loop whose head drops every binding.
#
# usage: check_rv64_irq_fence.sh <elf> <nm> <objdump> <kernel-cores>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_rv64_irq_fence.sh <elf> <nm> <objdump> <kernel-cores>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"
cores="${4:?$_usage}"
require_number "$cores" "the kernel-core count"

# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Ordinals, not addresses: the verdict is an order. Emits tab-separated records, one per memory
# access and one per fence, in program order, under a COUNT line.
# Half a program: `seen` and the body scope come from gate.sh's scoped_body, which reads
# tests/lib/objdump_scope.awk ahead of this file.
cat > "$TMP/reader.awk" <<'AWK'
function hexval(h,    i, c, v)
{
    v = 0
    h = tolower(h)
    sub(/^0x/, "", h)
    for (i = 1; i <= length(h); i++) {
        c = index("0123456789abcdef", substr(h, i, 1)) - 1
        if (c < 0) { return -1 }
        v = v * 16 + c
    }
    return v
}

function hex8(v,    out, i, d)
{
    out = ""
    for (i = 0; i < 8; i++) {
        d = v % 16
        out = substr("0123456789abcdef", d + 1, 1) out
        v = (v - d) / 16
    }
    return out
}

# A 64-bit address as two 32-bit halves, because awk's numbers are doubles.
function pad16(h)
{
    h = tolower(h)
    sub(/^0x/, "", h)
    while (length(h) < 16) { h = "0" h }
    return h
}

# The address `off` bytes from the 16-digit `a`, or "" where the low half carries: the reader
# refuses to guess rather than model the carry.
function addr_add(a, off,    hi, lo)
{
    hi = substr(a, 1, 8)
    lo = hexval(substr(a, 9, 8)) + off
    if (lo < 0 || lo >= 4294967296) { return "" }
    return hi hex8(lo)
}

BEGIN {
    if (symtab != "") {
        while ((getline sl < symtab) > 0) {
            n_sf = split(sl, sf, " ")
            if (n_sf >= 3 && sf[1] ~ /^[0-9a-fA-F]+$/) { symat[pad16(sf[1])] = sf[n_sf] }
        }
        close(symtab)
    }
}

function shortname(s)
{
    # The three words this gate knows, out of a mangled anonymous-namespace symbol whose
    # length prefixes shift with any rename.
    if (s ~ /g_irq_unmasked/) { return "g_irq_unmasked" }
    if (s ~ /g_irq_pending/)  { return "g_irq_pending" }
    if (s ~ /g_irq_raised/)   { return "g_irq_raised" }
    if (s ~ /g_acked/)        { return "g_acked" }
    if (s ~ /g_posted/)       { return "g_posted" }
    if (s ~ /g_irq_row/)      { return "g_irq_row" }
    return s
}

function is_mem(m)
{
    if (m ~ /^amo/) { return 1 }
    if (m ~ /^lr\./) { return 1 }
    if (m ~ /^sc\./) { return 1 }
    return (m == "lw" || m == "lwu" || m == "ld" || m == "lb" || m == "lbu" \
            || m == "lh" || m == "lhu" || m == "sw" || m == "sd" || m == "sb" || m == "sh")
}

# Instructions whose first operand is the register they write. Anything outside this list and
# outside is_nodef() drops every binding, so an instruction this reader does not model can
# never leave a stale one behind.
function is_def(m)
{
    if (m ~ /^amo/) { return 1 }
    if (m ~ /^lr\./ || m ~ /^sc\./) { return 1 }
    if (m ~ /^csrr/) { return 1 }
    return (m == "li" || m == "lui" || m == "auipc" || m == "mv" || m == "not" || m == "neg" \
            || m == "addi" || m == "addiw" || m == "add" || m == "addw" \
            || m == "sub" || m == "subw" || m == "mul" || m == "mulw" \
            || m == "and" || m == "andi" || m == "or" || m == "ori" \
            || m == "xor" || m == "xori" \
            || m == "sll" || m == "slli" || m == "sllw" || m == "slliw" \
            || m == "srl" || m == "srli" || m == "srlw" || m == "sra" || m == "srai" \
            || m == "sext.w" || m == "zext.w" || m == "seqz" || m == "snez" \
            || m == "slt" || m == "sltu" || m == "slti" || m == "sltiu" \
            || m == "lw" || m == "lwu" || m == "ld" || m == "lb" || m == "lbu" \
            || m == "lh" || m == "lhu" || m == "jal" || m == "jalr")
}

# Instructions that write no register. Stores and branches read their first operand, so a
# "first operand is the destination" rule would wrongly unbind on them.
function is_nodef(m)
{
    if (m ~ /^b/) { return 1 }
    if (m ~ /^fence/) { return 1 }
    return (m == "sw" || m == "sd" || m == "sb" || m == "sh" || m == "j" || m == "ret" \
            || m == "csrs" || m == "csrc" || m == "csrsi" || m == "csrci" || m == "nop" \
            || m == "ecall" || m == "ebreak" || m == "sret" || m == "mret" || m == "wfi" \
            || m == "sfence.vma" || m == "unimp")
}

function is_branch(m)
{
    if (m ~ /^b/) { return 1 }
    return (m == "j" || m == "jal")
}

{
    nl++
    line[nl] = $0
}

END {
    if (!seen) { print "NOSYM"; exit }
    if (nl == 0) { print "NOINSN"; exit }

    # Pass one: every branch target the body names, so a join point can drop its bindings.
    for (i = 1; i <= nl; i++) {
        text = line[i]
        sub(/^[^:]*:[ \t]*/, "", text)
        sub(/[ \t]*#.*$/, "", text)
        sub(/[ \t]*<[^>]*>[ \t]*$/, "", text)
        mnem = text
        sub(/[ \t].*$/, "", mnem)
        if (!is_branch(mnem)) { continue }
        ops = text
        sub(/^[^ \t]*[ \t]*/, "", ops)
        n_op = split(ops, o, ",")
        tgt = o[n_op]
        gsub(/[ \t]/, "", tgt)
        if (tgt ~ /^[0-9a-f]+$/) { target[tgt] = 1 }
    }

    print "COUNT", nl

    # Pass two: the walk.
    for (i = 1; i <= nl; i++) {
        text = line[i]
        addr = text
        sub(/:.*$/, "", addr)
        gsub(/[ \t]/, "", addr)

        sub(/^[^:]*:[ \t]*/, "", text)
        symref = ""
        if (match(text, /#[ \t]*[0-9a-f]+[ \t]*<[^>]*>/)) {
            symref = substr(text, RSTART, RLENGTH)
            sub(/^#[ \t]*[0-9a-f]+[ \t]*</, "", symref)
            sub(/>$/, "", symref)
        }
        sub(/[ \t]*#.*$/, "", text)
        sub(/[ \t]*<[^>]*>[ \t]*$/, "", text)

        mnem = text
        sub(/[ \t].*$/, "", mnem)
        ops = text
        sub(/^[^ \t]*[ \t]*/, "", ops)

        # A join point: a binding formed on one path says nothing on another.
        if (addr in target) { delete bind; delete val }

        if (mnem == "fence") {
            if (ops == "") {
                pred = "iorw"
                succ = "iorw"
            } else {
                split(ops, fs, ",")
                pred = fs[1]
                succ = fs[2]
                gsub(/[ \t]/, "", pred)
                gsub(/[ \t]/, "", succ)
            }
            kind = "FENCE_NO"
            if (pred ~ /w/ && succ ~ /r/) { kind = "FENCE_WR" }
            printf "%d\t%s\t%s\t%s,%s\t%s\n", i, kind, mnem, pred, succ, addr
            continue
        }
        if (mnem ~ /^fence\./) {
            # FENCE.TSO orders no store->load edge; FENCE.I is not a memory fence at all.
            printf "%d\t%s\t%s\t%s\t%s\n", i, "FENCE_NO", mnem, "-", addr
            continue
        }

        if (is_mem(mnem)) {
            base = "?"
            if (match(ops, /\([a-z][a-z0-9]*\)/)) {
                base = substr(ops, RSTART + 1, RLENGTH - 2)
            }
            word = "?"
            if (base in bind) { word = bind[base] }
            printf "%d\t%s\t%s\t%s\t%s\n", i, "MEM", mnem, word, addr
        }

        # The binding update comes after the access above: `lw a5,0(a5)` reads through the old
        # binding and then destroys it.
        if (is_def(mnem)) {
            rd = ops
            sub(/[ \t].*$/, "", rd)
            sub(/,.*$/, "", rd)
            if (rd != "") {
                # An index or an offset added to a bound base still names the same word, which is
                # how an array row is addressed. Computed before rd is dropped, as rd may be a
                # source.
                n_src = split(ops, src, ",")
                for (k = 1; k <= n_src; k++) { gsub(/[ \t]/, "", src[k]) }
                nv = ""
                if (mnem == "auipc" && n_src == 2) {
                    imm = hexval(src[2])
                    if (imm >= 524288) { imm -= 1048576 }
                    if (imm != -1) { nv = addr_add(pad16(addr), imm * 4096) }
                } else if (mnem == "mv" && (src[2] in val)) {
                    nv = val[src[2]]
                } else if (mnem == "addi" && symref == "" && (src[2] in val) \
                           && src[3] ~ /^-?[0-9]+$/) {
                    nv = addr_add(val[src[2]], src[3] + 0)
                }
                delete val[rd]
                if (nv != "") { val[rd] = nv }
                carried = ""
                if (mnem == "mv" || (mnem == "addi" && symref == "")) {
                    if (src[2] in bind) { carried = bind[src[2]] }
                } else if (mnem == "add" && n_src == 3) {
                    if ((src[2] in bind) && !(src[3] in bind)) { carried = bind[src[2]] }
                    if ((src[3] in bind) && !(src[2] in bind)) { carried = bind[src[3]] }
                }
                if (mnem == "addi" && symref != "") {
                    bind[rd] = shortname(symref)
                } else if (nv != "" && (nv in symat)) {
                    bind[rd] = shortname(symat[nv])
                } else if (carried != "") {
                    bind[rd] = carried
                } else {
                    delete bind[rd]
                }
            }
        } else if (!is_nodef(mnem)) {
            delete bind
            delete val
        }
    }
}
AWK

# The symbol table an unannotated address resolves against: the planted one for the controls,
# the image's own for the verdict.
FENCE_SYMTAB=""
read_body() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" -v symtab="$FENCE_SYMTAB"
}

# The verdict over one body's records: one line on stdout, or fail().
#   $1 listing  $2 symbol  $3 the word this body publishes  $4 the peer word, or - where a
#   binding cannot survive this body's branch layout
judge() {
    _list="$1"
    _sym="$2"
    _own="$3"
    _peer="$4"

    _rec="$TMP/rec"
    read_body "$_list" "$_sym" > "$_rec"
    case "$(head -n1 "$_rec")" in
        NOSYM)
            fail "the listing carries no body for '$_sym', so this reader started nowhere. The
  symbol was renamed, made static or inlined away, or the disassembler's output shape moved" ;;
        NOINSN)
            fail "the body of '$_sym' disassembles to no instruction at all, so the corpus is
  UNKNOWN rather than empty" ;;
    esac
    _total="$(awk '$1 == "COUNT" { print $2; exit }' "$_rec")"
    require_number "$_total" "the instruction count of $_sym"

    # The publish: the first atomic write this body makes to its OWN word.
    _pub="$(awk -F"$TAB" -v w="$_own" \
        '$2 == "MEM" && $4 == w && $3 ~ /^amo/ { print $1; exit }' "$_rec")"
    if [ -z "$_pub" ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "no atomic write to '$_own' in the body of '$_sym'. Either the publish is gone, or
  this reader could not resolve the base register that names it; both are UNKNOWN, and a gate
  passing here would assert nothing about the ordering it exists for"
    fi

    # What comes next, in order: nothing but a store->load fence may reach memory first.
    _next="$(awk -F"$TAB" -v p="$_pub" \
        '$1 > p && ($2 == "MEM" || $2 ~ /^FENCE/) { print $1 "\t" $2 "\t" $3 "\t" $4; exit }' \
        "$_rec")"
    if [ -z "$_next" ]; then
        fail "the body of '$_sym' publishes to '$_own' and then reaches neither memory nor a
  fence again across its $_total instruction(s). This gate has nothing to order, which is
  UNKNOWN: the peer read it must precede is not in this body at all"
    fi
    _no="$(printf '%s\n' "$_next" | cut -f1)"
    _nk="$(printf '%s\n' "$_next" | cut -f2)"
    _nm="$(printf '%s\n' "$_next" | cut -f3)"
    _nw="$(printf '%s\n' "$_next" | cut -f4)"

    if [ "$_nk" = MEM ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "in '$_sym' the write to '$_own' at instruction #$_pub is followed by a '$_nm' on
  '$_nw' at #$_no with NO FENCE between them. Under RVWMO that write may become visible after
  that read (RISC-V Unprivileged ISA 18.1.3), so both sides of this handshake can read the peer
  stale at once and NEITHER raises: the bit pending, the line unmasked, the driver asleep for
  good. The take-back settles which side delivers and not whether the write is visible"
    fi
    if [ "$_nk" = FENCE_NO ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "in '$_sym' the write to '$_own' at instruction #$_pub is followed at #$_no by
  '$_nm $_nw', which does NOT order stores before loads. FENCE.TSO omits exactly the
  store->load edge this handshake needs, and a fence carrying no w in its predecessor set or no
  r in its successor set orders nothing here (RISC-V Unprivileged ISA 2.7)"
    fi
    if [ "$_nk" != FENCE_WR ]; then
        fail "the reader emitted kind [$_nk] after the publish in '$_sym', a record this gate
  does not model"
    fi

    # The fence must precede something, or it is trailing dead code and the peer read is
    # somewhere this gate never looked.
    _after="$(awk -F"$TAB" -v f="$_no" \
        '$1 > f && $2 == "MEM" { print $1 "\t" $3 "\t" $4; exit }' "$_rec")"
    if [ -z "$_after" ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "in '$_sym' the fence at instruction #$_no is followed by no memory access at all,
  so it orders the publish against nothing in this body. The peer read this gate exists to
  order is not here, which is UNKNOWN and not a pass"
    fi
    _ao="$(printf '%s\n' "$_after" | cut -f1)"
    _am="$(printf '%s\n' "$_after" | cut -f2)"
    _aw="$(printf '%s\n' "$_after" | cut -f3)"

    if [ "$_peer" != "-" ] && [ "$_aw" != "$_peer" ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "in '$_sym' the first access after the fence is a '$_am' on '$_aw' and not on the
  peer word '$_peer'. Either the handshake changed shape or this reader lost the binding, and
  either way the fence is no longer known to sit where it must"
    fi
    _shown="$_aw"
    if [ "$_aw" = "?" ]; then
        _shown="an unresolved word"
    fi
    echo "   $_sym: $_total instruction(s), writes $_own at #$_pub, $_nm $_nw at #$_no, then
      $_am on $_shown at #$_ao"
}

# One body's records into $TMP/rec, refusing a body this reader cannot start from. Leaves the
# instruction count in _total.
body_records() { # <listing> <symbol>
    read_body "$1" "$2" > "$TMP/rec"
    case "$(head -n1 "$TMP/rec")" in
        NOSYM)
            fail "the listing carries no body for '$2', so this reader started nowhere. The
  symbol was renamed, made static or inlined away, or the disassembler's output shape moved" ;;
        NOINSN)
            fail "the body of '$2' disassembles to no instruction at all, so the corpus is
  UNKNOWN rather than empty" ;;
    esac
    _total="$(awk '$1 == "COUNT" { print $2; exit }' "$TMP/rec")"
    require_number "$_total" "the instruction count of $2"
}

# The owner's side above one kernel core: the body's first release store, a store directly
# behind a `fence rw,w`, must name $3 or a word this reader lost at a loop head, and must be
# followed by a store->load fence before its next memory access.
judge_release() { # <listing> <symbol> <word>
    body_records "$1" "$2"
    _pubrec="$(awk -F"$TAB" '
        $2 ~ /^FENCE/ { at = $1; ops = $4; next }
        $2 == "MEM" && $3 ~ /^s[bhwd]$/ && ops == "rw,w" && $1 == at + 1 {
            print $1 "\t" $4; exit
        }' "$TMP/rec")"
    if [ -z "$_pubrec" ]; then
        sed -n '2,$p' "$TMP/rec" >&2
        fail "no release store in the body of '$2'. The owner's write of '$3' is gone or no
  longer a release, and either way this gate would assert nothing about the ordering after it"
    fi
    _pub="$(printf '%s\n' "$_pubrec" | cut -f1)"
    _pw="$(printf '%s\n' "$_pubrec" | cut -f2)"
    if [ "$_pw" != "$3" ] && [ "$_pw" != "?" ]; then
        sed -n '2,$p' "$TMP/rec" >&2
        fail "the first release store in '$2' writes '$_pw' and not '$3', so the store this
  gate orders is not the one the handshake turns on"
    fi
    _next="$(awk -F"$TAB" -v p="$_pub" \
        '$1 > p && ($2 == "MEM" || $2 ~ /^FENCE/) { print $1 "\t" $2 "\t" $3 "\t" $4; exit }' \
        "$TMP/rec")"
    _nk="$(printf '%s\n' "$_next" | cut -f2)"
    _no="$(printf '%s\n' "$_next" | cut -f1)"
    if [ "$_nk" != FENCE_WR ]; then
        sed -n '2,$p' "$TMP/rec" >&2
        fail "in '$2' the release store at instruction #$_pub is followed by [$_next] and not
  by a fence ordering stores before loads. The owner may then read what the raise published
  before its acked write is visible, while the injector reads that acked write stale, and the
  raise is coalesced into a delivery that already missed it"
    fi
    _after="$(awk -F"$TAB" -v f="$_no" '$1 > f && $2 == "MEM" { print $1; exit }' "$TMP/rec")"
    if [ -z "$_after" ]; then
        fail "in '$2' the fence at instruction #$_no is followed by no memory access at all,
  so it orders the store against nothing in this body"
    fi
    echo "   $2: $_total instruction(s), release store at #$_pub, fence at #$_no, then an
      access at #$_after"
}

# The injector's side above one kernel core: the last fence ahead of the body's first read of
# $3 must order stores before loads.
judge_read() { # <listing> <symbol> <word>
    body_records "$1" "$2"
    _rd="$(awk -F"$TAB" -v w="$3" '$2 == "MEM" && $4 == w && $3 ~ /^l/ { print $1; exit }' \
        "$TMP/rec")"
    if [ -z "$_rd" ]; then
        sed -n '2,$p' "$TMP/rec" >&2
        fail "no read of '$3' in the body of '$2'. Either the read is gone or this reader could
  not resolve the base register that names it; both are UNKNOWN"
    fi
    _fence="$(awk -F"$TAB" -v r="$_rd" \
        '$1 < r && $2 ~ /^FENCE/ { last = $1 "\t" $2 "\t" $3 " " $4 } END { print last }' \
        "$TMP/rec")"
    if [ -z "$_fence" ]; then
        sed -n '2,$p' "$TMP/rec" >&2
        fail "in '$2' the read of '$3' at instruction #$_rd has no fence ahead of it, so the
  caller's stores may become visible after it"
    fi
    if [ "$(printf '%s\n' "$_fence" | cut -f2)" != FENCE_WR ]; then
        sed -n '2,$p' "$TMP/rec" >&2
        fail "in '$2' the last fence ahead of the read of '$3' at instruction #$_rd is
  [$(printf '%s\n' "$_fence" | cut -f3)], which does not order stores before loads"
    fi
    echo "   $2: $_total instruction(s), fence at #$(printf '%s\n' "$_fence" | cut -f1), then
      the read of $3 at #$_rd"
}

# --- the planted listings -----------------------------------------------------
# In the shape the invocation below produces, which is --no-show-raw-insn: a control carrying
# the raw-bytes column would read its first byte group as the mnemonic and prove the reader
# against input the gate never hands it.
#
# planted_inject also carries the branch-target trap the real body has: a block that binds the
# same register to g_irq_raised sits between the annotation and the publish and is reached only
# by the branch, so a reader that did not drop bindings at the join would name the wrong word.
cat > "$TMP/plant_ok" <<'EOF'
0000000000001000 <planted_unmask>:
    1000:	csrrci	a4,sstatus,2
    1004:	auipc	a5,0x40f
    1008:	addi	a5,a5,1720 # 000000000041a048 <_ZN12_GLOBAL__N_1L14g_irq_unmaskedE>
    100c:	amoor.w	zero,a0,(a5)
    1010:	fence	rw,rw
    1014:	auipc	a2,0x40f
    1018:	addi	a2,a2,1698 # 000000000041a044 <_ZN12_GLOBAL__N_1L13g_irq_pendingE>
    101c:	amoand.w	a5,a3,(a2)
    1020:	beqz	a5,1028 <planted_unmask+0x28>
    1024:	nop
    1028:	ret

0000000000001030 <planted_inject>:
    1030:	auipc	a5,0x40f
    1034:	addi	a5,a5,1600 # 000000000041a048 <_ZN12_GLOBAL__N_1L14g_irq_unmaskedE>
    1038:	lw	a4,0(a5)
    103c:	beqz	a4,1050 <planted_inject+0x20>
    1040:	auipc	a5,0x40f
    1044:	addi	a5,a5,1574 # 000000000041a040 <_ZN12_GLOBAL__N_1L12g_irq_raisedE>
    1048:	amoor.w	zero,a0,(a5)
    104c:	j	1064 <planted_inject+0x34>
    1050:	auipc	a4,0x40f
    1054:	addi	a4,a4,1558 # 000000000041a044 <_ZN12_GLOBAL__N_1L13g_irq_pendingE>
    1058:	amoor.w	zero,a0,(a4)
    105c:	fence	rw,rw
    1060:	lw	a5,0(a5)
    1064:	ret
EOF

# The defect: the fence deleted outright.
sed '/fence[[:space:]]*rw,rw/d' "$TMP/plant_ok" > "$TMP/plant_nofence"
# The fence present but after the peer read, where it orders nothing between the two.
awk '{
        if ($0 ~ /fence[ \t]*rw,rw/) { held = $0; next }
        print
        if (held != "" && $0 ~ /amoand|[ \t]lw[ \t]/) { print held; held = "" }
     }' "$TMP/plant_ok" > "$TMP/plant_late"
# FENCE.TSO in its place: present, named like a fence, and missing this exact edge.
sed 's/fence[[:space:]]*rw,rw/fence.tso/' "$TMP/plant_ok" > "$TMP/plant_tso"
# A fence that orders loads against loads only.
sed 's/fence[[:space:]]*rw,rw/fence	r,r/' "$TMP/plant_ok" > "$TMP/plant_rr"
# The publish through a base register no annotation ever bound.
sed 's/# 000000000041a048 <_ZN12_GLOBAL__N_1L14g_irq_unmaskedE>//' \
    "$TMP/plant_ok" > "$TMP/plant_unbound"

# The low half 0 that objdump prints as an unannotated `mv`, resolved only through the table.
cat > "$TMP/plant_mv" <<'EOF'
0000000000003040 <planted_mv>:
    3040:	csrrci	a4,sstatus,2
    3044:	nop
    3048:	auipc	a5,0x417
    304c:	mv	a5,a5
    3050:	amoor.w	zero,a0,(a5)
    3054:	fence	rw,rw
    3058:	auipc	a2,0x417
    305c:	addi	a2,a2,-20
    3060:	amoand.w	a5,a3,(a2)
    3064:	ret
EOF
cat > "$TMP/plant_syms" <<'EOF'
000000000041a040 0000000000000004 b _ZN12_GLOBAL__N_1L12g_irq_raisedE
000000000041a044 0000000000000004 b _ZN12_GLOBAL__N_1L13g_irq_pendingE
000000000041a048 0000000000000004 b _ZN12_GLOBAL__N_1L14g_irq_unmaskedE
EOF
grep -v g_irq_unmasked "$TMP/plant_syms" > "$TMP/plant_syms_short"

for _p in plant_nofence plant_late plant_tso plant_rr plant_unbound plant_mv plant_syms_short; do
    require_nonempty "$TMP/$_p" "the planted listing '$_p' came out empty, so the control it
  carries would refuse for the wrong reason"
done

# What each planted listing must be refused for. judge() runs in a subshell so its fail()
# leaves the control and not this script; reaching the far side is the failure here.
ctl_refuses() { # <listing> <symbol> <own> <peer> <what the refusal proves>
    if ( judge "$1" "$2" "$3" "$4" ) >/dev/null 2>&1; then
        fail "the reader ACCEPTED a planted listing it must refuse: $5. A gate that passes
  there cannot go red on the defect it exists for"
    fi
}

# What it must accept, and where: a control that only ever refuses proves nothing.
ctl_accepts() { # <listing> <symbol> <own> <peer> <expected substring> <what it proves>
    if ! ( judge "$1" "$2" "$3" "$4" ) > "$TMP/ctl_out" 2>&1; then
        cat "$TMP/ctl_out" >&2
        fail "the reader REFUSED a planted body carrying the shape this gate requires, so it
  does not recognise it and every verdict below would be meaningless: $6"
    fi
    if ! grep -q "$5" "$TMP/ctl_out"; then
        cat "$TMP/ctl_out" >&2
        fail "the reader accepted the planted body but did not report [$5], so its record does
  not describe the listing it read: $6"
    fi
}

echo "== the rv64imac IRQ handshake's store->load fence in $elf =="

ctl_accepts "$TMP/plant_ok" planted_unmask g_irq_unmasked g_irq_pending \
    'writes g_irq_unmasked at #4' \
    "the fence between a publish and the peer read"
ctl_accepts "$TMP/plant_ok" planted_unmask g_irq_unmasked g_irq_pending \
    'amoand.w on g_irq_pending at #8' \
    "the peer read resolved to the word it actually names"
ctl_accepts "$TMP/plant_ok" planted_inject g_irq_pending - \
    'writes g_irq_pending at #11' \
    "a publish reached only through a branch, past a block binding the same register to
  another word"

ctl_refuses "$TMP/plant_nofence" planted_unmask g_irq_unmasked g_irq_pending \
    "the fence deleted outright, which is the defect this gate exists to catch"
ctl_refuses "$TMP/plant_late" planted_unmask g_irq_unmasked g_irq_pending \
    "the fence moved AFTER the peer read, where it orders nothing between the two"
ctl_refuses "$TMP/plant_tso" planted_unmask g_irq_unmasked g_irq_pending \
    "FENCE.TSO in the fence's place, which omits exactly the store->load edge"
ctl_refuses "$TMP/plant_rr" planted_unmask g_irq_unmasked g_irq_pending \
    "a fence ordering loads against loads only"
ctl_refuses "$TMP/plant_unbound" planted_unmask g_irq_unmasked g_irq_pending \
    "a publish this reader cannot resolve to a named word, which is UNKNOWN and not a pass"
ctl_refuses "$TMP/plant_ok" a_symbol_no_listing_carries g_irq_unmasked - \
    "a symbol the listing does not carry, so a renamed body would read as a clean one"
ctl_refuses "$TMP/plant_mv" planted_mv g_irq_unmasked g_irq_pending \
    "an unannotated address with no symbol table to resolve it against"
FENCE_SYMTAB="$TMP/plant_syms"
ctl_accepts "$TMP/plant_mv" planted_mv g_irq_unmasked g_irq_pending \
    'writes g_irq_unmasked at #5' \
    "an auipc whose low half is 0, printed as an unannotated mv, resolved by exact address"
ctl_accepts "$TMP/plant_mv" planted_mv g_irq_unmasked g_irq_pending \
    'amoand.w on g_irq_pending at #9' \
    "an auipc and an unannotated addi resolved by exact address"
FENCE_SYMTAB="$TMP/plant_syms_short"
ctl_refuses "$TMP/plant_mv" planted_mv g_irq_unmasked g_irq_pending \
    "an address the symbol table names no word at, which is UNKNOWN and not a pass"
FENCE_SYMTAB=""

# --- the planted listings above one kernel core --------------------------------
# planted_take reaches its acked word through an index added to the annotated base, which is
# how the real body addresses a row.
cat > "$TMP/hart_ok" <<'EOF'
0000000000002000 <planted_take>:
    2000:	auipc	a3,0x418
    2004:	addi	a3,a3,-584 # 0000000000425c80 <_ZN12_GLOBAL__N_1L7g_ackedE>
    2008:	add	a3,a3,a1
    200c:	lw	a6,0(a4)
    2010:	lw	t1,0(a3)
    2014:	beq	a6,t1,2024 <planted_take+0x24>
    2018:	fence	rw,w
    201c:	sw	a6,0(a3)
    2020:	fence	rw,rw
    2024:	lw	a5,0(a0)
    2028:	ret

0000000000002040 <planted_post>:
    2040:	auipc	a2,0x418
    2044:	addi	a2,a2,38 # 0000000000425d80 <_ZN12_GLOBAL__N_1L8g_postedE>
    2048:	add	a3,a3,a2
    204c:	lw	a0,0(a3)
    2050:	fence	r,rw
    2054:	fence	rw,rw
    2058:	auipc	a2,0x418
    205c:	addi	a2,a2,-248 # 0000000000425c80 <_ZN12_GLOBAL__N_1L7g_ackedE>
    2060:	add	a5,a5,a2
    2064:	lw	a5,0(a5)
    2068:	fence	r,rw
    206c:	fence	rw,w
    2070:	sw	a1,0(a3)
    2074:	ret
EOF
sed '/fence[[:space:]]*rw,rw/d' "$TMP/hart_ok" > "$TMP/hart_nofence"
sed 's/fence[[:space:]]*rw,rw/fence.tso/' "$TMP/hart_ok" > "$TMP/hart_tso"
awk '{
        if ($0 ~ /fence[ \t]*rw,rw/) { held = $0; next }
        print
        if (held != "" && $0 ~ /[ \t]lw[ \t]/) { print held; held = "" }
     }' "$TMP/hart_ok" > "$TMP/hart_late"
sed '2,/^$/s/_ZN12_GLOBAL__N_1L7g_ackedE/_ZN12_GLOBAL__N_1L8g_postedE/' \
    "$TMP/hart_ok" > "$TMP/hart_wrongword"
sed '/^$/,$s/# 0000000000425c80 <_ZN12_GLOBAL__N_1L7g_ackedE>//' \
    "$TMP/hart_ok" > "$TMP/hart_unbound"
for _p in hart_nofence hart_tso hart_late hart_wrongword hart_unbound; do
    require_nonempty "$TMP/$_p" "the planted listing '$_p' came out empty, so the control it
  carries would refuse for the wrong reason"
    if cmp -s "$TMP/hart_ok" "$TMP/$_p"; then
        fail "the planted listing '$_p' is identical to the accepted one, so its mutation did
  not land and the control would refuse nothing"
    fi
done

hart_refuses() { # <judge> <listing> <symbol> <word> <what the refusal proves>
    if ( "$1" "$2" "$3" "$4" ) >/dev/null 2>&1; then
        fail "the reader ACCEPTED a planted listing it must refuse: $5"
    fi
}

hart_accepts() { # <judge> <listing> <symbol> <word> <expected substring> <what it proves>
    if ! ( "$1" "$2" "$3" "$4" ) > "$TMP/ctl_out" 2>&1; then
        cat "$TMP/ctl_out" >&2
        fail "the reader REFUSED a planted body carrying the shape this gate requires: $6"
    fi
    if ! grep -q "$5" "$TMP/ctl_out"; then
        cat "$TMP/ctl_out" >&2
        fail "the reader accepted the planted body but did not report [$5]: $6"
    fi
}

hart_accepts judge_release "$TMP/hart_ok" planted_take g_acked 'release store at #8' \
    "the owner's release store followed by the fence"
hart_accepts judge_read "$TMP/hart_ok" planted_post g_acked 'read of g_acked at #10' \
    "an acked read reached through an index added to the annotated base"
hart_refuses judge_release "$TMP/hart_nofence" planted_take g_acked \
    "the owner's fence deleted outright"
hart_refuses judge_read "$TMP/hart_nofence" planted_post g_acked \
    "the injector's fence deleted, leaving only the acquire fence ahead of the read"
hart_refuses judge_release "$TMP/hart_tso" planted_take g_acked \
    "FENCE.TSO in the owner's fence's place"
hart_refuses judge_read "$TMP/hart_tso" planted_post g_acked \
    "FENCE.TSO in the injector's fence's place"
hart_refuses judge_release "$TMP/hart_late" planted_take g_acked \
    "the owner's fence moved past the next read"
hart_refuses judge_release "$TMP/hart_wrongword" planted_take g_acked \
    "a first release store that writes another word"
hart_refuses judge_read "$TMP/hart_unbound" planted_post g_acked \
    "an acked read this reader cannot resolve, which is UNKNOWN and not a pass"

# --- the symbol table, and the bodies in it -----------------------------------
tool_out "$TMP/nm" "[0-9a-fA-F]" "$nm" -S --defined-only "$elf"
require_nonempty "$TMP/nm" "$nm printed no symbol at all for $elf, so the corpus is UNKNOWN
  rather than empty and every verdict below it would be vacuous"
syms="$(wc -l < "$TMP/nm" | tr -d ' ')"
require_number "$syms" "the defined-symbol count"
if [ "$syms" -lt "$SYM_FLOOR" ]; then
    fail "$nm reports $syms defined symbol(s) in $elf, below the floor of $SYM_FLOOR. A table
  that short is a misread, not a small image, and the corpus is UNKNOWN"
fi

bodies="arch_irq_unmask arch_irq_inject"
if [ "$cores" -gt 1 ]; then
    bodies="kickos_rv64_isr_dispatch arch_irq_clear_pending arch_irq_inject"
fi
for sym in $bodies; do
    size="$(awk -v s="$sym" 'NF == 4 && $4 == s { print $2; exit }' "$TMP/nm")"
    if [ -z "$size" ]; then
        fail "no sized defined symbol '$sym' in $elf. One side of the handshake was renamed,
  made static, inlined away or dropped by --gc-sections, so this gate has an absence it cannot
  tell apart from a failure to read"
    fi
    case "$size" in
        *[!0]*) ;;
        *) fail "'$sym' has size 0 in $elf: the symbol survived as a label but its body is
  gone" ;;
    esac
done

FENCE_SYMTAB="$TMP/nm"

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

# The peer word is asserted where the body's branch layout lets a binding reach it. In
# arch_irq_inject the re-read goes through a register the unmasked branch rebinds, so the walk
# drops it at the join and the assertion there stands on the publish side alone.
if [ "$cores" -gt 1 ]; then
    judge_release "$TMP/dis" kickos_rv64_isr_dispatch g_acked
    judge_release "$TMP/dis" arch_irq_clear_pending g_acked
    judge_read "$TMP/dis" arch_irq_inject g_acked
else
    judge "$TMP/dis" arch_irq_unmask g_irq_unmasked g_irq_pending
    judge "$TMP/dis" arch_irq_inject g_irq_pending -
fi

echo "PASS: both sides of the rv64imac IRQ handshake fence their publish before they read the
  peer's word"
exit 0
