#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every linker script whose app heap base is an ALIGN written INSIDE a section body carries
# the file-scope assert that proves the address the body produced is the one it claims.
#
# Run from the repo root, no arguments: tests/static/check_app_heap_align.sh
#
# WHY. Inside an output section body ld evaluates ALIGN RELATIVE to the section start, so
# `_kickos_heap_start = ALIGN(_appdata_used_end, 8)` yields an 8-aligned address only while
# the section itself starts 8-aligned. At FILE SCOPE the same expression is absolute, which is
# what KICKOS_APP_HEAP_ALIGN_ASSERT in arch/common/app_heap.ld.h compares. An image can be
# 8-aligned by accident, so a check on the low bits is not an arm and neither is a green link.
#
# MEMBERSHIP IS DERIVED, NEVER LISTED. The class is every tracked linker script that writes a
# heap base as an ALIGN inside a body; a twelfth script joins it by being written. A script
# whose heap base is a bare `.` is not in it: there is no relative evaluation to be wrong
# about, and nothing for the assert to compare.
#
# THE ASSERT IS HELD TO THE BODY IT SPEAKS FOR. Its two arguments must be the symbol the body
# assigned and the expression the body aligned, so an assert left pointing at a symbol some
# other edit renamed is a finding rather than an ASSERT that is true of nothing.
#
# THE SCANNER READS SOURCE TEXT AND CANNOT TELL A COMMENT FROM A STATEMENT: a comment
# spelling a heap-base assignment or the assert is read as one.
#
# THE CORPUS COMES FROM `git ls-files`, so an untracked script is invisible: stage before
# gating.

set -u
# The corpus is re-split unquoted into the scanner's argv; a glob character in a tracked path
# must not expand against the cwd and inventory a different file.
set -f
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

require_repo_root

scratch_dir

# The alignment the macro hardcodes. A body aligning to anything else is comparing against a
# constant it does not use.
WANT_ALIGN=8

# The class cannot collapse quietly: every clause below is decided per member, so a scanner
# that stopped seeing ten of the eleven reports the eleventh clean and says PASS. Members
# leave with the boards they belong to, so this sits below the live count.
CLASS_FLOOR=9

scan() { # <want-align> <file>... -> BAD/STAT records on stdout
    _wa="$1"; shift
    awk -v want="$_wa" '
function trim(s) {
    gsub(/^[[:space:]]+/, "", s)
    gsub(/[[:space:]]+$/, "", s)
    return s
}
# The text between the parenthesis at p and its match, so a nested call in the aligned
# expression does not end the extraction early.
function balanced(s, p,   d, i, c) {
    d = 0
    for (i = p; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "(") { d++ }
        if (c == ")") {
            d--
            if (d == 0) { return substr(s, p + 1, i - p - 1) }
        }
    }
    return ""
}
function last_comma(s,   d, i, c, k) {
    d = 0
    k = 0
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "(") { d++ }
        if (c == ")") { d-- }
        if (c == "," && d == 0) { k = i }
    }
    return k
}
FNR == 1 {
    if (FILENAME != "") { files++ }
    inc[FILENAME] = 0
    nbase[FILENAME] = 0
    nasrt[FILENAME] = 0
    order[files] = FILENAME
}
/^[[:space:]]*#[[:space:]]*include[[:space:]]*<app_heap\.ld\.h>/ { inc[FILENAME] = 1 }
# The heap base written as an ALIGN. The symbol carries one or two leading underscores across
# the fleet, so the class is keyed on the name and not on a spelling of it.
match($0, /_+kickos_heap_start[[:space:]]*=[[:space:]]*ALIGN[[:space:]]*\(/) {
    head = substr($0, RSTART, RLENGTH)
    sub(/[[:space:]]*=.*$/, "", head)
    sym = trim(head)
    args = balanced($0, RSTART + RLENGTH - 1)
    k = last_comma(args)
    if (k == 0) {
        printf "BAD %s %d ALIGN_UNREAD %s\n", FILENAME, FNR, sym
        next
    }
    expr = trim(substr(args, 1, k - 1))
    a = trim(substr(args, k + 1))
    nbase[FILENAME]++
    class++
    if (a + 0 != want || a !~ /^[0-9]+$/) {
        printf "BAD %s %d ALIGN_NOT_WANT %s\n", FILENAME, FNR, a
    }
    basesym[FILENAME] = sym
    baseexpr[FILENAME] = expr
    baseline[FILENAME] = FNR
}
match($0, /KICKOS_APP_HEAP_ALIGN_ASSERT[[:space:]]*\(/) {
    args = balanced($0, RSTART + RLENGTH - 1)
    k = last_comma(args)
    if (k == 0) {
        printf "BAD %s %d ASSERT_UNREAD -\n", FILENAME, FNR
        next
    }
    nasrt[FILENAME]++
    asrtsym[FILENAME] = trim(substr(args, 1, k - 1))
    asrtexpr[FILENAME] = trim(substr(args, k + 1))
    asrtline[FILENAME] = FNR
}
END {
    for (i = 1; i <= files; i++) {
        f = order[i]
        if (nbase[f] > 0) {
            if (!inc[f]) {
                printf "BAD %s %d NO_INCLUDE -\n", f, baseline[f]
            }
            if (nasrt[f] == 0) {
                printf "BAD %s %d NO_ASSERT -\n", f, baseline[f]
            } else {
                if (asrtsym[f] != basesym[f]) {
                    printf "BAD %s %d SYM_MISMATCH %s\n", f, asrtline[f], asrtsym[f]
                }
                if (asrtexpr[f] != baseexpr[f]) {
                    printf "BAD %s %d EXPR_MISMATCH %s\n", f, asrtline[f], asrtexpr[f]
                }
            }
        } else {
            # An assert left on a script whose heap base is no longer an ALIGN compares an
            # address against itself and reads as cover.
            if (nasrt[f] > 0) {
                printf "BAD %s %d STALE_ASSERT -\n", f, asrtline[f]
            }
        }
    }
    printf "STAT %d %d\n", files, class + 0
}
' "$@"
}

bad_count() { # <want-align> <file>... -> BAD line count
    scan "$@" | grep -c '^BAD ' || :
}

# --- self-test: one control per clause, each a minimal pair -------------------
# Every planted script differs from the passing one in EXACTLY ONE property, so a control kept
# quiet by the wrong clause shows up as the wrong reason rather than as a clean run.
ctl() { # <name> <base line> <assert line> <include 0|1>
    _f="$TMP/$1.ld"
    : > "$_f"
    if [ "$4" = 1 ]; then printf '#include <app_heap.ld.h>\n' >> "$_f"; fi
    printf 'SECTIONS\n{\n    .appbss (NOLOAD) :\n    {\n' >> "$_f"
    printf '        _appdata_used_end = .;\n' >> "$_f"
    if [ -n "$2" ]; then printf '        %s\n' "$2" >> "$_f"; fi
    printf '    } > RAM\n}\n' >> "$_f"
    if [ -n "$3" ]; then printf '%s\n' "$3" >> "$_f"; fi
    printf '%s\n' "$_f"
}

BASE='_kickos_heap_start = ALIGN(_appdata_used_end, 8);'
ASRT='KICKOS_APP_HEAP_ALIGN_ASSERT(_kickos_heap_start, _appdata_used_end)'

CTL_OK="$(ctl ok "$BASE" "$ASRT" 1)"
CTL_NOINC="$(ctl noinc "$BASE" "$ASRT" 0)"
CTL_NOASRT="$(ctl noasrt "$BASE" '' 1)"
CTL_SYM="$(ctl sym "$BASE" \
    'KICKOS_APP_HEAP_ALIGN_ASSERT(__kickos_heap_start, _appdata_used_end)' 1)"
CTL_EXPR="$(ctl expr "$BASE" \
    'KICKOS_APP_HEAP_ALIGN_ASSERT(_kickos_heap_start, _ebss)' 1)"
CTL_ALIGN="$(ctl align '_kickos_heap_start = ALIGN(_appdata_used_end, 16);' \
    'KICKOS_APP_HEAP_ALIGN_ASSERT(_kickos_heap_start, _appdata_used_end)' 1)"
CTL_STALE="$(ctl stale '_kickos_heap_start = .;' "$ASRT" 1)"
# A script whose heap base is a bare `.` is not in the class: nothing is evaluated relatively,
# so there is nothing for the assert to compare and demanding one would be an inventory.
CTL_PLAIN="$(ctl plain '_kickos_heap_start = .;' '' 0)"
# The double-underscore spelling one board uses. Reading it as a different symbol would drop
# that board out of the class silently.
CTL_DUNDER="$(ctl dunder '__kickos_heap_start = ALIGN(_appdata_used_end, 8);' \
    'KICKOS_APP_HEAP_ALIGN_ASSERT(__kickos_heap_start, _appdata_used_end)' 1)"
# A nested call in the aligned expression: a scanner splitting on the first comma would read
# the alignment as part of the expression and the expression as truncated.
CTL_NEST="$(ctl nest '_kickos_heap_start = ALIGN(MAX(_a, _b), 8);' \
    'KICKOS_APP_HEAP_ALIGN_ASSERT(_kickos_heap_start, MAX(_a, _b))' 1)"

for _p in "$CTL_OK" "$CTL_PLAIN" "$CTL_DUNDER" "$CTL_NEST"; do
    _n="$(bad_count "$WANT_ALIGN" "$_p" | tr -d ' ')"
    [ "$_n" -eq 0 ] \
        || fail "negative control $_p reports $_n finding(s): $(scan "$WANT_ALIGN" "$_p" | grep '^BAD ')"
done

ctl_reason() { # <file> -> the single reason, or a diagnostic
    scan "$WANT_ALIGN" "$1" \
        | awk '/^BAD /{ n++; r = $4 } END { if (n != 1) { printf "%d-findings\n", n + 0 } else { print r } }'
}
for _pair in \
    "$CTL_NOINC:NO_INCLUDE" \
    "$CTL_NOASRT:NO_ASSERT" \
    "$CTL_SYM:SYM_MISMATCH" \
    "$CTL_EXPR:EXPR_MISMATCH" \
    "$CTL_ALIGN:ALIGN_NOT_WANT" \
    "$CTL_STALE:STALE_ASSERT" ; do
    _file="${_pair%:*}"
    _want="${_pair##*:}"
    _got="$(ctl_reason "$_file")"
    [ "$_got" = "$_want" ] || fail "positive control $_file reported '$_got', expected exactly
      one $_want; the clause it is meant to prove is not the clause that fired"
done

# The tally the verdict is read off, on a corpus whose every number is known.
CTL_STAT="$(scan "$WANT_ALIGN" "$CTL_OK" "$CTL_PLAIN" "$CTL_DUNDER" | grep '^STAT ')"
[ "$CTL_STAT" = "STAT 3 2" ] || fail "the control tally reads '$CTL_STAT', expected 'STAT 3 2'"

# Move the wanted alignment and the verdict must move with it, which is what proves the
# constant is read rather than assumed.
CTL_A16="$(bad_count 16 "$CTL_OK" | tr -d ' ')"
[ "$CTL_A16" -eq 1 ] || fail "with the wanted alignment at 16 the passing control reported
      $CTL_A16 finding(s), expected 1; the constant is not what decides the clause"

# --- the corpus ---------------------------------------------------------------
corpus "$TMP/all" "linker script" '*.ld'
N="$(wc -l < "$TMP/all" | tr -d ' ')"
grep -q '[[:space:]]' "$TMP/all" \
    && fail "a tracked linker-script path contains whitespace; the corpus is re-split unquoted"

# One invocation over the whole corpus: the per-file verdicts are decided in END, and a
# per-file invocation would decide each of them against one file's records.
CORPUS="$(tr '\n' ' ' < "$TMP/all")"
scan "$WANT_ALIGN" $CORPUS > "$TMP/verdict" 2> "$TMP/scanerr" \
    || { sed 's/^/      /' "$TMP/scanerr" >&2; fail "the scanner failed over the corpus"; }

STAT="$(grep '^STAT ' "$TMP/verdict")"
[ -n "$STAT" ] || fail "the scanner emitted no tally over $N linker script(s)"
FILES="$(printf '%s\n' "$STAT" | awk '{ print $2 }')"
CLASS="$(printf '%s\n' "$STAT" | awk '{ print $3 }')"

[ "$FILES" = "$N" ] || fail "the scanner read $FILES of $N tracked linker script(s)"
if [ "$CLASS" -lt "$CLASS_FLOOR" ]; then
    fail "$CLASS script(s) write an app heap base as an ALIGN inside a section body, below the
      floor of $CLASS_FLOOR. Every clause above is per-member, so most of the class going
      unread still reports the rest clean"
fi

if grep -q '^BAD ' "$TMP/verdict"; then
    echo "FAIL: an app heap base is aligned inside a section body without the file-scope" >&2
    echo "      assert that proves it. ld aligns RELATIVE to the section start there, so the" >&2
    echo "      expression yields an 8-aligned address only while the section does; the link" >&2
    echo "      stays green either way and most images are aligned by accident." >&2
    echo "      arch/common/app_heap.ld.h holds the mechanism." >&2
    awk '/^BAD /{
        r = $4
        if (r == "NO_INCLUDE")          { m = "aligns its heap base in a body and does not #include <app_heap.ld.h>" }
        else if (r == "NO_ASSERT")      { m = "aligns its heap base in a body and invokes no KICKOS_APP_HEAP_ALIGN_ASSERT" }
        else if (r == "SYM_MISMATCH")   { m = "asserts on a symbol the body does not assign: " $5 }
        else if (r == "EXPR_MISMATCH")  { m = "asserts on an expression the body does not align: " $5 }
        else if (r == "ALIGN_NOT_WANT") { m = "aligns its heap base to a value the assert does not compare against: " $5 }
        else if (r == "STALE_ASSERT")   { m = "invokes the assert with no heap base aligned in a body left to prove" }
        else if (r == "ALIGN_UNREAD")   { m = "writes an ALIGN this scanner cannot read: " $5 }
        else                            { m = "invokes the assert with arguments this scanner cannot read" }
        printf "        %s:%s %s\n", $2, $3, m
    }' "$TMP/verdict" >&2
    exit 1
fi

echo "PASS: $CLASS app heap base(s) aligned inside a section body over $FILES tracked linker
  script(s), each proved at file scope against the symbol and the expression its own body used"
exit 0
