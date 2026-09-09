#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NO ARCHIVE HOLDING KERNEL TEXT MAY EMIT A GOT REFERENCE, where a translating backend
# splits the image in two. A static link has ONE .got, a GOT slot is reached by adrp like
# anything else, and the halves are 2^40 apart: a .got with users in both cannot be placed
# at all. virt_arm64.ld gives it to the app, so the kernel side must want none.
#
# Under the small code model a WEAK EXTERN is what emits one, and whether one is emitted is a
# property of the toolchain rather than of the source. The code model is scoped per TU
# (kickos_split_image_tu, cmake/kickos.cmake) and this gate is the only thing that says the
# list is still complete: a reach across the halves fails the link loudly, while a GOT
# reference is silent until some later change forces the .got to be placed.
#
# The fix is that TU's source path added to the kickos_split_image_tu() call for the target
# that built the archive, never a relaxation here and never a de-typed declaration: a weak
# extern is the chip<->kernel contract for a window a chip may not carve.
#
# SCOPE: the archives that hold kernel text (kernel, arch, chip). kickos_lib is NOT scanned,
# being app-side, and a region backend serves both privilege levels from one text mapping
# and has no split to protect.
#
# TWO PROPERTIES OF THE TOOL'S OUTPUT ARE LOAD-BEARING, and both are checked rather than
# assumed.
#
#   -W       without it readelf truncates the type column to 17 characters, and
#            R_AARCH64_LD64_GOT_LO12_NC comes out as R_AARCH64_LD64_GO, which does not hold
#            the substring this gate matches on: half the GOT relocation types would go
#            unseen. The wide column header is what says the flag took effect, so its
#            absence is refused. The match is a SUBSTRING for the same reason: the other
#            half, R_AARCH64_ADR_GOT_PAGE, truncates to R_AARCH64_ADR_GOT, so a pattern
#            anchored on a full relocation name matches nothing.
#
#   LC_ALL=C the host binutils is localised. Under a French locale readelf prints
#            `Fichier:` where `File:` is parsed, while leaving the TYPE column untranslated,
#            so the hits are still reported and only the part naming WHICH member is
#            silently empty. A hit no `File:` line attributes is therefore refused in its
#            own right.
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

# The substring that names a GOT relocation, and the wide column header that says the type
# column was not truncated. Handed to the readers below rather than written inside them, so
# the controls and the real scan cannot disagree about what the rule is.
GOT_ERE='_GOT'
WIDE_LIT="Symbol's Name"

# `File: <archive>(<member>)` is what attributes a record to its object.
parse_rel() { # <relocation-output> <archive-label> <got-ere> <hits-append>
    awk -v arch="$2" -v got="$3" '
    /^File: / {
        member = $2
        sub(/^.*\(/, "", member)
        sub(/\)$/, "", member)
        next
    }
    $1 ~ /^[0-9a-f]+$/ && $3 ~ /^R_/ {
        total++
        if ($3 !~ got) { next }
        # The record ends `<name> + <addend>`, so $NF is the addend and the name is three
        # fields from the end.
        name = "?"
        if (NF >= 7) { name = $(NF - 2) }
        if (member == "") { printf "NOFILE %s %s %s\n", arch, $3, name }
        printf "HIT %s(%s) %s %s\n", arch, member, $3, name
    }
    END { printf "RELS %d\n", total + 0 }' "$1" >> "$4"
}

# The wide column header is what says -W took effect, checked in front of the parse so a
# control can reach it too.
scan_rel() { # <relocation-output> <archive-label> <got-ere> <wide-literal> <hits-append>
    grep -qF "$4" "$1" \
        || fail "the relocation dump for $2 carries no wide column header, so the type column
      is truncated to 17 characters and R_AARCH64_LD64_GOT_LO12_NC comes out as
      R_AARCH64_LD64_GO, which no longer holds the substring this gate matches. Pass -W, and
      keep LC_ALL=C so the header is not translated instead."
    parse_rel "$1" "$2" "$3" "$5"
}

verdict() { # <hits> <archive-count>
    _rels="$(awk '/^RELS /{ t += $2 } END { print t + 0 }' "$1")"
    [ "$_rels" -gt 0 ] || fail "no relocation record in any of the $2 archive(s) given: wrong \
readelf, wrong files, or a link model this gate cannot read (guard would pass vacuously)"

    if grep -q '^NOFILE ' "$1"; then
        echo "FAIL: a GOT reference was found and no File: line attributes it to a member," >&2
        echo "      so the object that emits it cannot be named. Either LC_ALL is not C and" >&2
        echo "      readelf printed a translated header, or the input is not an archive." >&2
        awk '/^NOFILE /{ printf "        %s emits %s against %s\n", $2, $3, $4 }' "$1" >&2
        exit 1
    fi

    if grep -q '^HIT ' "$1"; then
        echo "FAIL: archive(s) holding kernel text emit GOT references" >&2
        echo "      A static link has one .got and it belongs to the app's half, which" >&2
        echo "      kernel text may not reach. Add the object's source to the" >&2
        echo "      kickos_split_image_tu() call for the target that built this archive." >&2
        awk '/^HIT /{ printf "        %s emits %s against %s\n", $2, $3, $4 }' "$1" >&2
        exit 1
    fi

    echo "PASS: $2 archive(s) holding kernel text, $_rels relocation record(s), no GOT reference"
}

# --- self-test: one control per clause, each a minimal pair -------------------
# THE CONTROLS ARE FORGED readelf OUTPUT, so what they prove is the PARSER and the VERDICT,
# never the invocation: that the records come from `readelf -rW` of the archives named on the
# command line, and that those archives hold kernel text, are outside them.
#
# The record shape is readelf's own: offset, info, type, symbol value, name, `+`, addend,
# which is why the name is read three fields from the end.
ctldir="$TMP/ctl"
mkdir -p "$ctldir"

rec() { # <type> <symbol>
    printf '0000000000000010  0000006e0000011b %s 0000000000000000 %s + 0\n' "$1" "$2"
}
head_wide() {
    printf "    Offset             Info             Type               Symbol's Value  %s + Addend\n" \
        'Symbol'"'"'s Name'
}
section() { # <name> <entries>
    printf "\nRelocation section '%s' at offset 0x100 contains %s entries:\n" "$1" "$2"
    head_wide
}

# The GOT spellings, and the near misses that must not be read as one. R_AARCH64_ADR_GOT is
# what a readelf run without -W prints for R_AARCH64_ADR_GOT_PAGE, so the substring match has
# to reach it; the ordinary types below share most of their name with it.
T_GOT_PAGE='R_AARCH64_ADR_GOT_PAGE'
T_GOT_TRUNC='R_AARCH64_ADR_GOT'
T_GOT_LD='R_AARCH64_LD64_GOT_LO12_NC'
T_ABS='R_AARCH64_ABS64'
T_ADR='R_AARCH64_ADR_PREL_PG_HI21'
T_LDST='R_AARCH64_LDST64_ABS_LO12_NC'
T_CALL='R_AARCH64_CALL26'

# The positive corpus is ONE forged output over TWO members, so the assertion is an exact
# COUNT of hits rather than a boolean, and a hit attributed to the wrong member shows up as
# the wrong name. Three of the five records are GOT references and two are not.
{
    printf 'File: libkickos_kernel.a(aspace.cc.obj)\n'
    section '.rela.text._Z6aspacev' 3
    rec "$T_GOT_PAGE" 'kickos_chip_window'
    rec "$T_GOT_TRUNC" 'kickos_chip_carve'
    rec "$T_ADR" '.text._Z6aspacev'
    printf 'File: libkickos_kernel.a(domain.cc.obj)\n'
    section '.rela.text._Z6domainv' 2
    rec "$T_GOT_LD" 'kickos_chip_window'
    rec "$T_ABS" 'kos_got_table'
} > "$ctldir/pos.rel"

: > "$ctldir/pos.hits"
parse_rel "$ctldir/pos.rel" 'libkickos_kernel.a' "$GOT_ERE" "$ctldir/pos.hits"

HITS="$(grep -c '^HIT ' "$ctldir/pos.hits")" || HITS=0
RELS="$(awk '/^RELS /{ t += $2 } END { print t + 0 }' "$ctldir/pos.hits")"
[ "$HITS" -eq 3 ] || {
    cat "$ctldir/pos.hits" >&2
    fail "the parser found $HITS of 3 planted GOT references, so it would miss a real one"
}
[ "$RELS" -eq 5 ] || {
    cat "$ctldir/pos.hits" >&2
    fail "the parser counted $RELS of 5 planted relocation records, so the floor below rests
      on a corpus it read wrong"
}
# The member and the symbol name, which are what a red run has to print for the fix to be
# actionable. An empty member reads as `libkickos_kernel.a()`.
grep -q '^HIT libkickos_kernel.a(aspace.cc.obj) R_AARCH64_ADR_GOT_PAGE kickos_chip_window$' \
    "$ctldir/pos.hits" \
    || {
        cat "$ctldir/pos.hits" >&2
        fail "a planted hit is not attributed to its member and symbol, so a red run names
      neither the object to fix nor the reference to remove"
    }
grep -q '^HIT libkickos_kernel.a(domain.cc.obj) ' "$ctldir/pos.hits" \
    || {
        cat "$ctldir/pos.hits" >&2
        fail "every planted hit was attributed to the FIRST member, so the File: line is not
      being read and one object's finding is reported against another"
    }
if grep -q '^NOFILE ' "$ctldir/pos.hits"; then
    fail "a hit under a File: line was reported as unattributed"
fi

# EACH negative on its own, so a control that is silent for the WRONG reason is visible: a
# whole-corpus zero cannot tell "the match is exact" from "the parser read nothing".
QUIET=0
for T in "$T_ABS" "$T_ADR" "$T_LDST" "$T_CALL"; do
    {
        printf 'File: libkickos_kernel.a(one.cc.obj)\n'
        section '.rela.text._Z3onev' 1
        rec "$T" 'kos_got_table'
    } > "$ctldir/one.rel"
    : > "$ctldir/one.hits"
    parse_rel "$ctldir/one.rel" 'libkickos_kernel.a' "$GOT_ERE" "$ctldir/one.hits"
    _r="$(awk '/^RELS /{ t += $2 } END { print t + 0 }' "$ctldir/one.hits")"
    [ "$_r" -eq 1 ] || fail "negative control $T was not read as a relocation record at all,
      so it is not a near miss of anything"
    if grep -q '^HIT ' "$ctldir/one.hits"; then
        cat "$ctldir/one.hits" >&2
        fail "negative control $T reports; the type column is being matched too loosely, or
      the SYMBOL name is being matched instead of the type"
    fi
    QUIET=$((QUIET + 1))
done
[ "$QUIET" -eq 4 ] || fail "$QUIET of 4 negative controls ran silent"

# Narrow the match and the count over the positive corpus must MOVE by an exact amount: that
# is what proves each planted spelling was a near miss of the pattern.
mutate() { # <what> <got-ere> <expect-hits>
    : > "$ctldir/mut.hits"
    parse_rel "$ctldir/pos.rel" 'libkickos_kernel.a' "$2" "$ctldir/mut.hits"
    _h="$(grep -c '^HIT ' "$ctldir/mut.hits")" || _h=0
    [ "$_h" -eq "$3" ] || {
        cat "$ctldir/mut.hits" >&2
        fail "with the $1, $_h of the planted GOT reference(s) are found, expected $3; the
      control for it is not a near miss and proves nothing"
    }
}
# A pattern anchored on the full relocation name loses both the truncated spelling and the
# other GOT type, which is the whole reason the match is a substring.
mutate "match anchored on R_AARCH64_ADR_GOT_PAGE" '^R_AARCH64_ADR_GOT_PAGE$' 1
mutate "match withdrawn"                          'KICKOS_NO_SUCH_RELOCATION' 0
mutate "match unchanged"                          "$GOT_ERE"                  3

# The refusals, each a whole run: a corpus the parser could not read must not reach a
# verdict at all.
ctl() { # <label> <expect-ere> <hits-file> <archive-count>
    if ( verdict "$3" "$4" ) > "$ctldir/out" 2>&1; then
        cat "$ctldir/out"
        fail "positive control '$1' passed, so the clause it plants for fires on nothing"
    fi
    grep -qE "$2" "$ctldir/out" || {
        cat "$ctldir/out" >&2
        fail "positive control '$1' reddened for the wrong reason, expected /$2/. A control
      another clause catches proves nothing about the clause it plants for"
    }
}

# The floor: an output with section headers and not one record. Every archive here carries
# relocated code, so a run that saw none read nothing.
{
    printf 'File: libkickos_kernel.a(empty.cc.obj)\n'
    section '.rela.text._Z5emptyv' 0
} > "$ctldir/norels.rel"
: > "$ctldir/norels.hits"
parse_rel "$ctldir/norels.rel" 'libkickos_kernel.a' "$GOT_ERE" "$ctldir/norels.hits"
ctl norels 'no relocation record in any of the 3 archive' "$ctldir/norels.hits" 3

# A hit no File: line attributes, which is what a translated header leaves behind: the type
# column stays English and reports, and only the member goes empty.
{
    section '.rela.text._Z6aspacev' 1
    rec "$T_GOT_PAGE" 'kickos_chip_window'
} > "$ctldir/nofile.rel"
: > "$ctldir/nofile.hits"
parse_rel "$ctldir/nofile.rel" 'libkickos_kernel.a' "$GOT_ERE" "$ctldir/nofile.hits"
ctl nofile 'no File: line attributes it to a member' "$ctldir/nofile.hits" 1

# And the hit itself, so the verdict is shown to fire on a corpus the parser read whole.
ctl hits 'emit GOT references' "$ctldir/pos.hits" 3

# The two output shapes the wide check refuses. NARROW is readelf without -W, whose second
# GOT type below reads R_AARCH64_LD64_GO and holds no `_GOT` at all, so a scan over it would
# report ONE of the two references and call the object clean. FRENCH is a translated locale,
# whose header carries the landmark in neither spelling.
{
    printf '  Offset          Info           Type           Sym. Value    Sym. Name + Addend\n'
    printf '000000000010  000600000137 %s 0000000000000000 kickos_chip_window + 0\n' "$T_GOT_TRUNC"
    printf '000000000014  000600000138 R_AARCH64_LD64_GO 0000000000000000 kickos_chip_window + 0\n'
} > "$ctldir/narrow.rel"
{
    printf 'Fichier: libkickos_kernel.a(aspace.cc.obj)\n'
    printf '    Decalage           Info             Type               Valeurs symbols Noms symboles + Addenda\n'
    rec "$T_GOT_PAGE" 'kickos_chip_window'
} > "$ctldir/french.rel"

wide_ctl() { # <label> <expect-ere> <relocation-output> <wide-literal>
    if ( scan_rel "$3" 'libkickos_kernel.a' "$GOT_ERE" "$4" "$ctldir/wide.hits" ) \
            > "$ctldir/out" 2>&1; then
        cat "$ctldir/out"
        fail "positive control '$1' passed, so the clause it plants for fires on nothing"
    fi
    grep -qE "$2" "$ctldir/out" || {
        cat "$ctldir/out" >&2
        fail "positive control '$1' reddened for the wrong reason, expected /$2/"
    }
}
: > "$ctldir/wide.hits"
wide_ctl narrow 'no wide column header' "$ctldir/narrow.rel" "$WIDE_LIT"
wide_ctl french 'no wide column header' "$ctldir/french.rel" "$WIDE_LIT"
# And the near miss: a WIDE output must pass the same check, or it would refuse every
# healthy run instead of the two shapes above.
: > "$ctldir/wide.hits"
scan_rel "$ctldir/pos.rel" 'libkickos_kernel.a' "$GOT_ERE" "$WIDE_LIT" "$ctldir/wide.hits"
[ "$(grep -c '^HIT ' "$ctldir/wide.hits")" -eq 3 ] \
    || fail "the wide check refused a forged WIDE output, so it would refuse every healthy run"
# The check withdrawn: an empty literal matches any output, and the narrow shape must then
# reach a verdict, which is what proves the landmark is load-bearing and not decoration.
: > "$ctldir/wide.hits"
scan_rel "$ctldir/narrow.rel" 'libkickos_kernel.a' "$GOT_ERE" '' "$ctldir/wide.hits"
[ "$(grep -c '^HIT ' "$ctldir/wide.hits")" -eq 1 ] \
    || fail "with the wide check withdrawn the narrow shape does not read as ONE of its two
      GOT references, so the truncation it is there to catch is not what these controls plant"

# --- the archives ---------------------------------------------------------------
: > "$TMP/hits"
N=$#
for A in "$@"; do
    [ -f "$A" ] || fail "archive not found: $A"
    # Positive control: every archive here carries relocated code, so a run that saw no
    # relocation record read nothing and the absence-assertion below would be vacuous.
    tool_out "$TMP/rel" '^[0-9a-f]+[[:space:]]+[0-9a-f]+[[:space:]]+R_' \
        "$READELF" -rW "$A"
    scan_rel "$TMP/rel" "$(basename "$A")" "$GOT_ERE" "$WIDE_LIT" "$TMP/hits"
done

verdict "$TMP/hits" "$N"
