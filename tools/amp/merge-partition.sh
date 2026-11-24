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
#
# A node whose ELF names __kickos_amp_load_apart_start and _end loads that span apart: the
# sections whose load address lies in it leave the node's blob and become one more PT_LOAD at the
# lowest of their load addresses, after every node's. A node naming neither merges as if the pair
# did not exist. Each section split off is named to objcopy, so a name that is no single section's
# is refused, and so is a load in the flash window an ELF names
# (__kickos_esp32_flash_window_start and _end) outside its span: its blob would reach across it.
#
# The output is a function of the node ELFs alone: two merges of the same ELFs are the same bytes.

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
#
# With <lo> and <hi>, the 16-digit forms symbol_span prints, only the segments loading inside that
# span count where <in> is 1, and only those outside it where <in> is 0.
lowest_paddr() { # <elf> [<lo> <hi> <in>]
    LC_ALL=C "$READELF" -lW "$1" \
        | LC_ALL=C awk -v lo="${2:-}" -v hi="${3:-}" -v in_span="${4:-}" '$1 == "LOAD" && $5 !~ /^(0[xX])?0*$/ {
                   a = $4
                   sub(/^0[xX]/, "", a)
                   while (length(a) < 16) { a = "0" a }
                   a = tolower(a)
                   if (lo != "") {
                       inside = ((a "") >= (lo "") && (a "") < (hi ""))
                       if (inside != in_span) { next }
                   }
                   print a
               }' \
        | LC_ALL=C sort | head -1 | sed -e 's/^/0x/'
}

# `<lo> <hi>` of the span an ELF names by the symbols <start> and <end>, zero-padded to 16
# lower-case digits, `half` where it names one of them, or nothing where it names neither.
symbol_span() { # <elf> <start> <end>
    LC_ALL=C "$READELF" -sW "$1" | LC_ALL=C awk -v s="$2" -v e="$3" '
        function pad(v) { while (length(v) < 16) { v = "0" v }; return tolower(v) }
        $8 == s { lo = pad($2) }
        $8 == e { hi = pad($2) }
        END {
            if (lo != "" && hi != "") { print lo, hi }
            else if (lo != "" || hi != "") { print "half" }
        }'
}

# The lowest load address with file bytes an ELF places in [<wlo>, <whi>) outside [<lo>, <hi>),
# or nothing where it places none there; <lo> and <hi> may be empty.
stray_load() { # <elf> <wlo> <whi> <lo> <hi>
    LC_ALL=C "$READELF" -lW "$1" \
        | LC_ALL=C awk -v wlo="$2" -v whi="$3" -v lo="${4:-}" -v hi="${5:-}" '$1 == "LOAD" && $5 !~ /^(0[xX])?0*$/ {
                   a = $4
                   sub(/^0[xX]/, "", a)
                   while (length(a) < 16) { a = "0" a }
                   a = tolower(a)
                   if ((a "") < (wlo "") || (a "") >= (whi "")) { next }
                   if (lo != "" && (a "") >= (lo "") && (a "") < (hi "")) { next }
                   print a
               }' \
        | LC_ALL=C sort | head -1 | sed -e 's/^/0x/'
}

# The sections of an ELF whose load address lies in [<lo>, <hi>), one name per line.
apart_sections() { # <elf> <lo> <hi>
    LC_ALL=C "$OBJDUMP" -h -w "$1" | LC_ALL=C awk -v lo="$2" -v hi="$3" '
        $1 ~ /^[0-9]+$/ && NF >= 7 {
            a = $5
            while (length(a) < 16) { a = "0" a }
            a = tolower(a)
            if ((a "") >= (lo "") && (a "") < (hi "")) { print $2 }
        }'
}

# Refuses a name in <list> that objcopy would read as a pattern, or that more than one section of
# <elf> carries: -R and -j take every section a name matches.
split_names() { # <elf> <list>
    LC_ALL=C "$OBJDUMP" -h -w "$1" | LC_ALL=C awk '$1 ~ /^[0-9]+$/ && NF >= 7 { print $2 }' \
        > "$WORK/names"
    while IFS= read -r _sn_name; do
        case "$_sn_name" in
            *[][*?!\\]*)
                echo "merge-partition.sh: $1 splits off a section named $_sn_name, which objcopy reads as a pattern" >&2
                exit 1
                ;;
            *) ;;
        esac
        _sn_count="$(grep -cxF -- "$_sn_name" "$WORK/names" || :)"
        if [ "$_sn_count" -ne 1 ]; then
            echo "merge-partition.sh: $1 splits off a section named $_sn_name, which $_sn_count of its sections carry" >&2
            exit 1
        fi
    done < "$2"
}

# objcopy -O binary of <elf> into <out>, with `-R` or `-j` before each section <list> names.
blob_sections() { # <elf> <out> <-R|-j> <list>
    _bs_elf="$1"
    _bs_out="$2"
    _bs_flag="$3"
    _bs_list="$4"
    set --
    while IFS= read -r _bs_name; do
        [ -n "$_bs_name" ] || continue
        set -- "$@" "$_bs_flag" "$_bs_name"
    done < "$_bs_list"
    "$OBJCOPY" -O binary "$@" "$_bs_elf" "$_bs_out"
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
    span="$(symbol_span "$elf" __kickos_amp_load_apart_start __kickos_amp_load_apart_end)"
    if [ "$span" = half ]; then
        echo "merge-partition.sh: $elf names one end of __kickos_amp_load_apart_start/_end, not both" >&2
        exit 1
    fi
    window="$(symbol_span "$elf" __kickos_esp32_flash_window_start __kickos_esp32_flash_window_end)"
    if [ "$window" = half ]; then
        echo "merge-partition.sh: $elf names one end of __kickos_esp32_flash_window_start/_end, not both" >&2
        exit 1
    fi
    if [ -n "$window" ]; then
        stray="$(stray_load "$elf" $window $span)"
        if [ -n "$stray" ]; then
            echo "merge-partition.sh: $elf loads $stray in its flash window outside any load-apart span" >&2
            exit 1
        fi
    fi
    echo "$span" > "$WORK/apart${node}"
    node=$((node + 1))
done
node=0
for elf in "$@"; do
    if [ -n "$(cat "$WORK/apart${node}")" ]; then
        echo "  knode${node}apart PT_LOAD FLAGS(5);" >> "$SCRIPT"
    fi
    node=$((node + 1))
done
printf '}\nSECTIONS\n{\n' >> "$SCRIPT"

: > "$WORK/apart.ld"
node=0
for elf in "$@"; do
    span="$(cat "$WORK/apart${node}")"
    if [ -z "$span" ]; then
        base="$(lowest_paddr "$elf")"
        [ -n "$base" ] || { echo "merge-partition.sh: $elf has no loadable segment" >&2; exit 1; }
        # -O binary walks the LMAs, so this blob is exactly what a loader would place at `base`.
        "$OBJCOPY" -O binary "$elf" "$WORK/n${node}.bin"
    else
        lo="$(echo "$span" | cut -d ' ' -f 1)"
        hi="$(echo "$span" | cut -d ' ' -f 2)"
        base="$(lowest_paddr "$elf" "$lo" "$hi" 0)"
        [ -n "$base" ] || { echo "merge-partition.sh: $elf has no loadable segment outside its load-apart span" >&2; exit 1; }
        apart="$(lowest_paddr "$elf" "$lo" "$hi" 1)"
        [ -n "$apart" ] || { echo "merge-partition.sh: $elf loads nothing in its load-apart span 0x$lo to 0x$hi" >&2; exit 1; }
        apart_sections "$elf" "$lo" "$hi" > "$WORK/apart${node}.sections"
        split_names "$elf" "$WORK/apart${node}.sections"
        blob_sections "$elf" "$WORK/n${node}.bin" -R "$WORK/apart${node}.sections"
        blob_sections "$elf" "$WORK/n${node}apart.bin" -j "$WORK/apart${node}.sections"
        "$OBJCOPY" -I binary -O "$fmt" -B "$mach" --strip-all \
            --rename-section ".data=.knode${node}apart,alloc,load,readonly,code,contents" \
            "$WORK/n${node}apart.bin" "$WORK/n${node}apart.o"
        printf '  .knode%sapart %s : { *(.knode%sapart) } :knode%sapart\n' \
            "$node" "$apart" "$node" "$node" >> "$WORK/apart.ld"
    fi
    # `contents` is load-bearing in this flag list: the list REPLACES the section's flags, and
    # without it the section becomes NOBITS and the merged artefact is the right size, in the
    # right place, and entirely zero. The symbols objcopy names a blob by carry its path.
    "$OBJCOPY" -I binary -O "$fmt" -B "$mach" --strip-all \
        --rename-section ".data=.knode${node},alloc,load,readonly,code,contents" \
        "$WORK/n${node}.bin" "$WORK/n${node}.o"
    printf '  . = %s;\n  .knode%s : { *(.knode%s) } :knode%s\n' \
        "$base" "$node" "$node" "$node" >> "$SCRIPT"
    node=$((node + 1))
done

cat "$WORK/apart.ld" >> "$SCRIPT"
# objcopy's own notes would land in the output.
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
    _i=0
    while [ "$_i" -lt "$_count" ]; do
        if [ -f "$WORK/n${_i}apart.o" ]; then
            set -- "$@" "$WORK/n${_i}apart.o"
        fi
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
LC_ALL=C "$READELF" -lW "$OUT" | LC_ALL=C awk '$1 == "LOAD" { printf "  node load %s size %s\n", $4, $6 }'
