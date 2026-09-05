#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Asserts out of the linked image and out of arch_xtensa.cc that no access to g_irq_unmasked or
# g_irq_pending is a read-modify-write: every byte store to a cell is a store of a materialised
# constant, and both arrays are declared Atomic<uint8_t, Order::RELAXED>[32].
#
# A store whose value this reader cannot attribute to a constant materialisation is a FAILURE,
# and so is an empty corpus: UNKNOWN and broken are the same finding here.
#
# usage: check_lx6_irq_cells.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_lx6_irq_cells.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/arch/xtensa/lx6/arch_xtensa.cc"

# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; neither the symbol table nor the size that bounds each body
  can be read out of the image, and every assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"
[ -f "$SRC" ] || fail "no source at $SRC; the declaration half of this gate has no corpus, and
  an absent file would read as a clean one"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Emits tab-separated records under a COUNT line, one per byte access, one per cell access of
# another width, and one per memw:
#   <ordinal> <kind> <mnemonic> <array> <register> <provenance> <address>
cat > "$TMP/reader.awk" <<'AWK'
function h2d(s,    d, n, k, c, v)
{
    d = 0
    n = length(s)
    for (k = 1; k <= n; k++) {
        c = tolower(substr(s, k, 1))
        v = index("0123456789abcdef", c) - 1
        if (v < 0) { return -1 }
        d = d * 16 + v
    }
    return d
}

function shortname(s)
{
    if (s ~ /g_irq_unmasked/) { return "g_irq_unmasked" }
    if (s ~ /g_irq_pending/)  { return "g_irq_pending" }
    if (s ~ /g_inject_line/)  { return "g_inject_line" }
    return s
}

function is_cell(w)
{
    return (w == "g_irq_unmasked" || w == "g_irq_pending")
}

function is_branch(m)
{
    if (m ~ /^b/) { return 1 }
    if (m ~ /^loop/) { return 1 }
    return (m == "j" || m == "jx")
}

# s8i's FIRST operand is the value and the second the base, the opposite of every other
# instruction here.
function is_store(m)
{
    return (m == "s8i" || m == "s16i" || m == "s32i" || m == "s32i.n" || m == "s32ri" \
            || m == "s32nb" || m == "s32e" || m == "s32c1i")
}

function is_load(m)
{
    return (m == "l8ui" || m == "l16ui" || m == "l16si" || m == "l32i" || m == "l32i.n" \
            || m == "l32ai" || m == "l32e")
}

function is_const(m)
{
    return (m == "movi" || m == "movi.n")
}

function is_mov(m)
{
    return (m == "mov" || m == "mov.n")
}

# The ONLY family an array binding survives: the index is added to the base before the access.
function is_addfam(m)
{
    if (m ~ /^addx/ || m ~ /^subx/) { return 1 }
    return (m == "add" || m == "add.n" || m == "addi" || m == "addi.n" || m == "sub")
}

# Instructions whose FIRST operand is the register they write, and across which no binding
# survives. Overbroad on purpose: a dropped binding costs a refusal, a wrong one the assertion.
function is_def(m)
{
    if (m ~ /^rsr/ || m ~ /^rur/) { return 1 }
    if (m ~ /^mul/ || m ~ /^quo/ || m ~ /^rem/ || m ~ /^div/) { return 1 }
    return (m == "and" || m == "or" || m == "xor" || m == "andb" || m == "orb" \
            || m == "neg" || m == "abs" || m == "extui" || m == "extus" || m == "sext" \
            || m == "sll" || m == "srl" || m == "sra" || m == "slli" || m == "srli" \
            || m == "srai" || m == "src" || m == "nsau" || m == "clamps" \
            || m == "moveqz" || m == "movnez" || m == "movltz" || m == "movgez" \
            || m == "min" || m == "max" || m == "minu" || m == "maxu" || m == "rsil")
}

# Instructions that write no register at all. A store and a branch READ their first operand, so
# a destination-first rule would wrongly unbind on them.
function is_nodef(m)
{
    if (is_branch(m)) { return 1 }
    if (m ~ /^wsr/ || m ~ /^wur/ || m ~ /^ssa/ || m ~ /^ssi/ || m ~ /^ssl/ || m ~ /^ssr/) {
        return 1
    }
    if (m ~ /^rf/ || m ~ /^ret/) { return 1 }
    return (m == "nop" || m == "nop.n" || m == "memw" || m == "extw" || m == "isync" \
            || m == "rsync" || m == "esync" || m == "dsync" || m == "excw" || m == "ill" \
            || m == "waiti" || m == "simcall" || m == "break" || m == "break.n" \
            || m == "syscall" || m == "idtlb" || m == "iitlb")
}

BEGIN {
    lim = 0
    if (endaddr != "") {
        lim = h2d(endaddr)
        if (lim <= 0) {
            bad = 1
            print "BADEND"
            exit
        }
    }
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
    a = $0
    sub(/:.*$/, "", a)
    gsub(/[ \t]/, "", a)
    if (lim > 0 && h2d(a) >= lim) { next }
    nl++
    line[nl] = $0
}

END {
    if (bad) { exit }
    if (!seen) { print "NOSYM"; exit }
    if (nl == 0) { print "NOINSN"; exit }

    # Pass one: every branch target the body names, so a join can drop what a path bound.
    for (i = 1; i <= nl; i++) {
        text = line[i]
        sub(/^[^:]*:[ \t]*/, "", text)
        sub(/[ \t]+$/, "", text)
        sub(/[ \t]*\([^()]*\)$/, "", text)
        sub(/[ \t]*<[^>]*>$/, "", text)
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
        sub(/[ \t]+$/, "", text)

        # The resolved symbol is the LAST parenthesised group; the first names the literal-pool
        # slot the l32r reads, which is some unrelated function plus an offset.
        symref = ""
        if (match(text, /\([0-9a-f]+ <[^>]*>\)$/)) {
            symref = substr(text, RSTART, RLENGTH)
            sub(/^\([0-9a-f]+ </, "", symref)
            sub(/>\)$/, "", symref)
        }
        sub(/[ \t]*\([^()]*\)$/, "", text)
        sub(/[ \t]*<[^>]*>$/, "", text)

        mnem = text
        sub(/[ \t].*$/, "", mnem)
        ops = text
        sub(/^[^ \t]*[ \t]*/, "", ops)
        n_op = split(ops, o, ",")
        for (k = 1; k <= n_op; k++) { gsub(/[ \t]/, "", o[k]) }

        if (addr in target) {
            delete bind
            delete val
        }

        # A windowed call rotates a8 to a15 out from under the caller, and entry rotates the
        # frame in, so neither leaves anything this reader may still trust.
        if (mnem ~ /^call/ || mnem == "entry") {
            delete bind
            delete val
            continue
        }

        if (mnem == "memw") {
            printf "%d\t%s\t%s\t%s\t%s\t%s\t%s\n", i, "MEMW", mnem, "-", "-", "-", addr
            continue
        }

        if (is_store(mnem)) {
            vreg = o[1]
            breg = o[2]
            word = "?"
            if (breg in bind) { word = bind[breg] }
            prov = "unknown"
            if (vreg in val) { prov = val[vreg] }
            kind = ""
            if (mnem == "s8i") {
                if (is_cell(word)) { kind = "CELL_STORE" }
                else if (word == "?") { kind = "STORE_UNRESOLVED" }
                else { kind = "STORE_OTHER" }
            } else if (is_cell(word)) {
                kind = "CELL_STORE_WIDE"
            }
            if (kind != "") {
                printf "%d\t%s\t%s\t%s\t%s\t%s\t%s\n", i, kind, mnem, word, vreg, prov, addr
            }
            # s32c1i is the one store here that also WRITES its first operand.
            if (mnem == "s32c1i") {
                delete bind[vreg]
                val[vreg] = "alu"
            }
            continue
        }

        if (is_load(mnem)) {
            dreg = o[1]
            breg = o[2]
            word = "?"
            if (breg in bind) { word = bind[breg] }
            kind = ""
            if (mnem == "l8ui") {
                if (is_cell(word)) { kind = "CELL_LOAD" }
                else { kind = "LOAD_OTHER" }
            } else if (is_cell(word)) {
                kind = "CELL_LOAD_WIDE"
            }
            if (kind != "") {
                printf "%d\t%s\t%s\t%s\t%s\t%s\t%s\n", i, kind, mnem, word, dreg, "-", addr
            }
            # The base is resolved ABOVE, before this destroys the destination's own binding:
            # `l8ui a8, a8, 0` reads through the old binding and then ends it.
            delete bind[dreg]
            val[dreg] = "load:" word
            continue
        }

        if (mnem == "l32r") {
            dreg = o[1]
            delete bind[dreg]
            if (symref != "") { bind[dreg] = shortname(symref) }
            val[dreg] = "literal"
            continue
        }

        if (is_const(mnem)) {
            dreg = o[1]
            delete bind[dreg]
            val[dreg] = "const"
            continue
        }

        if (is_mov(mnem)) {
            dreg = o[1]
            sreg = o[2]
            delete bind[dreg]
            delete val[dreg]
            if (sreg in bind) { bind[dreg] = bind[sreg] }
            if (sreg in val) { val[dreg] = val[sreg] }
            continue
        }

        if (is_addfam(mnem)) {
            dreg = o[1]
            hit = ""
            amb = 0
            for (k = 1; k <= n_op; k++) {
                if (!(o[k] in bind)) { continue }
                if (hit == "") { hit = bind[o[k]] }
                else if (hit != bind[o[k]]) { amb = 1 }
            }
            delete bind[dreg]
            if (hit != "" && amb == 0) { bind[dreg] = hit }
            val[dreg] = "alu"
            continue
        }

        if (is_def(mnem)) {
            dreg = o[1]
            delete bind[dreg]
            val[dreg] = "alu"
            continue
        }

        if (is_nodef(mnem)) { continue }

        delete bind
        delete val
    }
}
AWK

read_body() { # <listing> <symbol> <end-address, empty for the whole block>
    awk -v sym="$2" -v endaddr="$3" -f "$TMP/reader.awk" "$1"
}

# The verdict over one body's records: one line on stdout, or fail().
#   $1 listing  $2 symbol  $3 the address past the body, empty where nm has no size to give
judge() {
    _list="$1"
    _sym="$2"
    _end="$3"

    _rec="$TMP/rec"
    read_body "$_list" "$_sym" "$_end" > "$_rec"
    case "$(head -n1 "$_rec")" in
        NOSYM)
            fail "the listing carries no body for '$_sym', so this reader started nowhere. The
  symbol was renamed, made static or inlined away, or the disassembler's output shape moved" ;;
        NOINSN)
            fail "the body of '$_sym' disassembles to no instruction at all, so the corpus is
  UNKNOWN rather than empty" ;;
        BADEND)
            fail "the end address handed in for '$_sym' is not hex, so the body has no bound
  and this reader would decode the padding past it" ;;
    esac
    _total="$(awk '$1 == "COUNT" { print $2; exit }' "$_rec")"
    require_number "$_total" "the instruction count of $_sym"

    _wide="$(awk -F"$TAB" '$2 ~ /_WIDE$/ { print $3 "\t" $4 "\t" $7; exit }' "$_rec")"
    if [ -n "$_wide" ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "in '$_sym' a cell is reached by '$(printf '%s' "$_wide" | cut -f1)', which is not
  a byte access: $(printf '%s' "$_wide" | cut -f2) at $(printf '%s' "$_wide" | cut -f3). One
  cell per line holds only while each access is exactly one cell wide; a wider one carries 3
  neighbouring lines with it and every store of it is a read-modify-write of those"
    fi

    _unres="$(awk -F"$TAB" '$2 == "STORE_UNRESOLVED" { print $3 "\t" $5 "\t" $7; exit }' "$_rec")"
    if [ -n "$_unres" ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "in '$_sym' a byte store at $(printf '%s' "$_unres" | cut -f3) goes through a base
  register no l32r annotation ever bound, so this reader cannot say whether it names a cell.
  That is UNKNOWN, and a gate passing here would assert nothing: the binding was lost at a
  branch join, the address is formed some way this reader does not model, or the annotation
  shape moved"
    fi

    _bad="$(awk -F"$TAB" \
        '$2 == "CELL_STORE" && $6 != "const" { print $1 "\t" $4 "\t" $5 "\t" $6 "\t" $7; exit }' \
        "$_rec")"
    if [ -n "$_bad" ]; then
        _bw="$(printf '%s' "$_bad" | cut -f2)"
        _br="$(printf '%s' "$_bad" | cut -f3)"
        _bp="$(printf '%s' "$_bad" | cut -f4)"
        _ba="$(printf '%s' "$_bad" | cut -f5)"
        sed -n '2,$p' "$_rec" >&2
        case "$_bp" in
            load:*)
                fail "in '$_sym' the byte store to '$_bw' at $_ba stores $_br, which a LOAD of
  '$(printf '%s' "$_bp" | sed 's/^load://')' produced. That is a READ-MODIFY-WRITE of a cell,
  and it is the one shape this design forbids: the ISR's mask store is unlocked, so a peer
  mutating the same cell between this load and this store loses one of the two updates with no
  local symptom. rv64imac spends amoor.w / amoand.w on exactly this; one cell per line is what
  buys lx6 out of needing an atomic RMW at all. Store a whole constant, or the cell is a word
  and this backend owes the mechanism rv64's does" ;;
            *)
                fail "in '$_sym' the byte store to '$_bw' at $_ba stores $_br, whose value this
  reader attributes to [$_bp] and not to a constant materialisation. A cell holds one line's
  state and nothing else, so every write of it is a whole value: 0 for masked, 1 for unmasked.
  A computed value says the cell became a counter, a bitmask or a flag it folds into, and each
  of those is a read-modify-write across cores whether or not a load is spelled beside it" ;;
        esac
    fi

    _st="$(awk -F"$TAB" '$2 == "CELL_STORE" { n++ } END { print n + 0 }' "$_rec")"
    _ld="$(awk -F"$TAB" '$2 == "CELL_LOAD" { n++ } END { print n + 0 }' "$_rec")"
    require_number "$_st" "the cell-store count of $_sym"
    require_number "$_ld" "the cell-load count of $_sym"

    if [ "$(($_st + $_ld))" -eq 0 ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "the body of '$_sym' reaches neither cell across its $_total instruction(s), so
  this gate read a body it has nothing to say about. The array moved behind an accessor, the
  work moved to another symbol, or this reader lost every binding; all three are UNKNOWN"
    fi
    if [ "$_st" -eq 0 ]; then
        sed -n '2,$p' "$_rec" >&2
        fail "the body of '$_sym' loads a cell $_ld time(s) and stores none. Every one of these
  four bodies writes a cell, so a body that only reads one says the write moved somewhere this
  gate does not look, and the invariant is then unasserted rather than held"
    fi

    _words="$(awk -F"$TAB" '$2 == "CELL_STORE" || $2 == "CELL_LOAD" { print $4 }' "$_rec" \
        | sort -u | tr '\n' ' ' | sed 's/ $//')"

    # The memw census is informational and not a verdict: a relaxed atomic is owed no fence by
    # the language, so a toolchain may drop the memw on a tree that is still correct.
    _mw="$(awk -F"$TAB" '
        $2 == "MEMW" { memw[$1] = 1 }
        $2 == "CELL_STORE" || $2 == "CELL_LOAD" { acc[$1] = 1 }
        END {
            n = 0
            for (a in acc) {
                if ((a - 1) in memw) { n++ }
            }
            print n + 0
        }' "$_rec")"
    require_number "$_mw" "the memw census of $_sym"

    echo "   $_sym: $_total instruction(s) on [$_words], $_st cell store(s) all of a constant,
      $_ld cell load(s), memw precedes $_mw/$(($_st + $_ld)) cell access(es)"
}

# --- the planted listings -----------------------------------------------------
# In the shape the invocation below produces, which is --no-show-raw-insn: a listing carrying
# the raw-bytes column reads its first byte group as the mnemonic.
# the store may not touch at all.
cat > "$TMP/plant_ok" <<'EOF'
00001000 <planted_ok>:
    1000:	entry	a1, 32
    1003:	bltz	a2, 1048 <planted_ok+0x48>
    1006:	rsil	a3, 3
    1009:	l32r	a8, 1500 <planted_pool+0x20> (3ffb1220 <g_irq_unmasked>)
    100c:	extui	a9, a2, 0, 5
    100f:	add.n	a8, a8, a9
    1011:	movi.n	a9, 1
    1013:	memw
    1016:	s8i	a9, a8, 0
    1019:	or	a1, a1, a1
    101c:	call8	1600 <planted_callee>
    101f:	beqz.n	a3, 1025 <planted_ok+0x25>
    1022:	nop
    1025:	l32r	a8, 1504 <planted_pool+0x24> (3ffb1200 <g_irq_pending>)
    1028:	extui	a7, a2, 0, 5
    102b:	add.n	a8, a8, a7
    102d:	memw
    1030:	l8ui	a9, a8, 0
    1033:	beqz.n	a9, 1048 <planted_ok+0x48>
    1036:	movi	a10, 0
    1039:	memw
    103c:	s8i	a10, a8, 0
    103f:	movi	a8, 128
    1042:	wsr.intset	a8
    1045:	rsync
    1048:	retw.n

00001300 <planted_join>:
    1300:	entry	a1, 32
    1303:	bltz	a2, 1318 <planted_join+0x18>
    1306:	l32r	a8, 1500 <planted_pool+0x20> (3ffb1220 <g_irq_unmasked>)
    1309:	extui	a9, a2, 0, 5
    130c:	add.n	a8, a8, a9
    130e:	j	1318 <planted_join+0x18>
    1311:	nop
    1318:	movi.n	a9, 0
    131a:	memw
    131d:	s8i	a9, a8, 0
    1320:	retw.n

00001400 <planted_nocell>:
    1400:	entry	a1, 32
    1403:	rsil	a3, 3
    1406:	movi.n	a9, 0
    1408:	wsr.ps	a3
    140b:	rsync
    140e:	retw.n

00001480 <planted_empty>:

00001500 <planted_pool>:
    1500:	.long	0x3ffb1220
EOF

# A cell store giving back the byte the l8ui above it read.
sed 's/movi[[:space:]]*a10, 0/mov.n a10, a9/' "$TMP/plant_ok" > "$TMP/plant_rmw"
# The same as arithmetic, with no load in sight.
sed 's/movi[[:space:]]*a10, 0/addi a10, a9, 1/' "$TMP/plant_ok" > "$TMP/plant_alu"
# The same as a conditional move.
sed 's/movi[[:space:]]*a10, 0/movnez a10, a9, a3/' "$TMP/plant_ok" > "$TMP/plant_cmov"
# The store through a base register no annotation bound.
sed 's/ (3ffb1200 <g_irq_pending>)//' "$TMP/plant_ok" > "$TMP/plant_unbound"
# A cell reached a word at a time, which carries its 3 neighbours.
sed 's/s8i[[:space:]]*a10, a8, 0/s32i a10, a8, 0/' "$TMP/plant_ok" > "$TMP/plant_wide"
# Every memw deleted, which must still PASS: the census below is not a verdict.
sed '/memw/d' "$TMP/plant_ok" > "$TMP/plant_nomemw"

for _p in plant_rmw plant_alu plant_cmov plant_unbound plant_wide plant_nomemw; do
    require_nonempty "$TMP/$_p" "the planted listing '$_p' came out empty, so the control it
  carries would refuse for the wrong reason"
    if cmp -s "$TMP/plant_ok" "$TMP/$_p"; then
        fail "the planted listing '$_p' is identical to the one it derives from, so its sed
  matched nothing and the control asserts whatever plant_ok asserts"
    fi
done

# judge() runs in a subshell so its fail() leaves the control and not this script.
ctl_refuses() { # <listing> <symbol> <what the refusal proves>
    if ( judge "$1" "$2" "" ) >/dev/null 2>&1; then
        fail "the reader ACCEPTED a planted listing it must refuse: $3. A gate that passes
  there cannot go red on the defect it exists for"
    fi
}

ctl_accepts() { # <listing> <symbol> <expected substring> <what it proves>
    if ! ( judge "$1" "$2" "" ) > "$TMP/ctl_out" 2>&1; then
        cat "$TMP/ctl_out" >&2
        fail "the reader REFUSED a planted body carrying the shape this gate requires, so it
  does not recognise it and every verdict below would be meaningless: $4"
    fi
    if ! grep -q "$3" "$TMP/ctl_out"; then
        cat "$TMP/ctl_out" >&2
        fail "the reader accepted the planted body but did not report [$3], so its record does
  not describe the listing it read: $4"
    fi
}

echo "== the lx6 IRQ cells, one per line and never read-modify-written, in $elf =="

ctl_accepts "$TMP/plant_ok" planted_ok '2 cell store(s) all of a constant' \
    "two constant stores, one of them past a branch join and a windowed call"
ctl_accepts "$TMP/plant_ok" planted_ok 'g_irq_pending g_irq_unmasked' \
    "both arrays resolved from the LAST parenthesised group, and not from the pool slot the
  first one names"
ctl_accepts "$TMP/plant_ok" planted_ok 'memw precedes 3/3' \
    "the memw census counted the fence before each of the three cell accesses"
ctl_accepts "$TMP/plant_nomemw" planted_ok 'memw precedes 0/3' \
    "a listing with no memw at all still PASSES, the census being informational: a relaxed
  atomic is owed no fence by the language"

ctl_refuses "$TMP/plant_rmw" planted_ok \
    "a cell store giving back the byte a load of the same cell read, which is the
  read-modify-write this gate exists for"
ctl_refuses "$TMP/plant_alu" planted_ok \
    "a cell store of an arithmetic result, which is a counter and not a line's state"
ctl_refuses "$TMP/plant_cmov" planted_ok \
    "a cell store of a conditional move, which is a flag folded into the cell"
ctl_refuses "$TMP/plant_unbound" planted_ok \
    "a cell store through a base register no annotation bound, which is UNKNOWN and not a pass"
ctl_refuses "$TMP/plant_wide" planted_ok \
    "a cell reached a word at a time, which carries its neighbouring lines with it"
ctl_refuses "$TMP/plant_ok" planted_join \
    "a binding formed only on the branch-not-taken path with the store at the join, where a
  linear walk would name an array the store may never touch"
ctl_refuses "$TMP/plant_ok" planted_nocell \
    "a body that reaches neither cell, which says the accesses moved and not that they are
  clean"
ctl_refuses "$TMP/plant_ok" planted_empty \
    "a body that disassembles to no instruction, which is an UNKNOWN corpus and not an empty
  one"
ctl_refuses "$TMP/plant_ok" a_symbol_no_listing_carries \
    "a symbol the listing does not carry, so a renamed or inlined body would read as a clean
  one"

# --- A2: the declaration each cell owes ---------------------------------------
DECL_PRE='^[[:space:]]*static kickos::Atomic<uint8_t, kickos::Order::RELAXED> '
DECL_POST='\[32\] = [{][}];$'

# Sets KOS_DECL_N to the matching declarations of <name> in <file>. grep exits above 1 for a
# failure of its own, printing no count at all.
decl_count() { # <file> <name>
    if _dc_n="$(grep -c "$DECL_PRE$2$DECL_POST" "$1")"; then
        _dc_rc=0
    else
        _dc_rc=$?
    fi
    if [ "$_dc_rc" -gt 1 ]; then
        fail "exit $_dc_rc from grep while counting the declaration of '$2': the count is
  UNKNOWN and not zero"
    fi
    require_number "$_dc_n" "the declaration count of '$2'"
    KOS_DECL_N="$_dc_n"
}

cat > "$TMP/decl_ok" <<'EOF'
    static kickos::Atomic<uint8_t, kickos::Order::RELAXED> g_irq_unmasked[32] = {};
    static kickos::Atomic<uint8_t, kickos::Order::RELAXED> g_irq_pending[32] = {};
EOF
cat > "$TMP/decl_plain" <<'EOF'
    static uint8_t g_irq_unmasked[32] = {};
    static volatile uint8_t g_irq_pending[32] = {};
EOF
cat > "$TMP/decl_wide" <<'EOF'
    static kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_irq_unmasked[32] = {};
    static kickos::Atomic<int, kickos::Order::RELAXED> g_irq_pending[32] = {};
EOF
cat > "$TMP/decl_order" <<'EOF'
    static kickos::Atomic<uint8_t, kickos::Order::RELEASE> g_irq_unmasked[32] = {};
    static kickos::Atomic<uint8_t, kickos::Order::ACQUIRE> g_irq_pending[32] = {};
EOF

for _cell in g_irq_unmasked g_irq_pending; do
    decl_count "$TMP/decl_ok" "$_cell"
    if [ "$KOS_DECL_N" -ne 1 ]; then
        fail "the declaration matcher counted $KOS_DECL_N hit(s) for '$_cell' in a planted
  declaration spelled exactly as the tree owes it, so it cannot find the real one and A2 would
  refuse a correct tree"
    fi
    for _p in decl_plain decl_wide decl_order; do
        decl_count "$TMP/$_p" "$_cell"
        if [ "$KOS_DECL_N" -ne 0 ]; then
            fail "the declaration matcher counted $KOS_DECL_N hit(s) for '$_cell' in the
  planted '$_p', which does not carry the declaration this gate requires: a matcher that
  accepts it cannot report the revert it exists for"
        fi
    done
done

for _cell in g_irq_unmasked g_irq_pending; do
    decl_count "$SRC" "$_cell"
    if [ "$KOS_DECL_N" -ne 1 ]; then
        grep -n "$_cell\[32\]" "$SRC" >&2 || true
        fail "$SRC carries $KOS_DECL_N declaration(s) of '$_cell' spelled
  'static kickos::Atomic<uint8_t, kickos::Order::RELAXED> $_cell[32] = {};', and exactly one is
  required. A plain byte gives the compiler licence to cache the load across a branch, to sink
  the store past the lock that orders it, or to elide it outright, and none of the three has a
  local symptom; a wider type carries 3 neighbouring lines into every access; an ACQUIRE or
  RELEASE order here orders this cell where what must be ordered is the kernel lock after it"
    fi
done
echo "   both cells declared Atomic<uint8_t, Order::RELAXED>[32] in
      arch/xtensa/lx6/arch_xtensa.cc"

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

# --- A1: the instruction stream -----------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

# The whole set of bodies that touch a cell. A fifth belongs in this list.
for sym in arch_irq_mask arch_irq_unmask arch_irq_clear_pending arch_irq_inject; do
    _row="$(awk -v s="$sym" 'NF == 4 && $4 == s { print $1 "\t" $2; exit }' "$TMP/nm")"
    if [ -z "$_row" ]; then
        fail "no sized defined symbol '$sym' in $elf. The body was renamed, made static,
  inlined away or dropped by --gc-sections, so this gate has an absence it cannot tell apart
  from a failure to read"
    fi
    _start="$(printf '%s' "$_row" | cut -f1)"
    _size="$(printf '%s' "$_row" | cut -f2)"
    case "$_size" in
        *[!0]*) ;;
        *) fail "'$sym' has size 0 in $elf: the symbol survived as a label but its body is
  gone" ;;
    esac
    # Exclusive: past the body objdump decodes alignment padding and the literal pool as
    # instructions.
    _end="$(printf '%x' "$((0x$_start + 0x$_size))")"
    judge "$TMP/dis" "$sym" "$_end"
done

echo "PASS: every lx6 IRQ-cell store is a whole constant, no access is a read-modify-write, and
  both cells are declared Atomic<uint8_t, Order::RELAXED>"
exit 0
