#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Two LX6 bodies read out of the LINKED IMAGE, each by an ordinal reader.
#
# ATOMCTL, in kickos_lx6_init, on every image: Special Register 99 is written, and read back
# after the write. Table 190 of the ISA summary (p.313) leaves a WSR of bits 7:6 or 31:9
# undefined, so a write the part ignored is what the read-back catches. The binutils on this
# bench prints no mnemonic for Special Register 99 in this core configuration and emits `excw`
# for both instructions, so the reader keys on the three-byte encoding, whose fifth hex digit is
# the address register and is therefore wildcarded. A disassembler that learns the names is
# matched too.
#
# The park, in kickos_lx6_doorbell_park, where the image drives more than one core: the
# interrupt posture held across the test that decides whether to sleep. REFUSED: an RSIL to
# level 0 anywhere ahead of the first WAITI, a WAITI with nothing having raised the level, and a
# body carrying no WAITI. WAITI takes PS.INTLEVEL from its own immediate, so `waiti 0` is the
# unmask and the sleep together. This reader builds no control-flow graph: what it proves is
# that the body contains no unmask ahead of its sleep and does contain a raise ahead of it, not
# that the raise dominates the sleep on every path. The raise may be an inlined RSIL to a
# nonzero level or a call to arch_irq_save, and both count.
#
# AN EMPTY CORPUS IS A FAILURE, not a pass.
#
# usage: check_lx6_park_mask.sh <elf> <nm> <objdump> <cores>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_lx6_park_mask.sh <elf> <nm> <objdump> <cores>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"
cores="${4:?$_usage}"
require_number "$cores" "the core count"

scratch_dir

# --- the readers --------------------------------------------------------------
# Each emits exactly one record, with ordinals, since each verdict is an order.
# HALF A PROGRAM: `seen` and the body scope come from gate.sh's scoped_body, which reads
# tests/lib/objdump_scope.awk ahead of these files.
cat > "$TMP/insn.awk" <<'AWK'
{
    text = $0
    sub(/^[^:]*:[ \t]*/, "", text)
    raw = text
    sub(/[ \t].*$/, "", raw)
    sub(/^[0-9a-f]+[ \t]+/, "", text)
    mnem = text
    sub(/[ \t].*$/, "", mnem)
    ops = text
    sub(/^[^ \t]*[ \t]*/, "", ops)
    n++
}
AWK
cat "$TMP/insn.awk" - > "$TMP/atomctl.awk" <<'AWK'
# WSR/RSR of Special Register 99: opcode byte, sr byte 0x63, then the address register in the
# high nibble of the third byte.
(raw ~ /^1363[0-9a-f]0$/ || mnem == "wsr.atomctl") && wsr == 0 { wsr = n }
(raw ~ /^0363[0-9a-f]0$/ || mnem == "rsr.atomctl") && rsr == 0 { rsr = n }
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (wsr == 0) { print "NOSEAT " n; exit }
    if (rsr == 0) { print "NOREADBACK " n; exit }
    print "ORDER " wsr " " rsr " " n
}
AWK
cat "$TMP/insn.awk" - > "$TMP/park.awk" <<'AWK'
mnem == "waiti" && waiti == 0 { waiti = n }
mnem == "rsil" {
    imm = ops
    sub(/^.*,[ \t]*/, "", imm)
    gsub(/[ \t]/, "", imm)
    if (imm == "0") {
        if (open == 0) { open = n }
    }
    else if (mask == 0) { mask = n }
}
mnem ~ /^call/ && ops ~ /<arch_irq_save>/ && mask == 0 { mask = n }
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (waiti == 0) { print "NOWAITI " n; exit }
    if (mask == 0) { print "NOMASK " waiti " " n; exit }
    print "ORDER " mask " " (open + 0) " " waiti " " n
}
AWK

# --- the readers' controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which shows the raw-byte column.
ctl() { # <reader> <wanted record> <what a wrong answer means>; the listing on stdin
    cat > "$TMP/ctl"
    _got="$(scoped_body "$TMP/$1.awk" "$TMP/ctl" planted)"
    [ "$_got" = "$2" ] || fail "the $1 reader answered [$_got] rather than [$2] for a planted
  body, so $3"
}
ctl atomctl "ORDER 3 5 6" "it cannot recognise the seat and read-back this gate requires" <<'EOF'
40000000 <planted>:
40000000:	006136               	entry	a1, 48
40000003:	581c                	movi.n	a8, 21
40000005:	136380               	excw
40000008:	002010               	rsync
4000000b:	036360               	excw
4000000e:	f01d                	retw.n
EOF
ctl atomctl "ORDER 2 1 3" "a read-back ahead of its write, which reads the value the boot path
  left, would go unreported" <<'EOF'
40000000 <planted>:
40000000:	036360               	excw
40000003:	136380               	excw
40000006:	f01d                	retw.n
EOF
ctl atomctl "NOSEAT 2" "a body that never writes Special Register 99 would read as seated" <<'EOF'
40000000 <planted>:
40000000:	036360               	excw
40000003:	f01d                	retw.n
EOF
ctl atomctl "NOREADBACK 2" "a write the part ignored would read as a confirmed one" <<'EOF'
40000000 <planted>:
40000000:	136380               	excw
40000003:	f01d                	retw.n
EOF
ctl park "ORDER 2 5 4 6" "it cannot recognise a park that masks, tests and then sleeps with its
  only unmask after the WAITI" <<'EOF'
40000000 <planted>:
40000000:	004136               	entry	a1, 32
40000003:	084265               	call8	40095cf4 <arch_irq_save>
40000006:	0888                	l32i.n	a8, a8, 0
40000008:	007000               	waiti	0
4000000b:	006080               	rsil	a8, 0
4000000e:	f01d                	retw.n
EOF
ctl park "ORDER 3 2 4 5" "an unmask to level 0 ahead of the WAITI, the lost wake this gate exists
  to catch, would go unreported" <<'EOF'
40000000 <planted>:
40000000:	004136               	entry	a1, 32
40000003:	006080               	rsil	a8, 0
40000006:	084265               	call8	40095cf4 <arch_irq_save>
40000009:	007000               	waiti	0
4000000c:	f01d                	retw.n
EOF
ctl park "ORDER 1 0 2 2" "an inlined arch_irq_save would read as no mask at all" <<'EOF'
40000000 <planted>:
40000000:	006380               	rsil	a8, 3
40000003:	007000               	waiti	0
EOF
ctl park "NOMASK 2 2" "a park whose mask was deleted outright would read as a clean one" <<'EOF'
40000000 <planted>:
40000000:	0888                	l32i.n	a8, a8, 0
40000002:	007000               	waiti	0
EOF
ctl park "NOWAITI 2" "a restructured park would pass a check it never reached" <<'EOF'
40000000 <planted>:
40000000:	084265               	call8	40095cf4 <arch_irq_save>
40000003:	f01d                	retw.n
EOF
ctl_dead_reader "$(scoped_body "$TMP/park.awk" "$TMP/ctl" a_symbol_no_listing_carries)" \
    "a renamed body would read as a clean one"

# --- the image ----------------------------------------------------------------
image_listing "$elf" "$nm" "$objdump" 100

read_body() { # <reader> <symbol> -> REC, the reader's record for the image's body
    image_body "$2" "$elf"
    REC="$(scoped_body "$TMP/$1.awk" "$TMP/dis" "$2")"
    case "$REC" in
        NOSYM) fail "'$2' is a sized symbol in $elf but the disassembly carries no body for it,
  so the reader started nowhere. The disassembler's output shape has moved" ;;
        NOINSN) fail "the body of '$2' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
    esac
}

SYM=kickos_lx6_init
read_body atomctl "$SYM"
set -- $REC
case "$1" in
    NOSEAT)
        fail "the body of '$SYM' in $elf never writes Special Register 99 across its $2
  instruction(s). Every core then reaches arch_kernel_lock with ATOMCTL at whatever its boot
  path left, and the reset value 0x28 selects the core-local arm for both cacheable classes: the
  kernel lock's S32C1I would exclude each core from itself and the two cores from nothing" ;;
    NOREADBACK)
        fail "the body of '$SYM' in $elf writes Special Register 99 and never reads it back
  across its $2 instruction(s). Table 190 does not guarantee the fields are writable, so a part
  that ignored the write leaves an image that looks seated and excludes nothing, with no fault
  anywhere. That is UNKNOWN, not a pass" ;;
    ORDER) ;;
    *) fail "the reader emitted [$REC] for '$SYM', a record this gate does not model" ;;
esac
require_number "$2" "the ATOMCTL write's ordinal in $SYM"
require_number "$3" "the ATOMCTL read-back's ordinal in $SYM"
if [ "$3" -le "$2" ]; then
    image_body_dump "$SYM"
    fail "in '$SYM' Special Register 99 is read at instruction #$3 and written at #$2, so the
  read-back does NOT follow the seat. What that reads is the value the boot path left, which is
  the one thing this check exists not to trust"
fi
echo "PASS: '$SYM' seats Special Register 99 at instruction #$2 and verifies it at #$3 of $4,
  so no core reaches arch_kernel_lock on an ATOMCTL this image did not both set and confirm"

if [ "$cores" -le 1 ]; then
    exit 0
fi

SYM=kickos_lx6_doorbell_park
read_body park "$SYM"
set -- $REC
case "$1" in
    NOWAITI)
        fail "the body of '$SYM' in $elf carries no WAITI across its $2 instruction(s). The park
  no longer sleeps, or it was restructured out from under this reader, and either way this gate
  asserts nothing. That is UNKNOWN, not a pass" ;;
    NOMASK)
        fail "the body of '$SYM' in $elf reaches its WAITI at instruction #$2 having raised
  PS.INTLEVEL nowhere across its $3 instruction(s). The cells that decide whether to sleep are
  then read with this core's interrupts open, so a raise serviced between the read and the WAITI
  is a wake the core sleeps through, and the core-start raise is a single edge" ;;
    ORDER) ;;
    *) fail "the reader emitted [$REC] for '$SYM', a record this gate does not model" ;;
esac
mask="$2"
open="$3"
waiti="$4"
require_number "$mask" "the first mask's ordinal in $SYM"
require_number "$open" "the first unmask-to-0's ordinal in $SYM"
require_number "$waiti" "the first WAITI's ordinal in $SYM"
if [ "$mask" -ge "$waiti" ]; then
    image_body_dump "$SYM"
    fail "in '$SYM' the first mask is instruction #$mask and the first WAITI is #$waiti, so
  nothing raises PS.INTLEVEL ahead of the sleep decision and the test is made with this core's
  interrupts open"
fi
if [ "$open" -ne 0 ] && [ "$open" -lt "$waiti" ]; then
    image_body_dump "$SYM"
    fail "in '$SYM' an RSIL to level 0 is instruction #$open, ahead of the WAITI at #$waiti. The
  park then tests the cells that decide whether to sleep with its interrupts already open: the
  level-1 dispatch can take and clear the core-start raise between that test and the WAITI, and
  the core sleeps on a wake that is gone. WAITI takes PS.INTLEVEL from its immediate, so the
  unmask belongs in the WAITI and nowhere ahead of it"
fi
echo "PASS: '$SYM' raises PS.INTLEVEL at instruction #$mask and reaches its WAITI at #$waiti
  of $5 with no unmask between, so the sleep decision is made under this core's own mask and
  the WAITI's immediate is the only unmask ahead of the sleep"
