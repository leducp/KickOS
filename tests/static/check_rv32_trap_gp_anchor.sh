#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# EVERY mtvec TARGET SEATS gp BEFORE ITS FIRST gp-RELATIVE ACCESS, AND THE SEAT IS
# RELAXATION-PROOF. Why gp may not be trusted on entry, and why the seat is written under
# `.option norelax`, is stated at the seat itself (arch/riscv/rv32imac/switch.S); this gate
# holds the two properties and does not restate the reason.
#
# usage: check_rv32_trap_gp_anchor.sh <objdump> <chip-limits-h> <archive> <image>...
#   chip-limits-h  the chip's kickos/chip_limits.h, read for KICKOS_MAX_IRQ: the mcause
#                  interrupt-code space, which is how many slots the vector table owes
#   archive        the rv32imac arch archive, where the mtvec targets are assembled
#   image          one or more LINKED images
#
# TWO CORPORA, BECAUSE NEITHER PROPERTY EXISTS IN ONE PLACE.
#
#   THE ORDER IS ONLY IN THE LINKED IMAGE. gp addressing does not exist in an object file: the
#   linker MAKES it, relaxing an upper/lower pair whose target lands within gp +/- 0x800. An
#   object-level sweep reports zero gp accesses on a tree full of them, so a gp access standing
#   above the seat is visible in the image and nowhere else.
#
#   THE RELAXABILITY IS ONLY IN THE OBJECT. It is an annotation on a relocation, and a linked
#   image carries none. This ld declines gp relaxation for EVERY reference to
#   __global_pointer$, whichever register it lands in, so the relaxable and the norelax
#   spellings link to the same instruction stream and no image can answer this one: the refusal
#   has to be the relaxable SPELLING and not a linker that took the opportunity. objdump -dr and
#   NOT readelf -r, because readelf TRUNCATES a relocation type name (`R_RISCV_PCREL_HI2`) where
#   objdump prints it whole; the match is a PREFIX either way.
#
# HOW THE CORPUS IS DERIVED. mtvec runs in VECTORED mode over the table at TABLE_SYM, so
# hardware enters BASE for an exception and BASE + 4*i for interrupt cause i. The targets are
# the table's own slots, and three things are refused rather than worked around, each of them a
# corpus that would be incomplete:
#   a second mtvec seat anywhere in the image, which would name a table this gate never read;
#   a slot count other than KICKOS_MAX_IRQ, a cause above the last slot vectoring past the
#     table;
#   a slot that is not a 4-byte jump to the START of a symbol, there being no body to walk from
#     an interior address.
#
# THE WALK, ON BOTH CORPORA. From the target's first instruction to the first CONTROL TRANSFER,
# and no further: past a transfer, textual order is not execution order, so a seat below one
# does not answer for the path that jumped over it. The seat must stand before the transfer,
# and nothing naming gp may stand before the seat.
#
# THE SEAT IS A PAIR: `auipc gp,0x<hi>` immediately followed by `addi gp,gp,<lo>`, which
# objdump spells `mv gp,gp` when the low half is zero. The lower half ALONE adds to the gp it
# was meant to replace, and that is what a relaxed seat links to. The two are the same
# instruction and differ only in what precedes them, so the exemption is tested on the
# PRECEDING instruction.
#
# gp is matched as a REGISTER TOKEN in the operands, with objdump's trailing `#` annotation
# removed first, so a symbol whose name merely carries those two letters is not a hit.
#
# WHAT THIS DOES NOT COVER
#   - rv64imac, whose kernel half seats gp out of a link-time word instead;
#     check_riscv_kernel_gp.sh holds the kernel-access direction over that SPLIT image. A
#     whole-image ban on gp is FALSE on a flat rv32 image, where one .text carries kernel and
#     app under one anchor.
#   - every body reached from INSIDE the dispatch, svc_trampoline included: those run with gp
#     already seated.
#   - the exit path's own re-anchor, and the chip startup's gp seat. Neither is an mtvec target.
#   - the app's gp accesses.
#   - whether the seat's VALUE is right. The relocation names the symbol; the linker script
#     places it and its own ASSERTs hold the window.
#   - a trap taken before the mtvec seat runs, and an mtvec value computed at runtime.
#   - whether that one seat names THIS table. objdump annotates the lui/addi pair only when the
#     low half is non-zero, so reading the annotation cries wolf on a table sitting on a 4 KiB
#     boundary. What holds instead: the link is --gc-sections, so a table nothing references is
#     DROPPED, and a missing TABLE_SYM is refused above.
#   - the disassembler's output SHAPE. The self-test drives the judge through a stub objdump, so
#     a future binutils that moves the column layout leaves the controls passing, and only the
#     tool_out landmarks and the corpus refusals stand in the way.

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_rv32_trap_gp_anchor.sh <objdump> <chip-limits-h> <archive> <image>..."
OBJDUMP="${1:?$_usage}"
LIMITS="${2:?$_usage}"
ARCHIVE="${3:?$_usage}"
shift 3
if [ "$#" -lt 1 ]; then
    fail "no image given, so every assertion below would pass vacuously. $_usage"
fi

# The vector table and the relocation symbol the seat resolves to, spelled ONCE and handed to
# every reader: a control judged by a different reading from the tree's proves nothing.
TABLE_SYM=kickos_rv_mtvec
ANCHOR_SYM='__global_pointer$'
# A PREFIX, so a truncating reader and a type name that grows a suffix are both recognised.
RELAX_PREFIX=R_RISCV_RELAX

# Every rv32imac mnemonic that ends a straight-line run, spelled out rather than matched on a
# leading `b`: this ISA has no Zbs, but `bset` and `bext` are one extension away and neither
# transfers control. A leading `c.` is stripped before the lookup.
XFER='beq bne blt bge bltu bgeu beqz bnez blez bgez bltz bgtz bgt ble bgtu bleu
j jal jalr jr ret tail mret sret uret ecall ebreak'
# Every CSR WRITE form. `csrr` is absent on purpose: reading mtvec seats nothing.
CSRWRITE='csrw csrwi csrs csrsi csrc csrci csrrw csrrwi csrrs csrrsi csrrc csrrci'

ANCHOR_HI='^gp,0x[0-9a-f]+$'
ANCHOR_LO='^gp,gp(,-?[0-9]+)?$'
ANCHOR_LO_MNEM='addi mv'
GP_TOKEN='(^|[^0-9A-Za-z_])gp([^0-9A-Za-z_]|$)'
# objdump's trailing annotation. `[[:blank:]]` and not a backslash escape: awk processes escape
# sequences in a -v assignment, so a `\t` there is a tab under one implementation and a bare `t`
# under another.
COMMENT='[[:blank:]]*#.*$'
# The straight-line run's bound. Reaching it is a REFUSAL and never a pass: an entry point with
# no control transfer this close to its first instruction is a parse reading the wrong thing.
WALK_CAP=64

scratch_dir

# --- pass 1: the table, and the one mtvec seat --------------------------------
scan_table() { # <dis>
    awk -F"$TAB" -v tablesym="$TABLE_SYM" -v csrwrite="$CSRWRITE" -v comment="$COMMENT" '
        BEGIN {
            nw = split(csrwrite, W, " ")
            for (i = 1; i <= nw; i++) { iswrite[W[i]] = 1 }
        }
        NF == 1 && $1 ~ /^[0-9a-f]+ <.*>:$/ {
            cur = $1
            sub(/^[0-9a-f]+ </, "", cur)
            sub(/>:$/, "", cur)
            if (cur == tablesym) { table_seen++ }
            intable = (cur == tablesym)
            next
        }
        $1 !~ /^[[:blank:]]*[0-9a-f]+:$/ { next }
        {
            bytes = $2
            gsub(/[[:blank:]]/, "", bytes)
            mnem = $3
            ops = $4
            bare = ops
            sub(comment, "", bare)
            sub(/[[:blank:]]+$/, "", bare)
            if (intable) {
                slots++
                tgt = "-"
                if (mnem == "j" && bare ~ /^[0-9a-f]+ <[^>]*>$/) {
                    tgt = bare
                    sub(/^[0-9a-f]+ </, "", tgt)
                    sub(/>$/, "", tgt)
                }
                print "SLOT " slots " " (length(bytes) / 2) " " mnem " " tgt
            }
            if (cur == "") { next }
            if (iswrite[mnem] && bare ~ /(^|,)mtvec(,|$)/) {
                seat[cur]++
                total++
            }
        }
        END {
            print "MTVEC " total + 0
            print "TABLE " table_seen + 0
            for (s in seat) { print "SEAT " s " " seat[s] }
        }
    ' "$1"
}

# --- pass 2: the walk over the linked image -----------------------------------
scan_entries() { # <dis> <targets>
    awk -F"$TAB" -v targets="$2" -v xfer="$XFER" -v lomnem="$ANCHOR_LO_MNEM" \
        -v anchor_hi="$ANCHOR_HI" -v anchor_lo="$ANCHOR_LO" -v gptoken="$GP_TOKEN" \
        -v comment="$COMMENT" -v cap="$WALK_CAP" '
        function finish(rec) {
            print rec
            walking = 0
        }
        function close_walk() {
            if (walking) { finish("SHORT " who " " n) }
        }
        BEGIN {
            nx = split(xfer, X, " ")
            for (i = 1; i <= nx; i++) { isxfer[X[i]] = 1 }
            nl = split(lomnem, L, " ")
            for (i = 1; i <= nl; i++) { islo[L[i]] = 1 }
            nt = split(targets, T, " ")
            for (i = 1; i <= nt; i++) { want[T[i]] = 1 }
        }
        NF == 1 && $1 ~ /^[0-9a-f]+ <.*>:$/ {
            close_walk()
            cur = $1
            sub(/^[0-9a-f]+ </, "", cur)
            sub(/>:$/, "", cur)
            if (cur in want) {
                seen[cur]++
                who = cur
                walking = 1
                n = 0
                hi = 0
            }
            next
        }
        walking == 0 { next }
        $1 !~ /^[[:blank:]]*[0-9a-f]+:$/ { next }
        {
            n++
            if (n > cap) { finish("CAP " who " " n); next }
            mnem = $3
            ops = $4
            bare = ops
            sub(comment, "", bare)
            sub(/[[:blank:]]+$/, "", bare)
            m = mnem
            if (substr(m, 1, 2) == "c.") { m = substr(m, 3) }
            # The lower half is judged on what PRECEDED it and on nothing else.
            if (hi) {
                if (islo[m] && bare ~ anchor_lo) { finish("OK " who " " n); next }
                finish("HALF " who " " n " " mnem " " bare)
                next
            }
            if (isxfer[m]) { finish("XFERFIRST " who " " n " " mnem); next }
            if (m == "auipc" && bare ~ anchor_hi) { hi = 1; next }
            if (bare ~ gptoken) {
                if (islo[m] && bare ~ anchor_lo) {
                    finish("GPRELAXED " who " " n " " mnem " " bare)
                    next
                }
                finish("GP " who " " n " " mnem " " bare)
                next
            }
        }
        END {
            close_walk()
            for (t in want) {
                if (! (t in seen)) {
                    print "MISS " t
                } else if (seen[t] > 1) {
                    print "DUP " t " " seen[t]
                }
            }
        }
    ' "$1"
}

# --- pass 3: the same walk over the object, for the relocations ---------------
scan_relocs() { # <dr> <targets>
    awk -F"$TAB" -v targets="$2" -v xfer="$XFER" -v anchorsym="$ANCHOR_SYM" \
        -v relaxpfx="$RELAX_PREFIX" -v cap="$WALK_CAP" '
        # The span is judged WHOLE, so a relocation printed after its own instruction line is
        # still attributed to it. The second arm carries the pair: the lower half of the seat
        # comes from the same directive as the upper, so a relaxation annotation on the
        # instruction right after the anchor is the same finding.
        function close_walk(    k, gpn) {
            if (! walking) { return }
            walking = 0
            gpn = 0
            for (k = 1; k <= n; k++) {
                if (gpflag[k]) { gpn++ }
                if (gpflag[k] && rxflag[k]) {
                    print "ORELAXAT " who " " member " " k
                } else if (k > 1 && gpflag[k - 1] && rxflag[k]) {
                    print "ORELAXAT " who " " member " " k
                }
            }
            print "OSPAN " who " " member " " n " " gpn
        }
        BEGIN {
            nx = split(xfer, X, " ")
            for (i = 1; i <= nx; i++) { isxfer[X[i]] = 1 }
            nt = split(targets, T, " ")
            for (i = 1; i <= nt; i++) { want[T[i]] = 1 }
            member = "?"
        }
        NF == 1 && $0 ~ /file format/ {
            close_walk()
            member = $0
            sub(/:.*$/, "", member)
            next
        }
        NF == 1 && $1 ~ /^[0-9a-f]+ <.*>:$/ {
            close_walk()
            cur = $1
            sub(/^[0-9a-f]+ </, "", cur)
            sub(/>:$/, "", cur)
            if (cur in want) {
                seen[cur]++
                who = cur
                walking = 1
                n = 0
            }
            next
        }
        walking == 0 { next }
        # A relocation line carries no address in the first field and names its own offset in
        # the fourth.
        $1 == "" && $4 ~ /^[[:blank:]]*[0-9a-f]+: R_/ {
            if (n < 1) { next }
            t = $4
            sub(/^[[:blank:]]*[0-9a-f]+: /, "", t)
            if ($5 == anchorsym) { gpflag[n] = 1 }
            if (substr(t, 1, length(relaxpfx)) == relaxpfx) { rxflag[n] = 1 }
            next
        }
        $1 !~ /^[[:blank:]]*[0-9a-f]+:$/ { next }
        {
            n++
            gpflag[n] = 0
            rxflag[n] = 0
            if (n > cap) { print "OCAP " who " " member " " n; walking = 0; next }
            m = $3
            if (substr(m, 1, 2) == "c.") { m = substr(m, 3) }
            if (isxfer[m]) { close_walk() }
        }
        END {
            close_walk()
            for (t in want) {
                if (! (t in seen)) {
                    print "OMISS " t
                } else if (seen[t] > 1) {
                    print "ODUP " t " " seen[t]
                }
            }
        }
    ' "$1"
}

judge() { # <objdump> <limits> <archive> <image>...
    _objdump="$1"
    _limits="$2"
    _archive="$3"
    shift 3

    command -v "$_objdump" >/dev/null 2>&1 \
        || fail "no objdump at $_objdump; there is no instruction stream to read, so every
  absence below would be the tool's rather than the image's"
    [ -r "$_limits" ] || fail "cannot read $_limits: the slot count the vector table owes comes
  from KICKOS_MAX_IRQ there, and a count carried in this gate would be a second truth"
    [ -f "$_archive" ] || fail "no archive at $_archive; the relaxability of the seat is a
  relocation annotation and a linked image carries none"

    # A fresh subdirectory per call, so no file a previous control left behind is read as this
    # one's corpus.
    _t="$TMP/judge"
    rm -rf "$_t"
    mkdir -p "$_t"

    _slots="$(sed -n \
        's/^#define[[:space:]]\{1,\}KICKOS_MAX_IRQ[[:space:]]\{1,\}\([0-9]\{1,\}\).*$/\1/p' \
        "$_limits" | tail -n1)"
    require_number "$_slots" "KICKOS_MAX_IRQ out of $_limits"
    if [ "$_slots" -lt 1 ]; then
        fail "KICKOS_MAX_IRQ is $_slots in $_limits; a chip raises at least one cause"
    fi

    _targets=""
    for _img in "$@"; do
        [ -f "$_img" ] || fail "no image at $_img"
        tool_out "$_t/dis" '^[0-9a-f]+ <.*>:$' "$_objdump" -d "$_img"
        require_nonempty "$_t/dis" "$_objdump printed no disassembly for $_img, so the corpus is
  UNKNOWN rather than empty and every verdict below it would be vacuous"

        scan_table "$_t/dis" > "$_t/tab" \
            || fail "the table scan failed on $_img; a scanner that dies emits no record and
  this gate would judge an empty corpus"

        echo "== the mtvec targets of $_img =="

        # --- the one seat -----------------------------------------------------
        _seats="$(sed -n 's/^MTVEC //p' "$_t/tab")"
        require_number "$_seats" "the mtvec write count in $_img"
        if [ "$_seats" -eq 0 ]; then
            fail "not one mtvec write was decoded anywhere in $_img. Every rv32 image seats
  mtvec on its way up, so this is the disassembly parse or the CSR-write list having moved and
  the corpus is UNKNOWN, not an image without a trap vector"
        fi
        if [ "$_seats" -gt 1 ]; then
            awk '$1 == "SEAT" { printf "      %s write(s) in %s\n", $3, $2 }' "$_t/tab" >&2
            fail "$_img seats mtvec $_seats times. This gate derives its corpus from ONE vector
  table, so a second seat names entry points it never read and the verdict below would cover
  only part of the targets. Cover the second table here, or make the second seat name this one"
        fi

        # --- the table, and the slots that are the corpus ---------------------
        _table="$(sed -n 's/^TABLE //p' "$_t/tab")"
        require_number "$_table" "the $TABLE_SYM symbol count in $_img"
        if [ "$_table" -eq 0 ]; then
            fail "no symbol '$TABLE_SYM' in $_img. The vector table was renamed or discarded, so
  this gate has no slot to derive an entry point from and reports an absence it cannot tell
  apart from a failure to read"
        fi
        if [ "$_table" -gt 1 ]; then
            fail "'$TABLE_SYM' is defined $_table times in $_img, so which table the seat names
  is ambiguous and the corpus is UNKNOWN"
        fi

        _n="$(grep -c '^SLOT ' "$_t/tab" || :)"
        require_number "$_n" "the slot count in $_img"
        if [ "$_n" -ne "$_slots" ]; then
            fail "'$TABLE_SYM' holds $_n slot(s) in $_img and KICKOS_MAX_IRQ is $_slots. In
  vectored mode hardware enters BASE + 4*i, so a cause above the last slot vectors past the
  table into whatever follows it. Move both together"
        fi

        _wrong="$(awk '$1 == "SLOT" && ($3 != 4 || $4 != "j" || $5 == "-") \
            { printf "      slot %s: %s-byte %s %s\n", $2, $3, $4, $5 }' "$_t/tab")"
        if [ -n "$_wrong" ]; then
            printf '%s\n' "$_wrong" >&2
            fail "a slot of '$TABLE_SYM' in $_img is not a 4-byte jump to a symbol. Hardware
  enters BASE + 4*i, so a slot of another width moves every slot after it, and a slot that is
  not a jump is an entry point in its own right"
        fi
        _interior="$(awk '$1 == "SLOT" && $5 ~ /[+]/ \
            { printf "      slot %s -> %s\n", $2, $5 }' "$_t/tab")"
        if [ -n "$_interior" ]; then
            printf '%s\n' "$_interior" >&2
            fail "a slot of '$TABLE_SYM' in $_img jumps into the middle of a symbol. There is no
  body to walk from an interior address, so that target's verdict would be UNKNOWN"
        fi

        _img_targets="$(awk '$1 == "SLOT" { print $5 }' "$_t/tab" | sort -u | tr '\n' ' ')"
        require_literal "$_img_targets" "the mtvec target set of $_img"
        _ntg="$(printf '%s' "$_img_targets" | wc -w | tr -d ' ')"
        _disp="$(printf '%s' "$_img_targets" | sed 's/[[:blank:]]*$//')"
        echo "   corpus: $_n slot(s) at $TABLE_SYM, $_ntg target(s): $_disp"

        # --- the walk ---------------------------------------------------------
        scan_entries "$_t/dis" "$_img_targets" > "$_t/walk" \
            || fail "the entry walk failed on $_img; a scanner that dies emits no record and
  this gate would judge an empty corpus"

        for _tg in $_img_targets; do
            _rec="$(awk -v t="$_tg" '$2 == t { n++ } END { print n + 0 }' "$_t/walk")"
            require_number "$_rec" "the verdict count for '$_tg' in $_img"
            if [ "$_rec" -ne 1 ]; then
                fail "the entry walk reported $_rec verdicts for '$_tg' in $_img, and only one
  is read; the rest would go unjudged"
            fi
            _v="$(awk -v t="$_tg" '$2 == t { print $1; exit }' "$_t/walk")"
            require_literal "$_v" "the walk verdict for '$_tg' in $_img"
            _at="$(awk -v t="$_tg" '$2 == t { print $3; exit }' "$_t/walk")"
            _what="$(awk -v t="$_tg" \
                '$2 == t { $1 = ""; $2 = ""; $3 = ""; print; exit }' "$_t/walk")"
            case "$_v" in
                OK)
                    echo "   $_tg: gp seated at instruction $_at, nothing above it names gp"
                    ;;
                GP)
                    fail "'$_tg' ($_img) reaches memory through gp at instruction $_at, above
  the seat:$_what
  gp is not seated on entry, so that access lands wherever the interrupted thread left it"
                    ;;
                GPRELAXED)
                    fail "the gp seat of '$_tg' ($_img) is the LOWER HALF ALONE at instruction
  $_at, with no upper half in front of it:$_what
  That instruction ADDS to the gp it was meant to replace, which is the shape a relaxed seat
  links to, and nothing above it seated gp"
                    ;;
                HALF)
                    fail "the gp seat of '$_tg' ($_img) has an upper half at instruction
  $((_at - 1)) and no lower half after it; instruction $_at is:$_what
  Half a seat leaves gp holding whatever the interrupted thread left in it"
                    ;;
                XFERFIRST)
                    fail "'$_tg' ($_img) reaches a control transfer at instruction $_at before
  it seats gp. Past a transfer textual order is not execution order, so a seat below this one
  does not answer for the path that jumped over it"
                    ;;
                SHORT)
                    fail "'$_tg' ($_img) ends after $_at instruction(s) with neither a gp seat
  nor a control transfer, so the body this gate walked is not the entry point's and its verdict
  is UNKNOWN"
                    ;;
                CAP)
                    fail "'$_tg' ($_img) runs $WALK_CAP instructions with no control transfer.
  An entry point does not, so the disassembly parse is reading something other than an
  instruction stream and the corpus is UNKNOWN"
                    ;;
                MISS)
                    fail "'$_tg' is the target of an mtvec slot in $_img and the disassembly
  carries no body for it, so the walk starts nowhere. The disassembler's output shape has moved"
                    ;;
                DUP)
                    fail "'$_tg' is defined $_at times in $_img, so which body an mtvec slot
  enters is ambiguous and its verdict is UNKNOWN"
                    ;;
                *)
                    fail "the entry walk reported '$_v' for '$_tg' in $_img, which this gate
  does not model, so the verdict is UNKNOWN"
                    ;;
            esac
        done

        _targets="$_targets$_img_targets"
    done

    # --- the relaxability, out of the object ----------------------------------
    # ONE target set across every image: an entry point only one image links is an entry point
    # still, and the archive is where all of them are assembled.
    _all="$(printf '%s' "$_targets" | tr ' ' '\n' | sort -u | tr '\n' ' ')"
    require_literal "$_all" "the mtvec target set over every image"

    tool_out "$_t/dr" '^[0-9a-f]+ <.*>:$' "$_objdump" -dr "$_archive"
    require_nonempty "$_t/dr" "$_objdump printed no disassembly for $_archive"
    scan_relocs "$_t/dr" "$_all" > "$_t/rel" \
        || fail "the relocation walk failed on $_archive; a scanner that dies emits no record
  and this gate would judge an empty corpus"

    echo "== the gp seat of every mtvec target in $_archive =="

    for _tg in $_all; do
        if grep -q "^OMISS $_tg\$" "$_t/rel"; then
            fail "'$_tg' is an mtvec target in the image and $_archive carries no body for it.
  The relaxability of its gp seat is a relocation annotation and lives only here, so its
  verdict is UNKNOWN rather than clean"
        fi
        _dup="$(awk -v t="$_tg" '$1 == "ODUP" && $2 == t { print $3; exit }' "$_t/rel")"
        if [ -n "$_dup" ]; then
            fail "'$_tg' is defined in $_dup members of $_archive, so which one the image linked
  is ambiguous and its verdict is UNKNOWN"
        fi
        if grep -q "^OCAP $_tg " "$_t/rel"; then
            fail "'$_tg' runs $WALK_CAP instructions in $_archive with no control transfer, so
  the parse is reading something other than an instruction stream"
        fi
        _span="$(awk -v t="$_tg" '$1 == "OSPAN" && $2 == t { print $4; exit }' "$_t/rel")"
        require_number "$_span" "the span of '$_tg' in $_archive"
        _gpn="$(awk -v t="$_tg" '$1 == "OSPAN" && $2 == t { print $5; exit }' "$_t/rel")"
        require_number "$_gpn" "the '$ANCHOR_SYM' relocation count in the span of '$_tg'"
        _mem="$(awk -v t="$_tg" '$1 == "OSPAN" && $2 == t { print $3; exit }' "$_t/rel")"
        if [ "$_gpn" -eq 0 ]; then
            fail "the first $_span instruction(s) of '$_tg' ($_mem in $_archive) carry no
  relocation against '$ANCHOR_SYM', so nothing there seats gp out of the link-time anchor"
        fi
        _relax="$(awk -v t="$_tg" '$1 == "ORELAXAT" && $2 == t \
            { printf "      instruction %s of %s\n", $4, $3 }' "$_t/rel")"
        if [ -n "$_relax" ]; then
            printf '%s\n' "$_relax" >&2
            fail "the gp seat of '$_tg' ($_mem in $_archive) carries a $RELAX_PREFIX
  annotation, so the linker is free to fold it into an instruction that reads the gp it was
  meant to replace. The source reads correctly either way and this ld links both spellings to
  the same instruction stream, so nothing but this relocation shows it"
        fi
        echo "   $_tg: $_gpn '$ANCHOR_SYM' relocation(s) in the first $_span instruction(s) of $_mem, none relaxable"
    done

    echo "PASS: every mtvec target seats gp before its first gp-relative access, and no seat is
  relaxable"
}

# --- self-test: one control per clause, each a minimal pair -------------------
# The stub objdump answers for the real one: `-d <f>` prints <f>.dis and `-dr <f>` prints <f>.dr.
# Every planted world is ONE edit away from a clean one, so no other clause can be what reddens
# it.
CTL="$TMP/ctl"
mkdir -p "$CTL/bin"
cat > "$CTL/bin/objdump" <<'STUB'
#!/bin/sh
mode=dis
f=""
for a in "$@"; do
    case "$a" in
        -dr) mode=dr ;;
        -*) ;;
        *) f="$a" ;;
    esac
done
cat "$f.$mode"
STUB
printf '#!/bin/sh\nexit 7\n' > "$CTL/bin/dead"
chmod +x "$CTL/bin/objdump" "$CTL/bin/dead"

CW="$TMP/world"

# An instruction line. objdump prints NO trailing separator for an operand-less one.
insn() { # <addr> <bytes> <mnem> [<ops>]
    if [ -z "${4:-}" ]; then
        printf '%s:\t%s          \t%s\n' "$1" "$2" "$3"
        return
    fi
    printf '%s:\t%s          \t%s\t%s\n' "$1" "$2" "$3" "$4"
}
# The same with the address column padded, the way objdump pads a low-linked object.
oinsn() { # <addr> <bytes> <mnem> [<ops>]
    printf ' '
    insn "$@"
}
sym() { # <addr> <name>
    printf '\n%s <%s>:\n' "$1" "$2"
}
reloc() { # <offset> <type> <symbol>
    printf '\t\t\t%s: %s\t%s\n' "$1" "$2" "$3"
}
member() { # <name>
    printf '%s:     file format elf32-littleriscv\n' "$1"
}

# The head of an entry point in the linked image. ONE shape per control, and every one but
# `clean` differs from it in a single instruction.
entry_head() { # <shape>
    insn 80000a80 34011173 csrrw sp,mscratch,sp
    insn 80000a84 fe512e23 sw 't0,-4(sp)'
    case "$1" in
        gpfirst)
            insn 80000a88 fe612c23 lw 't1,-1444(gp)'
            ;;
        gpannot)
            # The gp here is in the ANNOTATION and not in an operand. Nothing forbids an
            # assembly label named `gp`, and the annotation is stripped before the register scan.
            insn 80000a88 fe612c23 lw 't1,8(a1) # 80028920 <gp>'
            ;;
        *)
            insn 80000a88 fe612c23 sw 't1,-8(sp)'
            ;;
    esac
    case "$1" in
        noanchor)
            :
            ;;
        gprelaxed)
            insn 80000a8c 00018193 addi gp,gp,1428
            ;;
        halfanchor)
            insn 80000a8c 00018197 auipc gp,0x18
            insn 80000a90 59410113 addi sp,sp,1428
            ;;
        xferabove)
            # The seat is HERE and correct; only the jump in front of it is planted, so the
            # transfer list is the one reading that can redden this world.
            insn 80000a8c 0080006f j '80000b00 <kos_elsewhere>'
            insn 80000a90 00018197 auipc gp,0x18
            insn 80000a94 59418193 addi 'gp,gp,1428 # 80028920 <__global_pointer$>'
            ;;
        mvlo)
            insn 80000a8c 00018197 auipc gp,0x18
            insn 80000a90 00018193 mv gp,gp
            ;;
        nocap)
            _c=0
            while [ "$_c" -lt 80 ]; do
                insn 80000a8c 00000013 nop
                _c=$((_c + 1))
            done
            ;;
        *)
            insn 80000a8c 00018197 auipc gp,0x18
            insn 80000a90 59418193 addi 'gp,gp,1428 # 80028920 <__global_pointer$>'
            ;;
    esac
    # Below the seat: a branch, and then a gp access that is the app's business and no finding.
    # The walk must stop before either.
    insn 80000a94 300022f3 csrr t0,mstatus
    insn 80000a98 06031563 bnez 't1,80000b00 <kos_elsewhere>'
    insn 80000a9c fe512e23 lw 'a5,-1444(gp)'
    insn 80000aa0 00008067 ret
}

# The vector table's slots. Only slot 0 ever differs.
slots() { # <count> <shape>
    _i=0
    while [ "$_i" -lt "$1" ]; do
        _a="$(printf '8000%04x' $((0x0a00 + _i * 4)))"
        if [ "$_i" -eq 0 ]; then
            case "$2" in
                nojump)
                    insn "$_a" 00000013 nop
                    _i=$((_i + 1))
                    continue
                    ;;
                narrow)
                    insn "$_a" a001 c.j '80000a80 <trap_entry>'
                    _i=$((_i + 1))
                    continue
                    ;;
                interior)
                    insn "$_a" 0800006f j '80000a84 <trap_entry+0x4>'
                    _i=$((_i + 1))
                    continue
                    ;;
                second)
                    insn "$_a" 0800006f j '80000b40 <kos_second_entry>'
                    _i=$((_i + 1))
                    continue
                    ;;
                *)
                    :
                    ;;
            esac
        fi
        insn "$_a" 0800006f j '80000a80 <trap_entry>'
        _i=$((_i + 1))
    done
}

W_SLOTS=32
W_SLOTSHAPE=plain
W_ENTRY=clean
W_SEATS=1
W_SEATSHAPE=annotated
W_TABLE=1
W_SECOND=clean
W_RELOC=clean

reset_world() {
    W_SLOTS=32
    W_SLOTSHAPE=plain
    W_ENTRY=clean
    W_SEATS=1
    W_SEATSHAPE=annotated
    W_TABLE=1
    W_SECOND=clean
    W_RELOC=clean
}

world() {
    rm -rf "$CW"
    mkdir -p "$CW"
    : > "$CW/elf"
    : > "$CW/arch.a"
    printf '#define KICKOS_MAX_IRQ 32\n' > "$CW/limits.h"
    {
        printf 'Disassembly of section .text:\n'
        sym 80000900 kickos_rv32_init
        insn 80000900 800017b7 lui a5,0x80001
        if [ "$W_SEATSHAPE" = page ]; then
            # The table landing on a 4 KiB boundary: the low half is zero, objdump spells the
            # pair `lui`+`mv` and prints NO annotation. This is the esp32c6 shape.
            insn 80000904 00078793 mv a5,a5
        else
            insn 80000904 a0078793 addi 'a5,a5,-1536 # 80000a00 <kickos_rv_mtvec>'
        fi
        insn 80000908 0017e793 ori a5,a5,1
        _s=0
        while [ "$_s" -lt "$W_SEATS" ]; do
            insn 8000090c 30579073 csrw mtvec,a5
            _s=$((_s + 1))
        done
        insn 80000910 00008067 ret
        if [ "$W_TABLE" -eq 1 ]; then
            sym 80000a00 kickos_rv_mtvec
            slots "$W_SLOTS" "$W_SLOTSHAPE"
        fi
        sym 80000a80 trap_entry
        entry_head "$W_ENTRY"
        sym 80000b00 kos_elsewhere
        insn 80000b00 fe512e23 lw 'a5,-1444(gp)'
        insn 80000b04 00008067 ret
        sym 80000b40 kos_second_entry
        if [ "$W_SECOND" = noanchor ]; then
            insn 80000b40 fe512e23 lw 'a5,-1444(gp)'
        else
            insn 80000b40 34011173 csrrw sp,mscratch,sp
            insn 80000b44 00018197 auipc gp,0x18
            insn 80000b48 59418193 addi 'gp,gp,1428 # 80028920 <__global_pointer$>'
        fi
        insn 80000b4c 00008067 ret
    } > "$CW/elf.dis"
    {
        member arch_rv32imac.cc.obj
        printf '\nDisassembly of section .text:\n'
        sym 00000010 kickos_rv32_init
        oinsn 10 00008067 ret
        member switch.S.obj
        printf '\nDisassembly of section .text:\n'
        sym 000000fe kickos_rv_mtvec
        oinsn fe 0820006f j '180 <trap_entry>'
        reloc fe R_RISCV_JAL trap_entry
        sym 00000180 trap_entry
        oinsn 180 34011173 csrrw sp,mscratch,sp
        oinsn 184 fe512e23 sw 't0,-4(sp)'
        oinsn 188 fe612c23 sw 't1,-8(sp)'
        if [ "$W_RELOC" != noanchor ]; then
            oinsn 18c 00000197 auipc gp,0x0
            reloc 18c R_RISCV_PCREL_HI20 '__global_pointer$'
            if [ "$W_RELOC" = relax ]; then
                reloc 18c R_RISCV_RELAX '*ABS*'
            fi
            oinsn 190 00018193 mv gp,gp
            reloc 190 R_RISCV_PCREL_LO12_I '.L0 '
            if [ "$W_RELOC" = relaxlo ]; then
                reloc 190 R_RISCV_RELAX '*ABS*'
            fi
        fi
        if [ "$W_RELOC" = postrelax ]; then
            # A relaxable `la` BELOW the seat, which is the bench window's or the app's
            # business: gp is already seated where it stands, so it is no finding.
            oinsn 194 00000317 auipc t1,0x0
            reloc 194 R_RISCV_PCREL_HI20 g_bench_cycle_src
            reloc 194 R_RISCV_RELAX '*ABS*'
            oinsn 198 00030313 mv t1,t1
            reloc 198 R_RISCV_PCREL_LO12_I '.L0 '
            reloc 198 R_RISCV_RELAX '*ABS*'
        fi
        oinsn 19c 06031563 bnez 't1,20a <.Ltrap_from_m>'
        reloc 19c R_RISCV_BRANCH .Ltrap_from_m
        oinsn 1a0 fe512e23 lw 'a5,-1444(gp)'
        oinsn 1a4 00008067 ret
        sym 000001b0 kos_second_entry
        oinsn 1b0 00000197 auipc gp,0x0
        reloc 1b0 R_RISCV_PCREL_HI20 '__global_pointer$'
        oinsn 1b4 00018193 mv gp,gp
        reloc 1b4 R_RISCV_PCREL_LO12_I '.L0 '
        oinsn 1b8 00008067 ret
        if [ "$W_RELOC" = dupmember ]; then
            member second.S.obj
            printf '\nDisassembly of section .text:\n'
            sym 00000000 trap_entry
            oinsn 0 00000197 auipc gp,0x0
            reloc 0 R_RISCV_PCREL_HI20 '__global_pointer$'
            oinsn 4 00018193 mv gp,gp
            oinsn 8 00008067 ret
        fi
    } > "$CW/arch.a.dr"
}

call() {
    ( judge "$CTL/bin/objdump" "$CW/limits.h" "$CW/arch.a" "$CW/elf" ) > "$CTL/out" 2>&1
}

FIRED=0
ctl() { # <label> <expect-ere>
    world
    if call; then
        cat "$CTL/out"
        fail "positive control '$1' passed, so the clause it plants for fires on nothing"
    fi
    if ! grep -qE "$2" "$CTL/out"; then
        cat "$CTL/out" >&2
        fail "positive control '$1' reddened for the wrong reason, expected /$2/. A control
      another clause catches proves nothing about the clause it plants for"
    fi
    FIRED=$((FIRED + 1))
    reset_world
}
QUIET=0
neg() { # <label>
    world
    if call; then
        QUIET=$((QUIET + 1))
    else
        cat "$CTL/out" >&2
        fail "negative control '$1' reports; the gate would redden a correct image"
    fi
    reset_world
}

# --- the negatives: every shape a correct image may carry ---------------------
neg clean
# The seat's lower half when the low twelve bits are zero: objdump spells it `mv gp,gp`, which
# is the SAME instruction the relaxed shape is, and only the upper half in front of it tells
# the two apart. This control and gp_seat_relaxed below are that minimal pair.
W_ENTRY=mvlo; neg seat_low_half_zero
# A symbol named `gp` in the annotation, which is not the register.
W_ENTRY=gpannot; neg gp_in_the_annotation
# A relaxable `la` BELOW the seat, where gp is already seated.
W_RELOC=postrelax; neg relaxable_below_the_seat
# Two entry points, both seated, which is what proves the corpus is the target SET.
W_SLOTSHAPE=second; neg two_targets_both_seated
# The mtvec seat with NO annotation on it, which is what objdump prints when the table lands on
# a 4 KiB boundary. Nothing above reads that annotation, and this control is what keeps a future
# edit from making the corpus rest on one.
W_SEATSHAPE=page; neg seat_without_an_annotation
[ "$QUIET" -eq 6 ] || fail "$QUIET of 6 negative controls ran silent"

# --- the three clauses this gate exists for ----------------------------------
# The seat deleted and a transfer standing above it are ONE clause read twice: with the seat
# gone the walk runs on to the demux branch, which is the transfer. The object corpus answers
# the deletion on its own terms further down.
W_ENTRY=noanchor
ctl seat_deleted 'reaches a control transfer at instruction [0-9]+ before'
W_ENTRY=gpfirst
ctl gp_above_the_seat 'reaches memory through gp at instruction'
W_ENTRY=gprelaxed
ctl gp_seat_relaxed 'is the LOWER HALF ALONE at instruction'
W_RELOC=relax
ctl seat_relaxable "carries a $RELAX_PREFIX"
W_RELOC=relaxlo
ctl seat_lower_half_relaxable "carries a $RELAX_PREFIX"
W_RELOC=noanchor
ctl seat_deleted_in_the_object 'relocation against .__global_pointer'

# --- the walk's own bounds ---------------------------------------------------
W_ENTRY=halfanchor
ctl half_a_seat 'and no lower half after it'
W_ENTRY=xferabove
ctl transfer_above_the_seat 'reaches a control transfer at instruction'
W_ENTRY=nocap
ctl no_transfer_within_the_cap "runs $WALK_CAP instructions with no control transfer"

# --- the corpus: the seat, the table, and the slots --------------------------
W_SEATS=2
ctl two_mtvec_seats 'seats mtvec 2 times'
W_SEATS=0
ctl no_mtvec_seat 'not one mtvec write was decoded'
W_TABLE=0
ctl table_absent "no symbol '$TABLE_SYM'"
W_SLOTS=31
ctl slot_count_short 'holds 31 slot\(s\).* and KICKOS_MAX_IRQ is 32'
W_SLOTSHAPE=nojump
ctl slot_is_not_a_jump 'is not a 4-byte jump to a symbol'
W_SLOTSHAPE=narrow
ctl slot_is_narrow 'is not a 4-byte jump to a symbol'
W_SLOTSHAPE=interior
ctl slot_jumps_into_a_symbol 'jumps into the middle of a symbol'
# A second target the table names whose own seat is missing: the walk covers every target and
# not just the first.
W_SLOTSHAPE=second
W_SECOND=noanchor
ctl second_target_unseated 'reaches memory through gp at instruction'
W_RELOC=dupmember
ctl target_in_two_members 'is defined in 2 members of'

# --- the inputs and the tool -------------------------------------------------
world
printf '#define KICKOS_MAX_IRQ zero\n' > "$CW/limits.h"
if call; then
    fail "a chip_limits.h with no readable KICKOS_MAX_IRQ read the whole image and said nothing"
fi
if ! grep -q 'KICKOS_MAX_IRQ out of' "$CTL/out"; then
    cat "$CTL/out" >&2
    fail "an unreadable KICKOS_MAX_IRQ reddened the gate without naming the count"
fi
FIRED=$((FIRED + 1))

world
if ( judge "$CTL/bin/dead" "$CW/limits.h" "$CW/arch.a" "$CW/elf" ) > "$CTL/out" 2>&1; then
    fail "an objdump that exits nonzero read the whole image and said nothing"
fi
if ! grep -q '^FAIL: exit 7 from: ' "$CTL/out"; then
    cat "$CTL/out" >&2
    fail "a dead objdump reddened the gate without naming the tool failure"
fi
FIRED=$((FIRED + 1))

world
printf 'no symbol header here\n' > "$CW/elf.dis"
if call; then
    fail "a disassembly with no symbol header at all read as a clean image"
fi
if ! grep -q 'nothing matching' "$CTL/out"; then
    cat "$CTL/out" >&2
    fail "a disassembly with no landmark reddened the gate without naming the landmark"
fi
FIRED=$((FIRED + 1))

world
rm -f "$CW/elf"
if call; then
    fail "a missing image read as a clean one"
fi
if ! grep -q 'no image at' "$CTL/out"; then
    cat "$CTL/out" >&2
    fail "a missing image reddened the gate for the wrong reason"
fi
FIRED=$((FIRED + 1))
reset_world

[ "$FIRED" -eq 22 ] || fail "$FIRED of 22 positive controls reddened"

# --- the readings withdrawn, one per negative control that needed one --------
# A control kept quiet by the WRONG clause cannot be told from one the gate is blind to, so each
# reading below is turned off and the control it exists for must change its answer.
NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'

# The control-transfer list. With it emptied the walk reaches the seat below the transfer and
# the control PASSES, so the list is what reddened it and not the absence of a seat.
W_ENTRY=xferabove
world
if ( XFER=""
     judge "$CTL/bin/objdump" "$CW/limits.h" "$CW/arch.a" "$CW/elf" ) > "$CTL/out" 2>&1; then
    :
else
    cat "$CTL/out" >&2
    fail "with the control-transfer list emptied the transfer-above-the-seat control still
      reddens, so the list is not what caught it and that control proves nothing"
fi
reset_world

# The seat's lower half. With its shape neutered the clean image must report half a seat, so
# the pairing is required and not merely listed.
world
if ( ANCHOR_LO="$NEVER"
     judge "$CTL/bin/objdump" "$CW/limits.h" "$CW/arch.a" "$CW/elf" ) > "$CTL/out" 2>&1; then
    fail "with the lower half's shape neutered the clean image still passed, so no verdict
      depends on the seat being a PAIR"
fi
if ! grep -q 'and no lower half after it' "$CTL/out"; then
    cat "$CTL/out" >&2
    fail "neutering the lower half's shape reddened something other than the pairing"
fi

# objdump's annotation. With the strip withdrawn a symbol named `gp` becomes a register hit and
# its control must report, so the strip is a near miss.
W_ENTRY=gpannot
world
if ( COMMENT="$NEVER"
     judge "$CTL/bin/objdump" "$CW/limits.h" "$CW/arch.a" "$CW/elf" ) > "$CTL/out" 2>&1; then
    fail "with the annotation strip withdrawn a symbol named 'gp' still reached no verdict, so
      that control is not a near miss and proves nothing"
fi
if ! grep -q 'reaches memory through gp at instruction' "$CTL/out"; then
    cat "$CTL/out" >&2
    fail "withdrawing the annotation strip reddened something other than the register scan"
fi
reset_world

# The relocation symbol that marks the seat. Re-pointed at the one the relaxable `la` BELOW the
# seat names, that control must report, so the scoping is what kept it quiet and the clause is
# not blind to a relaxation annotation.
W_RELOC=postrelax
world
if ( ANCHOR_SYM=g_bench_cycle_src
     judge "$CTL/bin/objdump" "$CW/limits.h" "$CW/arch.a" "$CW/elf" ) > "$CTL/out" 2>&1; then
    fail "with the seat's relocation symbol re-pointed at the one the relaxable la below it
      names, that control still reached no verdict, so the scoping is not what kept it quiet"
fi
if ! grep -q "carries a $RELAX_PREFIX" "$CTL/out"; then
    cat "$CTL/out" >&2
    fail "re-pointing the seat's relocation symbol reddened something other than the
      relaxability clause"
fi
reset_world

# --- the images ---------------------------------------------------------------
judge "$OBJDUMP" "$LIMITS" "$ARCHIVE" "$@"
exit 0
