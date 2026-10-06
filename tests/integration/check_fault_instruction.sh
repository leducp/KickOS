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
grep -qw "$WANT" "$TMP/main" || {
    sed -n '1,40p' "$TMP/main" >&2
    fail "main in $IMAGE does not execute \`$WANT\`"
}
echo "PASS: main in $(basename "$IMAGE") faults on \`$WANT\`"
