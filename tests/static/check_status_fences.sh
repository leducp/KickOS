#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The barriers of the init's status seqlock (user/src/init_status.cc, docs/design-m10-target.md
# sections 1.6 and 4), read out of the compiled kickos_user archive. The init writes a record as
# the count odd, a release fence, the fields, the count even; a watcher reads the count, an
# acquire fence, the fields, an acquire fence, the count again. The count's own release store
# orders what comes BEFORE it and nothing after, so without the fences a field store can
# overtake the odd count and a field load can pass either count load. No run sees it: QEMU's TCG
# models no reordering, and a single core reorders nothing a watcher can observe.
#
# Each fence is read as a position, not a presence:
#   status_write  a store-ordering barrier between the first store (the odd count) and the next
#                 store (the first field);
#   status_read   a load-ordering barrier between the first load (the count) and the next load
#                 (the first field), and another between the last load but one (the last field)
#                 and the last load (the count again).
# A barrier elsewhere in the body, or one ordering the other direction, does not count.
#
# REFUSED: a missing barrier at either site, a body this reader cannot find or decode, and a body
# with fewer memory accesses than the sites need. AN ABSENT SYMBOL IS A FAILURE.
#
# usage: check_status_fences.sh <archive> <objdump> <store-operands> <load-operands>
#   <store-operands>  the barrier operands that order stores here, e.g. `ish ishst sy st`
#   <load-operands>   the barrier operands that order loads here, e.g. `ish ishld sy ld`
#   A bare `dmb` reads as the operand `-`.

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_status_fences.sh <archive> <objdump> <store-operands> <load-operands>"
archive="${1:?$_usage}"
objdump="${2:?$_usage}"
store_ops="${3:?$_usage}"
load_ops="${4:?$_usage}"

WRITE=_ZN6kickos4init12status_writeEPNS0_12StatusRecordERKNS0_12StatusFieldsE
READ=_ZN6kickos4init11status_readEPKNS0_12StatusRecordEPNS0_12StatusFieldsE

[ -f "$archive" ] || fail "no archive at $archive"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# One record per body: whether each site holds a barrier of the owed direction. HALF A PROGRAM:
# `seen` and the body scope come from gate.sh's scoped_body, which reads
# tests/lib/objdump_scope.awk and tests/lib/objdump_window.awk ahead of this file.
#   mode=write  the site between store 1 and store 2, barriers from `ops`
#   mode=read   the sites between load 1 and load 2 and between the last two loads
cat > "$TMP/reader.awk" <<'AWK'
BEGIN {
    nops = split(ops, list, " ")
    for (i = 1; i <= nops; i++) { owed[list[i]] = 1 }
}
{
    mnem = win_text
    sub(/ .*$/, "", mnem)
    arg = win_text
    if (index(arg, " ") == 0) { arg = "" } else { sub(/^[^ ]* /, "", arg) }
    gsub(/ /, "", arg)
    if (mnem == "dmb" || mnem == "dsb") {
        if (arg == "") { arg = "-" }
        if (arg in owed) { bars++ }
        next
    }
    if (mnem ~ /^ld/) {
        loads++
        barred_before_load[loads] = bars
    } else if (mnem ~ /^st/) {
        stores++
        barred_before_store[stores] = bars
    }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (win_n == 0) { print "NOINSN"; exit }
    if (mode == "write") {
        if (stores < 2) { print "SHORT stores " stores; exit }
        if (barred_before_store[2] > barred_before_store[1]) { print "FENCED"; exit }
        print "BARE after the odd count store"
        exit
    }
    if (loads < 3) { print "SHORT loads " loads; exit }
    first = barred_before_load[2] > barred_before_load[1]
    last = barred_before_load[loads] > barred_before_load[loads - 1]
    if (first && last) { print "FENCED"; exit }
    if (!first) { print "BARE after the first count load"; exit }
    print "BARE before the second count load"
}
AWK

read_body() { # <listing> <symbol> <mode> <ops>
    scoped_body "$TMP/reader.awk" "$1" "$2" -v win_open= -v win_close= -v mode="$3" -v ops="$4"
}

store_one="${store_ops%% *}"
load_one="${load_ops%% *}"

# --- the reader's controls, before the archive is read ------------------------
# In the shape the invocation below produces (--no-show-raw-insn).
cat > "$TMP/ctl_write" <<EOF
0000000000000000 <$WRITE>:
   0:	ldr	w2, [x0]
   4:	stlr	w3, [x0]
   8:	dmb	$store_one
   c:	ldrh	w4, [x1]
  10:	strh	w4, [x3]
  14:	stlr	w2, [x0]
  18:	ret
EOF
cat > "$TMP/ctl_write_bare" <<EOF
0000000000000000 <$WRITE>:
   0:	ldr	w2, [x0]
   4:	dmb	$store_one
   8:	stlr	w3, [x0]
   c:	ldrh	w4, [x1]
  10:	strh	w4, [x3]
  14:	dmb	$store_one
  18:	stlr	w2, [x0]
  1c:	ret
EOF
cat > "$TMP/ctl_read" <<EOF
0000000000000000 <$READ>:
   0:	ldr	w2, [x0]
   4:	dmb	$load_one
   8:	ldrh	w3, [x3]
   c:	strh	w3, [x1]
  10:	ldrb	w3, [x3]
  14:	dmb	$load_one
  18:	ldr	w0, [x0]
  1c:	ret
EOF
sed '/^   4:/d' "$TMP/ctl_read" > "$TMP/ctl_read_first"
sed '/^  14:/d' "$TMP/ctl_read" > "$TMP/ctl_read_last"
cat > "$TMP/ctl_other" <<EOF
0000000000000000 <somebody_else>:
   0:	ldr	w2, [x0]
   4:	dmb	$load_one
   8:	ldr	w0, [x0]
EOF

_expect() { # <listing> <symbol> <mode> <ops> <verdict-prefix> <prose>
    _p="$(read_body "$1" "$2" "$3" "$4")"
    case "$_p" in
        "$5"*) : ;;
        *) fail "the reader answered [$_p] for $6" ;;
    esac
}
_expect "$TMP/ctl_write" "$WRITE" write "$store_ops" FENCED "a planted fenced write"
_expect "$TMP/ctl_write_bare" "$WRITE" write "$store_ops" BARE \
    "a planted write whose barriers sit before the odd count and before the even one, none between"
_expect "$TMP/ctl_read" "$READ" read "$load_ops" FENCED "a planted fenced read"
_expect "$TMP/ctl_read_first" "$READ" read "$load_ops" "BARE after the first" \
    "a planted read whose first fence was DELETED"
_expect "$TMP/ctl_read_last" "$READ" read "$load_ops" "BARE before the second" \
    "a planted read whose second fence was DELETED"
ctl_dead_reader "$(read_body "$TMP/ctl_other" "$READ" read "$load_ops")" \
    "a listing defining another symbol would read as this one's body"
echo "== control: the reader reports a fenced write and read, a write fenced only around its"
echo "   counts, each read fence deleted, and a foreign symbol"

# --- the archive --------------------------------------------------------------
tool_out "$TMP/listing" "" "$objdump" -d --no-show-raw-insn "$archive"
require_nonempty "$TMP/listing"

rc=0
for _site in "write $WRITE $store_ops" "read $READ $load_ops"; do
    _mode="${_site%% *}"
    _rest="${_site#* }"
    _sym="${_rest%% *}"
    _ops="${_rest#* }"
    verdict="$(read_body "$TMP/listing" "$_sym" "$_mode" "$_ops")"
    case "$verdict" in
        FENCED)
            echo "PASS: status_$_mode carries its seqlock barrier(s) at the fence site(s)" ;;
        NOSYM)
            bad "$archive defines no status_$_mode ($_sym): renamed or inlined, the fences are
  no longer where this gate reads them" ;;
        BARE*)
            bad "status_$_mode in $archive carries no barrier ordering ($_ops) ${verdict#BARE }.
  A watcher can then read a field from a write in progress with the count even both times." ;;
        *)
            bad "status_$_mode in $archive could not be read: $verdict" ;;
    esac
done
exit "$rc"
