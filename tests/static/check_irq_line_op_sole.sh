#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No file in the kernel layer may call arch_irq_mask, arch_irq_unmask or arch_irq_clear_pending
# except the one implementation that routes them, kernel/irq/irq_route.cc (freeze N3): each
# touches an image-wide word from whatever core the caller is on. The arch layer defines them and
# is outside the corpus. A caller that has to be allowed is a finding to raise, not a line to add.
#
# A call is the name, not inside a longer identifier, followed by `(`, in the residue
# tests/lib/strip_comments.awk leaves, so a mention in a comment or a literal is not one.
#
# usage: check_irq_line_op_sole.sh [repo-root]

set -u
. "$(dirname "$0")/../lib/gate.sh"

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
STRIP="$(cd "$(dirname "$0")/../lib" && pwd)/strip_comments.awk"
CALL='(^|[^A-Za-z0-9_])(arch_irq_mask|arch_irq_unmask|arch_irq_clear_pending)[[:space:]]*[(]'
ALLOWED='kernel/irq/irq_route.cc'
FILE_FLOOR=20
scratch_dir

calls() { # <file>: one `<line>:<text>` per call
    awk -f "$STRIP" < "$1" > "$TMP/stripped" || fail "the strip refused $1, so its verdict is UNKNOWN"
    grep -nE "$CALL" "$TMP/stripped"
}

printf 'void f(int l)\n{\n    arch_irq_mask(l);\n    arch_irq_unmask (l);\n    arch_irq_clear_pending(l);\n}\n' > "$TMP/pos.cc"
cat > "$TMP/neg.cc" <<'EOF'
// arch_irq_mask(line) in a comment
/* and arch_irq_unmask(line) in a block
   that spans lines arch_irq_clear_pending(line) */
char const* s = "arch_irq_clear_pending(line)";
int arch_irq_mask_count = 0;
void h(void) { my_arch_irq_unmask(1); int x = arch_irq_mask_count; (void)x; }
EOF
[ "$(calls "$TMP/pos.cc" | wc -l | tr -d ' ')" -eq 3 ] || fail "the reader missed a planted call"
[ "$(calls "$TMP/neg.cc" | wc -l | tr -d ' ')" -eq 0 ] || fail "the reader reported a comment, a literal or a longer name: $(calls "$TMP/neg.cc")"
echo "== control: three planted calls found, none in prose, literals or longer names =="

tool_out "$TMP/files" "" git -C "$ROOT" ls-files 'kernel/*.cc' 'kernel/*.h' 'kernel/*.h.in'
N="$(wc -l < "$TMP/files" | tr -d ' ')"
[ "$N" -ge "$FILE_FLOOR" ] || fail "$N tracked kernel file(s), below the floor of $FILE_FLOOR"
grep -Fxq "$ALLOWED" "$TMP/files" || fail "the routing implementation $ALLOWED is not tracked"

: > "$TMP/hits"
while IFS= read -r f; do
    [ "$f" = "$ALLOWED" ] && continue
    calls "$ROOT/$f" | sed "s|^|$f:|" >> "$TMP/hits"
done < "$TMP/files"
if [ -s "$TMP/hits" ]; then
    sed 's/^/      /' "$TMP/hits" >&2
    fail "kernel-layer call(s) to the delivery-gating seam outside $ALLOWED; route them through
  kickos::irq_line_op"
fi
echo "PASS: $N kernel-layer file(s): no call to arch_irq_mask, arch_irq_unmask or arch_irq_clear_pending outside $ALLOWED"
