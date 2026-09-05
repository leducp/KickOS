#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Combine one ELF per AMP node into ONE programmable artefact, so a partition is flashed once.
# docs/design-multicore.md N6b: deployment is a MERGE and not a second flash.
#
# usage: merge-partition.sh <objcopy> <ld> <readelf> <objdump> <out.elf> <node0.elf>
#                           [<node1.elf> ...]
#        the node ELFs are given IN NODE ORDER, node 0 first.
#
# THE OUTPUT IS AN ELF AND NOT A FLAT IMAGE: the nodes are a NODE_SHARE apart, so a flat span
# would be mostly padding and would grow with a knob unrelated to how much code there is. One
# PT_LOAD per node makes the artefact the sum of what the nodes hold. QEMU boots it with a
# plain `-kernel`.
#
# Every address comes out of the images, never restated here (N6g): each node's load address is
# the lowest p_paddr its own ELF carries, and the entry is node 0's own.

set -eu

OBJCOPY="${1:?usage: merge-partition.sh <objcopy> <ld> <readelf> <objdump> <out> <node.elf>...}"
LD="${2:?}"
READELF="${3:?}"
OBJDUMP="${4:?}"
OUT="${5:?}"
shift 5
[ "$#" -ge 1 ] || { echo "merge-partition.sh: no node ELF given" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The lowest LOADED physical address of an ELF: the address its flat blob starts at. Segments
# with a zero file size are skipped, a NOBITS one (.amp_shared, .bss) contributing no bytes and
# carrying an address that would drag this floor somewhere nothing is written.
#
# POSIX AWK ONLY, and both fields stay HEX STRINGS. A hex-to-number conversion needs a gawk
# extension, and a non-gawk awk refuses the whole program before a single record, so the merge
# would report this ELF as carrying no loadable segment. Zero-padded to one width the fields
# order under LC_ALL=C exactly as the numbers do, a size is nonzero iff its digits are not all
# zero, and the linker script takes the hex back verbatim.
# tests/static/check_awk_portable.sh is what keeps an extension from returning here.
lowest_paddr() { # <elf>
    LC_ALL=C "$READELF" -lW "$1" \
        | awk '$1 == "LOAD" && $5 !~ /^(0[xX])?0*$/ {
                   a = $4
                   sub(/^0[xX]/, "", a)
                   while (length(a) < 16) { a = "0" a }
                   print tolower(a)
               }' \
        | LC_ALL=C sort | head -1 | sed -e 's/^/0x/'
}

entry_of() { # <elf>
    LC_ALL=C "$READELF" -h "$1" | sed -n 's/.*Entry point address: *//p'
}

# The output format comes out of the images, exactly as every address does (N6h): `objdump -f`
# names the container format and the architecture, so a third architecture needs no edit here.
fmt="$(LC_ALL=C "$OBJDUMP" -f "$1" | sed -n 's/.*file format \(.*\)$/\1/p' | head -1)"
mach="$(LC_ALL=C "$OBJDUMP" -f "$1" | sed -n 's/^architecture: \([^,]*\).*/\1/p' | head -1)"
[ -n "$fmt" ] || { echo "merge-partition.sh: no file format in $1" >&2; exit 1; }
[ -n "$mach" ] || { echo "merge-partition.sh: no architecture in $1" >&2; exit 1; }
echo "== partition: container format $fmt, architecture $mach, read from node 0's image =="

SCRIPT="$WORK/partition.ld"
{
    echo "OUTPUT_FORMAT(\"$fmt\")"
    echo "OUTPUT_ARCH($mach)"
    echo 'PHDRS'
    echo '{'
} > "$SCRIPT"

node=0
for elf in "$@"; do
    echo "  knode${node} PT_LOAD FLAGS(5);" >> "$SCRIPT"
    node=$((node + 1))
done
printf '}\nSECTIONS\n{\n' >> "$SCRIPT"

node=0
OBJS=""
for elf in "$@"; do
    base="$(lowest_paddr "$elf")"
    [ -n "$base" ] || { echo "merge-partition.sh: $elf has no loadable segment" >&2; exit 1; }
    # -O binary walks the LMAs, so this blob is exactly what a loader would place at `base`.
    "$OBJCOPY" -O binary "$elf" "$WORK/n${node}.bin"
    # `contents` is load-bearing in this flag list: the list REPLACES the section's flags, and
    # without it the section becomes NOBITS and the merged artefact is the right size, in the
    # right place, and entirely zero.
    "$OBJCOPY" -I binary -O "$fmt" -B "$mach" \
        --rename-section ".data=.knode${node},alloc,load,readonly,code,contents" \
        "$WORK/n${node}.bin" "$WORK/n${node}.o"
    printf '  . = %s;\n  .knode%s : { *(.knode%s) } :knode%s\n' \
        "$base" "$node" "$node" "$node" >> "$SCRIPT"
    OBJS="$OBJS $WORK/n${node}.o"
    node=$((node + 1))
done

# The blobs carry no symbols worth keeping and objcopy's own notes would land in the output.
printf '  /DISCARD/ : { *(.note*) *(.comment) }\n}\n' >> "$SCRIPT"

# shellcheck disable=SC2086
"$LD" -T "$SCRIPT" -e "$(entry_of "$1")" -o "$OUT" $OBJS

echo "merged $# node image(s) into $OUT"
LC_ALL=C "$READELF" -lW "$OUT" | awk '$1 == "LOAD" { printf "  node load %s size %s\n", $4, $6 }'
