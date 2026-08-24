#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuses an anonymous namespace nested inside an `extern "C"` block.
#
# C language linkage does not stop at the namespace boundary: an entity declared in an
# anonymous namespace that sits INSIDE `extern "C" { ... }` gets an UNMANGLED GLOBAL
# symbol, so the internal linkage the anonymous namespace was written for is not there.
# Measured on all three toolchains this tree builds with (rx-elf 14.2, arm-none-eabi 15.3,
# host g++ 15.3): `extern "C" { namespace { int g; } }` emits `g` as a global B, while the
# same anonymous namespace OUTSIDE the block emits a local, mangled `_ZN12_GLOBAL__N_11gE`.
#
# The damage is not a miscompile, it is a claim on a generic identifier in the global C
# namespace of a KickOS archive: `g_pend_count`, `g_fixed_count`, `mpu_rasr`, `s_count`.
# A consumer app, a pure C main linking libkickos, that defines any of those names then
# fails to link, in the consumer's tree, over a symbol it never asked for.
#
# No allowlist, because the construct has no legitimate use. A seam symbol that MUST have C
# linkage and be global is written directly in the block (arch/CMakeLists.txt; that is how
# g_arch_current and g_arch_next reach switch.S), and a file-local one is written `static`,
# which keeps internal linkage under C language linkage. Each intent has an unambiguous
# spelling and neither of them is this one, so anything the scan finds is a defect.
#
# Source-tree gate: reads the tree through `git ls-files` and never opens the build
# directory, so it registers on every board and requires no build.
#
# usage: check_extern_c_linkage.sh    (from the repo root)

set -eu
. "$(dirname "$0")/../lib/gate.sh"

AWK_PROG="$(dirname "$0")/extern_c_linkage.awk"
[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
[ -f "$AWK_PROG" ] || fail "scanner missing: $AWK_PROG"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

# `git ls-files`, not find: an untracked scratch file is neither gated nor counted.
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' > "$TMP/all" \
    || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched no C/C++ file; every check below would pass vacuously"

# Only a file carrying BOTH an extern "C" and a namespace opener can be a hit, so the
# character-level scan runs over a few dozen files rather than the whole corpus.
: > "$TMP/cand"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    # NEWLINE-AGNOSTIC, because the awk scanner below is: it accumulates across lines, so it
    # would flag `extern` and `"C"` split over two lines, but a single-line grep would never
    # hand it the file, and the header claims this scan is exhaustive. A hand re-wrap is
    # enough to produce that spelling and silently re-open the hazard.
    if tr '\n' ' ' < "$f" | grep -q 'extern[[:space:]]*"C' && grep -q 'namespace' "$f"; then
        printf '%s\n' "$f" >> "$TMP/cand"
    fi
done < "$TMP/all"

corpus="$(wc -l < "$TMP/all" | tr -d ' ')"
cand="$(wc -l < "$TMP/cand" | tr -d ' ')"
# Positive control on the pre-filter: this tree's arch backends are built out of exactly
# this pairing, so a run that selected nothing selected wrongly and must not report clean.
[ "$cand" -gt 0 ] || fail "no tracked file carries both extern \"C\" and a namespace; the pre-filter is broken"

: > "$TMP/hits"
: > "$TMP/refused"
while IFS= read -r f; do
    # A refusal (exit 2) means the file could not be counted, NOT that it is clean, so it is
    # collected and failed on separately below.
    if awk -v FNAME="$f" -f "$AWK_PROG" "$f" >> "$TMP/hits" 2>> "$TMP/refused"; then
        :
    else
        rc=$?
        [ "$rc" -eq 2 ] || fail "awk exited $rc scanning $f"
    fi
done < "$TMP/cand"

if [ -s "$TMP/refused" ]; then
    n="$(wc -l < "$TMP/refused" | tr -d ' ')"
    echo "FAIL: the scan could not count $n file(s), so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/refused" >&2
    exit 1
fi

if [ -s "$TMP/hits" ]; then
    echo "FAIL: anonymous namespace inside an extern \"C\" block:" >&2
    echo "      C language linkage overrides it, so every entity declared there gets an" >&2
    echo "      UNMANGLED GLOBAL symbol instead of internal linkage. Put \`static\` on each" >&2
    echo "      entity (it keeps internal linkage under C language linkage). If a symbol IS" >&2
    echo "      a seam that asm or another TU must reach, declare it directly in the block." >&2
    sed 's/^/      /' "$TMP/hits" >&2
    exit 1
fi

echo "PASS: $cand of $corpus tracked C/C++ files pair an extern \"C\" block with a namespace;"
echo "      none of them nests an anonymous namespace inside the block"
