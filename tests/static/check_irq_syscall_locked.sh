#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# For each named `case KOS_SYS_*` arm of kernel/syscall/syscall.cc, an `IrqLock` must be in
# scope at the point the arm reaches the seam call.
#
# The scope test is textual: a lock taken inside a function the arm calls is not modelled, so an
# arm that delegates its bracket that way must not be listed here.
#
# An arm that vanishes is a failure, not a pass.
#
# usage: check_irq_syscall_locked.sh [repo-root]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

LC_ALL=C
export LC_ALL

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
SRC="kernel/syscall/syscall.cc"

# <case label>:<seam call> pairs. Each arm must carry an IrqLock enclosing that call.
ARMS='KOS_SYS_IRQ_INJECT:arch_irq_inject
KOS_SYS_IRQ_UNMASK:irq_line_op
KOS_SYS_IRQ_ATTACH:irq_line_op'

[ -f "$ROOT/$SRC" ] || fail "no $SRC under $ROOT: the syscall dispatch moved, and an absent
  corpus below would read as a clean one"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Brace depth is tracked from the `case` label, so a lock in a block that has already closed
# does not count as enclosing. Emits one record:
#   NOCASE | NOCALL <n> | UNLOCKED <lockline> <callline> | OK <lockline> <callline>
cat > "$TMP/reader.awk" <<'AWK'
BEGIN { state = 0; depth = 0; lockdepth = -1; lockline = 0; callline = 0 }
# A brace or a name inside a comment or a string literal is not code.
{
    line = $0
    sub(/\/\/.*$/, "", line)
    gsub(/"[^"]*"/, "", line)
    gsub(/'\''[^'\'']*'\''/, "", line)
}
state == 0 {
    if (line ~ ("case[ \t]+" arm "[ \t]*:")) { state = 1; depth = 0 }
    next
}
state == 1 {
    # A sibling `case` at depth 0 ends this arm.
    if (depth == 0 && line ~ /case[ \t]+[A-Za-z_]/ && line !~ ("case[ \t]+" arm)) {
        state = 2
        next
    }
    if (line ~ /IrqLock[ \t]+[A-Za-z_]/ && lockline == 0) {
        lockline = FNR
        lockdepth = depth
    }
    if (line ~ (seam "[ \t]*\\(") && callline == 0) {
        callline = FNR
        calldepth = depth
    }
    n = gsub(/\{/, "{", line)
    m = gsub(/\}/, "}", line)
    depth += n - m
    if (depth < 0) { state = 2 }
    next
}
END {
    if (state == 0) { print "NOCASE"; exit }
    if (callline == 0) { print "NOCALL " FNR; exit }
    # Enclosing means: declared before the call, at a depth the call is still inside.
    if (lockline == 0 || lockline > callline || lockdepth > calldepth) {
        print "UNLOCKED " lockline " " callline
        exit
    }
    print "OK " lockline " " callline
}
AWK

read_arm() { # <file> <case-label> <seam>
    awk -v arm="$2" -v seam="$3" -f "$TMP/reader.awk" "$1"
}

# --- the reader's controls, before the real file is read ----------------------
cat > "$TMP/ctl_ok.cc" <<'EOF'
switch (nr)
{
    case KOS_SYS_PLANTED:
    {
        int irq = 0;
        {
            IrqLock lock;
            planted_seam(irq);
        }
        return 0;
    }
    case KOS_SYS_OTHER:
    {
        return 1;
    }
}
EOF
cat > "$TMP/ctl_nolock.cc" <<'EOF'
switch (nr)
{
    case KOS_SYS_PLANTED:
    {
        int irq = 0;
        planted_seam(irq);
        return 0;
    }
}
EOF
cat > "$TMP/ctl_after.cc" <<'EOF'
switch (nr)
{
    case KOS_SYS_PLANTED:
    {
        planted_seam(0);
        IrqLock lock;
        return 0;
    }
}
EOF
cat > "$TMP/ctl_closed.cc" <<'EOF'
switch (nr)
{
    case KOS_SYS_PLANTED:
    {
        {
            IrqLock lock;
            something_else();
        }
        planted_seam(0);
        return 0;
    }
}
EOF
cat > "$TMP/ctl_sibling.cc" <<'EOF'
switch (nr)
{
    case KOS_SYS_PLANTED:
    {
        return 0;
    }
    case KOS_SYS_OTHER:
    {
        IrqLock lock;
        planted_seam(0);
        return 1;
    }
}
EOF
cat > "$TMP/ctl_comment.cc" <<'EOF'
switch (nr)
{
    case KOS_SYS_PLANTED:
    {
        // IrqLock lock; would go here, and planted_seam(0) below needs it
        IrqLock lock;
        planted_seam(0);
        return 0;
    }
}
EOF

ctl="$(read_arm "$TMP/ctl_ok.cc" KOS_SYS_PLANTED planted_seam)"
case "$ctl" in
    "OK 7 8") ;;
    *) fail "the reader answered [$ctl] for a planted arm holding IrqLock across its seam call,
  so it cannot recognise the shape this gate requires and every verdict below is meaningless" ;;
esac

ctl="$(read_arm "$TMP/ctl_nolock.cc" KOS_SYS_PLANTED planted_seam)"
case "$ctl" in
    "UNLOCKED 0 6") ;;
    *) fail "the reader answered [$ctl] for a planted arm with NO lock at all. That is the
  defect this gate exists to catch, so a reader that does not report it cannot go red" ;;
esac

ctl="$(read_arm "$TMP/ctl_after.cc" KOS_SYS_PLANTED planted_seam)"
case "$ctl" in
    "UNLOCKED 6 5") ;;
    *) fail "the reader answered [$ctl] for a planted arm whose lock is declared AFTER the seam
  call, which brackets nothing" ;;
esac

ctl="$(read_arm "$TMP/ctl_closed.cc" KOS_SYS_PLANTED planted_seam)"
case "$ctl" in
    "UNLOCKED 6 9") ;;
    *) fail "the reader answered [$ctl] for a planted arm whose lock sits in a block that CLOSED
  before the seam call. A lock out of scope is not a lock, and a reader that counts it would
  pass the one refactor most likely to introduce this bug" ;;
esac

ctl="$(read_arm "$TMP/ctl_sibling.cc" KOS_SYS_PLANTED planted_seam)"
case "$ctl" in
    NOCALL*) ;;
    *) fail "the reader answered [$ctl] for a planted arm that makes no seam call, its
  neighbour's lock and call sitting in the NEXT case. It read past the end of its own arm, so
  every verdict is about the wrong code" ;;
esac

ctl="$(read_arm "$TMP/ctl_comment.cc" KOS_SYS_PLANTED planted_seam)"
case "$ctl" in
    "OK 6 7") ;;
    *) fail "the reader answered [$ctl] for a planted arm whose comment names both the lock and
  the call a line early. It counts commented-out code, so a deleted lock left as a comment
  would read as present" ;;
esac

ctl="$(read_arm "$TMP/ctl_ok.cc" KOS_SYS_NO_SUCH_ARM planted_seam)"
case "$ctl" in
    NOCASE) ;;
    *) fail "the reader answered [$ctl] for a case label the file does not carry, so a renamed
  or deleted arm would read as a clean one" ;;
esac

# --- the real arms ------------------------------------------------------------
echo "== the IrqLock bracket on the IRQ syscall arms, in $SRC =="

nchecked=0
printf '%s\n' "$ARMS" | while IFS=: read -r arm seam; do
    [ -n "$arm" ] || continue
    rec="$(read_arm "$ROOT/$SRC" "$arm" "$seam")"
    kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
    f2="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
    f3="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
    case "$kind" in
        NOCASE)
            fail "$SRC carries no 'case $arm:'. The arm was renamed or removed, so this gate
  has nothing to assert about it and reports an absence it cannot tell apart from a pass. If
  the arm is genuinely gone, take its row out of this gate deliberately" ;;
        NOCALL)
            fail "the '$arm' arm of $SRC makes no '$seam(' call across its $f2 line(s). Either
  the arm stopped reaching the controller seam, in which case its row here is stale, or it now
  reaches it under another name and this gate has stopped measuring it. UNKNOWN, not a pass" ;;
        UNLOCKED)
            if [ "$f2" = "0" ]; then
                fail "the '$arm' arm of $SRC calls '$seam' at line $f3 with NO IrqLock in the
  arm at all. That call read-modify-writes image-wide controller words under the calling core's
  own interrupt mask, which is exclusion against this core's handler and none against another
  core: a lost mask or a lost latched raise, with no fault anywhere"
            fi
            fail "the '$arm' arm of $SRC declares IrqLock at line $f2 and calls '$seam' at line
  $f3, and that lock does NOT enclose the call: it is declared after it, or in a block that had
  already closed. A lock out of scope brackets nothing" ;;
        OK) ;;
        *)
            fail "the reader emitted [$rec] for '$arm', a record this gate does not model" ;;
    esac
    echo "   $arm: IrqLock at line $f2 encloses $seam at line $f3"
    nchecked=$((nchecked + 1))
done

# The subshell above cannot export its count, so the corpus is re-derived here.
narms="$(printf '%s\n' "$ARMS" | grep -c ':' || true)"
require_number "$narms" "the declared arm count"
if [ "$narms" -lt 3 ]; then
    fail "this gate declares $narms arm(s). The list was narrowed, and an arm dropped from it is
  an arm nothing checks"
fi

echo "PASS: all $narms IRQ syscall arm(s) hold IrqLock across the controller seam, so the
  image-wide masked and pending words are never read-modify-written from two cores at once"
exit 0
