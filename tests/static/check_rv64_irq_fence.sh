#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The store->load FENCE the rv64imac interrupt-controller handshake owes, read out of the LINKED
# IMAGE, and its POSITION relative to the word each side publishes.
#
# arch_irq_unmask and arch_irq_inject are DEKKER-SHAPED against each other: each writes its OWN
# word and then reads the PEER's. Unmask writes g_irq_unmasked then takes from g_irq_pending,
# inject writes g_irq_pending then re-reads g_irq_unmasked. RVWMO preserves no order between a
# store and a later load to a different address (RISC-V Unprivileged ISA, Version 20260120,
# section 18.1.3) and an AMO with .aq and .rl both clear imposes no additional ordering
# (section 13.1), so with no fence on both sides both writes may sit behind both reads: unmask's
# take finds nothing pending and inject's re-read finds the line still masked, the bit stays set,
# the line stays unmasked, and NOTHING raises sip.SSIP. The driver then sleeps for good. The take
# settles which side delivers, never whether the peer's write is visible.
#
# REFUSED: a body with no fence between its publish and its next memory access, a fence that does
# not order stores before loads (FENCE.TSO is the trap; it omits exactly this edge), a fence
# with nothing after it, a body whose publish this reader cannot resolve to a named word, and a
# listing this reader cannot decode.
#
# ASSERTED STRUCTURALLY BECAUSE NO RUN CAN WITNESS IT. QEMU's TCG gives stronger ordering than
# RVWMO, so an image carrying no fence at all passes every arm in this tree; the defect and the
# fix are indistinguishable under emulation, and there is no RVWMO silicon on this bench. A soak
# would be a probabilistic arm that reads exactly like a broken one. So the ordering rests on the
# specification and on this gate, and is recorded as unwitnessed in STATE.md.
#
# The reader resolves WHICH WORD an access names, rather than guessing from position: objdump
# annotates the `addi` that forms a global's address with that global's symbol, so a base
# register is tracked from the annotation to the access that uses it. Bindings are dropped at
# every branch TARGET and at every instruction this reader does not model, because the
# disassembly is in address order and a basic-block boundary is where a linear walk would
# otherwise carry a binding in from a branch it never took. An unresolved publish is UNKNOWN and
# fails; an unresolved PEER read is reported as unresolved, the ordering assertion then standing
# on the publish side alone.
#
# The reader goes through eight planted judgements before the image is read, one per refusal.
#
# AN EMPTY CORPUS IS A FAILURE, not a pass. A body with no publish, or no memory access at all,
# says the symbol moved or the disassembly shape changed, and that is UNKNOWN.
#
# usage: check_rv64_irq_fence.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_rv64_irq_fence.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Ordinals, not addresses: the verdict is an ORDER. Emits tab-separated records, one per memory
# access and one per fence, in program order, under a COUNT line.
cat > "$TMP/reader.awk" <<'AWK'
function shortname(s)
{
    # The three words this gate knows, out of a mangled anonymous-namespace symbol whose
    # length prefixes shift with any rename.
    if (s ~ /g_irq_unmasked/) { return "g_irq_unmasked" }
    if (s ~ /g_irq_pending/)  { return "g_irq_pending" }
    if (s ~ /g_irq_raised/)   { return "g_irq_raised" }
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

# Instructions whose FIRST operand is the register they write. Anything outside this list and
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

# Instructions that write no register. Stores and branches READ their first operand, so a
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

/^[0-9a-f]+ <.*>:$/ {
    name = $2
    gsub(/[<>:]/, "", name)
    inbody = (name == sym)
    if (inbody) { seen = 1 }
    next
}
!inbody { next }
$0 !~ /^[ \t]*[0-9a-f]+:/ { next }
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
        if (addr in target) { delete bind }

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

        # The binding update comes AFTER the access above: `lw a5,0(a5)` reads through the old
        # binding and then destroys it.
        if (is_def(mnem)) {
            rd = ops
            sub(/[ \t].*$/, "", rd)
            sub(/,.*$/, "", rd)
            if (rd != "") {
                if (mnem == "addi" && symref != "") {
                    bind[rd] = shortname(symref)
                } else {
                    delete bind[rd]
                }
            }
        } else if (!is_nodef(mnem)) {
            delete bind
        }
    }
}
AWK

read_body() { # <listing> <symbol>
    awk -v sym="$2" -f "$TMP/reader.awk" "$1"
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

# --- the planted listings -----------------------------------------------------
# In the shape the invocation below produces, which is --no-show-raw-insn: a control carrying the
# raw-bytes column would read its first byte group as the mnemonic and prove the reader against
# input the gate never hands it.
#
# planted_inject also carries the BRANCH-TARGET trap the real body has: a block that binds the
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
# The fence present but AFTER the peer read, where it orders nothing between the two.
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

for _p in plant_nofence plant_late plant_tso plant_rr plant_unbound; do
    require_nonempty "$TMP/$_p" "the planted listing '$_p' came out empty, so the control it
  carries would refuse for the wrong reason"
done

# What each planted listing must be REFUSED for. judge() runs in a subshell so its fail()
# leaves the control and not this script; reaching the far side is the failure here.
ctl_refuses() { # <listing> <symbol> <own> <peer> <what the refusal proves>
    if ( judge "$1" "$2" "$3" "$4" ) >/dev/null 2>&1; then
        fail "the reader ACCEPTED a planted listing it must refuse: $5. A gate that passes
  there cannot go red on the defect it exists for"
    fi
}

# What it must ACCEPT, and where: a control that only ever refuses proves nothing.
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

for sym in arch_irq_unmask arch_irq_inject; do
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

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

# The peer word is asserted where the body's branch layout lets a binding reach it. In
# arch_irq_inject the re-read goes through a register the unmasked branch rebinds, so the walk
# drops it at the join and the assertion there stands on the publish side alone.
judge "$TMP/dis" arch_irq_unmask g_irq_unmasked g_irq_pending
judge "$TMP/dis" arch_irq_inject g_irq_pending -

echo "PASS: both sides of the rv64imac IRQ handshake fence their publish before they read the
  peer's word"
exit 0
