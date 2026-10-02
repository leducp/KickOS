#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Reject every relocation against an undefined weak symbol in x86-64 UEFI objects and archive
# members, but a pure virtual function's vtable word.
# Usage: check-x86_64-weak-undef.sh <readelf> <object-or-archive>...
#
# PE32+ holds no undefined symbol, and ld -m i386pep resolves one to link address 0 with no
# error at the default image base: a call becomes `call 0`, a PC-relative lea yields a zero that
# relocation then moves off zero, and an absolute word stays 0 with no base relocation
# (docs/design-m10-toolchain.md section 5.5). tools/check-x86_64-no-got.sh sees GOT loads alone,
# so this reads every relocation of an allocated section, whatever its type, and refuses one
# whose symbol is undefined and weak in the member carrying it. Debug sections are not loaded
# and are not read.
#
# The one admission: __cxa_pure_virtual as an R_X86_64_64 word in a vtable's .data.rel.ro. GCC's
# C++ front end declares it weak in every abstract class's vtable, and the slot links to zero,
# which only a call to a pure virtual function reaches: a contained fault.

set -u

# Use the C locale so readelf's File: markers and headings can be parsed.
LC_ALL=C
export LC_ALL

fail() { echo "FAIL: $*" >&2; exit 1; }

if [ "$#" -lt 2 ]; then
    fail "usage: check-x86_64-weak-undef.sh <readelf> <object>..."
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
    found=$("$READELF" -h "$obj" 2>/dev/null | grep -c '^ELF Header' || true)
    [ "$found" -gt 0 ] || fail "readelf found no ELF header in $obj, so the scan of that input \
read a dead tool or an empty archive as clean"
    members=$((members + found))
    # One member at a time: its allocated sections, its relocations by the section they apply
    # to, and its weak undefined symbols, judged together when the next member begins.
    out="$("$READELF" -SrsW "$obj" 2>/dev/null | awk -v input="$obj" '
        function judge(    i, r)
        {
            for (i = 1; i <= nrel; i++) {
                if (!(rsec[i] in alloc) || !(rsym[i] in weak)) { continue }
                if (rsym[i] == "__cxa_pure_virtual" && rtype[i] == "R_X86_64_64" \
                    && rsec[i] ~ /^\.data\.rel\.ro/) { continue }
                print member ": " rtype[i] " against " rsym[i] " in " rsec[i]
            }
            delete alloc
            delete weak
            nrel = 0
        }
        BEGIN { member = input }
        /^File: / { judge(); member = $2; next }
        /^ *\[ *[0-9]+\] / {
            line = $0
            sub(/^ *\[ *[0-9]+\] /, "", line)
            n = split(line, f, " ")
            # Name Type Address Off Size ES Flg Lk Inf Al; Flg is absent where empty.
            if (n >= 10 && f[7] ~ /A/) { alloc[f[1]] = 1 }
            next
        }
        /^Relocation section / {
            sec = $3
            gsub(/\047/, "", sec)
            sub(/^\.rela?/, "", sec)
            next
        }
        /^ *[0-9a-f]+ +[0-9a-f]+ R_X86_64_/ {
            nrel++
            rsec[nrel] = sec
            rtype[nrel] = $3
            rsym[nrel] = $5
            next
        }
        /^ *[0-9]+: [0-9a-f]+ / {
            if ($5 == "WEAK" && $7 == "UND" && $8 != "") { weak[$8] = 1 }
            next
        }
        END { judge() }' || true)"
    if [ -n "$out" ]; then
        echo "$obj:" >&2
        echo "$out" >&2
        hits=$((hits + 1))
    fi
done

[ "$scanned" -gt 0 ] || fail "no input was scanned, so this asserted nothing"

if [ "$hits" -ne 0 ]; then
    fail "$hits of $scanned input(s) carry a relocation against an undefined weak symbol. \
ld -m i386pep resolves it to address 0 with no error: define the symbol, make the reference \
strong, or state it in the image's linker script (include/kickos/klink.h)."
fi

echo "weak-undef: $scanned input(s), $members ELF header(s), clean"
