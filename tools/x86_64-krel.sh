#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The retained copy of an x86_64 image's base-relocation directory, which the boot walks to
# relocate the app window a second time (arch/x86/x86_64/apprel_x86_64.cc). .reloc itself is a
# discardable section a loader is free not to keep, so the image carries a copy in .krel, and
# the copy's size is known only once a link has produced the directory: the image is linked
# twice (tools/x86_64-link.sh).
#
#   x86_64-krel.sh extract <objdump> <objcopy> <first-link.efi> <out.bin>
#       The directory's bytes, exactly as long as the data directory states, not the section's
#       file-aligned size, which is what the boot compares the copy against.
#   x86_64-krel.sh check <objdump> <objcopy> <first-link.efi> <image.efi>
#       Refuses an image whose directory differs from the first link's, or whose .krel is not a
#       byte-for-byte copy of it: either would mean the second pass moved a fixup site, and the
#       boot would relocate against records that describe another image.
set -u

fail() { echo "FAIL: x86_64-krel: $*" >&2; exit 1; }

# The base-relocation data directory entry's size, in bytes, as decimal.
dir_size() { # <objdump> <image>
    _line=$(LC_ALL=C "$1" -p "$2" | grep -E '^Entry 5 ') || fail "no data directory in $2"
    _hex=$(printf '%s\n' "$_line" | awk '{ print $4 }')
    [ -n "$_hex" ] || fail "cannot read the base-relocation entry of $2"
    printf '%d\n' "0x$_hex"
}

# The first <bytes> of a section, read out of the FILE at the offset the section header states.
# Not `objcopy -O binary`, which answers zeros for a PE image's .reloc and would let a copy of
# nothing compare equal to itself.
# <objdump> <image> <section> <bytes> <out>
section_bytes() {
    if [ "$4" -eq 0 ]; then
        : > "$5"
        return 0
    fi
    _hdr=$(LC_ALL=C "$1" -h "$2" | awk -v s="$3" '$2 == s { print $3, $6; exit }')
    [ -n "$_hdr" ] || fail "$2 carries no $3 section"
    _size=$(printf '%d' "0x${_hdr% *}")
    _off=$(printf '%d' "0x${_hdr#* }")
    [ "$_size" -ge "$4" ] || fail "$3 of $2 holds $_size bytes, the directory states $4"
    tail -c +"$((_off + 1))" "$2" | head -c "$4" > "$5" || fail "cannot write $5"
    [ "$(wc -c < "$5")" -eq "$4" ] || fail "$2 ends inside $3"
}

mode="${1:-}"
case "$mode" in
    extract)
        [ $# -eq 5 ] || fail "usage: extract <objdump> <objcopy> <first-link.efi> <out.bin>"
        n=$(dir_size "$2" "$4")
        section_bytes "$2" "$4" .reloc "$n" "$5"
        ;;
    check)
        [ $# -eq 5 ] || fail "usage: check <objdump> <objcopy> <first-link.efi> <image.efi>"
        n1=$(dir_size "$2" "$4")
        n2=$(dir_size "$2" "$5")
        [ "$n1" -eq "$n2" ] || fail "$5: the second link's directory is $n2 bytes, the first's $n1"
        t=$(mktemp -d) || fail "mktemp failed"
        section_bytes "$2" "$4" .reloc "$n1" "$t/first"
        section_bytes "$2" "$5" .reloc "$n2" "$t/final"
        section_bytes "$2" "$5" .krel "$n2" "$t/krel"
        if [ "$n1" -ne 0 ] && [ "$(tr -d '\000' < "$t/first" | head -c 1 | wc -c)" -eq 0 ]; then
            rm -rf "$t"
            fail "$4: the directory reads as all zeros, so nothing here was compared"
        fi
        cmp -s "$t/first" "$t/final" || { rm -rf "$t"; fail "$5: the second link moved a fixup site"; }
        cmp -s "$t/final" "$t/krel" || { rm -rf "$t"; fail "$5: .krel is not the image's directory"; }
        rm -rf "$t"
        ;;
    *)
        fail "usage: x86_64-krel.sh extract|check ..."
        ;;
esac
