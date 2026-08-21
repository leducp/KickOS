#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The no-RMW rule of docs/reference/style.md: no atomic read-modify-write anywhere in the
# tracked tree. An RMW on a 32-bit atomic is a `__atomic_fetch_add_4`-class LIBCALL on armv6m
# and rxv3 and inline on armv7m, xtensa and the host, so an RMW written against the sim or a
# Cortex-M4 board builds and links green, and only picopi or microbit refuses it, at LINK
# time: a freestanding link has no libatomic. `is_always_lock_free` cannot express the rule
# either: it is 0 on armv6m and rxv3 even where the load and the store are inline plain
# instructions.
# The replacement is a load/store pair under whatever already serialises the field:
#   x.store(x.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_atomic_rmw.sh
#
#   corpus     tracked *.c, *.cc, *.cpp, *.h, *.hh, *.hpp, *.inc, *.h.in and *.S. Source
#              only: a scan of the docs would report style.md stating the rule.
#
#   comments   tests/lib/strip_comments.awk blanks `//`, `/* */` and every literal BEFORE
#              the match; comments and headers in this tree name the banned spellings and
#              are findings to a plain grep. A file whose block comment or literal is still
#              open at EOF is REFUSED by name, not skipped: its verdict is UNKNOWN.
#
#   named      the method, C11 free-function and builtin RMW spellings, listed one by one in
#              RMW_METHOD and RMW_FUNC below. The rule is READ-MODIFY-WRITE, never the
#              `__atomic_` prefix, and the self-test's negative corpus pins the plain load,
#              store and fence spellings out of the patterns: that is what fails a widening
#              of `__atomic_(...)` back into a prefix.
#
#   operator   `++ -- += -= &= |= ^=` applied to an atomic. The use site carries no atomic
#              spelling, so the only handle is which identifiers were DECLARED atomic, which
#              tests/static/atomic_decls.awk harvests from the same stripped corpus. The two
#              shapes are scoped differently:
#
#                member  `.name++` / `->name++`, over the names declared anywhere in the
#                        corpus: the atomic fields that matter are declared in a header and
#                        incremented in a driver.
#                bare    `name++`, over the names THAT FILE declares, never the whole
#                        corpus. A short field name is an ordinary local-variable name too,
#                        so a cross-file bare match would fire on a plain `uint32_t mode` in
#                        an unrelated file.
#
# How far the shapes above reach:
#   - the operator shapes read a plain identifier chain, so `(*p)++`, `v[i].f++` with a call
#     in the subscript, and a PREFIX `++` written to the LEFT of a call in the chain sit
#     outside them. A POSTFIX increment through a call (`stats_block()->stats.f++`) does hit.
#   - the harvest learns a name from a declaration whose type and name share ONE line, and
#     the bare shape needs that declaration in the file that uses the name. A member declared
#     through a macro, a declaration split over two lines, and an `extern` bare atomic
#     incremented in a third file therefore stay unlearned, as does `std::atomic<int> x(0);`,
#     `(` being excluded from the declarator terminators.
#   - a field of the house wrapper `kickos::Atomic` is not harvested. That type exposes no
#     RMW at all, so the compiler refuses those sites.
#   - the corpus is source text as committed, so an RMW the preprocessor assembles, or one
#     that becomes an RMW only after a macro argument is substituted, sits outside it.
#   - a 64-bit atomic LOAD is a `__atomic_load_8` libcall on every backend, armv7m included.
#     That is a separate rule, and style.md points `volatile` at those fields.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: every finding must be collected in one run.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
DECLS="$(dirname "$0")/atomic_decls.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can strip a comment"
[ -r "$DECLS" ] || fail "tests/static/atomic_decls.awk is unreadable; the operator half cannot run"

# The one way a member-shape finding can be classified away, keyed on the FILE AND THE NAME
# together, never on a name alone: one name can be a harvested atomic in one header and a
# plain integer field in another. Every entry in use is PRINTED on every run.
member_exempt_names() { # <file> -> names this file uses non-atomically, one per line
    case "$1" in
        # system/driver/foo/bar.cc) printf 'mode\n' ;;  # kos_bus_xfer's uint8_t, not the atomic
        *) : ;;
    esac
}

# --- the patterns, as EREs, built once so the self-test and the corpus scan cannot differ --

# The RMW methods, over `.` and `->` alike. `compare_exchange_weak` does not contain
# `.exchange(`, so the two alternatives cannot double-report one call.
RMW_METHOD='(\.|->)[[:space:]]*(fetch_(add|sub|and|or|xor|nand|max|min)|exchange|compare_exchange_(weak|strong)|test_and_set)[[:space:]]*\('

# The free-function and builtin spellings, listed BY NAME and never by an
# `__atomic_[a-z_]*` / `__sync_[a-z_]*` wildcard: a prefix would also refuse
# `__atomic_load_n`, `__atomic_store_n` and `__sync_synchronize`, none of which is an RMW.
RMW_FUNC='(^|[^A-Za-z0-9_])(atomic_(fetch_(add|sub|and|or|xor|nand)|exchange|compare_exchange_(weak|strong)|flag_test_and_set)(_explicit)?|__atomic_((add|sub|and|or|xor|nand)_fetch|fetch_(add|sub|and|or|xor|nand)|exchange(_n)?|compare_exchange(_n)?|test_and_set)|__sync_(fetch_and_(add|sub|and|or|xor|nand)|(add|sub|and|or|xor|nand)_and_fetch|bool_compare_and_swap|val_compare_and_swap|lock_test_and_set))[[:space:]]*\('

# The compound-assign and increment operators, and an optional subscript before them so
# `g_dev_line[i]++` reads as the array element it is.
OPS='(\+\+|--|\+=|-=|&=|\|=|\^=)'
SUB='([[:space:]]*\[[^]]*\])?'

# --- the scanner, as one function, so the self-test runs the SAME program as the tree -----
#
# detect <file-list> <workdir>, leaving in <workdir>: `findings` (file:line:text),
# `names` (file:line:name, every atomic declared), `refused` (unstrippable files) and
# `kept` (a per-file count of surviving non-RMW atomic accesses, the positive control).
detect() {
    _list="$1"
    _w="$2"
    mkdir -p "$_w/s" || fail "mkdir failed under $_w"
    : > "$_w/findings"
    : > "$_w/names"
    : > "$_w/refused"
    : > "$_w/kept"
    : > "$_w/idx"
    : > "$_w/exempt"

    # Pass 1: strip once, keep the result. The stripped copy is indexed by a COUNTER, not a
    # flattened path: `a/b` and `a__b` flatten alike and one would overwrite the other.
    _i=0
    while IFS= read -r f; do
        [ -f "$f" ] || fail "file in the corpus is missing from the worktree: $f"
        _i=$((_i + 1))
        if awk -f "$STRIP" "$f" > "$_w/s/$_i" 2>> "$_w/refused"; then
            printf '%s%s%s\n' "$_i" "$TAB" "$f" >> "$_w/idx"
        else
            _rc=$?
            [ "$_rc" -eq 2 ] || fail "awk exited $_rc stripping $f"
            rm -f "$_w/s/$_i"
        fi
    done < "$_list"

    # Pass 2: harvest every declaration, corpus-wide, BEFORE any operator match; the member
    # shape needs the whole corpus's names.
    while IFS="$TAB" read -r n f; do
        awk -v F="$f" -f "$DECLS" "$_w/s/$n" >> "$_w/names"
    done < "$_w/idx"

    # The corpus-wide set for the member shape: `member` records ONLY. A parameter's scope is
    # one function, so a parameter called `c` or `mode` would poison a cross-file pattern.
    _alt=""
    if [ -s "$_w/names" ]; then
        _alt="$(awk -F"$TAB" '$4 == "member" { print $3 }' "$_w/names" | sort -u | tr '\n' '|' | sed 's/|$//')"
    fi

    # Pass 3: match. The operator shapes are skipped when their name set is EMPTY:
    # `(...)()(...)` is an ERE that matches every line.
    while IFS="$TAB" read -r n f; do
        _mine="$_alt"
        _ex="$(member_exempt_names "$f")"
        if [ -n "$_ex" ]; then
            printf '%s\t%s\n' "$f" "$(printf '%s' "$_ex" | tr '\n' ' ')" >> "$_w/exempt"
            # A temp file, not `grep -f <(...)`: process substitution is a bashism and a
            # syntax error under the dash /bin/sh of the CI images.
            printf '%s\n' "$_ex" > "$_w/ex.names"
            _mine="$(awk -F"$TAB" '$4 == "member" { print $3 }' "$_w/names" | sort -u \
                     | grep -Fxv -f "$_w/ex.names" | tr '\n' '|' | sed 's/|$//')"
        fi
        {
            grep -nE "$RMW_METHOD" "$_w/s/$n"
            grep -nE "$RMW_FUNC" "$_w/s/$n"
            if [ -n "$_mine" ]; then
                grep -nE "(\.|->)[[:space:]]*($_mine)$SUB[[:space:]]*$OPS" "$_w/s/$n"
                grep -nE "(\+\+|--)[[:space:]]*([A-Za-z_][A-Za-z0-9_]*[[:space:]]*(\.|->)[[:space:]]*)+($_mine)([^A-Za-z0-9_]|$)" "$_w/s/$n"
            fi
            # The file's OWN declarations, parameters included. Never the corpus: a harvested
            # name is an ordinary local name elsewhere, and a corpus-wide bare match would
            # fire on that.
            _own="$(awk -F"$TAB" -v F="$f" '$1 == F { print $3 }' "$_w/names" | sort -u | tr '\n' '|' | sed 's/|$//')"
            if [ -n "$_own" ]; then
                # No `.` or `>` to the left, or a member access would report twice on one line.
                grep -nE "(^|[^A-Za-z0-9_.>])($_own)$SUB[[:space:]]*$OPS" "$_w/s/$n"
                grep -nE "(\+\+|--)[[:space:]]*($_own)([^A-Za-z0-9_]|$)" "$_w/s/$n"
            fi
        } | sort -t: -k1,1n -u | awk -v F="$f" '{ print F ":" $0 }' >> "$_w/findings"

        # The positive control: the legitimate non-RMW atomic accesses, builtin spellings
        # included. The total is checked below.
        grep -cE "(\.|->)[[:space:]]*(load|store)[[:space:]]*\(|atomic_(load|store)_explicit[[:space:]]*\(|__atomic_(load|store)_n[[:space:]]*\(" \
            "$_w/s/$n" >> "$_w/kept"
    done < "$_w/idx"
}

# --- self-test: prove the scanner both ways before reading the tree -----------------------
# The positive corpus is three files, not one: the member shape is cross-file, and a
# single-file self-test would pass while that scope was broken.
mkdir -p "$TMP/st"
cat > "$TMP/st/decl.h" <<'EOF'
struct probe_stats
{
    std::atomic<uint32_t> probe_other;
    atomic_uint32_t probe_c11;
    _Atomic uint32_t probe_c;
};
EOF
cat > "$TMP/st/pos.cc" <<'EOF'
std::atomic<uint32_t> g_probe = 0;
std::atomic<uint32_t> g_arr[4] = {0, 0, 0, 0};
std::atomic_flag g_flag;
void named(std::atomic<uint32_t>* p, uint32_t e)
{
    g_probe.fetch_add(1, std::memory_order_relaxed);
    g_probe.fetch_sub(1, std::memory_order_relaxed);
    g_probe.fetch_and(1, std::memory_order_relaxed);
    g_probe.fetch_or(1, std::memory_order_relaxed);
    g_probe.fetch_xor(1, std::memory_order_relaxed);
    g_probe.exchange(1, std::memory_order_relaxed);
    g_probe.compare_exchange_weak(e, 1);
    g_probe.compare_exchange_strong(e, 1);
    p->fetch_add(1, std::memory_order_relaxed);
    g_flag.test_and_set();
    atomic_fetch_add_explicit(p, 1u, memory_order_relaxed);
    atomic_fetch_or(p, 1u);
    atomic_exchange(p, 1u);
    atomic_compare_exchange_strong(p, &e, 1u);
    atomic_flag_test_and_set(&g_flag);
    __atomic_fetch_add(p, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(p, 1, __ATOMIC_RELAXED);
    __sync_fetch_and_add(p, 1);
    __sync_bool_compare_and_swap(p, 0, 1);
}
void bare(void)
{
    g_probe++;
    g_probe--;
    ++g_probe;
    --g_probe;
    g_probe += 1u;
    g_probe -= 1u;
    g_probe &= 1u;
    g_probe |= 1u;
    g_probe ^= 1u;
    g_arr[2]++;
}
EOF
cat > "$TMP/st/use.cc" <<'EOF'
void member(struct probe_stats* q, struct probe_stats& s)
{
    s.probe_other++;
    q->probe_c11 += 1u;
    s.probe_c |= 1u;
    stats_block()->stats.probe_other++;
    // NOT a finding: `p` is a PARAMETER in pos.cc, so it never enters the corpus-wide member
    // set. If it ever did, this line makes the count below wrong instead of letting a
    // one-function name start matching every .p++ in the tree.
    cfg.p++;
}
EOF
: > "$TMP/st/pos.list"
for f in decl.h pos.cc use.cc; do
    printf '%s\n' "$TMP/st/$f" >> "$TMP/st/pos.list"
done
detect "$TMP/st/pos.list" "$TMP/st/pw"

# 19 named + 10 bare in pos.cc, and 4 member in use.cc. Counted as LINES: the scan
# deduplicates per line, so two shapes seeing one RMW are one finding.
POS="$(wc -l < "$TMP/st/pw/findings" | tr -d ' ')"
[ "$POS" -eq 33 ] || {
    cat "$TMP/st/pw/findings" >&2
    fail "the scanner found $POS of 33 planted RMWs; it would miss real ones"
}
# The member shape, in the file that declares NONE of the names it uses. A regression of that
# cross-file scope is otherwise silent.
MEM="$(grep -c '/use\.cc:' "$TMP/st/pw/findings" | tr -d ' ')"
[ "$MEM" -eq 4 ] || fail "the member shape found $MEM of 4 planted RMWs across a file boundary"
# The harvest itself. An empty one silently disables the whole operator half.
HM="$(awk -F"$TAB" '$4 == "member" { print $3 }' "$TMP/st/pw/names" | sort -u | wc -l | tr -d ' ')"
[ "$HM" -eq 6 ] || {
    sort -u "$TMP/st/pw/names" >&2
    fail "the harvest learned $HM member names, not the 6 planted (probe_other probe_c11 probe_c g_probe g_arr g_flag)"
}
# The member/param split. `p` is a parameter of named() and must be classified as one; if the
# classifier ever calls it a member, `cfg.p++` in use.cc becomes an extra finding above.
HP="$(awk -F"$TAB" '$4 == "param" { print $3 }' "$TMP/st/pw/names" | sort -u | tr '\n' ' ' | sed 's/ $//')"
[ "$HP" = "p" ] || fail "the harvest classified the parameter set as '$HP', not 'p'; the corpus-wide member scan would widen to one-function names"

cat > "$TMP/st/neg.cc" <<'EOF'
// The rule itself: never fetch_add, and never x.fetch_add(1) or p->exchange(v).
/* A block comment naming .compare_exchange_weak( and g_probe++ over
   two lines, and probe_other += 1u as well. */
char const* s = "fetch_add";
char const* t = ".exchange(";
// A client and a driver exchange 1:1 over an endpoint; the exchange is a request/reply.
void legit(struct kos_byte_ring* r, std::atomic<uint32_t>& x, uint32_t v)
{
    atomic_store_explicit(&r->head, 0u, memory_order_relaxed);
    uint32_t const head = atomic_load_explicit(&r->head, memory_order_relaxed);
    atomic_store_explicit(&r->tail, (head + 1u) & r->mask, memory_order_relaxed);
    x.store(x.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    __atomic_store_n(&r->tail, __atomic_load_n(&r->head, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED);
    atomic_thread_fence(std::memory_order_release);
    __sync_synchronize();
    bus_exchange(&v);
    uint32_t probe_other = 0;
    probe_other++;
    probe_other += 2u;
    uint32_t g_probe_local = v;
    g_probe_local++;
    if (v <= 1u and v != 0u) { v = v - 1u; }
}
EOF
: > "$TMP/st/neg.list"
printf '%s\n' "$TMP/st/decl.h" >> "$TMP/st/neg.list"
printf '%s\n' "$TMP/st/neg.cc" >> "$TMP/st/neg.list"
detect "$TMP/st/neg.list" "$TMP/st/nw"
NEG="$(grep -c '/neg\.cc:' "$TMP/st/nw/findings" | tr -d ' ')"
[ "$NEG" -eq 0 ] || {
    grep '/neg\.cc:' "$TMP/st/nw/findings" >&2
    fail "the scanner reported $NEG hit(s) on comments, literals, prose, load/store pairs and non-atomic locals; every finding would be noise"
}
# `probe_other` above is a plain uint32_t LOCAL in neg.cc while decl.h declares an atomic of
# that name: the bare shape must stay file-scoped or NEG rises.
NEGK="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/st/nw/kept")"
[ "$NEGK" -gt 0 ] || fail "the positive control counted no load/store in the negative corpus; the strip ate it"

# --- the corpus ---------------------------------------------------------------------------
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' '*.S' \
    > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched no C/C++ file; every check below would pass vacuously"
N="$(wc -l < "$TMP/all" | tr -d ' ')"

detect "$TMP/all" "$TMP/w"

DN="$(cut -f3 "$TMP/w/names" | sort -u | wc -l | tr -d ' ')"
DM="$(awk -F"$TAB" '$4 == "member" { print $3 }' "$TMP/w/names" | sort -u | wc -l | tr -d ' ')"
echo "== checked $N tracked C/C++ file(s) for an atomic RMW, comments and literals stripped =="
echo "== $DN distinct atomic identifier(s) harvested; $DM of them scanned corpus-wide as a member =="

if [ -s "$TMP/w/exempt" ]; then
    echo "== member-shape names classified away, by file (see member_exempt_names for the reason) =="
    sed 's/^/   /' "$TMP/w/exempt"
fi

if [ -s "$TMP/w/refused" ]; then
    echo "FAIL: the scan could not strip $(wc -l < "$TMP/w/refused" | tr -d ' ') file(s), so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/w/refused" >&2
    exit 1
fi

# These two prove the scan ran over THIS corpus: a zero harvest, or a strip that ate the
# tree, would make every absence-assertion above vacuous.
[ "$DN" -gt 0 ] || fail "not one atomic declaration was harvested from $N file(s); the operator half scanned for nothing"
KEPT="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/w/kept")"
[ "$KEPT" -gt 0 ] || fail "not one atomic load or store survived the strip across $N file(s); the scan read no code"

if [ -s "$TMP/w/findings" ]; then
    cat "$TMP/w/findings" >&2
    echo "" >&2
    # Which declaration taught the gate each identifier it fired on.
    _alt="$(cut -f3 "$TMP/w/names" | sort -u | tr '\n' '|' | sed 's/|$//')"
    grep -oE "$_alt" "$TMP/w/findings" | sort -u > "$TMP/hit"
    if [ -s "$TMP/hit" ]; then
        echo "the identifiers above are treated as atomic because of these declarations:" >&2
        while IFS= read -r nm; do
            awk -F"$TAB" -v N="$nm" '$3 == N { printf("      %s  <-  %s:%s (%s)\n", N, $1, $2, $4) }' \
                "$TMP/w/names" >&2
        done < "$TMP/hit"
        echo "      If one of those is NOT the object the finding touches, the name collides:" >&2
        echo "      record it in member_exempt_names at the top of this script, with the reason." >&2
        echo "" >&2
    fi
    echo "FAIL: $(wc -l < "$TMP/w/findings" | tr -d ' ') atomic read-modify-write(s)." >&2
    echo "      An RMW is a libatomic call on armv6m and rxv3 and a link failure there, while" >&2
    echo "      armv7m, xtensa and the sim inline it and link green. Write the load/store pair" >&2
    echo "      under whatever already serialises the field:" >&2
    echo "        x.store(x.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);" >&2
    echo "      Two real writers need the lock fixed, not an RMW." >&2
    exit 1
fi

echo "PASS: no atomic RMW across $N tracked C/C++ file(s), $KEPT plain atomic access(es) read"
