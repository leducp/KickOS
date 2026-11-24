#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/amp/merge-partition.sh over node ELFs linked here with the host's tools: a node naming no
# load-apart span merges into the blob `objcopy -O binary` makes of it whole; a node naming one
# merges the rest into that blob and the span's sections into one more load segment at the span;
# two merges of the same nodes are the same bytes; and a split by a name two sections carry or
# objcopy reads as a pattern, or a load in a node's flash window outside its span, is refused.
#
#   check_merge_partition.sh <cc> <objcopy> <ld> <readelf> <objdump>

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 5 ] || fail "usage: check_merge_partition.sh <cc> <objcopy> <ld> <readelf> <objdump>"
CC="$1"
OBJCOPY="$2"
LD="$3"
READELF="$4"
OBJDUMP="$5"
MERGE="$(cd "$(dirname "$0")/../.." && pwd)/tools/amp/merge-partition.sh"
[ -f "$MERGE" ] || fail "no $MERGE"

scratch_dir

cat > "$TMP/node.s" <<'EOF'
.section .text,"ax"
.ascii "node text"
.section .data,"aw"
.ascii "node data"
.section .flash,"a"
.ascii "flash bytes"
.section .flash2,"a"
.ascii "more flash"
EOF
"$CC" -c "$TMP/node.s" -o "$TMP/node.o" || fail "$CC does not assemble a node"

# <name> <base> <sections>: an ELF of the node's text and data at <base>, then <sections>, linker
# script lines, each placing more of it.
node() {
    {
        echo 'SECTIONS {'
        echo "  . = $2;"
        echo '  .text : { *(.text) }'
        echo '  .data : { *(.data) }'
        printf '%s\n' "$3"
        echo '  /DISCARD/ : { *(*) }'
        echo '}'
    } > "$TMP/$1.ld"
    "$LD" --build-id=none -T "$TMP/$1.ld" -o "$TMP/$1" "$TMP/node.o" > "$TMP/$1.ldlog" 2>&1 \
        || fail "$LD does not link $1: $(cat "$TMP/$1.ldlog")"
}
SPAN='  . = 0x800000;
  __kickos_amp_load_apart_start = .;
  .flash : { *(.flash) }
  __kickos_amp_load_apart_end = .;'
WINDOW='  __kickos_esp32_flash_window_start = 0x800000;
  __kickos_esp32_flash_window_end = 0x1000000;'

# <out> <node>...: the merge, into $TMP/merge.out.
merge() {
    _mg_out="$1"
    shift
    sh "$MERGE" "$OBJCOPY" "$LD" "$READELF" "$OBJDUMP" "$TMP/$_mg_out" "$@" > "$TMP/merge.out" 2>&1
}
# <elf> <section> <out>: the section's bytes.
section() {
    "$OBJCOPY" -O binary -j "$2" "$1" "$3" || fail "$OBJCOPY cannot take $2 out of $1"
}
# <elf> <section>: the section's address, as readelf prints it.
address() {
    LC_ALL=C "$READELF" -SW "$1" | awk -v s="$2" '{ sub(/^ *\[ *[0-9]+\] */, "") } $1 == s { print $3 }'
}

node plain 0x100000 ''
node peer 0x200000 ''
merge two.elf "$TMP/plain" "$TMP/peer" || fail "two nodes naming no span are refused: $(cat "$TMP/merge.out")"
"$OBJCOPY" -O binary "$TMP/plain" "$TMP/plain.bin"
section "$TMP/two.elf" .knode0 "$TMP/two.knode0"
cmp -s "$TMP/plain.bin" "$TMP/two.knode0" \
    || fail "a node naming no span does not merge into the blob objcopy makes of it whole"
merge two-again.elf "$TMP/plain" "$TMP/peer" || fail "the second merge is refused: $(cat "$TMP/merge.out")"
cmp -s "$TMP/two.elf" "$TMP/two-again.elf" || fail "two merges of the same nodes differ"

node span 0x100000 "$SPAN"
merge apart.elf "$TMP/span" "$TMP/peer" || fail "a node naming a span is refused: $(cat "$TMP/merge.out")"
"$OBJCOPY" -O binary -R .flash "$TMP/span" "$TMP/span.bin"
section "$TMP/apart.elf" .knode0 "$TMP/apart.knode0"
cmp -s "$TMP/span.bin" "$TMP/apart.knode0" \
    || fail "a node's blob is not the node without its span's sections"
"$OBJCOPY" -O binary -j .flash "$TMP/span" "$TMP/span.flash"
section "$TMP/apart.elf" .knode0apart "$TMP/apart.flash"
cmp -s "$TMP/span.flash" "$TMP/apart.flash" || fail "the span's load segment is not its sections' bytes"
at="$(address "$TMP/apart.elf" .knode0apart)"
case "$at" in
    *0800000) ;;
    *) fail "the span's load segment is at $at, not 0x800000" ;;
esac

# <name> <refusal>: the node <name> with the peer is refused, naming <refusal>.
refused() {
    if merge refused.elf "$TMP/$1" "$TMP/peer"; then
        fail "$1 merges"
    fi
    grep -qF -- "$2" "$TMP/merge.out" || fail "$1 is refused for another reason: $(cat "$TMP/merge.out")"
}
node dup 0x100000 "$SPAN
  . = 0x900000;
  .flash : { *(.flash2) }"
refused dup 'splits off a section named .flash, which 2 of its sections carry'
node glob 0x100000 '  . = 0x800000;
  __kickos_amp_load_apart_start = .;
  ".fl*sh" : { *(.flash) }
  __kickos_amp_load_apart_end = .;'
refused glob 'splits off a section named .fl*sh, which objcopy reads as a pattern'
node window 0x100000 "$WINDOW
  . = 0x800000;
  .flash : { *(.flash) }"
refused window 'loads 0x0000000000800000 in its flash window outside any load-apart span'
node stray 0x100000 "$WINDOW
$SPAN
  . = 0x900000;
  .flash2 : { *(.flash2) }"
refused stray 'loads 0x0000000000900000 in its flash window outside any load-apart span'

echo "PASS: a node naming no span merges whole, one naming a span merges it apart, two merges are"
echo "  the same bytes, and a split name two sections carry or objcopy reads as a pattern, and a load"
echo "  in a node's flash window outside its span, are refused"
