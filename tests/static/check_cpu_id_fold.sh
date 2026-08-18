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
# A DECLARATION (`uint32_t arch_cpu_id(void);`) and a CALL are both allowed and both end
# the statement, so leg 2 keys on the definition shape: the opening brace, on the line or
# on the next non-blank one (this tree is Allman, so the next line is the usual case).
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - this reads SOURCE, never the link. A definition reaching the image from a generated
#     or untracked file is invisible here; check_seam_defaults.sh is what reads symbols.
#   - a macro whose literal is the WRONG literal. Leg 1 pins the shape, not the value; at
#     one core the only correct index is 0, and nothing here says so.
#   - a caller that spells the seam without parentheses. That cannot compile against a
#     function-like macro, so the compiler is the check, not this.
#   - arch.h being on no include path, or a second arch.h shadowing it. The path is
#     hard-coded below and its absence is a refusal, but its REACHABILITY is not tested.

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
    # same, so judge the literal inside them. Only one layer: nesting past that is not a
    # spelling anyone reaches for, and unwrapping arbitrarily deep would need a real parser.
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
    # Cheap pre-filter: most of the corpus never names the seam at all.
    grep -q 'arch_cpu_id' "$f" || continue
    awk -v F="$f" '
        # Strip a whole-line comment and any trailing // comment before judging, so the
        # seam header prose (which names arch_cpu_id repeatedly) cannot register.
        { line = $0; sub(/\/\/.*$/, "", line) }
        line ~ /^[[:space:]]*[*]/ { next }
        line !~ /arch_cpu_id[[:space:]]*\(/ { next }
        # The macro itself, which leg 1 owns.
        line ~ /^[[:space:]]*#[[:space:]]*define/ { next }
        {
            code = line
            sub(/[[:space:]]+$/, "", code)
            # A declaration or a call ends the statement; neither is a definition.
            if (code ~ /;[[:space:]]*$/) { next }
            if (code ~ /\{/) { print F ":" FNR ": " $0 > FINDINGS; next }
            # Allman: the brace is on the next non-blank line.
            nxt = ""
            while ((getline nxt) > 0) { if (nxt ~ /[^[:space:]]/) { break } }
            if (nxt ~ /^[[:space:]]*\{/) { print F ":" FNR ": " $0 > FINDINGS; next }
            # Neither a terminated statement nor a brace: this scan cannot classify it.
            print F ":" FNR ": " $0 > REFUSED
        }
    ' FINDINGS="$TMP/findings" REFUSED="$TMP/refused" "$f"
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
