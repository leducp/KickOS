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
# Each body is read off tests/lib/strip_comments.awk by tests/static/fn_body.awk, and one that
# is absent, or carries no FIFO write or INT_ENA arm to place the drop against, is UNKNOWN.
#
# usage: check_esp_tx_latch_ack.sh [repo-root]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
STRIP="$(dirname "$0")/../lib/strip_comments.awk"
BODY="$(dirname "$0")/fn_body.awk"
[ -r "$STRIP" ] && [ -r "$BODY" ] || fail "strip_comments.awk or fn_body.awk is unreadable"

# A FIFO data-register write: the register name on the left of an assignment.
FIFO_RE='(OFF_FIFO|::FIFO)[^=]*=[^=]'
# A latch drop naming the TX-empty source. The bit name is what discriminates it from the
# blanket 0xFFFFFFFF scrub the bring-up paths write.
CLR_RE='(OFF_INT_CLR|::INT_CLR)[^=]*=.*TXFIFO_EMPTY'
# Arming the source at the peripheral.
ENA_RE='(OFF_INT_ENA|::INT_ENA)[^=]*=.*TXFIFO_EMPTY'

scratch_dir

first() { # <body> <ere>: the line of its first match
    grep -E "$2" "$1" | sed -n '1s/:.*//p'
}

# <clause> <file> <function>: a finding on stdout, or nothing.
judge() {
    awk -f "$STRIP" < "$2" > "$TMP/raw" || fail "the strip refused $2, so its verdict is UNKNOWN"
    sed 's/^[[:space:]]*//' "$TMP/raw" > "$TMP/stripped"
    awk -v FN="$3" -f "$BODY" "$TMP/stripped" > "$TMP/body" \
        || fail "$2 defines no '$3', so this gate reports an absence it cannot tell from a pass"
    case "$1" in
        push) _re="$FIFO_RE"; _what="FIFO write" ;;
        enable) _re="$ENA_RE"; _what="INT_ENA arm" ;;
    esac
    _a="$(first "$TMP/body" "$_re")"
    [ -n "$_a" ] || fail "'$3' in $2 has no $_what, so there is no boundary to place the latch
      drop against: UNKNOWN, not a pass"
    _c="$(first "$TMP/body" "$CLR_RE")"
    if [ -z "$_c" ]; then
        echo "'$3' in $2 drops no TX-empty latch"
    elif [ "$1" = push ] && [ "$_c" -lt "$_a" ]; then
        echo "'$3' in $2 drops the latch at line $_c, BEFORE its FIFO write at line $_a"
    elif [ "$1" = enable ] && [ "$_c" -gt "$_a" ]; then
        echo "'$3' in $2 drops the latch at line $_c, AFTER its INT_ENA arm at line $_a"
    fi
}

# The planted pair: a push whose drop sits in a comment, and one that drops before it writes.
cat > "$TMP/planted.cc" <<'EOF'
void planted_commented(uint8_t b)
{
    r32(reg::uart::FIFO) = b;
    // r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
}
void planted_reversed(uint8_t b)
{
    r32(reg::uart::INT_CLR) = reg::uart::TXFIFO_EMPTY_INT;
    r32(reg::uart::FIFO) = b;
}
EOF
judge push "$TMP/planted.cc" planted_commented | grep -q 'drops no TX-empty latch' \
    && judge push "$TMP/planted.cc" planted_reversed | grep -q 'BEFORE its FIFO write' \
    || fail "a planted push whose drop is commented out, or runs before its FIFO write, reads clean"

: > "$TMP/findings"
while read -r clause rel fn; do
    judge "$clause" "$ROOT/$rel" "$fn" >> "$TMP/findings"
done <<'BODIES'
push arch/xtensa/chip/esp32/chip_esp32.cc esp32_tx_push
push arch/riscv/chip/esp32c6/chip_esp32c6.cc c6_tx_push
push system/driver/espuart/uart_esp.cc kos_uart_write
enable arch/xtensa/chip/esp32/chip_esp32.cc esp32_tx_irq_enable
enable arch/riscv/chip/esp32c6/chip_esp32c6.cc c6_tx_irq_enable
BODIES
if [ -s "$TMP/findings" ]; then
    sed 's/^/      /' "$TMP/findings" >&2
    fail "an ESP UART TX-empty latch is left standing. After a push, the source stays asserted
      on a condition the FIFO has already left and the drain storms the dispatcher; before an
      arm, a latch from the last burst raises the instant the source is armed."
fi
echo "PASS: the three ESP UART push bodies drop the TX-empty latch after writing the FIFO, and
      both console arm bodies drop it before arming the source"
