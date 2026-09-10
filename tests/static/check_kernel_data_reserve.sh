#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The app window's base must be a DECLARED address, not the alignment that happens to sit
# above kernel .bss. A pow2-sized, pow2-aligned app window based on `.` makes ONE BYTE of
# kernel .bss cost a WHOLE window: the window, the newlib heap and the user-RAM arena above
# them all slide one _appdata_size higher, and the link stays green. The mechanism that
# closes it is arch/common/kernel_data_reserve.ld.h.
#
# Run from the repo root, no arguments: tests/static/check_kernel_data_reserve.sh
#
# MEMBERSHIP IS DERIVED, NEVER LISTED. The class is every tracked linker script whose
# .appdata output section is declared with an alignment that can lose a window; a tenth
# script joins it by being written, not by being added here. The rule is TOTAL over the
# .appdata declarations in the corpus, so a THIRD spelling is refused rather than read as an
# escape: a literal window like ALIGN(0x8000), a symbol this gate does not know, no ALIGN at
# all. Two shapes pass:
#
#   .appdata MAX(., _kernel_data_top) : ALIGN(_appdata_size)   pinned window
#   .appdata : ALIGN(32)                                       granular, nothing to lose
#
# The granule bound is the coarsest MPU granularity in the fleet (PMSAv8 and SysMPU are
# 32-byte granular), so at or below it the worst case a slide can cost is 31 bytes.
#
# THE SCANNER READS SOURCE TEXT AND CANNOT TELL A COMMENT FROM A STATEMENT: a comment line
# whose first token is `.appdata` is read as the start of a declaration, and so is everything
# down to the next brace. Reflowing one of the .ld comments into that shape reddens this gate.
# That is the price of deciding membership on the section NAME rather than on a spelling of
# the rest of the declaration, and it is the right way round: prose that looks like a member
# is a red gate, and a member that looks like prose was a green one.

set -u
# The corpus is re-split unquoted into the scanner's argv; a glob character in a tracked
# path must not expand against the cwd and inventory a different file.
set -f
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

export LC_ALL=C
scratch_dir

# The coarsest MPU granule in the fleet. Handed in so the self-test below can move it and
# watch the verdict move with it.
GRANULE=32

# The class cannot collapse quietly. Every clause below is decided per member, so a scanner
# that stopped seeing eight of the nine pinned windows reports the ninth clean and says PASS.
CLASS_FLOOR=8

scan() { # <granule> <file>... -> BAD/STAT records on stdout
    _g="$1"; shift
    awk -v granule="$_g" '
# A numeric ld alignment, or -1 for anything that is not one (a symbol, an expression, an
# empty extraction). ld accepts K and M suffixes, and a scanner that did not know them would
# read ALIGN(32K) as a granular escape.
function alignval(s,   v, i, c, d, n) {
    if (s ~ /^0[xX][0-9a-fA-F]+$/) {
        v = 0
        for (i = 3; i <= length(s); i++) {
            c = tolower(substr(s, i, 1))
            d = index("0123456789abcdef", c) - 1
            v = v * 16 + d
        }
        return v
    }
    if (s ~ /^[0-9]+$/) { return s + 0 }
    if (s ~ /^[0-9]+[kK]$/) { n = substr(s, 1, length(s) - 1); return (n + 0) * 1024 }
    if (s ~ /^[0-9]+[mM]$/) { n = substr(s, 1, length(s) - 1); return (n + 0) * 1048576 }
    return -1
}
function trim(s) {
    gsub(/^[[:space:]]+/, "", s)
    gsub(/[[:space:]]+$/, "", s)
    return s
}
BEGIN { JOIN_MAX = 12 }
# A record left open by the previous file is still a declaration; dropping it would be the
# same escape by another spelling. ENDFILE would say this once, and is a gawk extension.
FNR == 1 {
    if (rec != "") { emit(rec, recline, recfile); rec = "" }
    if (FILENAME != "") { files++ }
    inc[FILENAME] = 0; decl[FILENAME] = 0; asrt[FILENAME] = 0; ndecl[FILENAME] = 0
    nclass[FILENAME] = 0
    order[files] = FILENAME
}
/^[[:space:]]*#[[:space:]]*include[[:space:]]*<kernel_data_reserve\.ld\.h>/ { inc[FILENAME] = 1 }
/KICKOS_KERNEL_DATA_RESERVE_DECL[[:space:]]*\(/                             { decl[FILENAME] = 1 }
/KICKOS_KERNEL_DATA_RESERVE_ASSERT[[:space:]]*\(/                           { asrt[FILENAME] = 1 }
# THE DECLARATION IS A RECORD AND NOT A LINE. ld puts no constraint on where the address
# expression, the colon and the ALIGN sit relative to the section name, so the record is
# joined from the name to the brace that opens the section body before one character of it is
# read. Membership is decided on the token `.appdata` standing first in a statement, which
# is what makes the class TOTAL: a script joins it by being written, in any spelling ld
# accepts, and cannot decline by wrapping.
# A RECORD RUNNING PAST ITS CAP IS JUDGED ANYWAY, so a shape that never reaches a brace fails
# the alignment clause rather than leaving the corpus.
{
    if (rec != "") {
        rec = rec " " $0
        joined++
        if (index($0, "{") > 0 || joined >= JOIN_MAX) { emit(rec, recline, recfile); rec = "" }
    } else if ($0 ~ /^[[:space:]]*\.appdata([[:space:]]|$)/) {
        rec = $0
        recline = FNR
        recfile = FILENAME
        joined = 1
        if (index($0, "{") > 0) { emit(rec, recline, recfile); rec = "" }
    }
}
function emit(text, line, file,   a, p, q, rest, tail, v, pinned) {
    ndecl[file]++
    decls++
    pinned = 0
    if (text ~ /MAX\([[:space:]]*\.[[:space:]]*,[[:space:]]*_kernel_data_top[[:space:]]*\)/) {
        pinned = 1
    }
    # The alignment is the one AFTER the colon: the address expression may carry parenthesised
    # calls of its own, and an AT() clause sits between the colon and the ALIGN.
    a = ""
    p = index(text, ":")
    if (p > 0) {
        rest = substr(text, p + 1)
        if (match(rest, /ALIGN\(/)) {
            tail = substr(rest, RSTART + RLENGTH)
            q = index(tail, ")")
            if (q > 0) { a = trim(substr(tail, 1, q - 1)) }
        }
    }
    if (a == "_appdata_size") {
        nclass[file]++
        class++
        if (!pinned) {
            printf "BAD %s %d NO_PIN %s\n", file, line, a
        }
    } else {
        v = alignval(a)
        if (v >= 0 && v <= granule) {
            granular++
        } else {
            if (a == "") { a = "(none)" }
            printf "BAD %s %d UNCOVERED_ALIGN %s\n", file, line, a
        }
    }
}
END {
    if (rec != "") { emit(rec, recline, recfile); rec = "" }
    for (i = 1; i <= files; i++) {
        f = order[i]
        if (nclass[f] > 0) {
            if (!inc[f])  { printf "BAD %s 0 NO_INCLUDE -\n", f }
            if (!decl[f]) { printf "BAD %s 0 NO_DECL -\n", f }
            if (!asrt[f]) { printf "BAD %s 0 NO_ASSERT -\n", f }
        } else {
            # A pin left behind on a script whose window no longer needs one guards nothing
            # and reads as cover.
            if (asrt[f] || decl[f]) { printf "BAD %s 0 STALE_PIN -\n", f }
        }
    }
    printf "STAT %d %d %d %d\n", files, decls + 0, class + 0, granular + 0
}
' "$@"
}

bad_count() { # <granule> <file>... -> BAD line count
    scan "$@" | grep -c '^BAD ' || :
}

# --- self-test: one control per clause, each a minimal pair -------------------
# Every planted script differs from the passing one in EXACTLY ONE property, so a control
# kept quiet by the wrong clause shows up as the wrong reason rather than as a clean run.
ctl() { # <name> <align-text> <address-expr> <include 0|1> <decl 0|1> <assert 0|1>
    _f="$TMP/$1.ld"
    : > "$_f"
    if [ "$4" = 1 ]; then printf '#include <kernel_data_reserve.ld.h>\n' >> "$_f"; fi
    printf '_appdata_size = KICKOS_APPDATA_SIZE;\n' >> "$_f"
    if [ "$5" = 1 ]; then printf 'KICKOS_KERNEL_DATA_RESERVE_DECL(ORIGIN(RAM))\n' >> "$_f"; fi
    printf 'SECTIONS\n{\n' >> "$_f"
    printf '    .appdata %s: %s\n    {\n        *(.data .data.*)\n    } > RAM\n' "$3" "$2" >> "$_f"
    printf '}\n' >> "$_f"
    if [ "$6" = 1 ]; then printf 'KICKOS_KERNEL_DATA_RESERVE_ASSERT(_ebss)\n' >> "$_f"; fi
    printf '%s\n' "$_f"
}

# The same declaration ld also accepts with the name, the address expression, the colon and
# the ALIGN on separate lines. A scanner requiring them on one line puts such a script in no
# class at all, which is the shape membership has to stay total over.
ctl_wrap() { # <name> <align-text> <address-expr> <include 0|1> <decl 0|1> <assert 0|1>
    _f="$TMP/$1.ld"
    : > "$_f"
    if [ "$4" = 1 ]; then printf '#include <kernel_data_reserve.ld.h>\n' >> "$_f"; fi
    printf '_appdata_size = KICKOS_APPDATA_SIZE;\n' >> "$_f"
    if [ "$5" = 1 ]; then printf 'KICKOS_KERNEL_DATA_RESERVE_DECL(ORIGIN(RAM))\n' >> "$_f"; fi
    printf 'SECTIONS\n{\n' >> "$_f"
    printf '    .appdata\n        %s:\n        %s\n    {\n        *(.data .data.*)\n    } > RAM\n' \
        "$3" "$2" >> "$_f"
    printf '}\n' >> "$_f"
    if [ "$6" = 1 ]; then printf 'KICKOS_KERNEL_DATA_RESERVE_ASSERT(_ebss)\n' >> "$_f"; fi
    printf '%s\n' "$_f"
}

CTL_OK="$(ctl ok 'ALIGN(_appdata_size)' 'MAX(., _kernel_data_top) ' 1 1 1)"
CTL_WRAPOK="$(ctl_wrap wrapok 'ALIGN(_appdata_size)' 'MAX(., _kernel_data_top) ' 1 1 1)"
CTL_WRAPNOPIN="$(ctl_wrap wrapnopin 'ALIGN(_appdata_size)' '' 1 1 1)"
CTL_WRAPGRAN="$(ctl_wrap wrapgran 'ALIGN(32)' '' 0 0 0)"
# THE WHOLE ESCAPE IN ONE FILE: a script that joins the class by being written, wrapped, with
# no mechanism of any kind. It reported NOTHING, which is the "a tenth script joins by being
# written" case the header claims totality over.
CTL_WRAPBARE="$(ctl_wrap wrapbare 'ALIGN(_appdata_size)' '' 0 0 0)"
# A declaration that never reaches its brace. Judged on what it has rather than dropped: a
# record leaving the corpus is the escape, whatever the reason it is malformed.
printf 'SECTIONS\n{\n    .appdata\n' > "$TMP/nobrace.ld"
CTL_NOBRACE="$TMP/nobrace.ld"
CTL_NOPIN="$(ctl nopin 'ALIGN(_appdata_size)' '' 1 1 1)"
CTL_NOINC="$(ctl noinc 'ALIGN(_appdata_size)' 'MAX(., _kernel_data_top) ' 0 1 1)"
CTL_NODECL="$(ctl nodecl 'ALIGN(_appdata_size)' 'MAX(., _kernel_data_top) ' 1 0 1)"
CTL_NOASRT="$(ctl noasrt 'ALIGN(_appdata_size)' 'MAX(., _kernel_data_top) ' 1 1 0)"
CTL_GRAN="$(ctl gran 'ALIGN(32)' '' 0 0 0)"
CTL_SMALL="$(ctl small 'ALIGN(8)' '' 0 0 0)"
CTL_HEX="$(ctl hex 'ALIGN(0x20)' '' 0 0 0)"
# The tenth-script hazard, spelled to slip past a grep for _appdata_size: a literal window.
CTL_LITWIN="$(ctl litwin 'ALIGN(0x8000)' '' 0 0 0)"
CTL_KWIN="$(ctl kwin 'ALIGN(32K)' '' 0 0 0)"
CTL_NOALIGN="$(ctl noalign '' '' 0 0 0)"
CTL_STALE="$(ctl stale 'ALIGN(8)' '' 1 1 1)"
# A script with no app window at all is not in the class and must stay silent.
printf 'SECTIONS\n{\n    .text : { *(.text) } > FLASH\n}\n' > "$TMP/nowindow.ld"
CTL_NOWIN="$TMP/nowindow.ld"
# The prose the class members carry about their own window, which the scanner must not read
# as a declaration. These lines are copied from the shapes in the tree.
cat > "$TMP/prose.ld" <<'EOF'
    /* The .appdata/.appbss catch-alls that follow then take EVERYTHING ELSE. */
    /* .appdata is loaded from flash; .appbss + the pad are zeroed. */
    /* ALIGN(_appdata_size) on .appdata aligns the window base above it. */
    .appdata_note = 0;
EOF
CTL_PROSE="$TMP/prose.ld"

for _p in "$CTL_OK" "$CTL_WRAPOK" "$CTL_WRAPGRAN" "$CTL_GRAN" "$CTL_SMALL" "$CTL_HEX" \
          "$CTL_NOWIN" "$CTL_PROSE"; do
    _n="$(bad_count "$GRANULE" "$_p" | tr -d ' ')"
    [ "$_n" -eq 0 ] || fail "negative control $_p reports $_n finding(s): $(scan "$GRANULE" "$_p" | grep '^BAD ')"
done

ctl_reason() { # <file> -> the single reason, or a diagnostic
    scan "$GRANULE" "$1" | awk '/^BAD /{ n++; r = $4 } END { if (n != 1) { printf "%d-findings\n", n + 0 } else { print r } }'
}
for _pair in \
    "$CTL_NOPIN:NO_PIN" \
    "$CTL_NOINC:NO_INCLUDE" \
    "$CTL_NODECL:NO_DECL" \
    "$CTL_NOASRT:NO_ASSERT" \
    "$CTL_LITWIN:UNCOVERED_ALIGN" \
    "$CTL_KWIN:UNCOVERED_ALIGN" \
    "$CTL_NOALIGN:UNCOVERED_ALIGN" \
    "$CTL_STALE:STALE_PIN" \
    "$CTL_WRAPNOPIN:NO_PIN" \
    "$CTL_NOBRACE:UNCOVERED_ALIGN" ; do
    _file="${_pair%:*}"
    _want="${_pair##*:}"
    _got="$(ctl_reason "$_file")"
    [ "$_got" = "$_want" ] || fail "positive control $_file reported '$_got', expected exactly one $_want;
      the clause it is meant to prove is not the clause that fired"
done

_n="$(bad_count "$GRANULE" "$CTL_WRAPBARE" | tr -d ' ')"
[ "$_n" -eq 4 ] || fail "a wrapped app window carrying no reserve mechanism at all reported
      $_n finding(s), expected 4 (no pin, no include, no decl, no assert). At 0 it is outside
      the class entirely and a new script joins this rule by declining to be read by it"

# The corpus tally the verdict is read off, on a corpus whose every number is known.
CTL_STAT="$(scan "$GRANULE" "$CTL_OK" "$CTL_GRAN" "$CTL_NOWIN" | grep '^STAT ')"
[ "$CTL_STAT" = "STAT 3 2 1 1" ] || fail "the control tally reads '$CTL_STAT', expected 'STAT 3 2 1 1'"
# The wrapped spelling counts in the SAME tally cells, so a scanner that saw it and then filed
# it somewhere else would still pass every clause above.
CTL_WSTAT="$(scan "$GRANULE" "$CTL_WRAPOK" "$CTL_WRAPGRAN" "$CTL_NOWIN" | grep '^STAT ')"
[ "$CTL_WSTAT" = "STAT 3 2 1 1" ] \
    || fail "the wrapped control tally reads '$CTL_WSTAT', expected 'STAT 3 2 1 1'"

# Move the granule and the verdict must move with it: at granule 8 the ALIGN(32) escape stops
# being one, which is what proves the bound is read and not assumed.
CTL_G8="$(bad_count 8 "$CTL_GRAN" | tr -d ' ')"
[ "$CTL_G8" -eq 1 ] || fail "with the granule at 8 the ALIGN(32) control reported $CTL_G8 finding(s),
      expected 1; the bound is not what decides the escape"
CTL_G64="$(bad_count 64 "$CTL_LITWIN" | tr -d ' ')"
[ "$CTL_G64" -eq 1 ] || fail "with the granule at 64 the ALIGN(0x8000) control reported $CTL_G64 finding(s),
      expected 1; a literal window is escaping through the numeric bound"

# --- the corpus ---------------------------------------------------------------
git ls-files -- '*.ld' > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched no linker script; every check below would pass vacuously"
N="$(wc -l < "$TMP/all" | tr -d ' ')"
grep -q '[[:space:]]' "$TMP/all" \
    && fail "a tracked linker-script path contains whitespace; the corpus is re-split unquoted below"

# One invocation over the whole corpus: the per-file verdicts are decided in END, and a
# per-file invocation would decide each of them against one file's records.
CORPUS="$(tr '\n' ' ' < "$TMP/all")"
scan "$GRANULE" $CORPUS > "$TMP/verdict" 2> "$TMP/scanerr" \
    || { sed 's/^/      /' "$TMP/scanerr" >&2; fail "the scanner failed over the corpus"; }

STAT="$(grep '^STAT ' "$TMP/verdict")"
[ -n "$STAT" ] || fail "the scanner emitted no tally over $N linker script(s)"
FILES="$(printf '%s\n' "$STAT" | awk '{ print $2 }')"
DECLS="$(printf '%s\n' "$STAT" | awk '{ print $3 }')"
CLASS="$(printf '%s\n' "$STAT" | awk '{ print $4 }')"
GRAN="$(printf '%s\n' "$STAT" | awk '{ print $5 }')"

[ "$FILES" = "$N" ] || fail "the scanner read $FILES of $N tracked linker script(s)"
[ "$DECLS" -gt 0 ] || fail "no .appdata output section was found in any of $N linker script(s):
      the declaration shape moved and this gate is reading nothing"
[ "$CLASS" -gt 0 ] || fail "no linker script aligns its app window to _appdata_size, so the class
      this gate exists for is empty; either the mechanism was replaced or the scanner is blind"
if [ "$CLASS" -lt "$CLASS_FLOOR" ]; then
    fail "$CLASS pinned window(s) over $FILES tracked linker script(s), below the floor of
      $CLASS_FLOOR. Every clause above is per-member, so nine members going to one satisfies all
      of them: what is left of the class is still correct and most of it is no longer read.
      Members leave with the boards they belong to, so this floor sits below the live count and
      moves down only with a board"
fi

if grep -q '^BAD ' "$TMP/verdict"; then
    echo "FAIL: an app window is based on the alignment above kernel .bss instead of on a" >&2
    echo "      declared address. One byte of kernel .bss then costs a whole _appdata_size:" >&2
    echo "      the window, the newlib heap and the user-RAM arena all slide one window" >&2
    echo "      higher and the link stays green. arch/common/kernel_data_reserve.ld.h holds" >&2
    echo "      the mechanism; each member states its reserve default and pins .appdata's" >&2
    echo "      own address expression to _kernel_data_top." >&2
    awk '/^BAD /{
        r = $4
        if (r == "NO_PIN")          { m = "aligns .appdata to _appdata_size without MAX(., _kernel_data_top) in its address expression" }
        else if (r == "NO_INCLUDE") { m = "is in the class and does not #include <kernel_data_reserve.ld.h>" }
        else if (r == "NO_DECL")    { m = "is in the class and invokes no KICKOS_KERNEL_DATA_RESERVE_DECL" }
        else if (r == "NO_ASSERT")  { m = "is in the class and invokes no KICKOS_KERNEL_DATA_RESERVE_ASSERT" }
        else if (r == "STALE_PIN")  { m = "carries the reserve mechanism but has no _appdata_size-aligned app window left to pin" }
        else                        { m = "declares .appdata with an alignment this rule does not cover: " $5 }
        if ($3 == "0") { printf "        %s %s\n", $2, m }
        else           { printf "        %s:%s %s\n", $2, $3, m }
    }' "$TMP/verdict" >&2
    exit 1
fi

echo "PASS: $CLASS pinned + $GRAN granular .appdata window(s) over $FILES tracked linker script(s)"
