#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Kernel::current, idle and boot take a KernelCore, which no integer converts to, so passing
# arch_cpu_id() or any integer holding it does not compile. The one thing the type cannot refuse
# is a conversion written out, `static_cast<KernelCore>(arch_cpu_id())`, which under AMP indexes
# past the one slot while every SMP and single-core preset runs green. A conversion is therefore
# written only by the kernel's mints (instance.h) and the unit fixture's (kfixture.h).
#
# A conversion is the name, not inside a longer identifier, followed by `>`, `(`, `{` or `)`, in
# the residue tests/lib/strip_comments.awk leaves. Not reached: a conversion through an alias of
# the type, or one split across lines.
#
# usage: check_kernel_core_index.sh [repo-root]

set -u
. "$(dirname "$0")/../lib/gate.sh"

ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
STRIP="$(cd "$(dirname "$0")/../lib" && pwd)/strip_comments.awk"
CONV='(^|[^A-Za-z0-9_])KernelCore[[:space:]]*[>({)]'
MINTS='kernel/include/kickos/instance.h tests/unit/kfixture/kfixture.h'
scratch_dir

# Never inside a pipeline or a substitution: a refused strip must end this shell, not a subshell.
convs() { # <file>: one `<line>:<text>` per conversion, into $TMP/convs
    awk -f "$STRIP" < "$1" > "$TMP/stripped" || fail "the strip refused $1, so its verdict is UNKNOWN"
    grep -nE "$CONV" "$TMP/stripped" > "$TMP/convs"
}

cat > "$TMP/pos.cc" <<'EOF'
Thread* a(void) { return kernel().current(static_cast<KernelCore>(arch_cpu_id())); }
Thread* b(void) { return kernel().idle(KernelCore(arch_cpu_id())); }
void c(void) { kernel().current(::kickos::KernelCore{1u}) = nullptr; }
void d(uint32_t x) { arch_start(&kernel().boot((KernelCore)x), nullptr); }
EOF
cat > "$TMP/neg.cc" <<'EOF'
// static_cast<KernelCore>(arch_cpu_id()) in a comment
/* and KernelCore(x) in a block
   that spans lines (KernelCore)x */
char const* s = "KernelCore{1u}";
void e(KernelCore const me, KernelCore core) { for (KernelCore const c : KernelCores::all()) {} }
EOF
convs "$TMP/pos.cc"
[ "$(wc -l < "$TMP/convs" | tr -d ' ')" -eq 4 ] || fail "the reader missed a planted conversion"
convs "$TMP/neg.cc"
[ -s "$TMP/convs" ] && fail "the reader reported a comment, a literal, a declaration or a longer name: $(cat "$TMP/convs")"
echo "== control: four planted conversions found, none in prose, literals, declarations or longer names =="

corpus_sources "$TMP/files"
N="$(wc -l < "$TMP/files" | tr -d ' ')"
for m in $MINTS; do
    grep -Fxq "$m" "$TMP/files" || fail "the mint file $m is not tracked"
    convs "$ROOT/$m"
    [ -s "$TMP/convs" ] || fail "the mint file $m converts nothing; the reader or the mints moved"
done
: > "$TMP/hits"
while IFS= read -r f; do
    case " $MINTS " in
        *" $f "*) continue ;;
    esac
    convs "$ROOT/$f"
    sed "s|^|$f:|" "$TMP/convs" >> "$TMP/hits"
done < "$TMP/files"
if [ -s "$TMP/hits" ]; then
    sed 's/^/      /' "$TMP/hits" >&2
    fail "conversion(s) to KernelCore outside its mints; take a slot from kickos_kernel_core(),
  queue_core_of() or KernelCores, never from arch_cpu_id(), the machine's identity"
fi
echo "PASS: $N tracked source file(s): no conversion to KernelCore outside $MINTS"
