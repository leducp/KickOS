#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ESP UART's TX-empty acknowledgement, read out of the SOURCE TREE.
#
# UART_INT_RAW is a LATCH that only a write of 1 to the matching UART_INT_CLR bit drops (ESP32
# TRM v5.8 appendix "Interrupt Configuration Registers", Table 31.6-4; C6 TRM v1.2 Registers
# 27.3 to 27.6), and UART_TXFIFO_EMPTY's own condition is level on occupancy (ESP32 TRM
# Register 19.10; C6 TRM section 27.4.11). So a push that carries the FIFO past the threshold
# leaves the source asserted on a condition the part has already left, and a drain running with
# the line unmasked re-enters the dispatcher for as long as the ring holds bytes.
#
# TWO CLAUSES, and they bind different sets:
#   push:   every TX push body drops the latch AFTER writing the FIFO.
#   enable: the two KERNEL console backends drop it BEFORE arming INT_ENA. The userspace
#           driver is excluded by ruling: a burst there stops on a full FIFO and re-arms to be
#           re-raised by the latch it left standing (system/driver/espuart/uart_esp.cc).
#
# STRUCTURAL, and it is the only instrument the tree has for this: no emulator in the fleet
# models either part's UART, so nothing can raise the interrupt whose acknowledgement this is.
#
# AN ABSENT BODY IS A FAILURE, not a pass.
#
# usage: check_esp_tx_latch_ack.sh [repo-root]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"

# A FIFO data-register write: the register name on the left of an assignment.
FIFO_RE='(OFF_FIFO|::FIFO)[^=]*=[^=]'
# A latch drop naming the TX-empty source. The bit name is what discriminates it from the
# blanket 0xFFFFFFFF scrub the bring-up paths write.
CLR_RE='(OFF_INT_CLR|::INT_CLR)[^=]*=.*TXFIFO_EMPTY'
# Arming the source at the peripheral.
ENA_RE='(OFF_INT_ENA|::INT_ENA)[^=]*=.*TXFIFO_EMPTY'

# <file>:<function> for every body that pushes a byte at an ESP UART TX FIFO.
PUSH_BODIES='arch/xtensa/chip/esp32/chip_esp32.cc:esp32_tx_push
arch/riscv/chip/esp32c6/chip_esp32c6.cc:c6_tx_push
system/driver/espuart/uart_esp.cc:kos_uart_write'

# <file>:<function> for every body that arms the source for a KERNEL console ring.
ENABLE_BODIES='arch/xtensa/chip/esp32/chip_esp32.cc:esp32_tx_irq_enable
arch/riscv/chip/esp32c6/chip_esp32c6.cc:c6_tx_irq_enable'

scratch_dir

# --- the reader ---------------------------------------------------------------
# Ordinals inside ONE function body, for two caller-supplied patterns. Emits exactly one
# record:
#
#   NOBODY | NOA <n> | NOB <n> | ORDER <a> <b> <n>
#
# `a` and `b` are the line ordinals, counted from the body's opening brace, of the first match
# of a_re and of b_re. A prototype (a line ending in a semicolon) is not a body.
cat > "$TMP/reader.awk" <<'AWK'
BEGIN { state = 0; depth = 0; n = 0; a = 0; b = 0; seen = 0 }
{
    line = $0
    sub(/\/\/.*$/, "", line)
    gsub(/"[^"]*"/, "", line)
}
state == 0 {
    if (line ~ ("(^|[ \t*&])" fn "[ \t]*\\(") && line !~ /;[ \t]*$/) { state = 1; seen = 1 }
    if (state != 1) { next }
}
state == 1 {
    if (line !~ /\{/) { next }
    state = 2
    depth = gsub(/\{/, "{", line) - gsub(/\}/, "}", line)
    next
}
state == 2 {
    n++
    if (line ~ a_re && a == 0) { a = n }
    if (line ~ b_re && b == 0) { b = n }
    depth += gsub(/\{/, "{", line) - gsub(/\}/, "}", line)
    if (depth <= 0) { state = 3 }
}
END {
    if (!seen) { print "NOBODY"; exit }
    if (a == 0) { print "NOA " n; exit }
    if (b == 0) { print "NOB " n; exit }
    print "ORDER " a " " b " " n
}
AWK

read_body() { # <file> <fn> <a_re> <b_re>
    awk -v fn="$2" -v a_re="$3" -v b_re="$4" -f "$TMP/reader.awk" "$1"
}

# --- the reader's controls, before any backend is read -----------------------
cat > "$TMP/ctl_ok.cc" <<'EOF'
void planted_push(uint8_t b)
{
    r32(reg::uart::FIFO) = b;
    r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
}
EOF
cat > "$TMP/ctl_reversed.cc" <<'EOF'
void planted_push(uint8_t b)
{
    r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
    r32(reg::uart::FIFO) = b;
}
EOF
cat > "$TMP/ctl_noclr.cc" <<'EOF'
void planted_push(uint8_t b)
{
    r32(reg::uart::FIFO) = b;
}
EOF
cat > "$TMP/ctl_wrongbit.cc" <<'EOF'
void planted_push(uint8_t b)
{
    r32(reg::uart::FIFO) = b;
    r32(reg::uart::INT_CLR) = reg::uart::RXFIFO_FULL_INT;
}
EOF
cat > "$TMP/ctl_blanket.cc" <<'EOF'
void planted_push(uint8_t b)
{
    r32(reg::uart::FIFO) = b;
    r32(reg::uart::INT_CLR) = 0xFFFFFFFFu;
}
EOF
cat > "$TMP/ctl_commented.cc" <<'EOF'
void planted_push(uint8_t b)
{
    r32(reg::uart::FIFO) = b;
    // r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
}
EOF
cat > "$TMP/ctl_proto.cc" <<'EOF'
void planted_push(uint8_t b);
EOF

ctl="$(read_body "$TMP/ctl_ok.cc" planted_push "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    "ORDER 1 2 3") ;;
    *) fail "the reader answered [$ctl] for a planted push that writes the FIFO and then drops
  the TX-empty latch, so it cannot recognise the correct shape and every verdict below is
  meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_reversed.cc" planted_push "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    "ORDER 2 1 3") ;;
    *) fail "the reader answered [$ctl] for a planted push that clears BEFORE it writes the
  FIFO. That clear drops a latch the push about to happen will set again, so a reader that does
  not report the order cannot go red on it" ;;
esac

ctl="$(read_body "$TMP/ctl_noclr.cc" planted_push "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    NOB*) ;;
    *) fail "the reader answered [$ctl] for a planted push with no clear at all. That is the
  defect this gate exists for, so a reader that passes it is the whole gate failing open" ;;
esac

ctl="$(read_body "$TMP/ctl_wrongbit.cc" planted_push "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    NOB*) ;;
    *) fail "the reader answered [$ctl] for a planted push clearing the RX source instead of
  the TX one. It keys on the register and not on the bit, so a push acknowledging the wrong
  source would read as a clean one" ;;
esac

ctl="$(read_body "$TMP/ctl_blanket.cc" planted_push "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    NOB*) ;;
    *) fail "the reader answered [$ctl] for a planted push writing the blanket 0xFFFFFFFF
  scrub. That word belongs to bring-up and reclaim, where it drops every source including ones
  a driver owns; counting it here would pass a push that names no source at all" ;;
esac

ctl="$(read_body "$TMP/ctl_commented.cc" planted_push "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    NOB*) ;;
    *) fail "the reader answered [$ctl] for a planted push whose clear is COMMENTED OUT, so it
  reads comment text as code and the gate would stay green through the exact edit that removes
  the acknowledgement" ;;
esac

ctl="$(read_body "$TMP/ctl_proto.cc" planted_push "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    NOA*|NOB*|NOBODY) ;;
    *) fail "the reader answered [$ctl] for a file holding only a PROTOTYPE, so it would read a
  declaration as a body and report on statements belonging to whatever follows it" ;;
esac

ctl="$(read_body "$TMP/ctl_ok.cc" a_function_this_file_lacks "$FIFO_RE" "$CLR_RE")"
case "$ctl" in
    NOBODY) ;;
    *) fail "the reader answered [$ctl] for a function the file does not define, so a renamed
  push body would read as a clean one" ;;
esac

# --- the real bodies ----------------------------------------------------------
rc=0

# The ANCHOR is the device touch the acknowledgement is placed against, and its absence means
# this gate has no boundary to judge: UNKNOWN, which is a hard failure and never a pass. The
# ACK is the latch drop, and its absence is the defect itself.
check_ack() { # <clause> <rel> <fn> <anchor re> <anchor name> <where> <consequence>
    _clause="$1"; _rel="$2"; _fn="$3"; _anchor_re="$4"; _anchor="$5"; _where="$6"; _why="$7"
    [ -f "$ROOT/$_rel" ] || fail "no $_rel under $ROOT. An ESP UART backend moved, and this
  gate would assert nothing about it while staying green"
    _rec="$(read_body "$ROOT/$_rel" "$_fn" "$_anchor_re" "$CLR_RE")"
    _kind="$(printf '%s\n' "$_rec" | cut -d' ' -f1)"
    _f2="$(printf '%s\n' "$_rec" | cut -d' ' -f2)"
    _f3="$(printf '%s\n' "$_rec" | cut -d' ' -f3)"
    case "$_kind" in
        NOBODY)
            fail "$_rel defines no '$_fn'. It was renamed, so this gate reports an absence it
  cannot tell apart from a pass" ;;
        NOA)
            fail "'$_fn' in $_rel has no $_anchor across its $_f2 line(s). This gate has no
  boundary to place the latch drop against: UNKNOWN, not a pass" ;;
        NOB)
            bad "'$_fn' in $_rel drops no TX-empty latch across its $_f2 line(s). $_why"
            return ;;
        ORDER) ;;
        *)
            fail "the reader emitted [$_rec] for '$_fn', a record this gate does not model" ;;
    esac
    require_number "$_f2" "the $_anchor ordinal in $_fn"
    require_number "$_f3" "the latch drop ordinal in $_fn"
    case "$_where" in
        after)
            if [ "$_f3" -lt "$_f2" ]; then
                bad "in '$_fn' ($_rel) the latch drop is line #$_f3 and the $_anchor is #$_f2,
  so the drop runs BEFORE it and the touch that follows sets the latch straight back. $_why"
                return
            fi ;;
        before)
            if [ "$_f3" -gt "$_f2" ]; then
                bad "in '$_fn' ($_rel) the latch drop is line #$_f3 and the $_anchor is #$_f2,
  so the drop runs AFTER it, leaving a window in which the armed source is asserted on a stale
  latch. $_why"
                return
            fi ;;
        *)
            fail "check_ack was asked for placement [$_where], which it does not model" ;;
    esac
    echo "   $_clause $_fn ($_rel): $_anchor #$_f2, latch drop #$_f3"
}

echo "== the TX-empty latch drop in every ESP UART push body =="
np=0
for _row in $PUSH_BODIES; do
    rel="${_row%%:*}"
    fn="${_row#*:}"
    check_ack push "$rel" "$fn" "$FIFO_RE" "FIFO write" after \
      "The source stays asserted on a condition the FIFO has already left, and a drain running
  with the line unmasked re-enters the dispatcher for as long as the ring holds bytes: an
  interrupt storm, which costs the machine and not only the console"
    np=$((np + 1))
done
if [ "$np" -lt 3 ]; then
    fail "this gate declares $np push body/bodies. Two chips and the shared userspace driver
  push at an ESP UART TX FIFO, and one dropped from the list is one nothing checks"
fi

echo "== the latch drop ahead of the arm, in the kernel console backends =="
ne=0
for _row in $ENABLE_BODIES; do
    rel="${_row%%:*}"
    fn="${_row#*:}"
    check_ack enable "$rel" "$fn" "$ENA_RE" "INT_ENA arm" before \
      "A latch left standing from a previous burst raises the instant the source is armed, on
  an occupancy the FIFO no longer has"
    ne=$((ne + 1))
done
if [ "$ne" -lt 2 ]; then
    fail "this gate declares $ne console arm body/bodies. Two chips carry a kernel console
  ring, and one dropped from the list is one nothing checks"
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: all $np ESP UART push body/bodies drop the TX-empty latch after writing the FIFO,
  and both $ne console arm bodies drop it before arming the source"
exit 0
