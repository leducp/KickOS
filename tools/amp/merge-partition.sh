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
    node=$((node + 1))
done

# The blobs carry no symbols worth keeping and objcopy's own notes would land in the output.
printf '  /DISCARD/ : { *(.note*) *(.comment) }\n}\n' >> "$SCRIPT"

# The node objects reach `ld` as SEPARATE arguments and never as one flattened string: they
# live under a mktemp directory, so a TMPDIR carrying whitespace re-splits a flattened list
# into paths that do not exist. `set --` inside a function replaces the FUNCTION's positional
# parameters, so the node ELFs the caller still holds in "$@" are untouched by it.
link_nodes() { # <node count> <entry>; the objects are $WORK/n<i>.o, in node order
    _count="$1"
    _entry="$2"
    _i=0
    set --
    while [ "$_i" -lt "$_count" ]; do
        set -- "$@" "$WORK/n${_i}.o"
        _i=$((_i + 1))
    done
    "$LD" -T "$SCRIPT" -e "$_entry" -o "$OUT" "$@"
}

link_nodes "$#" "$(entry_of "$1")"

# The node blobs are wrapped with `objcopy -I binary`, which carries no ABI attributes, so `ld`
# emits an identification byte and a flags word of its own and the merged artefact differs from
# every node it contains. An emulator boots it regardless; picotool refuses such a file as
# "Unrecognized ABI". Both are copied from node 0 rather than named here.
#
# ELF32 puts e_flags at 0x24 and ELF64 at 0x30; EI_CLASS at byte 4 says which, and EI_OSABI is
# byte 7 in both.
eclass="$(od -An -tu1 -j4 -N1 "$1" | tr -d ' ')"
if [ "$eclass" = "1" ]; then
    flags_off=36
else
    flags_off=48
fi
# POSIX dd has no `status` operand; it is a GNU extension, and a dd that does not know it
# refuses the whole invocation. The transfer report is dropped by redirection instead, which
# takes dd's own error message with it, so each failure is named here.
dd if="$1" of="$OUT" bs=1 skip=7 seek=7 count=1 conv=notrunc 2>/dev/null \
    || { echo "merge-partition.sh: could not copy EI_OSABI from $1 into $OUT" >&2; exit 1; }
dd if="$1" of="$OUT" bs=1 skip="$flags_off" seek="$flags_off" count=4 conv=notrunc 2>/dev/null \
    || { echo "merge-partition.sh: could not copy e_flags from $1 into $OUT" >&2; exit 1; }
echo "== partition: ELF identification and flags taken from node 0's image =="

echo "merged $# node image(s) into $OUT"
LC_ALL=C "$READELF" -lW "$OUT" | awk '$1 == "LOAD" { printf "  node load %s size %s\n", $4, $6 }'
