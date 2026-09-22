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
require_repo_root
[ -f "$AWK_PROG" ] || fail "scanner missing: $AWK_PROG"

scratch_dir

# --- the scanner's controls, before the corpus is read ------------------------
# The scan is character-level and carries STATE across lines, so a mis-parse does not report a
# wrong line: it stops counting braces at all and every file past the slip reads as clean at
# depth 0. Nothing in the corpus can show that, which is what these planted files are for.
cat > "$TMP/ctl_hit.cc" <<'EOF'
extern "C" {
namespace {
int g;
}
}
EOF
cat > "$TMP/ctl_clean.cc" <<'EOF'
namespace {
int g;
}
extern "C" {
int h;
}
EOF
# THE CONTINUED DIRECTIVE, which is the slip: a `#` line is skipped, its continuations are not
# unless they are skipped WITH it, and an apostrophe on one of them opens a character literal
# that swallows the rest of the file.
cat > "$TMP/ctl_contd.cc" <<'EOF'
#define KICKOS_CTL_TEXT "this part's release is the reset controller's, and \
a continuation line that spells don't leaves an apostrophe unpaired \
before this closing quote"
extern "C" {
namespace {
int g;
}
}
EOF

# Sets CTL_HITS to the `<file>:<line>` hits and CTL_EXT to the trailing `<tagged> <gaps>`
# record. NOT through a command substitution at the call site, which would run it in a subshell
# and leave CTL_EXT unset in the caller.
scan_ctl() { # <file>
    if awk -v FNAME="$1" -f "$AWK_PROG" "$1" > "$TMP/ctl.out" 2>"$TMP/ctl.err"; then
        _rc=0
    else
        _rc=$?
    fi
    if [ "$_rc" -ne 0 ]; then
        sed 's/^/      /' "$TMP/ctl.err" >&2
        fail "the scanner exited $_rc on the planted $1, so it cannot count a file of that
    shape and its verdict over the corpus below would be UNKNOWN"
    fi
    CTL_EXT="$(sed -n 's/^EXT //p' "$TMP/ctl.out")"
    [ -n "$CTL_EXT" ] || fail "the scanner printed no EXT record for the planted $1, so nothing
    below can tell a file it found no linkage block in from one it could not read"
    CTL_HITS="$(grep -v '^EXT ' "$TMP/ctl.out" || :)"
}

scan_ctl "$TMP/ctl_hit.cc"
ctl="$CTL_HITS"
[ "$ctl" = "$TMP/ctl_hit.cc:2" ] || fail "the scanner reported [$ctl] for a planted anonymous
    namespace INSIDE an extern \"C\" block, rather than its line 2. That is the one defect this
    gate exists to catch, so a scanner that misses it cannot go red"
[ "$CTL_EXT" = "1 0" ] || fail "the scanner tagged and gapped [$CTL_EXT] rather than [1 0] on a
    planted file carrying one plain extern \"C\" block, so neither count says anything"

scan_ctl "$TMP/ctl_clean.cc"
ctl="$CTL_HITS"
[ -z "$ctl" ] || fail "the scanner reported [$ctl] for a planted anonymous namespace OUTSIDE the
    extern \"C\" block, which is the legitimate spelling every arch backend uses"

scan_ctl "$TMP/ctl_contd.cc"
ctl="$CTL_HITS"
[ "$ctl" = "$TMP/ctl_contd.cc:5" ] || fail "the scanner reported [$ctl] rather than line 5 for a
    planted file whose LINE-CONTINUED preprocessor directive leaves an apostrophe unpaired. The
    continuation is not a directive line and is scanned as code, so an unpaired quote or
    apostrophe there shifts the state machine and no brace after it is counted: the file, and
    every hit in it, then reads as clean at depth 0"

# THE BLIND FILE: a linkage block this scanner reads as an ordinary one, because something
# stands between the spec and the brace. Every hit is gated on the tag, so the anonymous
# namespace inside it is reported by nothing and the file reads as clean.
cat > "$TMP/ctl_gap.cc" <<'EOF'
extern "C" KICKOS_CTL_MACRO {
namespace {
int g;
}
}
EOF
scan_ctl "$TMP/ctl_gap.cc"
ctl="$CTL_HITS"
[ -z "$ctl" ] || fail "the scanner reported [$ctl] for a planted linkage block it does not tag,
    so the gap count below is measuring nothing"
[ "$CTL_EXT" = "0 1" ] || fail "the scanner tagged and gapped [$CTL_EXT] rather than [0 1] on a
    planted extern \"C\" block with a macro between the spec and the brace. That file carries
    the defect and reports no hit, so without the gap count it reads as clean"

# AND A DECLARATOR IS NOT A GAP, or every file defining a function with C linkage is refused.
cat > "$TMP/ctl_fndef.cc" <<'EOF'
extern "C" void kos_ctl_f(void) {
}
namespace {
int g;
}
EOF
scan_ctl "$TMP/ctl_fndef.cc"
ctl="$CTL_HITS"
[ -z "$ctl" ] || fail "the scanner reported [$ctl] for a planted function definition with C
    linkage, which opens no block and nests nothing"
[ "$CTL_EXT" = "0 0" ] || fail "the scanner tagged and gapped [$CTL_EXT] rather than [0 0] on a
    planted function definition with C linkage. It is not a linkage block and not a gap, and a
    gap count that says otherwise refuses every backend in the tree"

# `git ls-files`, not find: an untracked scratch file is neither gated nor counted.
corpus "$TMP/all" "C/C++ file" '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp'

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
: > "$TMP/gapped"
TAGGED=0
while IFS= read -r f; do
    # A refusal (exit 2) means the file could not be counted, NOT that it is clean, so it is
    # collected and failed on separately below.
    if awk -v FNAME="$f" -f "$AWK_PROG" "$f" > "$TMP/one" 2>> "$TMP/refused"; then
        _ext="$(sed -n 's/^EXT //p' "$TMP/one")"
        [ -n "$_ext" ] || fail "the scanner printed no EXT record for $f, so what it tagged
      there is unknown"
        _t="${_ext%% *}"
        _g="${_ext##* }"
        require_number "$_t" "the linkage blocks tagged in $f"
        require_number "$_g" "the untagged linkage braces in $f"
        TAGGED=$((TAGGED + _t))
        [ "$_g" -eq 0 ] || printf '%s: %s brace(s)\n' "$f" "$_g" >> "$TMP/gapped"
        grep -v '^EXT ' "$TMP/one" >> "$TMP/hits" || :
    else
        rc=$?
        [ "$rc" -eq 2 ] || fail "awk exited $rc scanning $f"
    fi
done < "$TMP/cand"

# A BRACE THE SCANNER HALF READ IS UNKNOWN, NOT CLEAN. Every hit is gated on a block tagged as
# language linkage, so a spelling the tag regex misses hides whatever is nested in it.
if [ -s "$TMP/gapped" ]; then
    echo "FAIL: an open brace whose code text carries extern \"C\" without ending in it, so" >&2
    echo "      this scanner read it as an ordinary block and anything nested inside it went" >&2
    echo "      unjudged. That is UNKNOWN, not clean. Put the brace directly after the" >&2
    echo "      linkage spec, or teach the tag regex in tests/static/extern_c_linkage.awk:" >&2
    sed 's/^/      /' "$TMP/gapped" >&2
    exit 1
fi

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

# WHAT THE SCANNER SAW, not what the pre-filter selected: the pre-filter matches a textual
# `extern "C"`, a declaration included, and a banner counting those asserts a pairing the
# scanner may never have opened a block for.
[ "$TAGGED" -gt 0 ] || fail "the scanner tagged no extern \"C\" block anywhere in the $cand
      candidate file(s) this tree's pre-filter selected, so every one of them reported no hit
      for want of a window rather than for want of a defect"

echo "PASS: $cand of $corpus tracked C/C++ files pair an extern \"C\" with a namespace, and the"
echo "      scanner tagged $TAGGED linkage block(s) across them; none nests an anonymous"
echo "      namespace inside one, and no brace was half read"
