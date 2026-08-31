#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# arch_cpu_id() must stay a PREPROCESSOR FOLD at one core, never an inline the optimiser
# is trusted to remove. A single-core image is required to be byte-identical to one with
# no arch_cpu_id at all, and that is a property of the macro, not of -Os: this tree has
# already measured GCC out-lining an always_inline candidate at -Os
# (system/include/kickos/sys/atomic.h), so "the optimiser will fold it" is known-false
# here. Softening the macro to a `static inline` keeps every build green and kills the
# property silently.
#
# Usage, one argument, the GENERATED board config (never a CMake variable: the gate reads
# the same knob spelling C reads, so the two cannot drift):
#   tests/static/check_cpu_id_fold.sh <build>/generated/include/kickos/board_config.h
#
#   posture    KICKOS_NUM_CORES is read from that header. At > 1 the seam is a real
#              function and MUST have a symbol, so the gate SKIPS (exit 77 -> CTest SKIP,
#              never a green PASS). A header it cannot read, or one carrying no
#              KICKOS_NUM_CORES at all, is REFUSED: an unreadable posture is unknown, and
#              a gate that cannot tell which arm shipped must not vote.
#
#   leg 1      arch/include/kickos/arch/arch.h defines arch_cpu_id as a function-like
#              MACRO whose body is an integer LITERAL, bare or in one layer of
#              parentheses. The literal is the point. A macro body that calls something
#              (`#define arch_cpu_id() kos_cpu()`) is still a macro and still fails,
#              because it is no longer a fold.
#
#   leg 2      no tracked source DEFINES arch_cpu_id as a function. Corpus is every
#              tracked C/C++/asm source and header from git ls-files. A tracked file
#              missing from the worktree is a hard failure, not a file to skip: the
#              difference between "no definition" and "not looked at" is the whole gate.
#
# A DECLARATION (`uint32_t arch_cpu_id(void);`) and a CALL are both allowed, so leg 2 keys on
# the definition SHAPE. A line whose statement ends in `;` is one of those two. What is left
# is read on both sides of the seam's own parenthesised list:
#
#   after      a definition's list is followed by nothing but trailing qualifiers and the
#              body's opening brace, on the line or on the next non-blank one (this tree is
#              Allman, so the next line is the usual case). A call sits inside an expression,
#              so the rest of that expression follows its list instead: `] == nullptr)`,
#              `!= 0u)`, a bare `)`. That is what separates `if (k.current[arch_cpu_id()])`
#              and its Allman brace from a definition.
#
#   before     a definition names a RETURN TYPE first, so the text from the last `{`, `}` or
#              `;` on the line is identifier tokens, `*` and the `"C"` of `extern "C"`, and
#              its final identifier is not one an expression can put there (`return`, `and`,
#              `case`). This reading only ever ADDS a refusal: a return type in front of a
#              list the rest of an expression follows is a shape the scan cannot name. It is
#              not required for a finding, because an attribute or a macro in front of a real
#              definition (`__attribute__((noinline)) uint32_t arch_cpu_id(void)`) would then
#              read as an expression and escape.
#
# A parameter list this scan cannot bound, and a complete declarator no brace follows, are
# both REFUSED: unclassified is not clean.
#
# What a green run states:
#   - the corpus is tracked SOURCE, never the link, so check_seam_defaults.sh is what reads
#     symbols out of an image.
#   - leg 1 pins the SHAPE of the expansion, not the value: any integer literal folds, and at
#     one core the correct index is 0.
#   - a caller must spell the seam with parentheses to compile against a function-like macro
#     at all, so the compiler holds that half.
#   - the seam path is hard-coded below and its absence is a refusal; whether that arch.h is
#     the one on a build's include path is a separate question.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

SEAM="arch/include/kickos/arch/arch.h"

[ "$#" -eq 1 ] || fail "usage: $0 <build>/generated/include/kickos/board_config.h"
CONFIG="$1"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

# --- posture: which arm did this board actually build? ------------------------
[ -r "$CONFIG" ] || fail "cannot read the generated board config: $CONFIG"

# The generated header spells it `#define KICKOS_NUM_CORES <n>` inside an #ifndef. Take
# the define line, not the #ifndef, or the guard's own mention doubles the match.
CORES="$(awk '$1 == "#define" && $2 == "KICKOS_NUM_CORES" { print $3; exit }' "$CONFIG")"
[ -n "$CORES" ] \
    || fail "no KICKOS_NUM_CORES in $CONFIG; the Kconfig knob is gone or was renamed, and
      this gate cannot tell which arm of the seam shipped"
case "$CORES" in
    *[!0-9]*) fail "KICKOS_NUM_CORES is '$CORES' in $CONFIG, which is not a plain integer" ;;
    *) ;;
esac

if [ "$CORES" -gt 1 ]; then
    echo "SKIP: KICKOS_NUM_CORES=$CORES, so arch_cpu_id is a real function here and is"
    echo "      required to have a symbol. The fold is a single-core property only."
    exit 77
fi

# --- leg 1: the seam is a function-like macro expanding to a literal ----------
[ -f "$SEAM" ] || fail "the seam header is missing: $SEAM"

# Anchored on the define, and the body taken as everything after the parameter list, so a
# trailing comment does not read as part of the expansion.
MACRO="$(sed -n 's|^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*arch_cpu_id()[[:space:]][[:space:]]*\(.*\)$|\1|p' \
    "$SEAM" | sed 's|[[:space:]]*/[/*].*$||' | sed 's|[[:space:]]*$||')"

LEG1=0
if [ -z "$MACRO" ]; then
    LEG1=1
else
    # One layer of wrapping parentheses is idiomatic around a macro body and folds just the
    # same, so judge the literal inside them. One layer only: unwrapping arbitrarily deep
    # would need a real parser.
    BODY="$MACRO"
    case "$BODY" in
        '('*')') BODY="$(printf '%s' "$BODY" | sed 's|^([[:space:]]*||; s|[[:space:]]*)$||')" ;;
        *) ;;
    esac
    case "$BODY" in
        # An integer literal, optionally suffixed. Anything else is not a fold.
        *[!0-9uUlL]* | '') LEG1=2 ;;
        *[0-9]*) ;;
        *) LEG1=2 ;;
    esac
fi

# --- leg 2: nothing in the tree DEFINES the seam -----------------------------
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.S' '*.inc' '*.h.in' \
    > "$TMP/sources" || fail "git ls-files failed"
require_nonempty "$TMP/sources" \
    "git ls-files matched no C/C++ file; the definition scan would pass vacuously"
SOURCES="$(wc -l < "$TMP/sources" | tr -d ' ')"

: > "$TMP/findings"
: > "$TMP/refused"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    # Pre-filter only: most of the corpus never names the seam.
    grep -q 'arch_cpu_id' "$f" || continue
    awk -v F="$f" '
        BEGIN {
            # Tokens that may sit immediately before the seam name inside an EXPRESSION. Any
            # other identifier in that position is a return type.
            kws = "return case sizeof alignof new delete throw goto else do "
            kws = kws "if while for switch and or not xor compl bitand bitor "
            kws = kws "and_eq or_eq xor_eq not_eq co_return co_yield co_await"
            n = split(kws, w, " ")
            for (i = 1; i <= n; i++) { EXPR[w[i]] = 1 }
        }
        # Strip a whole-line comment and any trailing // comment before judging, so the
        # seam header prose (which names arch_cpu_id repeatedly) cannot register.
        { line = $0; sub(/\/\/.*$/, "", line) }
        # A definition inside `#if KICKOS_NUM_CORES > 1` is not compiled at one core, so it
        # cannot break the fold and is not a finding here. This scan has no preprocessor, so
        # it tracks that ONE guard by nesting depth: the multi-core arm opens at the depth its
        # `#if` reached, and closes at the `#else` or `#endif` that returns to that depth. A
        # guard spelled any other way (`>= 2`) is not recognised and its body is scanned, so
        # the canonical spelling is the one arch/include/kickos/arch/arch.h uses.
        line ~ /^[[:space:]]*#[[:space:]]*(if|ifdef|ifndef)/ {
            depth++
            if (multi == 0 && line ~ /#[[:space:]]*if[[:space:]]+KICKOS_NUM_CORES[[:space:]]*>[[:space:]]*1[[:space:]]*$/)
            {
                multi = depth
            }
            next
        }
        line ~ /^[[:space:]]*#[[:space:]]*(else|elif)/ {
            if (multi == depth) { multi = 0 }
            next
        }
        line ~ /^[[:space:]]*#[[:space:]]*endif/ {
            if (multi == depth) { multi = 0 }
            depth--
            next
        }
        multi > 0 { next }
        line ~ /^[[:space:]]*[*]/ { next }
        line !~ /arch_cpu_id[[:space:]]*\(/ { next }
        # The macro itself, which leg 1 owns.
        line ~ /^[[:space:]]*#[[:space:]]*define/ { next }
        {
            code = line
            sub(/[[:space:]]+$/, "", code)

            # The text after the seam name AND its own parenthesised list. A list this scan
            # cannot bound (a parenthesis nested inside it) leaves the verdict UNKNOWN, unless
            # the statement ends anyway.
            suf = code
            if (sub(/^.*arch_cpu_id[[:space:]]*\([^()]*\)/, "", suf) == 0) {
                if (code ~ /;[[:space:]]*$/) { next }
                print F ":" FNR ": " $0 > REFUSED
                next
            }
            sub(/^([[:space:]]*(const|volatile|noexcept|override|final))*[[:space:]]*/, "", suf)

            # The body opens right after the list. Judged BEFORE the statement terminator,
            # because a stray `;` after the closing brace still leaves a definition.
            if (suf ~ /^\{/) {
                print F ":" FNR ": " $0 > FINDINGS
                next
            }
            # No body here, so a terminated statement is a declaration or a call.
            if (code ~ /;[[:space:]]*$/) { next }

            # The text before the seam name, from the last scope or statement boundary on the
            # line, so an `extern "C" {` or a closing brace ahead of it does not read as part
            # of the declarator.
            pre = code
            sub(/arch_cpu_id[[:space:]]*\(.*$/, "", pre)
            sub(/^.*[{};]/, "", pre)
            sub(/^[[:space:]]+/, "", pre)
            sub(/[[:space:]]+$/, "", pre)
            kw = pre
            sub(/[^A-Za-z0-9_]+$/, "", kw)
            sub(/^.*[^A-Za-z0-9_]/, "", kw)
            type_pre = (pre ~ /^[A-Za-z0-9_"*[:space:]]+$/ && pre ~ /[A-Za-z_]/ && !(kw in EXPR))

            if (suf != "") {
                # The rest of an enclosing expression follows the list, so the seam is a CALL:
                # `] == nullptr)`, `!= 0u)`, a bare `)`. A return type in front of one is
                # neither an expression nor a shape this scan can name.
                if (pre != "" && type_pre) { print F ":" FNR ": " $0 > REFUSED }
                next
            }
            # Nothing follows the list. Allman: the brace is on the next non-blank line, and
            # FNR walks with getline, so the declarator line is kept.
            ln = FNR
            nxt = ""
            while ((getline nxt) > 0) { if (nxt ~ /[^[:space:]]/) { break } }
            if (nxt ~ /^[[:space:]]*\{/) {
                print F ":" ln ": " $0 > FINDINGS
                next
            }
            # A complete declarator that no brace follows: this scan cannot classify it.
            print F ":" ln ": " $0 > REFUSED
        }
    ' FINDINGS="$TMP/findings" REFUSED="$TMP/refused" "$f" \
        || fail "the definition scan failed on $f; a scanner that dies writes no finding and
      this gate would report PASS over a corpus it never read"
done < "$TMP/sources"

echo "== checked $SOURCES tracked C/C++ file(s) for an arch_cpu_id definition, and $SEAM for the fold =="

RC=0

if [ "$LEG1" -eq 1 ]; then
    echo "FAIL: $SEAM defines no function-like arch_cpu_id() macro." >&2
    echo "      At KICKOS_NUM_CORES == 1 the seam MUST be a preprocessor fold, so that a" >&2
    echo "      single-core image is byte-identical to one with no arch_cpu_id at all." >&2
    echo "      An inline function is NOT equivalent: -Os has been measured out-lining an" >&2
    echo "      always_inline candidate in system/include/kickos/sys/atomic.h." >&2
    echo "      Restore the macro. Do not suppress this gate." >&2
    RC=1
fi
if [ "$LEG1" -eq 2 ]; then
    echo "FAIL: arch_cpu_id() expands to '$MACRO', which is not an integer literal." >&2
    echo "      A macro that expands to a CALL is still not a fold: it emits a symbol" >&2
    echo "      reference and the byte-identity property dies with it." >&2
    echo "      Restore a literal expansion. Do not suppress this gate." >&2
    RC=1
fi

if [ -s "$TMP/refused" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/refused" | tr -d ' ') site(s) name arch_cpu_id in a shape this scan cannot classify," >&2
    echo "      so their verdict is UNKNOWN, not clean. A definition is the seam name, a" >&2
    echo "      parameter list, then a brace; a declaration or call ends in a semicolon:" >&2
    sed 's/^/      /' "$TMP/refused" >&2
    RC=1
fi

if [ -s "$TMP/findings" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') definition(s) of arch_cpu_id in a KICKOS_NUM_CORES == 1 build:" >&2
    sed 's/^/      /' "$TMP/findings" >&2
    echo "" >&2
    echo "      At one core the seam is a macro and NOTHING may define it: a definition" >&2
    echo "      means the fold was replaced by a real function, so the image now carries a" >&2
    echo "      call and a symbol it is required not to have." >&2
    echo "      Delete the definition and restore the macro. Do not suppress this gate." >&2
    RC=1
fi

[ "$RC" -eq 0 ] || exit 1

echo "PASS: arch_cpu_id() folds to the literal $MACRO, and no tracked source defines it"
