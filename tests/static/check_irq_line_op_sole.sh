#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No file in the kernel layer may call arch_irq_mask, arch_irq_unmask or
# arch_irq_clear_pending except the one implementation that routes them,
# kernel/irq/irq_route.cc (freeze N3). The arch layer is outside this corpus: it defines those
# symbols.
#
# A kernel-layer caller that has to be allowed is a FINDING to raise, not a line to add to the
# allowlist.
#
# THE CORPUS COMES FROM `git ls-files`, so an untracked file is invisible: stage before gating.
# An EMPTY corpus is a failure, not a pass.
#
# usage: check_irq_line_op_sole.sh [repo-root]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

LC_ALL=C
export LC_ALL

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
[ -d "$ROOT/kernel" ] || fail "no kernel/ under $ROOT: this is not a KickOS tree, and an empty
  corpus below would read as a clean one"

# The three seam members whose state is image-wide. arch_irq_inject raises rather than gates and
# is deliberately absent; the syscall reaching it is bracketed by IrqLock instead.
SEAM='arch_irq_mask|arch_irq_unmask|arch_irq_clear_pending'
# The one file that may call them.
ALLOWED='kernel/irq/irq_route.cc'
# A corpus below this says git printed nothing useful, whatever it exited with.
FILE_FLOOR=20

scratch_dir

# --- the reader ---------------------------------------------------------------
# One record per offending line: <path>:<lineno>:<symbol>. A call is the symbol followed by an
# open parenthesis; a mention inside a comment or a string literal is not.
cat > "$TMP/reader.awk" <<'AWK'
BEGIN { inblock = 0 }
{
    line = $0
    # A block comment spans lines, which a per-line strip cannot see: the state opened by `/*`
    # survives to the matching `*/`.
    while (inblock || line ~ /\/\*/) {
        if (inblock) {
            if (line ~ /\*\//) { sub(/^.*\*\//, "", line); inblock = 0 }
            else { line = ""; break }
        }
        else {
            if (line ~ /\/\*.*\*\//) { sub(/\/\*.*\*\//, " ", line) }
            else { sub(/\/\*.*$/, "", line); inblock = 1; break }
        }
    }
    sub(/\/\/.*$/, "", line)
    gsub(/"[^"]*"/, "", line)
    # Without the parens the open-paren requirement binds to the last alternative alone, and
    # without the leading class foo_arch_irq_mask( counts as a call.
    rest = " " line
    while (match(rest, "[^A-Za-z0-9_](" seam ")[ \t]*\\(")) {
        sym = substr(rest, RSTART, RLENGTH)
        sub(/^[^A-Za-z0-9_]/, "", sym)
        sub(/[ \t]*\($/, "", sym)
        print FILENAME ":" FNR ":" sym
        rest = substr(rest, RSTART + RLENGTH)
    }
}
AWK

read_calls() { # <file>...
    awk -v seam="$SEAM" -f "$TMP/reader.awk" "$@"
}

# --- the reader's controls, before any tree is read ---------------------------
mkdir -p "$TMP/ctl"
cat > "$TMP/ctl/calls.cc" <<'EOF'
void f(int line)
{
    arch_irq_mask(line);
    arch_irq_unmask(line);
    arch_irq_clear_pending(line);
}
EOF
cat > "$TMP/ctl/prose.cc" <<'EOF'
// arch_irq_mask(line) named in a comment, and arch_irq_unmask( too
char const* s = "arch_irq_clear_pending(line)";
void g(int line) { other_call(line); } // arch_irq_mask(
EOF
cat > "$TMP/ctl/block.cc" <<'EOF'
/* arch_irq_mask(line) named in a one-line block comment */
void f(int line)
{
    /* a block comment that spans lines and names
       arch_irq_unmask(line) and arch_irq_clear_pending(line)
       across two of them */
    other_call(line);
}
/* arch_irq_mask(line) again, opener and closer on one line */ void g(void) { }
EOF
cat > "$TMP/ctl/mention.cc" <<'EOF'
void h(void)
{
    int arch_irq_mask_count = 0;
    (void)arch_irq_mask_count;
    int x = arch_irq_mask;
    (void)x;
    my_arch_irq_unmask(1);
}
EOF

ctl="$(read_calls "$TMP/ctl/calls.cc" | wc -l | tr -d ' ')"
if [ "$ctl" != "3" ]; then
    fail "the reader found $ctl call(s) in a planted file carrying exactly three, so it cannot
  recognise the shape this gate refuses and every verdict below is meaningless"
fi

ctl="$(read_calls "$TMP/ctl/prose.cc" | wc -l | tr -d ' ')"
if [ "$ctl" != "0" ]; then
    fail "the reader found $ctl call(s) in a planted file whose only mentions are inside a
  comment and a string literal. It would redden on the prose that explains the rule, which is
  a gate nobody can keep green honestly"
fi

ctl="$(read_calls "$TMP/ctl/block.cc" | wc -l | tr -d ' ')"
if [ "$ctl" != "0" ]; then
    fail "the reader found $ctl call(s) in a planted file whose only mentions are inside block
  comments, one of them spanning three lines. It counts prose as code, so the only way to keep
  this gate green would be to stop explaining the rule in the files the rule is about"
fi

ctl="$(read_calls "$TMP/ctl/mention.cc" | wc -l | tr -d ' ')"
if [ "$ctl" != "0" ]; then
    fail "the reader found $ctl call(s) in a planted file mentioning a seam name only as part
  of a longer identifier, so an unrelated symbol would be reported as a violation"
fi

# A planted offender outside the allowlist, through the whole pipeline and not only the reader.
mkdir -p "$TMP/tree/kernel/irq" "$TMP/tree/kernel/init"
cp "$TMP/ctl/calls.cc" "$TMP/tree/kernel/init/planted_offender.cc"
: > "$TMP/tree/kernel/irq/irq_route.cc"
planted="$(read_calls "$TMP/tree/kernel/init/planted_offender.cc" | wc -l | tr -d ' ')"
if [ "$planted" != "3" ]; then
    fail "the pipeline reported $planted call(s) for a planted offender under kernel/, so a real
  one would pass unreported"
fi

# --- the corpus ---------------------------------------------------------------
( cd "$ROOT" && git ls-files 'kernel/*.cc' 'kernel/*.h' ) > "$TMP/files" 2>/dev/null || true
require_nonempty "$TMP/files" "git ls-files printed no kernel source at all under $ROOT, so the
  corpus is UNKNOWN rather than empty and every verdict below it would be vacuous"
nfiles="$(wc -l < "$TMP/files" | tr -d ' ')"
require_number "$nfiles" "the kernel-layer file count"
if [ "$nfiles" -lt "$FILE_FLOOR" ]; then
    fail "$nfiles tracked kernel file(s), below the floor of $FILE_FLOOR. A corpus that small is
  a misread and not a small kernel"
fi

grep -Fxq "$ALLOWED" "$TMP/files" || fail "the allowlisted implementation '$ALLOWED' is not a
  tracked file. It was renamed or never staged, and this gate would then refuse every legitimate
  call in it while asserting nothing about the rest"

echo "== the sole decider of a logical line's delivery gating =="
echo "   corpus: $nfiles tracked kernel-layer file(s); allowlist: $ALLOWED"

: > "$TMP/hits"
while IFS= read -r f; do
    [ "$f" = "$ALLOWED" ] && continue
    [ -f "$ROOT/$f" ] || continue
    ( cd "$ROOT" && read_calls "$f" ) >> "$TMP/hits"
done < "$TMP/files"

if [ -s "$TMP/hits" ]; then
    sed 's/^/      /' "$TMP/hits" >&2
    nh="$(wc -l < "$TMP/hits" | tr -d ' ')"
    fail "$nh kernel-layer call(s) to the delivery-gating seam outside '$ALLOWED', listed above.
  Each one touches an image-wide word from whatever core the caller happens to be on, which
  freeze N3 makes unsound and which no fault reports: it costs a lost mask or a lost latched
  raise. Route it through kickos::irq_line_op instead. If a caller genuinely cannot be routed,
  that is a finding to raise and not a line to add to this gate's allowlist"
fi

echo "PASS: no kernel-layer caller reaches arch_irq_mask, arch_irq_unmask or
  arch_irq_clear_pending outside '$ALLOWED', so the routed core is the only core that gates a
  line and the rule is enforced rather than remembered"
exit 0
