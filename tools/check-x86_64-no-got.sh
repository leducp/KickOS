#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Reject GOT relocations in x86-64 UEFI objects and archive members.
# Usage: check-x86_64-no-got.sh <readelf> <object-or-archive>...
# PE32+ does not build a GOT or relax GOTPCRELX loads. Match GOT rather
# than full relocation names because readelf truncates its type column.

set -u

# Use the C locale so readelf's File: markers can be parsed.
LC_ALL=C
export LC_ALL

fail() { echo "FAIL: $*" >&2; exit 1; }

if [ "$#" -lt 2 ]; then
    fail "usage: check-x86_64-no-got.sh <readelf> <object>..."
fi
READELF="$1"
shift

command -v "$READELF" >/dev/null 2>&1 || [ -x "$READELF" ] \
    || fail "no readelf at $READELF"

hits=0
scanned=0
members=0
for obj in "$@"; do
    [ -f "$obj" ] || fail "no input at $obj"
    scanned=$((scanned + 1))
    # Attach the preceding File: marker to each matching relocation.
    out="$("$READELF" -r "$obj" 2>/dev/null \
           | awk '/^File: /{f=$2; next}
                  /GOT/{print f": "$0}
                  END{}' || true)"
    # Require nonempty readelf output for every input; empty output is not a clean result.
    found=$("$READELF" -h "$obj" 2>/dev/null | grep -c '^ELF Header' || true)
    [ "$found" -gt 0 ] || fail "readelf found no ELF header in $obj, so the scan of that input \
read a dead tool or an empty archive as clean"
    members=$((members + found))
    if [ -n "$out" ]; then
        echo "$obj:" >&2
        echo "$out" >&2
        hits=$((hits + 1))
    fi
done

[ "$scanned" -gt 0 ] || fail "no input was scanned, so this asserted nothing"

if [ "$hits" -ne 0 ]; then
    fail "$hits of $scanned input(s) carry a global-offset-table relocation. \
Give the specific declaration __attribute__((visibility(\"hidden\"))), and where it is WEAK \
state the symbol in the image's linker script instead (include/kickos/klink.h): \
ld -m i386pep leaves the load in place and the address becomes the bytes AT the symbol."
fi

echo "no-got: $scanned input(s), $members ELF header(s), clean"
