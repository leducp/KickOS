#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every header a consumer can include must compile standalone in the language it claims. Two
# corpora, one rule:
#
#   package  check_public_headers.sh <prefix> <cxx> <std> <cc> <defs-file>
#            every INSTALLED header at the language level the package ADVERTISES, not the one
#            the kernel is built with: the kernel compiles at C++20, the exported targets carry
#            cxx_std_17, so a C++20 construct in a public header compiles under the kernel's own
#            flags and fails only in the consumer's build. Then the C arm below over the
#            installed C-facing headers, which the prefix makes ONE merged include root.
#   tree     check_public_headers.sh --tree <cc> <include-root>...   (from the repo root)
#            the C arm alone over every TRACKED C-facing header, with the board's own C
#            compiler, so a header the package does not ship is held too. The tree's .c files
#            reach only the headers they include, so a break in the C claim any other one makes
#            surfaces in a consumer's tree and nowhere else.
#
# Standalone also means self-contained: a header that compiles only after some other header
# has been included has a wrong include list.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# The C arm's compiler arguments are written one per line to $TMP/cargs by the mode that
# chose them and read back into the positional parameters below, so each reaches the compiler
# as the single word it is. Each mode also leaves its subjects in $TMP/headers, the count of
# them in n, where a subject lives (BASE) and how a TU spells it (KOS_C_FORM).
scratch_dir
ROOTS="$TMP/roots"
: > "$ROOTS"

package_cxx() { # <prefix> <cxx> <std> <cc> <defs-file>
    _usage="usage: check_public_headers.sh <prefix> <cxx> <std> <cc> <defs-file>"
    PREFIX="${1:?$_usage}"
    CXX="${2:?$_usage}"
    STD="${3:?$_usage}"
    CC="${4:?$_usage}"
    DEFS_FILE="${5:?$_usage}"
    INC="$PREFIX/include"
    [ -d "$INC" ] || fail "no include directory in the package at $PREFIX"

    # The definitions THIS package puts on a consumer's compile line, one -D per line, taken by
    # the caller from the built out-of-tree app's compile_commands.json. Never a set written
    # here: a hardcoded -DKICKOS_HAVE_MPU=1 is false on every arch no board enforces on, where
    # arch.h then reaches for an mpu_encoded.h the package correctly does not ship, and 26 of
    # the installed headers report a missing include that no consumer would ever see.
    #
    # ONE LINE OF THE FILE IS ONE ARGUMENT, carried in the POSITIONAL PARAMETERS and never
    # joined into a string the shell re-splits. A define whose value holds a space is a value
    # and not a hazard, and one holding a glob character keeps the value the package gave it
    # instead of the name of whatever file sits in the working directory.
    [ -s "$DEFS_FILE" ] || fail "no package definitions at $DEFS_FILE, so every header below
      would be compiled without the ones a consumer inherits"
    set --
    _n_defs=0
    while IFS= read -r _d; do
        case "$_d" in
            -D*) set -- "$@" "$_d"; _n_defs=$((_n_defs + 1)) ;;
            "")  ;;
            *)   fail "$DEFS_FILE holds a line that is not a -D argument: $_d" ;;
        esac
    done < "$DEFS_FILE"
    [ "$_n_defs" -gt 0 ] || fail "$DEFS_FILE holds no -D argument"

    # The instrument, proven BOTH ways before it is used: one #ifndef per definition, so the
    # probe reddens if any of them fails to arrive and not merely if the set is empty.
    : > "$TMP/defs_probe.h"
    for _d in "$@"; do
        _dn="${_d#-D}"
        _dn="${_dn%%=*}"
        printf '#ifndef %s\n#error "a package definition did not reach the compiler"\n#endif\n' \
            "$_dn" >> "$TMP/defs_probe.h"
    done
    "$CXX" -std="$STD" -fsyntax-only "$@" -x c++ "$TMP/defs_probe.h" 2>"$TMP/defs.err" || {
        sed -n '1,4p' "$TMP/defs.err" >&2
        fail "$_n_defs package definition(s) were passed and at least one did not reach the
      compiler, so every verdict below would be taken under the wrong configuration"
    }
    if "$CXX" -std="$STD" -fsyntax-only -x c++ "$TMP/defs_probe.h" 2>/dev/null; then
        fail "the same probe compiles with NO definition passed, so it is blind to a dropped
      -D and proves nothing about the ones above"
    fi
    echo "== $_n_defs package definition(s) from $DEFS_FILE reach the compiler =="

    # THE ENUMERATION IS READ A LINE AT A TIME, never `for h in $(find ...)`: a name holding a
    # space or a glob character would be re-split or rewritten there, and the header the name
    # belongs to would go uncompiled while the pieces compiled clean. A name holding a NEWLINE
    # is the one shape a line-delimited list cannot carry, so it is counted against the number
    # of files found and refused rather than read as two headers.
    ( cd "$INC" && find kickos -name '*.h' ) | sort > "$TMP/headers"
    _nlines=$(wc -l < "$TMP/headers" | tr -d ' ')
    _nfiles=$( (cd "$INC" && find kickos -name '*.h' -exec printf 'x\n' ';') | wc -l | tr -d ' ')
    [ "$_nlines" -eq "$_nfiles" ] || fail "$_nfiles installed header(s) came back as $_nlines
      line(s), so a name holds a newline and the header it names would go unchecked while its
      pieces report"

    n=0
    _bad=0
    while IFS= read -r h; do
        n=$((n + 1))
        if ! printf '#include <%s>\n' "$h" | "$CXX" -std="$STD" -fsyntax-only "$@" \
             -I"$INC" -x c++ - 2>"$TMP/hdr.err"; then
            _bad=$((_bad + 1))
            echo "FAIL $h"
            head -4 "$TMP/hdr.err"
        fi
    done < "$TMP/headers"
    [ "$n" -gt 0 ] || fail "no headers found under $INC, so this gate proved nothing"
    [ "$_bad" -eq 0 ] || fail "$_bad of $n installed header(s) do not compile at $STD"
    echo "PASS: $n installed headers compile standalone at $STD"

    # A subject is a spelling under the prefix, compiled the way a consumer reaches it.
    printf '%s\n' "$@" "-I$INC" > "$TMP/cargs"
    printf '%s\n' "$INC" > "$ROOTS"
    BASE="$INC/"
    KOS_C_FORM=angled
}

tree_setup() { # <cc> <include-root>...
    require_repo_root
    CC="${1:-}"
    [ -n "$CC" ] || fail "usage: check_public_headers.sh --tree <c-compiler> <include-root>..."
    shift

    # Every in-tree include root derived from the tracked paths but the arch/ and boards/ ones:
    # six directories provide kickos/arch/context.h, so putting them all on one -I line
    # resolves to whichever came first. The caller passes the one this board builds, with the
    # generated directory and the chip and board ones.
    corpus_all "$TMP/tracked"
    sed -n 's|^\(.*\)include/kickos/.*|\1include|p' "$TMP/tracked" | sort -u > "$TMP/roots.tree"
    require_nonempty "$TMP/roots.tree" "no include root under a tracked kickos/ path; the corpus would resolve nothing"
    while IFS= read -r _r; do
        case "$_r" in
            arch/*|boards/*) continue ;;
        esac
        [ -d "$_r" ] || fail "derived include root is not a directory: $_r"
        printf '%s\n' "$_r" >> "$ROOTS"
    done < "$TMP/roots.tree"
    for _r in "$@"; do
        [ -d "$_r" ] || fail "include root passed on the command line does not exist: $_r"
        case "$_r" in
            *"$NL"*) fail "include root holds a newline, so a line-delimited list reads it as
      two roots and resolves against neither: $_r" ;;
        esac
        printf '%s\n' "$_r" >> "$ROOTS"
    done
    sed 's/^/-I/' "$ROOTS" > "$TMP/cargs"

    # A subject is a tracked path, compiled with NO -D at all, so the C branch of a
    # `#ifdef __cplusplus` is the one branch read and any other conditional stays unread.
    corpus_headers "$TMP/headers"
    n="$(wc -l < "$TMP/headers" | tr -d ' ')"
    BASE=""
    KOS_C_FORM=quoted
}

NL='
'
if [ "${1:-}" = --tree ]; then
    shift
    tree_setup "$@"
else
    package_cxx "$@"
fi
command -v "$CC" >/dev/null 2>&1 || fail "not an executable C compiler: $CC"
set --
while IFS= read -r _a; do
    set -- "$@" "$_a"
done < "$TMP/cargs"

# ===========================================================================
# The C arm.
#
# A header guarding `extern "C"` with __cplusplus tells a consumer their C translation unit
# may include it, so it has to compile as C. An UNGUARDED `extern "C"` is a C syntax error, so
# a C++-only header says so by leaving the guard off.
#
#   corpus  DERIVED, never listed: a header whose CODE both names __cplusplus and carries an
#           `extern "C"`, plus every header such a header includes, transitively. The closure
#           takes what the roots resolve, tracked or not, so a generated header is in it.
#
#   comments tests/lib/strip_comments.awk blanks them before the claim is read, and the raw
#           line supplies the string literal the stripper takes with them. A file whose
#           comment or literal is still open at EOF is REFUSED by name: whether it is C-facing
#           was then read off a partial file.
#
#   std     -std=c11, never the compiler's default. gcc 15 defaults to gnu23, and C23
#           adopted `bool`, `alignas`, `static_assert` and `nullptr` as keywords, so a
#           default-std run accepts four C++-only spellings and reports them clean.
#
#   pedantic a SECOND pass over the same corpus at -pedantic-errors. The first mirrors a
#           real consumer build, extensions on; this one fails a GNU-only construct before
#           a consumer compiling strictly conforming C11 does.
#
#   refusal a header whose own #include cannot be found is REFUSED by name, not reported as
#           invalid C: the compiler judged nothing, so the verdict is UNKNOWN.
#
# SCOPE. What both passes read is whether a TU including the header PARSES: the selector
# tests that __cplusplus and `extern "C"` co-occur in code rather than that the one guards
# the other, a preprocessor conditional the -D's do not select stays unparsed, a C++ construct
# that is ALSO valid C with another meaning (a cast `(T)x`, a struct tag used as a type) reads
# clean, and -fsyntax-only says nothing about LINKING. In tree mode the arch, chip and board
# headers are the ones THIS board builds; the fleet sweep covers the other boards.
# ===========================================================================

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"

# The probe headers written under $TMP/p are reached by an include like any corpus header, so
# a control compiles through the same path a corpus header does.
KOS_C_CC="$CC"
set -- "$@" "-I$TMP/p"
. "$(dirname "$0")/../lib/c_probe.sh"

# --- the compiler and the pin, proven both ways ----------------------------
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
if ! compile_as_c ok.h "$TMP/p/ok.err" "$@"; then
    sed -n '1,4p' "$TMP/p/ok.err" >&2
    fail "$CC refuses a plain C11 header at $KOS_C_FLAGS, so every finding below would be its own;
      the corpus needs stdint.h, stdatomic.h, _Static_assert and __asm from this compiler"
fi

# Each is valid C++ and invalid C11, one construct per probe so a compiler blind to one
# cannot hide behind the others. The last four are C23 keywords, so they also pin -std=c11.
probe_neg() { # <tag> <one line of C++> <compiler argument>...
    _tag="$1"
    printf '%s\n' "$2" > "$TMP/p/neg.h"
    shift 2
    if compile_as_c neg.h "$TMP/p/neg.err" "$@"; then
        fail "$CC accepts \`$_tag\`, which is C++ only, so this arm is blind to it: the compiler is
      in the wrong mode or the -std=$KOS_C_STD in KOS_C_FLAGS did not take"
    fi
}
probe_neg namespace     'namespace kos_probe { }' "$@"
probe_neg template      'template <typename T> struct kos_probe_t { T v; };' "$@"
probe_neg static_cast   'static inline unsigned f(unsigned v) { return static_cast<unsigned>(v); }' "$@"
probe_neg nullptr       'static inline void* f(void) { return nullptr; }' "$@"
probe_neg bool          'bool kos_probe_b(void);' "$@"
probe_neg static_assert 'static_assert(1, "the C++ spelling");' "$@"
probe_neg alignas       'struct s { alignas(8) unsigned char b[8]; };' "$@"

# Strictly conforming C11 and nothing else: a red at KOS_C_PEDFLAGS must never be the probe's own.
cat > "$TMP/p/ped_ok.h" <<'EOF'
#include <stdint.h>
_Static_assert(sizeof(uint32_t) == 4, "the C11 spelling");
EOF
compile_pedantic_c ped_ok.h "$TMP/p/ped_ok.err" "$@" || {
    sed -n '1,4p' "$TMP/p/ped_ok.err" >&2
    fail "$CC refuses strictly conforming C11 at $KOS_C_PEDFLAGS, so every pedantic finding below
      would be its own"
}

# Each is a GNU extension the first pass accepts, so the pedantic pass is the one that has to
# reject it.
probe_ped() { # <tag> <one line of GNU-extension C> <compiler argument>...
    _tag="$1"
    printf '%s\n' "$2" > "$TMP/p/ped.h"
    shift 2
    if ! compile_as_c ped.h "$TMP/p/ped.err" "$@"; then
        sed -n '1,4p' "$TMP/p/ped.err" >&2
        fail "the first pass rejects \`$_tag\`, so it is no longer the extension-tolerant pass the
      pedantic one is meant to sit beside"
    fi
    if compile_pedantic_c ped.h "$TMP/p/ped.err" "$@"; then
        fail "$CC accepts \`$_tag\` at $KOS_C_PEDFLAGS, which is a GNU extension and not ISO C11, so the
      pedantic pass is blind to it and -pedantic-errors did not take"
    fi
}
probe_ped fixed_enum 'enum kos_probe_fe : unsigned { KOS_PROBE_FE = 0 };' "$@"
probe_ped zero_array 'struct kos_probe_za { unsigned n; int v[0]; };' "$@"

kos_c_prove_missing_include "$TMP/p" "$@"

# --- the selector and the closure -------------------------------------------
# A subject names the file "$BASE$subject". Both readers judge a file by its CODE, pairing the
# stripped copy with the raw line: the stripper blanks string literals as well, so `extern "C"`
# and `#include "x.h"` survive it only as `extern ` and `#include `, the raw line supplies the
# literal and the stripped line proves the line was code.
: > "$TMP/strip.err"
: > "$TMP/unstrippable"
strip_into() { # <subject>; 0 -> $TMP/stripped holds it, 1 -> refused and named in $TMP/unstrippable
    if LC_ALL=C awk -f "$STRIP" "$BASE$1" > "$TMP/stripped" 2>> "$TMP/strip.err"; then
        return 0
    fi
    _rc=$?
    [ "$_rc" -eq 2 ] || fail "awk exited $_rc stripping $BASE$1"
    grep -Fxq "$1" "$TMP/unstrippable" || printf '%s\n' "$1" >> "$TMP/unstrippable"
    return 1
}

c_facing() { # <subject>; 0 when its code names __cplusplus and has an extern "C"
    strip_into "$1" || return 1
    grep -q '__cplusplus' "$TMP/stripped" || return 1
    awk 'NR == FNR { s[FNR] = $0; next }
         /extern[[:space:]]*"C/ && s[FNR] ~ /extern/ { found = 1 }
         END { exit !found }' "$TMP/stripped" "$BASE$1"
}

# Its include targets that are code, as "<a|q><TAB><target>", angled or quoted.
includes_of() { # <subject>
    strip_into "$1" || return 0
    awk 'NR == FNR { s[FNR] = $0; next }
         s[FNR] ~ /^[[:space:]]*#[[:space:]]*include/ {
             if (match($0, /<[^>]*>/)) { print "a\t" substr($0, RSTART + 1, RLENGTH - 2) }
             else if (match($0, /"[^"]*"/)) { print "q\t" substr($0, RSTART + 1, RLENGTH - 2) }
         }' "$TMP/stripped" "$BASE$1"
}

# The subject an include names, as the compiler resolves it: a quoted one against the
# DIRECTORY OF THE INCLUDING FILE first (C11 6.10.2p3), then both against the roots in order.
# 1 for a freestanding or libc header, which is not ours to judge.
resolve_inc() { # <includer subject> <a|q> <target>
    if [ "$2" = q ]; then
        case "$1" in
            */*) _ri="${1%/*}/$3" ;;
            *)   _ri="$3" ;;
        esac
        if [ -f "$BASE$_ri" ]; then
            printf '%s\n' "$_ri"
            return 0
        fi
    fi
    while IFS= read -r _r; do
        if [ -f "$_r/$3" ]; then
            _ri="$_r/$3"
            printf '%s\n' "${_ri#"$BASE"}"
            return 0
        fi
    done < "$ROOTS"
    return 1
}

close_over_includes() { # <seed list> <workdir> -> <workdir>/corpus and <workdir>/added
    _w="$2"
    mkdir -p "$_w" || fail "mkdir failed under $_w"
    sort -u "$1" > "$_w/corpus"
    cp "$_w/corpus" "$_w/todo"
    : > "$_w/added"
    while [ -s "$_w/todo" ]; do
        : > "$_w/next"
        while IFS= read -r _f; do
            includes_of "$_f" | while IFS="$TAB" read -r _k _inc; do
                _p="$(resolve_inc "$_f" "$_k" "$_inc")" || continue
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

seeds_of() { # <subject list> -> the C-facing seeds
    while IFS= read -r _f; do
        [ -f "$BASE$_f" ] || fail "header in the scan list is missing: $BASE$_f"
        if c_facing "$_f"; then
            printf '%s\n' "$_f"
        fi
    done < "$1"
}

# --- control: the selector and the closure over a synthetic tree, then one end to end -------
# probe_leaf arrives by a quoted include beside its includer and probe_deep by an angled one,
# two hops out; probe_orphan is included by nobody, probe_cxx guards nothing and probe_prose
# names both spellings in comments only. Compared as names, not a count: a count passes while
# the wrong three files are selected.
mkdir -p "$TMP/st/inc/kickos/sub"
cat > "$TMP/st/inc/kickos/sub/probe_seed.h" <<'EOF'
#include <stdint.h>
#include "probe_leaf.h"
#ifdef __cplusplus
extern "C"
{
#endif
uint32_t kos_probe_seed(void);
#ifdef __cplusplus
}
#endif
EOF
printf '#include <kickos/probe_deep.h>\nstruct kos_probe_leaf { unsigned v; };\n' \
    > "$TMP/st/inc/kickos/sub/probe_leaf.h"
printf 'enum kos_probe_deep { KOS_PROBE_DEEP = 1 };\n' > "$TMP/st/inc/kickos/probe_deep.h"
printf 'extern "C"\n{\nnamespace kos_probe_ns\n{\n}\n}\n' > "$TMP/st/inc/kickos/probe_cxx.h"
printf 'struct kos_probe_orphan { unsigned v; };\n' > "$TMP/st/inc/kickos/probe_orphan.h"
cat > "$TMP/st/inc/kickos/probe_prose.h" <<'EOF'
// No __cplusplus guard here, and no extern "C" block: this note is the only place
/* either spelling appears, and #include <kickos/probe_orphan.h> is commented out too. */
struct kos_probe_prose { unsigned v; };
EOF
_saved_base="$BASE"
_saved_roots="$ROOTS"
BASE="$TMP/st/inc/"
ROOTS="$TMP/st/roots"
printf '%s\n' "$TMP/st/inc" > "$ROOTS"
( cd "$TMP/st/inc" && find kickos -name '*.h' ) | sort > "$TMP/st/headers"
seeds_of "$TMP/st/headers" > "$TMP/st/seeds"
close_over_includes "$TMP/st/seeds" "$TMP/st/w"
SEL="$(sort "$TMP/st/w/corpus" | tr '\n' ' ' | sed 's/ $//')"
[ "$SEL" = "kickos/probe_deep.h kickos/sub/probe_leaf.h kickos/sub/probe_seed.h" ] \
    || fail "the selector chose '$SEL'; it must take a guarded extern \"C\" plus its include
      closure, a quoted include resolved beside its includer and an angled one through the
      roots, and leave out an unguarded extern \"C\", an unincluded header and a claim made
      in a comment"
[ -s "$TMP/unstrippable" ] && fail "the stripper refused a synthetic probe header: $(tr '\n' ' ' < "$TMP/unstrippable")"
if [ "$KOS_C_FORM" = angled ]; then
    _seed=kickos/sub/probe_seed.h
else
    _seed="$TMP/st/inc/kickos/sub/probe_seed.h"
fi
compile_as_c "$_seed" "$TMP/st/e.err" "-I$TMP/st/inc" \
    || fail "the synthetic C-facing header does not compile as C11; the corpus verdicts are its own"
printf 'namespace kos_probe_tail { }\n' >> "$TMP/st/inc/kickos/sub/probe_seed.h"
if compile_as_c "$_seed" "$TMP/st/e.err" "-I$TMP/st/inc"; then
    fail "a namespace in a selected header passed as C11; this arm would report a C++ header clean"
fi
BASE="$_saved_base"
ROOTS="$_saved_roots"

# --- the corpus -------------------------------------------------------------
seeds_of "$TMP/headers" > "$TMP/seeds"
require_nonempty "$TMP/seeds" "not one header guards an extern \"C\" block with __cplusplus, so
      the C corpus is empty and every check below would pass vacuously"
close_over_includes "$TMP/seeds" "$TMP/w"

CSEEDS=$(wc -l < "$TMP/seeds" | tr -d ' ')
CADDED=$(wc -l < "$TMP/w/added" | tr -d ' ')
CN=$(wc -l < "$TMP/w/corpus" | tr -d ' ')
echo "== $CN C-facing header(s) of $n: $CSEEDS guard an extern \"C\" block, $CADDED reached by include =="
echo "== compiled standalone with $CC ($("$CC" -dumpversion 2>/dev/null)) at $KOS_C_FLAGS =="
if [ -s "$TMP/w/added" ]; then
    echo "== in the C corpus by include only, not by a guard of their own =="
    sort "$TMP/w/added" | sed 's/^/   /'
fi

subject_arg() { # <subject> -> what the TU includes
    if [ "$KOS_C_FORM" = angled ]; then
        printf '%s\n' "$1"
    else
        printf '%s\n' "$BASE$1"
    fi
}

: > "$TMP/cbad"
: > "$TMP/crefused"
: > "$TMP/cbad.err"
: > "$TMP/crefused.err"
: > "$TMP/pedbad"
: > "$TMP/pedbad.err"
sort "$TMP/w/corpus" > "$TMP/ccorpus.s"
while IFS= read -r h; do
    _s="$(subject_arg "$h")"
    compile_as_c "$_s" "$TMP/c.err" "$@"
    case $? in
        0) ;;
        2) printf '%s\n' "$h" >> "$TMP/crefused"
           { printf '%s\n' "$h"; sed -n '1,4p' "$TMP/c.err"; } >> "$TMP/crefused.err"
           continue ;;
        *) printf '%s\n' "$h" >> "$TMP/cbad"
           { printf '%s\n' "$h"; sed -n '1,6p' "$TMP/c.err"; } >> "$TMP/cbad.err"
           continue ;;
    esac
    if ! compile_pedantic_c "$_s" "$TMP/ped.err" "$@"; then
        printf '%s\n' "$h" >> "$TMP/pedbad"
        { printf '%s\n' "$h"; sed -n '1,6p' "$TMP/ped.err"; } >> "$TMP/pedbad.err"
    fi
done < "$TMP/ccorpus.s"

rc=0
# A file the stripper could not finish is UNKNOWN, not clean: it may be a C-facing header this
# run never selected.
if [ -s "$TMP/unstrippable" ]; then
    sed -n '1,6p' "$TMP/strip.err" >&2
    sed 's/^/      /' "$TMP/unstrippable" >&2
    bad "the comment stripper could not finish the header(s) above, so whether they are
      C-facing is UNKNOWN and this run may have skipped them"
fi
if [ -s "$TMP/crefused" ]; then
    cat "$TMP/crefused.err" >&2
    bad "an #include could not be found for $(wc -l < "$TMP/crefused" | tr -d ' ') C-facing
      header(s), so the compiler judged nothing and their verdict is UNKNOWN, not clean. Pass
      the missing include root, or supply the freestanding header this compiler lacks"
fi
if [ -s "$TMP/cbad" ]; then
    cat "$TMP/cbad.err" >&2
    bad "$(wc -l < "$TMP/cbad" | tr -d ' ') C-facing header(s) are not valid C11. Each guards
      an extern \"C\" block with __cplusplus, or is included by one that does, which tells a
      consumer their C translation unit may include it. Either write the C-valid spelling of
      what it needs, or DROP the guard so the header declares itself C++ only and leaves this
      corpus."
fi
if [ -s "$TMP/pedbad" ]; then
    cat "$TMP/pedbad.err" >&2
    bad "$(wc -l < "$TMP/pedbad" | tr -d ' ') C-facing header(s) compile as C11 only with GNU
      extensions on. A consumer building strictly conforming C11 gets a hard error out of a
      shipped header, so write the ISO C11 spelling of what it needs."
fi
[ "$rc" -eq 0 ] || exit 1

echo "PASS: $CN C-facing header(s) compile standalone at $KOS_C_FLAGS, and at $KOS_C_PEDFLAGS"
