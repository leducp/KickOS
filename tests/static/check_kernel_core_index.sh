#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Kernel::current, Kernel::idle and Kernel::boot are KICKOS_KERNEL_CORES long and take
# kickos_kernel_core(). Subscripting one of them with arch_cpu_id() is an out-of-bounds write
# under AMP, where the image drives four cores and the kernel schedules on one: the array holds
# one cell and a peer's identity is 1..3. The two seams are the same expansion wherever the
# counts agree, so such a subscript builds and runs green on every SMP and single-core preset
# and only the AMP one writes past the array.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_kernel_core_index.sh
#
#   corpus     tracked *.c, *.cc, *.cpp, *.h, *.hh, *.hpp, *.inc, *.h.in and *.S, the unit
#              fixtures included: a fixture builds at one core, where the mistake is invisible,
#              and an exclusion would be a second rule about which subscript is meant.
#
#   comments   tests/lib/strip_comments.awk blanks `//`, `/* */` and every literal BEFORE the
#              match; this tree's headers name the banned subscript in prose. A file whose
#              block comment or literal is still open at EOF is REFUSED by name, not skipped.
#
#   direct     the seam named inside the subscript: `current[arch_cpu_id()]`.
#
#   bound      a local the file binds to the seam and then subscripts with. This is the shape
#              the defect actually took (`uint32_t const cpu = arch_cpu_id(); k.current[cpu]`),
#              so the direct shape alone would have passed over it. The binding is learned from
#              the FILE ITSELF and never corpus-wide: `cpu` and `me` are ordinary local names.
#
# What the scan does NOT reach:
#   - a name bound to the seam through a call or a second local (`uint32_t c = id_of();`).
#   - a subscript holding a nested one (`current[map[i]]`), which is REFUSED rather than
#     skipped: unclassified is not clean.
#   - the array's IDENTITY. The left context excludes an identifier character, so `g_current`
#     is a different array and is not read; two arrays of these three names would need this
#     gate taught the difference.
#
# The rule is that the two seams stay DISTINCT, so the fix is never one accessor that hides
# which is meant: arch_cpu_id() is the machine's identity and kickos_kernel_core() is this
# kernel's slot, and their divergence IS the AMP model.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: every finding must be collected in one run.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
SCAN="$(dirname "$0")/kernel_core_index.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can strip a comment"
[ -r "$SCAN" ] || fail "tests/static/kernel_core_index.awk is unreadable; the scan cannot run"

# detect <file-list> <workdir>, leaving in <workdir>: `findings` (file:line:text), `refused`
# (a subscript the scan cannot bound, and a file it cannot strip) and `seen` (a per-file count
# of the subscripts read at all, the positive control).
detect() {
    _list="$1"
    _w="$2"
    mkdir -p "$_w" || fail "mkdir failed under $_w"
    : > "$_w/findings"
    : > "$_w/refused"
    : > "$_w/seen"

    while IFS= read -r f; do
        [ -f "$f" ] || fail "file in the corpus is missing from the worktree: $f"
        if awk -f "$STRIP" "$f" > "$_w/stripped" 2>> "$_w/refused"; then
            :
        else
            _rc=$?
            [ "$_rc" -eq 2 ] || fail "awk exited $_rc stripping $f"
            continue
        fi
        # Pre-filter only: most of the corpus subscripts none of the three.
        grep -qE '(^|[^A-Za-z0-9_])(current|idle|boot)[ \t]*\[' "$_w/stripped" || continue
        awk -v F="$f" -f "$SCAN" "$_w/stripped" > "$_w/records" \
            || fail "the scan failed on $f; a scanner that dies writes no finding and this
      gate would report PASS over a file it never read"
        sed -n 's/^FINDING //p' "$_w/records" >> "$_w/findings"
        sed -n 's/^REFUSED //p' "$_w/records" >> "$_w/refused"
        sed -n 's/^SEEN //p' "$_w/records" >> "$_w/seen"
    done < "$_list"
}

# --- self-test: prove the scan both ways before reading the tree ---------------------------
mkdir -p "$TMP/st"
cat > "$TMP/st/pos.cc" <<'EOF'
void direct(void)
{
    kernel().current[arch_cpu_id()] = nullptr;
    kernel().idle[arch_cpu_id()] = t;
    arch_start(&kernel().boot[arch_cpu_id()], &first->ctx);
}
void bound(void)
{
    uint32_t const cpu = arch_cpu_id();
    if (k.current[cpu] == nullptr) { return; }
    k.idle[cpu] = nullptr;
}
void cast_bound(void)
{
    size_t const me = static_cast<size_t>(arch_cpu_id());
    k.boot[me] = {};
}
EOF
cat > "$TMP/st/neg.cc" <<'EOF'
// The rule itself: never current[arch_cpu_id()], and never idle[cpu] after cpu = arch_cpu_id().
/* A block comment naming k.boot[arch_cpu_id()] over
   two lines. */
char const* s = "current[arch_cpu_id()]";
void legit(void)
{
    kernel().current[kickos_kernel_core()] = next;
    kernel().idle[core] = t;
    arch_start(&kernel().boot[kickos_kernel_core()], &first->ctx);
    // A DIFFERENT array, one cell per core the image drives, so the machine identity is right.
    uint32_t const cpu = arch_cpu_id();
    g_current[cpu] = nullptr;
    kickos_rv64_percpu[arch_cpu_id()].trap_stack = 0;
    uint32_t const other = id_of();
    k.current[other] = nullptr;
}
EOF
cat > "$TMP/st/ref.cc" <<'EOF'
void nested(void)
{
    k.current[map[arch_cpu_id()]] = nullptr;
}
EOF
: > "$TMP/st/pos.list"
printf '%s\n' "$TMP/st/pos.cc" >> "$TMP/st/pos.list"
detect "$TMP/st/pos.list" "$TMP/st/pw"
POS="$(wc -l < "$TMP/st/pw/findings" | tr -d ' ')"
[ "$POS" -eq 6 ] || {
    cat "$TMP/st/pw/findings" >&2
    fail "the scan found $POS of 6 planted subscripts; it would miss real ones"
}
[ -s "$TMP/st/pw/refused" ] && {
    cat "$TMP/st/pw/refused" >&2
    fail "the scan refused a shape the positive corpus plants as a finding"
}

: > "$TMP/st/neg.list"
printf '%s\n' "$TMP/st/neg.cc" >> "$TMP/st/neg.list"
detect "$TMP/st/neg.list" "$TMP/st/nw"
NEG="$(wc -l < "$TMP/st/nw/findings" | tr -d ' ')"
[ "$NEG" -eq 0 ] || {
    cat "$TMP/st/nw/findings" >&2
    fail "the scan reported $NEG hit(s) on comments, a literal, the kernel-core seam, a
      different array and a name bound to neither; every finding would be noise"
}
NEGS="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/st/nw/seen")"
[ "$NEGS" -gt 0 ] || fail "the positive control read no subscript in the negative corpus; the strip ate it"

: > "$TMP/st/ref.list"
printf '%s\n' "$TMP/st/ref.cc" >> "$TMP/st/ref.list"
detect "$TMP/st/ref.list" "$TMP/st/rw"
[ -s "$TMP/st/rw/refused" ] \
    || fail "the scan classified a nested subscript instead of refusing it, so an
      unclassified shape would read as clean"

# --- the corpus ----------------------------------------------------------------------------
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' '*.S' \
    > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched no C/C++ file; every check below would pass vacuously"
N="$(wc -l < "$TMP/all" | tr -d ' ')"

detect "$TMP/all" "$TMP/w"

SEEN="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/w/seen")"
echo "== checked $N tracked C/C++ file(s); $SEEN current/idle/boot subscript(s) read =="

# The positive control: a strip that ate the tree, or a pre-filter that matched nothing, would
# make every absence-assertion above vacuous.
[ "$SEEN" -gt 0 ] \
    || fail "not one current/idle/boot subscript was read across $N file(s); the scan read no code"

RC=0
if [ -s "$TMP/w/refused" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/w/refused" | tr -d ' ') site(s) or file(s) the scan cannot classify, so their verdict is" >&2
    echo "      UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/w/refused" >&2
    RC=1
fi

if [ -s "$TMP/w/findings" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/w/findings" | tr -d ' ') subscript(s) of Kernel::current / ::idle / ::boot by the machine's core" >&2
    echo "      identity:" >&2
    sed 's/^/      /' "$TMP/w/findings" >&2
    echo "" >&2
    echo "      Those arrays are KICKOS_KERNEL_CORES long. Under KICKOS_MULTICORE_AMP that is" >&2
    echo "      1 while the image drives four cores, so arch_cpu_id() on a peer indexes past" >&2
    echo "      the array. Use kickos_kernel_core()." >&2
    echo "      Do NOT answer this by folding the two seams into one accessor: arch_cpu_id is" >&2
    echo "      the machine's identity, kickos_kernel_core is this kernel's slot, and their" >&2
    echo "      divergence is the AMP model." >&2
    RC=1
fi

[ "$RC" -eq 0 ] || exit 1

echo "PASS: every current/idle/boot subscript across $N tracked file(s) takes the kernel-core seam"
