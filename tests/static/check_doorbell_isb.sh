#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The doorbell service's instruction barrier, read out of the LINKED IMAGE: armv8a's
# service_fence must carry an ISB.
#
# A64 broadcasts translation and instruction cache invalidation but no operation causes a Context
# synchronization event on another PE (DDI 0487 M.b, Glossary, "Context Synchronization event"),
# and until a PE takes one, instructions it has already fetched may be re-executed with no bound
# (section B2.7.4.2), so each PE executing changed code must execute its own ISB (section B2.2.5,
# step 3). Where it sits is held by the service's step types: only service_fence mints the token
# the answers need. That it is there at all is not, and QEMU's TCG models no prefetch queue, so a
# runtime arm stays green on an image carrying no ISB.
#
# usage: check_doorbell_isb.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_doorbell_isb.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

# kickos::doorbell::service_fence(kickos::doorbell::Observed)
SYM=_ZN6kickos8doorbell13service_fenceENS0_8ObservedE
# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# Emits ISB <ordinal> <count>, NOISB <count>, NOINSN or NOSYM.
# HALF A PROGRAM: `seen` and the body scope come from gate.sh's scoped_body.
cat > "$TMP/reader.awk" <<'AWK'
{
    text = $0
    sub(/^[^:]*:[ \t]*/, "", text)
    mnem = text
    sub(/[ \t].*$/, "", mnem)
    n++
    if (mnem == "isb" && isb == 0) { isb = n }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (isb == 0) { print "NOISB " n; exit }
    print "ISB " isb " " n
}
AWK

read_body() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2"
}

# Planted listings in the --no-show-raw-insn shape the image is read in.
cat > "$TMP/ctl_ok" <<'EOF'
0000000000001000 <planted_fence>:
    1000:	isb
    1004:	mov	w0, #0x0
    1008:	ret
EOF
cat > "$TMP/ctl_noisb" <<'EOF'
0000000000001000 <planted_fence>:
    1000:	mov	w0, #0x0
    1004:	ret
EOF

ctl="$(read_body "$TMP/ctl_ok" planted_fence)"
[ "$ctl" = "ISB 1 3" ] || fail "the reader answered [$ctl] for a planted body carrying an ISB,
  so every verdict below is meaningless"
ctl="$(read_body "$TMP/ctl_noisb" planted_fence)"
[ "$ctl" = "NOISB 2" ] || fail "the reader answered [$ctl] for a planted body carrying no ISB,
  so an image whose barrier was deleted would not be reported"
ctl_dead_reader "$(read_body "$TMP/ctl_ok" a_symbol_no_listing_carries)" \
    "a renamed fence would read as a clean one"

image_listing "$elf" "$nm" "$objdump" "$SYM_FLOOR" --no-show-raw-insn
image_body "$SYM" "$elf"

rec="$(read_body "$TMP/dis" "$SYM")"
case "$rec" in
    ISB*) ;;
    NOISB*)
        image_body_dump "$SYM"
        fail "'$SYM' in $elf carries NO ISB across its ${rec#NOISB } instruction(s). A peer
  serviced through it takes no Context synchronization event, and an initiator that removed an
  executable mapping is told the rendezvous completed when nothing synchronized" ;;
    NOSYM)
        fail "'$SYM' is a sized symbol in $elf but the disassembly carries no body for it" ;;
    NOINSN)
        fail "the body of '$SYM' in $elf disassembles to no instruction at all" ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

echo "PASS: '$SYM' takes its ISB at instruction #$(echo "$rec" | cut -d' ' -f2)"
exit 0
