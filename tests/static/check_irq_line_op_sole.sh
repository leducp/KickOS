#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No file in the kernel layer may use a sole seam outside the one file allowed it (freeze N3):
#
#   arch_irq_mask, arch_irq_unmask, arch_irq_clear_pending   kernel/irq/irq_route.cc
#   arch_irq_inject                                          kernel/include/kickos/irq_route.h
#   kickos_ipc_fastpath                                      none
#
# The first four touch an image-wide word from whatever core the caller is on. The fastpath runs
# under the trap's own mask and mints a Held from it, so a C caller would hold a token for an
# exclusion it does not have; only the arch trap entries call it. The arch layer is outside the
# corpus. A use that has to be allowed is a finding to raise, not a line to add.
#
# A use is the name, not inside a longer identifier, in the residue tests/lib/strip_comments.awk
# leaves, so a mention in a comment or a literal is not one. A declaration, the name right behind
# its return type's `*`, is not one either.
#
# usage: check_irq_line_op_sole.sh [repo-root]

set -u
. "$(dirname "$0")/../lib/gate.sh"

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
STRIP="$(cd "$(dirname "$0")/../lib" && pwd)/strip_comments.awk"
SEAMS='arch_irq_mask kernel/irq/irq_route.cc
arch_irq_unmask kernel/irq/irq_route.cc
arch_irq_clear_pending kernel/irq/irq_route.cc
arch_irq_inject kernel/include/kickos/irq_route.h
kickos_ipc_fastpath -'
FASTPATH_DEF='kernel/syscall/syscall_ipc_fast.cc'
FILE_FLOOR=20
scratch_dir
printf '%s\n' "$SEAMS" > "$TMP/seams"

uses() { # <file> <listed-path> <out>: each use outside its allowed file, as `<path>:<line>:<text>`
    awk -f "$STRIP" < "$1" > "$TMP/stripped" || fail "the strip refused $1, so its verdict is UNKNOWN"
    : > "$3"
    while read -r name allowed; do
        [ "$2" = "$allowed" ] && continue
        grep -nE "(^|[^A-Za-z0-9_])$name([^A-Za-z0-9_]|\$)" "$TMP/stripped" > "$TMP/named" || true
        grep -vE "[*][[:space:]]*$name[[:space:]]*[(]" "$TMP/named" | sed "s|^|$2:|" >> "$3"
    done < "$TMP/seams"
}

# Never `uses | ...`: a refusal inside a pipeline exits only its subshell, and the gate passes.
scan() { # <root> <file-list>: every use outside its allowed file, into $TMP/hits
    : > "$TMP/hits"
    while IFS= read -r f; do
        uses "$1/$f" "$f" "$TMP/found"
        cat "$TMP/found" >> "$TMP/hits"
    done < "$2"
}

cat > "$TMP/pos.cc" <<'EOF'
void f(int l, uint32_t* a)
{
    arch_irq_mask(l);
    arch_irq_unmask (l);
    arch_irq_clear_pending(l);
    arch_irq_inject(l);
    (void)kickos_ipc_fastpath(a);
    auto* p = &kickos_ipc_fastpath;
}
EOF
cat > "$TMP/neg.cc" <<'EOF'
// arch_irq_mask(line) in a comment
/* and arch_irq_unmask(line) in a block
   that spans lines arch_irq_clear_pending(line) */
char const* s = "arch_irq_inject(line) kickos_ipc_fastpath(a)";
int arch_irq_mask_count = 0;
void h(void) { my_arch_irq_unmask(1); int x = arch_irq_mask_count; (void)x; }
extern "C" struct arch_context* kickos_ipc_fastpath(uint32_t* args);
friend ::arch_context* kickos_ipc_fastpath(uint32_t* args);
extern "C" struct arch_context *kickos_ipc_fastpath(uint32_t* args)
EOF
printf 'void g(int l)\n{\n    arch_irq_mask(l);\n    arch_irq_inject(l);\n}\n' > "$TMP/allow.cc"
uses "$TMP/pos.cc" planted.cc "$TMP/pos.out"
[ "$(wc -l < "$TMP/pos.out" | tr -d ' ')" -eq 6 ] \
    || fail "the reader missed a planted use: $(cat "$TMP/pos.out")"
uses "$TMP/neg.cc" planted.cc "$TMP/neg.out"
[ -s "$TMP/neg.out" ] \
    && fail "the reader reported a comment, a literal, a longer name or a declaration:
  $(cat "$TMP/neg.out")"
uses "$TMP/allow.cc" kernel/include/kickos/irq_route.h "$TMP/allow.out"
grep -q 'arch_irq_mask' "$TMP/allow.out" && [ "$(wc -l < "$TMP/allow.out" | tr -d ' ')" -eq 1 ] \
    || fail "one seam's allowed file excused another seam's use: [$(cat "$TMP/allow.out")]"
printf 'void g(void) {}\n/* open at the end of the file\n' > "$TMP/open.cc"
printf 'open.cc\n' > "$TMP/open.list"
if (scan "$TMP" "$TMP/open.list") > /dev/null 2> "$TMP/open.err"; then
    fail "the scan passed a file the strip refused, so a refusal reads as a clean file"
fi
grep -q 'is never closed' "$TMP/open.err" \
    || fail "the scan failed on the open comment for another cause: $(cat "$TMP/open.err")"
echo "== control: six planted uses found, none in prose, literals, longer names or declarations, an
   allowance excuses its own seam alone, and a file the strip refuses fails the scan =="

tool_out "$TMP/files" "" git -C "$ROOT" ls-files 'kernel/*.cc' 'kernel/*.h' 'kernel/*.h.in'
N="$(wc -l < "$TMP/files" | tr -d ' ')"
[ "$N" -ge "$FILE_FLOOR" ] || fail "$N tracked kernel file(s), below the floor of $FILE_FLOOR"
while read -r name allowed; do
    [ "$allowed" = "-" ] || grep -Fxq "$allowed" "$TMP/files" \
        || fail "$allowed, the one file allowed $name, is not tracked"
done < "$TMP/seams"
awk -f "$STRIP" < "$ROOT/$FASTPATH_DEF" > "$TMP/def" || fail "the strip refused $FASTPATH_DEF"
grep -qE "[*][[:space:]]*kickos_ipc_fastpath[[:space:]]*[(]" "$TMP/def" \
    || fail "$FASTPATH_DEF no longer defines kickos_ipc_fastpath, so the name this gate refuses
  names nothing"

scan "$ROOT" "$TMP/files"
if [ -s "$TMP/hits" ]; then
    sed 's/^/      /' "$TMP/hits" >&2
    fail "kernel-layer use(s) of a sole seam outside its allowed file; gate a line through
  kickos::irq_line_op, raise one through kickos::irq_inject, and leave the fastpath to its trap"
fi
echo "PASS: $N kernel-layer file(s): no sole seam used outside its allowed file"
