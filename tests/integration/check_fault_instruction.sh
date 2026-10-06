#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The instruction an image's main faults on, where no emulator runs the image to see the fault.
#
#   check_fault_instruction.sh <objdump> <image> <mnemonic>

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 3 ] || fail "usage: check_fault_instruction.sh <objdump> <image> <mnemonic>"
OBJDUMP="$1"
IMAGE="$2"
WANT="$3"
[ -f "$IMAGE" ] || fail "no image at $IMAGE"
scratch_dir
"$OBJDUMP" -d "$IMAGE" > "$TMP/dis" || fail "$OBJDUMP could not disassemble $IMAGE"
# main is kickos_app_main once the build renames it; RX spells C names with a leading `_`. A
# `.L` local label inside it is still main.
awk '/^[0-9a-f]+ <_*kickos_app_main>:/ { on = 1; next } on && /^[0-9a-f]+ <[^.]/ { exit } on' \
    "$TMP/dis" > "$TMP/main"
[ -s "$TMP/main" ] || fail "$IMAGE has no kickos_app_main to read"
# The mnemonic column alone, which objdump separates by tabs from the encoding before it and the
# operands after it: a symbol or an operand spelling the word is not the instruction.
awk -F '\t' -v want="$WANT" 'NF >= 3 { split($3, m, " "); if (m[1] == want) { found = 1 } }
    END { exit !found }' "$TMP/main" || {
    sed -n '1,40p' "$TMP/main" >&2
    fail "main in $IMAGE does not execute \`$WANT\`"
}
echo "PASS: main in $(basename "$IMAGE") faults on \`$WANT\`"
