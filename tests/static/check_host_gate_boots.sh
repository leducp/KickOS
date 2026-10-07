#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No test labelled host boots an image under an emulator: a step selecting `-L host` runs where
# no emulator need run the board, and the decline pass keeps every host gate. A test boots when
# its command or ENVIRONMENT sets QEMU_MACHINE or names a qemu-system program, read from the
# build's own ctest list. A planted list is read first, so a reader that finds nothing fails.
#
#   check_host_gate_boots.sh <kickos-source> <ctest> <build>

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_host_gate_boots.sh <kickos-source> <ctest> <build>"
KICKOS_SRC="${1:?$USAGE}"
CTEST="${2:?$USAGE}"
BUILD="${3:?$USAGE}"

scratch_dir

host_boots() { # <json> <out>
    python3 "$KICKOS_SRC/tests/static/ctest_tests.py" --json "$1" --out "$TMP/tests.tsv" \
        --host-boots "$2" > "$TMP/reader.err" 2>&1 \
        || fail "ctest_tests.py could not read $1: $(sed -n '1,3p' "$TMP/reader.err" | tr '\n' ' ')"
}

cat > "$TMP/planted.json" <<'EOF'
{ "tests": [
  { "name": "by_command", "command": ["/usr/bin/cmake", "-E", "env", "QEMU_MACHINE=q35", "/g.sh"],
    "properties": [ { "name": "LABELS", "value": ["host"] } ] },
  { "name": "by_environment", "command": ["/g.sh"],
    "properties": [ { "name": "LABELS", "value": ["host"] },
                    { "name": "ENVIRONMENT", "value": ["QEMU_MACHINE=virt"] } ] },
  { "name": "by_program", "command": ["/usr/bin/qemu-system-arm", "-M", "virt"],
    "properties": [ { "name": "LABELS", "value": ["host"] } ] },
  { "name": "image_gate", "command": ["/usr/bin/cmake", "-E", "env", "QEMU_MACHINE=q35", "/g.sh"],
    "properties": [ { "name": "LABELS", "value": ["image"] } ] },
  { "name": "declined", "command": ["/usr/bin/cmake", "-E", "env", "QEMU_MACHINE=q35", "/g.sh"],
    "properties": [ { "name": "LABELS", "value": ["host"] }, { "name": "DISABLED", "value": true } ] },
  { "name": "host_only", "command": ["/g.sh", "--controls"],
    "properties": [ { "name": "LABELS", "value": ["host"] },
                    { "name": "ENVIRONMENT", "value": ["QEMU_TIMEOUT=60"] } ] }
] }
EOF
host_boots "$TMP/planted.json" "$TMP/planted.out"
printf 'by_command\nby_environment\nby_program\n' > "$TMP/planted.want"
cmp -s "$TMP/planted.out" "$TMP/planted.want" \
    || fail "the planted list's booting host gates read as '$(tr '\n' ' ' < "$TMP/planted.out")', \
not 'by_command by_environment by_program'"

"$CTEST" --test-dir "$BUILD" --show-only=json-v1 > "$TMP/tests.json" \
    || fail "ctest could not list the tests of $BUILD"
host_boots "$TMP/tests.json" "$TMP/boots"
awk -F '\t' '$2 ~ /,host,/ && $3 == 0 { found = 1 } END { exit !found }' "$TMP/tests.tsv" \
    || fail "$BUILD lists no enabled host gate, so this check read nothing"
if [ -s "$TMP/boots" ]; then
    fail "host-labelled gates that boot an image under an emulator: $(tr '\n' ' ' < "$TMP/boots")"
fi
echo "PASS: no host gate of $BUILD boots an image"
