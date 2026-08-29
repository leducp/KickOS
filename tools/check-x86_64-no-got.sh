#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuse any global-offset-table relocation in the inputs that make up an x86_64 UEFI image.
#
#   tools/check-x86_64-no-got.sh <readelf> <object-or-archive>...
#
# ARCHIVES AS WELL AS OBJECTS: handed the image objects alone this check reads clean while a
# kernel archive carries the relocation. `readelf -r` walks an archive member by member and
# prints a `File: <archive>(<member>)` line before each, which is what names the TU to fix.
#
# `ld -m i386pep` builds no global offset table and performs none of the GOTPCRELX relaxations
# GNU ld applies to an ELF link, so a `mov sym@GOTPCREL(%rip), %reg` survives as a LOAD whose
# displacement resolves to the symbol itself: an address the compiler meant to compute becomes
# the first eight bytes of whatever it names. The image links clean and faults later.
# -fvisibility=hidden in cmake/toolchain-x86_64-uefi.cmake keeps the compiler from emitting
# the form; this refuses the survivor.
#
# THE TYPE COLUMN IS TRUNCATED. readelf prints R_X86_64_REX_GOTPCRELX as `R_X86_64_REX_GOTP`,
# so a grep for the full spelling reports zero on a file full of them. The match here is on
# GOT alone. tests/static/check_x86_64_no_got_selftest.sh is the positive control.
#
# POSIX sh (dash-clean).

set -u

# Every readelf below reads its own output back, so the locale is part of the contract: a
# French binutils prints `Fichier:` where this parses `File:`, while the relocation TYPE
# column stays untranslated, so the GOT match keeps working while the member name goes
# silent.
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
    # readelf's own `File:` lines carry the member name for an archive and the path for a
    # plain object, so one pass reports both shapes. A relocation LINE is what the grep
    # keeps; the awk below re-attaches the File: line above it.
    out="$("$READELF" -r "$obj" 2>/dev/null \
           | awk '/^File: /{f=$2; next}
                  /GOT/{print f": "$0}
                  END{}' || true)"
    # An archive with no members, or a readelf that failed, both produce nothing, so an empty
    # $out is not evidence. PER INPUT, not over the total: a floor summed across the loop is
    # satisfied by the other inputs, so one input contributing nothing reads as clean.
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
state the symbol in the image's linker script instead (kernel/include/kickos/klink.h): \
ld -m i386pep leaves the load in place and the address becomes the bytes AT the symbol."
fi

echo "no-got: $scanned input(s), $members ELF header(s), clean"
