#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Compiles every C-facing header as a standalone C11 translation unit. The tree tracks ONE
# .c file (apps/hello_c), which reaches only the headers it includes, so a break in the C
# claim any other one makes surfaces in a consumer's tree and nowhere else.
#
# Run from the repo root:
#   tests/static/check_c_headers.sh <c-compiler> <include-root>...
#
#   corpus    a header is C-facing when it guards `extern "C"` with __cplusplus, plus every
#             header such a header includes, transitively. An UNGUARDED `extern "C"` is a C
#             syntax error, so a C++-only header says so by leaving the guard off. Nothing is
#             listed by name.
#             The seed scan reads `git ls-files`; the CLOSURE resolves through the include
#             roots and takes what it finds there tracked or not, so the generated and
#             installed kickos/config/cap_width.h is in the corpus.
#
#   comments  tests/lib/strip_comments.awk blanks them before the claim is read, and the raw
#             line supplies the string literal the stripper takes with them. Not optional: a
#             header naming __cplusplus and `extern "C"` in prose only is a hit to a plain
#             grep. A file whose comment or literal is still open at EOF is REFUSED by name:
#             whether it is C-facing was then read off a partial file.
#
#   standalone one TU per header. A header that compiles only after another has supplied
#             <stdint.h> is a finding here instead of in a consumer's include order.
#
#   roots     every in-tree include root is derived from the tracked paths. The arch/ and
#             boards/ ones are NOT: six directories provide kickos/arch/context.h, so putting
#             them all on one -I line resolves to whichever came first. The caller passes the
#             one this board builds, with the generated directory and the chip and board ones.
#
#   compiler  passed in, and it is the board's own CMAKE_C_COMPILER; there is no search and no
#             default. It is PROVEN BOTH WAYS below before the corpus is read, and one that
#             cannot be proven is refused by name.
#
#   std       -std=c11, never the compiler's default. C23, the gcc 15 default, adopts `bool`,
#             `alignas`, `static_assert` and `nullptr` as keywords, so a default-std probe
#             accepts four spellings that are C++ only under this rule. The negative probes
#             fail the compiler if the pin did not take.
#
#   refusal   a header whose own #include cannot be found is REFUSED by name, not reported as
#             invalid C: the compiler judged nothing, so the verdict is UNKNOWN. Fix is a
#             missing root on the command line, or a freestanding header the compiler lacks.
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - `//` comments, compound literals and designated initialisers. All legal C99, and the
#     positive probe pins `//` as accepted.
#   - a quoted include that resolves next to the including file rather than under a root. It
#     is compiled as part of its includer and never standalone.
#   - anything behind a preprocessor conditional this gate does not define. It compiles with
#     NO -D at all, so the C branch of a `#ifdef __cplusplus` is the only branch read.
#   - a GNU extension. The probe TUs use `__asm volatile`, so -pedantic-errors is deliberately
#     absent and a GNU-only spelling passes as C here as it does in a consumer build.
#   - LINKING. -fsyntax-only proves a C TU parses and type-checks against the header; it
#     proves nothing about a symbol the consumer then has to find.
#   - a C++ construct that is ALSO valid C with different meaning: a cast-expression spelled
#     `(T)x`, or a struct tag reused as a type name.
#   - the arch, chip and board headers of every board but this one; the fleet sweep covers the
#     rest.

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

CC="${1:-}"
[ -n "$CC" ] || fail "usage: check_c_headers.sh <c-compiler> <include-root>..."
shift
command -v "$CC" >/dev/null 2>&1 || fail "not an executable C compiler: $CC"

CFLAGS="-std=c11 -ffreestanding -fsyntax-only"

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can read code apart from prose"

scratch_dir
: > "$TMP/unstrippable"
: > "$TMP/strip.err"

# --- include roots -------------------------------------------------------------------------

git ls-files > "$TMP/tracked" || fail "git ls-files failed"
require_nonempty "$TMP/tracked" "git ls-files matched nothing; every check below would pass vacuously"

ROOTS=""
sed -n 's|^\(.*\)include/kickos/.*|\1include|p' "$TMP/tracked" | sort -u > "$TMP/roots.tree"
require_nonempty "$TMP/roots.tree" "no include root under a tracked kickos/ path; the corpus would resolve nothing"
while IFS= read -r r; do
    case "$r" in
        arch/*|boards/*) continue ;;
    esac
    [ -d "$r" ] || fail "derived include root is not a directory: $r"
    ROOTS="$ROOTS $r"
done < "$TMP/roots.tree"

for r in "$@"; do
    # A root that does not exist turns every include under it into an UNKNOWN.
    [ -d "$r" ] || fail "include root passed on the command line does not exist: $r"
    ROOTS="$ROOTS $r"
done

INCARGS=""
for r in $ROOTS; do
    INCARGS="$INCARGS -I$r"
done

# --- the selector and the compile, as functions, so the self-test runs the SAME code -------

# resolve_header reads the global ROOTS, which the self-test repoints and restores.
resolve_header() { # <relative include path> -> the resolved path, or 1
    for _r in $ROOTS; do
        if [ -f "$_r/$1" ]; then
            printf '%s\n' "$_r/$1"
            return 0
        fi
    done
    return 1
}

# Both readers below judge a file by its CODE, pairing the stripped copy with the raw line.
# The stripper blanks string literals as well, so `extern "C"` and `#include "x.h"` survive it
# only as `extern ` and `#include `: the raw line supplies the literal and the stripped line
# proves the line was code.
STRIPPED="$TMP/stripped"
strip_into() { # <file>; 0 -> $STRIPPED holds it, 1 -> refused and named in $TMP/unstrippable
    if awk -f "$STRIP" "$1" > "$STRIPPED" 2>> "$TMP/strip.err"; then
        return 0
    fi
    _rc=$?
    [ "$_rc" -eq 2 ] || fail "awk exited $_rc stripping $1"
    grep -Fxq "$1" "$TMP/unstrippable" || printf '%s\n' "$1" >> "$TMP/unstrippable"
    return 1
}

is_c_facing() { # <file>; 0 when its code guards an extern "C" block with __cplusplus
    strip_into "$1" || return 1
    grep -q '__cplusplus' "$STRIPPED" || return 1
    awk 'NR == FNR { s[FNR] = $0; next }
         /extern[[:space:]]*"C/ && s[FNR] ~ /extern/ { found = 1 }
         END { exit !found }' "$STRIPPED" "$1"
}

includes_of() { # <file> -> the include targets that are code, quoted and angled alike
    strip_into "$1" || return 0
    awk 'NR == FNR { s[FNR] = $0; next }
         s[FNR] ~ /^[[:space:]]*#[[:space:]]*include/ {
             if (match($0, /[<"][^">]*[">]/)) { print substr($0, RSTART + 1, RLENGTH - 2) }
         }' "$STRIPPED" "$1"
}

seeds_of() { # <header list file> -> the C-facing seeds
    while IFS= read -r _f; do
        [ -f "$_f" ] || fail "header in the scan list is missing from the worktree: $_f"
        if is_c_facing "$_f"; then
            printf '%s\n' "$_f"
        fi
    done < "$1"
}

close_over_includes() { # <seed list file> <workdir> -> <workdir>/corpus and <workdir>/added
    _w="$2"
    mkdir -p "$_w" || fail "mkdir failed under $_w"
    sort -u "$1" > "$_w/corpus"
    cp "$_w/corpus" "$_w/todo"
    : > "$_w/added"
    while [ -s "$_w/todo" ]; do
        : > "$_w/next"
        while IFS= read -r _f; do
            includes_of "$_f" | while IFS= read -r _inc; do
                _p="$(resolve_header "$_inc")" || continue
                if ! grep -Fxq "$_p" "$_w/corpus"; then
                    printf '%s\n' "$_p" >> "$_w/corpus"
                    printf '%s\n' "$_p" >> "$_w/added"
                    printf '%s\n' "$_p" >> "$_w/next"
                fi
              done
        done < "$_w/todo"
        mv "$_w/next" "$_w/todo"
    done
}

# 0 valid C11, 1 a language error, 2 an #include was not found. The TU comes from stdin so a
# quoted include resolves against the repo root, which every corpus path is relative to; a TU
# written into $TMP would resolve them against $TMP.
# $CFLAGS and $INCARGS are unquoted so that they word-split into separate arguments.
compile_as_c() { # <header path> <stderr file>
    # shellcheck disable=SC2086
    if printf '#include "%s"\n' "$1" | "$CC" $CFLAGS $INCARGS -x c - 2>"$2"; then
        return 0
    fi
    if grep -q 'No such file or directory' "$2"; then
        return 2
    fi
    return 1
}

# --- the compiler, proven both ways --------------------------------------------------------

mkdir -p "$TMP/p"
cat > "$TMP/p/ok.h" <<'EOF'
#include <stdint.h>
#include <stdatomic.h>
// A line comment is C99, so it is not a C++ marker.
struct kos_probe
{
    _Atomic uint32_t v;
};
_Static_assert(sizeof(uint32_t) == 4, "the C11 spelling");
static inline uint32_t kos_probe_load(struct kos_probe const* p)
{
    __asm volatile("" ::: "memory");
    return atomic_load_explicit(&p->v, memory_order_relaxed);
}
EOF

if ! compile_as_c "$TMP/p/ok.h" "$TMP/p/ok.err"; then
    sed -n '1,4p' "$TMP/p/ok.err" >&2
    fail "$CC refuses a plain C11 header at $CFLAGS, so every finding below would be its own;
      the corpus needs stdint.h, stdatomic.h, _Static_assert and __asm from this compiler"
fi

# Each of these is valid C++ and invalid C11, one construct per TU so a compiler blind to one
# cannot hide behind the others. `bool`, `alignas`, `static_assert` and `nullptr` are C23
# keywords, so they also pin the -std=c11 above.
: > "$TMP/p/neg.list"
neg() { # <tag> <one line of C++>
    printf '%s\n' "$2" > "$TMP/p/neg_$1.h"
    printf '%s\n' "$1" >> "$TMP/p/neg.list"
}
neg namespace     'namespace kos_probe { }'
neg template      'template <typename T> struct kos_probe_t { T v; };'
neg static_cast   'static inline unsigned kos_probe_c(unsigned v) { return static_cast<unsigned>(v); }'
neg nullptr       'static inline void* kos_probe_n(void) { return nullptr; }'
neg alignas       'struct kos_probe_a { alignas(8) unsigned char b[8]; };'
neg bool          'bool kos_probe_b(void);'
neg static_assert 'static_assert(1, "the C++ spelling");'

while IFS= read -r tag; do
    if compile_as_c "$TMP/p/neg_$tag.h" "$TMP/p/neg.err"; then
        fail "$CC accepts \`$tag\`, which is C++ only, so this gate is blind to it;
      the compiler is in the wrong mode or the -std=c11 in CFLAGS did not take"
    fi
done < "$TMP/p/neg.list"

# --- the selector, proven both ways --------------------------------------------------------

mkdir -p "$TMP/st/inc/kickos"
cat > "$TMP/st/inc/kickos/probe_seed.h" <<'EOF'
#include <stdint.h>
#include <kickos/probe_leaf.h>
#ifdef __cplusplus
extern "C"
{
#endif
uint32_t kos_probe_seed(void);
#ifdef __cplusplus
}
#endif
EOF
cat > "$TMP/st/inc/kickos/probe_leaf.h" <<'EOF'
#include <kickos/probe_deep.h>
struct kos_probe_leaf
{
    unsigned v;
};
EOF
cat > "$TMP/st/inc/kickos/probe_deep.h" <<'EOF'
enum kos_probe_deep
{
    KOS_PROBE_DEEP = 1
};
EOF
cat > "$TMP/st/inc/kickos/probe_cxx.h" <<'EOF'
extern "C"
{
namespace kos_probe_ns
{
}
}
EOF
cat > "$TMP/st/inc/kickos/probe_orphan.h" <<'EOF'
struct kos_probe_orphan
{
    unsigned v;
};
EOF
cat > "$TMP/st/inc/kickos/probe_prose.h" <<'EOF'
// No __cplusplus guard here, and no extern "C" block: this note is the only place
/* either spelling appears, and #include <kickos/probe_orphan.h> is commented out too. */
struct kos_probe_prose
{
    unsigned v;
};
EOF

# resolve_header reads ROOTS and compile_as_c reads INCARGS: both are repointed at the
# synthetic tree and restored. The tree roots stay OFF the path, or a kickos/probe_*.h could
# resolve out of the real tree.
ls "$TMP/st/inc/kickos/"*.h | sort > "$TMP/st/headers"
SAVED_ROOTS="$ROOTS"
SAVED_INCARGS="$INCARGS"
ROOTS="$TMP/st/inc"
INCARGS="-I$TMP/st/inc"
seeds_of "$TMP/st/headers" > "$TMP/st/seeds"
close_over_includes "$TMP/st/seeds" "$TMP/st/w"

# probe_seed carries the guard; probe_cxx has an extern "C" with no guard; probe_leaf and
# probe_deep arrive by include, one and two hops out; probe_orphan is included by nobody;
# probe_prose names both spellings and an #include in COMMENTS only. Compared as names, not a
# count: a count passes while the wrong three files are selected.
SEL="$(sed 's|.*/||' "$TMP/st/w/corpus" | sort | tr '\n' ' ' | sed 's/ $//')"
[ "$SEL" = "probe_deep.h probe_leaf.h probe_seed.h" ] || {
    fail "the selector chose '$SEL', not 'probe_deep.h probe_leaf.h probe_seed.h';
      it must take a guarded extern \"C\" plus its include closure, and leave out an unguarded
      extern \"C\", an unincluded header, and a header whose only claim is in a comment"
}
if [ -s "$TMP/unstrippable" ]; then
    fail "the stripper refused a synthetic probe header: $(tr '\n' ' ' < "$TMP/unstrippable")"
fi

# End to end on a selected header: clean passes, the same header with one C++-only line fails.
compile_as_c "$TMP/st/inc/kickos/probe_seed.h" "$TMP/st/e.err" \
    || fail "the synthetic C-facing header does not compile as C11; the corpus verdicts are its own"
printf 'namespace kos_probe_tail { }\n' >> "$TMP/st/inc/kickos/probe_seed.h"
if compile_as_c "$TMP/st/inc/kickos/probe_seed.h" "$TMP/st/e.err"; then
    fail "a namespace in a selected header passed as C11; this gate would report a C++ header clean"
fi

ROOTS="$SAVED_ROOTS"
INCARGS="$SAVED_INCARGS"

# --- the corpus ----------------------------------------------------------------------------

git ls-files -- '*.h' '*.hh' '*.hpp' > "$TMP/headers" || fail "git ls-files failed"
require_nonempty "$TMP/headers" "git ls-files matched no header; every check below would pass vacuously"
HDRS="$(wc -l < "$TMP/headers" | tr -d ' ')"

seeds_of "$TMP/headers" > "$TMP/seeds"
require_nonempty "$TMP/seeds" "not one tracked header guards an extern \"C\" block with __cplusplus; the selector found no C-facing header at all"
close_over_includes "$TMP/seeds" "$TMP/w"

SEEDS="$(wc -l < "$TMP/seeds" | tr -d ' ')"
ADDED="$(wc -l < "$TMP/w/added" | tr -d ' ')"
N="$(wc -l < "$TMP/w/corpus" | tr -d ' ')"

echo "== $N C-facing header(s) of $HDRS tracked: $SEEDS guard an extern \"C\" block, $ADDED reached by include =="
echo "== compiled standalone with $CC ($("$CC" -dumpversion 2>/dev/null)) at $CFLAGS =="
if [ -s "$TMP/w/added" ]; then
    echo "== in the corpus by include only, not by a guard of their own =="
    sort "$TMP/w/added" | sed 's/^/   /'
fi

: > "$TMP/bad"
: > "$TMP/refused"
sort "$TMP/w/corpus" > "$TMP/corpus.s"
while IFS= read -r f; do
    compile_as_c "$f" "$TMP/c.err"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        continue
    fi
    if [ "$rc" -eq 2 ]; then
        printf '%s\n' "$f" >> "$TMP/refused"
        { printf '%s\n' "$f"; sed -n '1,4p' "$TMP/c.err"; } >> "$TMP/refused.err"
        continue
    fi
    printf '%s\n' "$f" >> "$TMP/bad"
    { printf '%s\n' "$f"; sed -n '1,6p' "$TMP/c.err"; } >> "$TMP/bad.err"
done < "$TMP/corpus.s"

RC=0

# A file the stripper could not finish is UNKNOWN, not clean: it may be a C-facing header this
# run never selected.
if [ -s "$TMP/unstrippable" ]; then
    echo "" >&2
    sed -n '1,6p' "$TMP/strip.err" >&2
    echo "FAIL: $(wc -l < "$TMP/unstrippable" | tr -d ' ') header(s) could not be stripped of comments, so whether they" >&2
    echo "      are C-facing at all is UNKNOWN and this run may have skipped them:" >&2
    sed 's/^/      /' "$TMP/unstrippable" >&2
    RC=1
fi

if [ -s "$TMP/refused" ]; then
    echo "" >&2
    cat "$TMP/refused.err" >&2
    echo "" >&2
    echo "FAIL: an #include could not be found for $(wc -l < "$TMP/refused" | tr -d ' ') header(s), so the" >&2
    echo "      compiler judged nothing and their verdict is UNKNOWN, not clean. Pass the missing" >&2
    echo "      include root on the command line, or supply the freestanding header this" >&2
    echo "      compiler lacks." >&2
    RC=1
fi

if [ -s "$TMP/bad" ]; then
    echo "" >&2
    cat "$TMP/bad.err" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/bad" | tr -d ' ') C-facing header(s) are not valid C11." >&2
    echo "      Each guards an extern \"C\" block with __cplusplus, or is included by one that" >&2
    echo "      does, which states that a consumer's C TU may include it. Either write the" >&2
    echo "      C-valid spelling of what it needs, or drop the guard so the header declares" >&2
    echo "      itself C++ only and leaves this corpus." >&2
    RC=1
fi

[ "$RC" -eq 0 ] || exit 1

echo "PASS: $N C-facing header(s) compile standalone as C11"
