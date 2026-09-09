#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# arch_cpu_id() must stay a PREPROCESSOR FOLD at one core, never an inline the optimiser
# is trusted to remove: a single-core image is required to be byte-identical to one with no
# arch_cpu_id at all, which is a property of the macro and not of -Os. Softening the macro
# to a `static inline` keeps every build green and kills the property silently.
#
# Usage, one argument, the GENERATED board config (never a CMake variable: the gate reads
# the same knob spelling C reads, so the two cannot drift):
#   tests/static/check_cpu_id_fold.sh <build>/generated/include/kickos/board_config.h
#
#   posture    KICKOS_NUM_CORES is read from that header. At > 1 the seam is a real
#              function and MUST have a symbol, so the gate SKIPS (exit 77 -> CTest SKIP,
#              never a green PASS). The CTest entry therefore has to carry
#              SKIP_RETURN_CODE 77, or the skip reads as a failure. A header it cannot
#              read, or one carrying no KICKOS_NUM_CORES at all, is REFUSED: an unreadable
#              posture is unknown, and a gate that cannot tell which arm shipped must not
#              vote.
#
#   leg 1      arch/include/kickos/arch/arch.h defines arch_cpu_id as a function-like
#              MACRO whose body is an integer LITERAL, bare or in one layer of
#              parentheses. The literal is the point. A macro body that calls something
#              (`#define arch_cpu_id() kos_cpu()`) is still a macro and still fails,
#              because it is no longer a fold.
#
#   leg 2      no tracked source DEFINES arch_cpu_id as a function. Corpus is every
#              tracked C/C++/asm source and header from git ls-files, sorted by
#              tests/static/cpu_id_fold.awk, which is where the definition SHAPE is
#              written down. A tracked file missing from the worktree is a hard failure,
#              not a file to skip: the difference between "no definition" and "not looked
#              at" is the whole gate. So is a corpus in which NO file names the seam: that
#              is the pre-filter or the seam name having moved, and it is refused rather
#              than passed.
#
# What a green run states:
#   - the corpus is tracked SOURCE, never the link, so check_seam_defaults.sh is what reads
#     symbols out of an image.
#   - leg 1 pins the SHAPE of the expansion, not the value: any integer literal folds, and at
#     one core the correct index is 0.
#   - the seam path is hard-coded below and its absence is a refusal; whether that arch.h is
#     the one on a build's include path is a separate question.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

SEAM="arch/include/kickos/arch/arch.h"
SCAN="$(dirname "$0")/cpu_id_fold.awk"

[ "$#" -eq 1 ] || fail "usage: $0 <build>/generated/include/kickos/board_config.h"
CONFIG="$1"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"
[ -r "$SCAN" ] || fail "tests/static/cpu_id_fold.awk is unreadable; nothing below can
      classify a site that names the seam"

scratch_dir

# The two readings the scanner takes from the caller, so the controls below and the corpus
# scan cannot disagree about what the rule is.
GUARD_ERE='#[[:space:]]*if[[:space:]]+KICKOS_NUM_CORES[[:space:]]*>[[:space:]]*1[[:space:]]*$'
# Tokens that may sit immediately before the seam name inside an EXPRESSION. Any other
# identifier in that position is a return type.
EXPR_KWS='return case sizeof alignof new delete throw goto else do if while for switch
    and or not xor compl bitand bitor and_eq or_eq xor_eq not_eq co_return co_yield co_await'

# --- posture: which arm did this board actually build? ------------------------
CORES=""
posture() { # <config>; sets CORES, or exits 77 for the multi-core arm
    [ -r "$1" ] || fail "cannot read the generated board config: $1"
    # The generated header spells it `#define KICKOS_NUM_CORES <n>` inside an #ifndef. Take
    # the define line, not the #ifndef, or the guard's own mention doubles the match.
    CORES="$(awk '$1 == "#define" && $2 == "KICKOS_NUM_CORES" { print $3; exit }' "$1")"
    [ -n "$CORES" ] \
        || fail "no KICKOS_NUM_CORES in $1; the Kconfig knob is gone or was renamed, and
      this gate cannot tell which arm of the seam shipped"
    case "$CORES" in
        *[!0-9]*) fail "KICKOS_NUM_CORES is '$CORES' in $1, which is not a plain integer" ;;
        *) ;;
    esac
    if [ "$CORES" -gt 1 ]; then
        echo "SKIP: KICKOS_NUM_CORES=$CORES, so arch_cpu_id is a real function here and is"
        echo "      required to have a symbol. The fold is a single-core property only."
        exit 77
    fi
}

# --- leg 1: the seam is a function-like macro expanding to a literal ----------
LEG1=0
MACRO=""
leg1() { # <header> <unwrap-one-paren 0|1>; sets LEG1 and MACRO
    LEG1=0
    MACRO=""
    if [ ! -f "$1" ]; then
        LEG1=3
        return 0
    fi
    # Anchored on the define, and the body taken as everything after the parameter list, so a
    # trailing comment does not read as part of the expansion.
    MACRO="$(sed -n 's|^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*arch_cpu_id()[[:space:]][[:space:]]*\(.*\)$|\1|p' \
        "$1" | sed 's|[[:space:]]*/[/*].*$||' | sed 's|[[:space:]]*$||')"
    if [ -z "$MACRO" ]; then
        LEG1=1
        return 0
    fi
    _body="$MACRO"
    if [ "$2" -eq 1 ]; then
        # One layer of wrapping parentheses is idiomatic around a macro body and folds just
        # the same, so judge the literal inside them. One layer only: unwrapping arbitrarily
        # deep would need a real parser.
        case "$_body" in
            '('*')') _body="$(printf '%s' "$_body" | sed 's|^([[:space:]]*||; s|[[:space:]]*)$||')" ;;
            *) ;;
        esac
    fi
    case "$_body" in
        # An integer literal, optionally suffixed. Anything else is not a fold.
        *[!0-9uUlL]* | '') LEG1=2 ;;
        *[0-9]*) ;;
        *) LEG1=2 ;;
    esac
}

# --- leg 2: nothing in the tree DEFINES the seam -----------------------------
SCANNED=0
scan_corpus() { # <list> <scanner> <guard-ere> <expr-kws> <findings-out> <refused-out>
    _list="$1"
    _prog="$2"
    : > "$5"
    : > "$6"
    SCANNED=0
    while IFS= read -r _f; do
        [ -f "$_f" ] || fail "tracked file is missing from the worktree: $_f"
        # Pre-filter only: most of the corpus never names the seam.
        grep -q 'arch_cpu_id' "$_f" || continue
        SCANNED=$((SCANNED + 1))
        awk -v F="$_f" -v GUARD="$3" -v EXPRKWS="$4" -f "$_prog" \
            FINDINGS="$5" REFUSED="$6" "$_f" \
            || fail "the definition scan failed on $_f; a scanner that dies writes no finding
      and this gate would report PASS over a corpus it never read"
    done < "$_list"
}

# --- self-test: one control per clause, each a minimal pair -------------------
# Everything below is PLANTED, so no control is in the tree's own corpus and none is tracked.
ctldir="$TMP/ctl"
mkdir -p "$ctldir"

# Each positive differs from a clean site in ONE property, and each lives in its OWN file:
# the scanner runs once per corpus file and appends to both output files, so a corpus of one
# file would not show a scanner that overwrote what the file before it found.
cat > "$ctldir/p_allman.cc" <<'EOF'
uint32_t arch_cpu_id(void)
{
    return 0;
}
EOF
cat > "$ctldir/p_sameline.cc" <<'EOF'
uint32_t arch_cpu_id(void) {
    return 0;
}
EOF
cat > "$ctldir/p_attr.cc" <<'EOF'
__attribute__((noinline)) uint32_t arch_cpu_id(void)
{
    return 0;
}
EOF
# The guard must CLOSE at its #endif: a definition below one is compiled at one core, and a
# scan that never left the multi-core arm would skip the rest of the file.
cat > "$ctldir/p_afterendif.cc" <<'EOF'
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void)
{
    return kos_mpidr();
}
#endif
uint32_t arch_cpu_id(void)
{
    return 0;
}
EOF
cat > "$ctldir/p_unbounded.cc" <<'EOF'
    kos_log(arch_cpu_id(kos_index(0)))
EOF
cat > "$ctldir/p_declarator.cc" <<'EOF'
uint32_t arch_cpu_id(void)
EOF
cat > "$ctldir/p_typedcall.cc" <<'EOF'
    uint32_t arch_cpu_id(void) == 0)
EOF
for F in p_allman.cc p_sameline.cc p_attr.cc p_afterendif.cc p_unbounded.cc \
         p_declarator.cc p_typedcall.cc; do
    printf '%s\n' "$ctldir/$F"
done > "$ctldir/pos.list"

# The negatives each differ in a property the rule TOLERATES.
cat > "$ctldir/n_decl.h" <<'EOF'
uint32_t arch_cpu_id(void);
EOF
cat > "$ctldir/n_call.cc" <<'EOF'
    if (k.current[arch_cpu_id()])
    {
        return;
    }
EOF
cat > "$ctldir/n_return.cc" <<'EOF'
    return arch_cpu_id();
EOF
cat > "$ctldir/n_define.h" <<'EOF'
#define arch_cpu_id() 0
EOF
cat > "$ctldir/n_guarded.cc" <<'EOF'
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void)
{
    return kos_mpidr();
}
#endif
EOF
cat > "$ctldir/n_exprkw.cc" <<'EOF'
    return arch_cpu_id() == 0u
           and g_ready;
EOF
cat > "$ctldir/n_prose.h" <<'EOF'
// arch_cpu_id() folds to a literal at one core
/*
 * arch_cpu_id() is the seam, and this line names arch_cpu_id() again.
 */
EOF
NEG_FILES='n_decl.h n_call.cc n_return.cc n_define.h n_guarded.cc n_exprkw.cc n_prose.h'
for F in $NEG_FILES; do
    printf '%s\n' "$ctldir/$F"
done > "$ctldir/neg.list"
# A corpus that names the seam nowhere, for the vacuity floor.
printf '%s\n' "$ctldir/n_empty.c" > "$ctldir/quiet.list"
printf 'int kos_unrelated(void) { return 1; }\n' > "$ctldir/n_empty.c"

count() { # <file>
    wc -l < "$1" | tr -d ' '
}

# The counts are EXACT: a control kept quiet by the wrong clause then shows up as the wrong
# number rather than as a pass.
scan_corpus "$ctldir/pos.list" "$SCAN" "$GUARD_ERE" "$EXPR_KWS" "$ctldir/f" "$ctldir/r"
[ "$SCANNED" -eq 7 ] || fail "the scanner read $SCANNED of 7 planted positive control file(s)"
if [ "$(count "$ctldir/f")" -ne 4 ] || [ "$(count "$ctldir/r")" -ne 3 ]; then
    cat "$ctldir/f" "$ctldir/r" >&2
    fail "the scanner reported $(count "$ctldir/f") definition(s) and $(count "$ctldir/r") refusal(s) over the
      planted corpus, expected 4 and 3; it would misjudge a real definition the same way"
fi
grep -q 'p_allman' "$ctldir/f" && grep -q 'p_sameline' "$ctldir/f" \
    && grep -q 'p_attr' "$ctldir/f" && grep -q 'p_afterendif' "$ctldir/f" \
    || fail "a planted definition was counted as a refusal, so the two verdicts are swapped"
grep -q 'p_unbounded' "$ctldir/r" && grep -q 'p_declarator' "$ctldir/r" \
    && grep -q 'p_typedcall' "$ctldir/r" \
    || fail "a planted unclassifiable site was counted as a definition or as clean"

# EACH negative on its own, so a control that is silent for the WRONG reason is visible: a
# whole-corpus zero cannot tell "every clause works" from "one clause swallowed the file".
QUIET=0
for F in $NEG_FILES; do
    printf '%s\n' "$ctldir/$F" > "$ctldir/one.list"
    scan_corpus "$ctldir/one.list" "$SCAN" "$GUARD_ERE" "$EXPR_KWS" "$ctldir/f" "$ctldir/r"
    [ "$SCANNED" -eq 1 ] || fail "negative control $F does not name the seam at all, so it is
      not a near miss of anything"
    if [ -s "$ctldir/f" ] || [ -s "$ctldir/r" ]; then
        cat "$ctldir/f" "$ctldir/r" >&2
        fail "negative control $F reports; the gate would redden a declaration, a call or prose"
    fi
    QUIET=$((QUIET + 1))
done
[ "$QUIET" -eq 7 ] || fail "$QUIET of 7 negative controls ran silent"

# Withdraw one reading and the count over the negative corpus must MOVE by an exact amount:
# that is what proves each negative was a near miss rather than merely clean.
NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'
mutate() { # <what> <guard-ere> <expr-kws> <expect-findings> <expect-refusals>
    scan_corpus "$ctldir/neg.list" "$SCAN" "$2" "$3" "$ctldir/f" "$ctldir/r"
    if [ "$(count "$ctldir/f")" -ne "$4" ] || [ "$(count "$ctldir/r")" -ne "$5" ]; then
        cat "$ctldir/f" "$ctldir/r" >&2
        fail "with the $1 withdrawn the scanner reports $(count "$ctldir/f") definition(s) and
      $(count "$ctldir/r") refusal(s) over the negative corpus, expected $4 and $5; the control for
      it is not a near miss and proves nothing"
    fi
}
mutate "multi-core guard"   "$NEVER"      "$EXPR_KWS" 1 0
mutate "expression keyword" "$GUARD_ERE"  ''          0 1
mutate "nothing"            "$GUARD_ERE"  "$EXPR_KWS" 0 0

# THE DEAD SCANNER, which is the case a planted violation cannot reach: an awk that dies
# writes no finding, and a gate that reads its own empty output reports the absence it was
# testing for. The corpus here is CLEAN, so only the tool death can redden it.
sed 's|^BEGIN {|BEGIN { kos_this_is_not_awk(|' "$SCAN" > "$ctldir/broken.awk"
if ( scan_corpus "$ctldir/neg.list" "$ctldir/broken.awk" "$GUARD_ERE" "$EXPR_KWS" \
        "$ctldir/f" "$ctldir/r" ) > "$ctldir/out" 2>&1; then
    fail "a scanner that cannot even parse read the whole control corpus and said nothing;
      every PASS this gate prints would be over a corpus it never read"
fi
grep -q 'the definition scan failed on' "$ctldir/out" \
    || {
        cat "$ctldir/out" >&2
        fail "a dead scanner reddened the gate without naming the tool failure, so the run
      reports a clean corpus instead of an unread one"
    }

# And the floor under it: a corpus no file of which names the seam has not been searched, it
# has been missed.
scan_corpus "$ctldir/quiet.list" "$SCAN" "$GUARD_ERE" "$EXPR_KWS" "$ctldir/f" "$ctldir/r"
[ "$SCANNED" -eq 0 ] || fail "the vacuity control names the seam, so the floor below it is
      not the thing being proven"

# leg 1, on planted headers. The macro reader is handed the unwrap knob, so the one layer of
# parentheses it tolerates can be withdrawn and shown to be a near miss.
printf '/* no arch_cpu_id here */\n' > "$ctldir/h_none.h"
printf '#define arch_cpu_id() kos_cpu()\n' > "$ctldir/h_call.h"
printf '#define arch_cpu_id() (kos_cpu())\n' > "$ctldir/h_wrapcall.h"
printf '#define arch_cpu_id() 0\n' > "$ctldir/h_bare.h"
printf '#define arch_cpu_id() (0)\n' > "$ctldir/h_wrapped.h"
printf '#define arch_cpu_id() 0u\n' > "$ctldir/h_suffix.h"
printf '#define arch_cpu_id() 0 // the only core\n' > "$ctldir/h_comment.h"

leg1_is() { # <label> <header> <unwrap> <expect>
    leg1 "$2" "$3"
    [ "$LEG1" -eq "$4" ] || fail "leg 1 control '$1' reads verdict $LEG1, expected $4"
}
leg1_is missing   "$ctldir/h_absent.h"   1 3
leg1_is nomacro   "$ctldir/h_none.h"     1 1
leg1_is call      "$ctldir/h_call.h"     1 2
leg1_is wrapcall  "$ctldir/h_wrapcall.h" 1 2
leg1_is bare      "$ctldir/h_bare.h"     1 0
leg1_is wrapped   "$ctldir/h_wrapped.h"  1 0
leg1_is suffix    "$ctldir/h_suffix.h"   1 0
leg1_is comment   "$ctldir/h_comment.h"  1 0
# The tolerance withdrawn: with no unwrapping, the parenthesised literal must newly report
# and the bare one must not.
leg1_is wrapped_nounwrap "$ctldir/h_wrapped.h" 0 2
leg1_is bare_nounwrap    "$ctldir/h_bare.h"    0 0

# posture, including the SKIP. Exit 77 is the CTest skip code, and the run that first returns
# it is a run whose CTest entry has to carry SKIP_RETURN_CODE 77, so the code is asserted
# EXACTLY and not merely as nonzero.
printf '#define KICKOS_NUM_CORES 1\n' > "$ctldir/b_one.h"
printf '#ifndef KICKOS_NUM_CORES\n#define KICKOS_NUM_CORES 1\n#endif\n' > "$ctldir/b_guarded.h"
printf '#ifndef KICKOS_NUM_CORES\n#define KICKOS_NUM_CORES 4\n#endif\n' > "$ctldir/b_four.h"
printf '#define KICKOS_BOARD_NAME "x"\n' > "$ctldir/b_noknob.h"
printf '#define KICKOS_NUM_CORES two\n' > "$ctldir/b_word.h"

posture_is() { # <label> <config> <expect-rc> <expect-ere>
    ( posture "$2" ) > "$ctldir/out" 2>&1
    _rc=$?
    [ "$_rc" -eq "$3" ] || {
        cat "$ctldir/out" >&2
        fail "posture control '$1' exits $_rc, expected $3"
    }
    [ -z "$4" ] || grep -qE "$4" "$ctldir/out" \
        || {
            cat "$ctldir/out" >&2
            fail "posture control '$1' exits $_rc for the wrong reason, expected /$4/"
        }
}
posture_is one      "$ctldir/b_one.h"      0  ''
posture_is guarded  "$ctldir/b_guarded.h"  0  ''
posture_is four     "$ctldir/b_four.h"     77 '^SKIP: KICKOS_NUM_CORES=4'
posture_is noknob   "$ctldir/b_noknob.h"   1  'no KICKOS_NUM_CORES in'
posture_is word     "$ctldir/b_word.h"     1  "is 'two' in"
posture_is unreadable "$ctldir/b_absent.h" 1  'cannot read the generated board config'

# --- the board, and then the tree --------------------------------------------
posture "$CONFIG"
leg1 "$SEAM" 1

git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.S' '*.inc' '*.h.in' \
    > "$TMP/sources" || fail "git ls-files failed"
require_nonempty "$TMP/sources" \
    "git ls-files matched no C/C++ file; the definition scan would pass vacuously"
SOURCES="$(wc -l < "$TMP/sources" | tr -d ' ')"

scan_corpus "$TMP/sources" "$SCAN" "$GUARD_ERE" "$EXPR_KWS" "$TMP/findings" "$TMP/refused"

echo "== checked $SOURCES tracked C/C++ file(s), $SCANNED of them naming arch_cpu_id, and $SEAM for the fold =="

RC=0

[ "$SCANNED" -gt 0 ] || {
    echo "FAIL: not one tracked file names arch_cpu_id, so leg 2 read nothing at all." >&2
    echo "      Every caller spells the seam and the seam header defines it, so this is the" >&2
    echo "      pre-filter or the seam's name having moved, and the corpus is UNKNOWN." >&2
    RC=1
}

if [ "$LEG1" -eq 3 ]; then
    echo "FAIL: the seam header is missing: $SEAM" >&2
    echo "      Leg 1 has nothing to read, so whether the fold shipped is UNKNOWN." >&2
    RC=1
fi
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
