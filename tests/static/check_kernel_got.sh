#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NO ARCHIVE HOLDING KERNEL TEXT MAY EMIT A GOT REFERENCE, where a translating backend
# splits the image in two (docs/design-m6-mmu.md, T5b.1). A static link has ONE .got, a GOT
# slot is reached by adrp like anything else, and the halves are 2^40 apart: a .got with
# users in both cannot be placed at all. virt_arm64.ld gives it to the app, so the kernel
# side must want none.
#
# Under the small code model a WEAK EXTERN is what emits one, and which construct emits one
# is a property of the toolchain rather than of the source: of the four kernel TUs holding
# such externs, two (mem/aspace.cc, domain/domain.cc) produce NO LINK ERROR when compiled
# small, their symbols resolving. The code model is scoped per TU (kickos_split_image_tu,
# cmake/kickos.cmake) and this is the only thing that says the list is still complete: the
# reach class fails the link loudly, while this class is silent until some later change
# forces the .got to be placed.
#
# A red run NAMES THE OBJECT. The fix is that TU's source path added to the
# kickos_split_image_tu() call for the target that built the archive, never a relaxation
# here and never a de-typed declaration: a weak extern is the chip<->kernel contract for a
# window a chip may not carve.
#
# SCOPE: the archives that hold kernel text (kernel, arch, chip). kickos_lib is NOT scanned,
# being app-side, and a region backend serves both privilege levels from one text mapping
# and has no split to protect.
#
# usage: check_kernel_got.sh <readelf> <archive>...

set -eu
. "$(dirname "$0")/../lib/gate.sh"

export LC_ALL=C

if [ "$#" -lt 2 ]; then
    echo "usage: $0 <readelf> <archive>..." >&2
    exit 2
fi

READELF="$1"; shift

command -v "$READELF" >/dev/null 2>&1 || fail "readelf not found: $READELF"
[ "$#" -gt 0 ] || fail "no archives given (guard would pass vacuously)"

scratch_dir

: > "$TMP/hits"
for A in "$@"; do
    [ -f "$A" ] || fail "archive not found: $A"
    # Positive control: every archive here carries relocated code, so a run that saw no
    # relocation record read nothing and the absence-assertion below would be vacuous.
    tool_out "$TMP/rel" '^[0-9a-f]+[[:space:]]+[0-9a-f]+[[:space:]]+R_' \
        "$READELF" -rW "$A"
    base="$(basename "$A")"
    # readelf TRUNCATES the type column, so R_AARCH64_ADR_GOT_PAGE prints as
    # R_AARCH64_ADR_GOT and a pattern anchored on a trailing underscore matches nothing.
    # `File: <archive>(<member>)` is what attributes a record to its object.
    awk -v arch="$base" '
    /^File: / {
        member = $2
        sub(/^.*\(/, "", member)
        sub(/\)$/, "", member)
        next
    }
    $1 ~ /^[0-9a-f]+$/ && $3 ~ /^R_/ {
        total++
        if ($3 !~ /_GOT/) { next }
        # The record ends `<name> + <addend>`, so $NF is the addend and the name is three
        # fields from the end.
        name = "?"
        if (NF >= 7) { name = $(NF - 2) }
        printf "HIT %s(%s) %s %s\n", arch, member, $3, name
    }
    END { printf "RELS %d\n", total + 0 }' "$TMP/rel" >> "$TMP/hits"
done

rels="$(awk '/^RELS /{ t += $2 } END { print t + 0 }' "$TMP/hits")"
[ "$rels" -gt 0 ] || fail "no relocation record in any of the $# archive(s) given: wrong \
readelf, wrong files, or a link model this gate cannot read (guard would pass vacuously)"

if grep -q '^HIT ' "$TMP/hits"; then
    echo "FAIL: archive(s) holding kernel text emit GOT references" >&2
    echo "      A static link has one .got and it belongs to the app's half" >&2
    echo "      (docs/design-m6-mmu.md, T5b.1). Add the object's source to the" >&2
    echo "      kickos_split_image_tu() call for the target that built this archive." >&2
    awk '/^HIT /{ printf "        %s emits %s against %s\n", $2, $3, $4 }' "$TMP/hits" >&2
    exit 1
fi

echo "PASS: $# archive(s) holding kernel text, $rels relocation record(s), no GOT reference"
