#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every INSTALLED header must compile standalone at the language level the package
# ADVERTISES, not the one the kernel is built with: the kernel compiles at C++20, the
# exported targets carry cxx_std_17. A C++20 construct in a public header compiles under
# the kernel's own flags and fails only in the consumer's build.
#
# Usage: check_public_headers.sh <install-prefix> <c++-compiler> <std> <c-compiler>
#
# Standalone also means self-contained: a header that compiles only after some other header
# has been included has a wrong include list.
#
# The C arm at the bottom: a header guarding `extern "C"` with __cplusplus tells a consumer
# their C translation unit may include it, so it has to compile as C. The C compiler is a
# REQUIRED argument, never defaulted.

set -u
. "$(dirname "$0")/../lib/gate.sh"

PREFIX="${1:?usage: check_public_headers.sh <prefix> <cxx> <std> <cc>}"
CXX="${2:?}"
STD="${3:?}"
CC="${4:?usage: check_public_headers.sh <prefix> <cxx> <std> <cc>}"
command -v "$CC" >/dev/null 2>&1 || fail "not an executable C compiler: $CC"

scratch_dir

INC="$PREFIX/include"
[ -d "$INC" ] || fail "no include directory in the package at $PREFIX"

# The usage requirements a consumer inherits from the target, minus the generator
# expressions CMake resolves per configuration. An ARRAY, not a joined string: joined, the
# whole thing arrives as one unrecognised argument and every define is dropped.
DEFS=(-Dmain=kickos_app_main -D__KickOS__=1
      -DKICKOS_TELEMETRY=0 -DKICKOS_TELEMETRY_RTT=0 -DKICKOS_TRACE_ARCH=0
      -DKICKOS_HAVE_MPU=1 -DKICKOS_DEBUG=0)
# A sim package ships no chip_limits.h, and config/board.h refuses to guess an IRQ count
# without one. The real consumer gets this from the target's INTERFACE definitions.
if [ ! -f "$INC/kickos/chip_limits.h" ]; then
    DEFS+=(-DKICKOS_ARCH_SIM=1)
fi

n=0
bad=0
for h in $(cd "$INC" && find kickos -name '*.h' | sort); do
    n=$((n + 1))
    if ! echo "#include <$h>" | "$CXX" -std="$STD" -fsyntax-only "${DEFS[@]}" \
         -I"$INC" -x c++ - 2>"$TMP/hdr.err"; then
        bad=$((bad + 1))
        echo "FAIL $h"
        head -4 "$TMP/hdr.err"
    fi
done

[ "$n" -gt 0 ] || fail "no headers found under $INC, so this gate proved nothing"
[ "$bad" -eq 0 ] || fail "$bad of $n installed header(s) do not compile at $STD"
echo "PASS: $n installed headers compile standalone at $STD"

# ===========================================================================
# The C arm.
#
#   corpus  DERIVED, never listed: an installed header whose CODE both names __cplusplus
#           and carries an `extern "C"`, plus every installed header such a header
#           includes, transitively.
#           The installed prefix is ONE merged include root, so this needs none of the
#           per-arch root disambiguation the same rule needs against the source tree (six
#           directories there provide kickos/arch/context.h).
#
#   std     -std=c11, never the compiler's default. gcc 15 defaults to gnu23, and C23
#           adopted `bool`, `alignas`, `static_assert` and `nullptr` as keywords, so a
#           default-std run accepts four C++-only spellings and reports them clean.
#
#   pedantic a SECOND pass over the same corpus at -pedantic-errors. The first is not
#           pedantic: it mirrors a real consumer build, extensions on. This one is what
#           fails a GNU-only construct before a consumer compiling strictly conforming C11
#           does. Its TU carries a trailing typedef, because -pedantic-errors forbids the
#           empty translation unit a macros-only header would otherwise produce.
#
# NOT CAUGHT: anything behind a preprocessor conditional these -D's do not select; LINKING,
# since -fsyntax-only proves only that a C TU parses against the header. Nor does the selector
# check that the __cplusplus it found is what GUARDS the `extern "C"`; co-occurrence is what
# it tests.
# ===========================================================================

CSTD=c11
STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"

# $CFLAGS is deliberately unquoted at the call sites so it word-splits.
CFLAGS="-std=$CSTD -ffreestanding -fsyntax-only"
PEDFLAGS="$CFLAGS -pedantic-errors"

compile_as_c() { # <include-spelling> <stderr file>; 0 ok, 1 not valid C, 2 an include was missing
  # The TU comes from stdin so nothing resolves relative to a scratch directory.
  if echo "#include <$1>" | "$CC" $CFLAGS "${DEFS[@]}" -I"$INC" -I"$TMP/p" -x c - 2>"$2"; then
    return 0
  fi
  grep -q 'No such file or directory' "$2" && return 2
  return 1
}

# The trailing typedef keeps the TU non-empty: -pedantic-errors forbids an empty translation
# unit, which is what a macros-only header would otherwise produce.
compile_pedantic_c() { # <include-spelling> <stderr file>
  printf '#include <%s>\ntypedef int kos_gate_pedantic_tu;\n' "$1" \
    | "$CC" $PEDFLAGS "${DEFS[@]}" -I"$INC" -I"$TMP/p" -x c - 2>"$2"
}

# --- the compiler and the pin, proven both ways ----------------------------
mkdir -p "$TMP/p"
cat > "$TMP/p/ok.h" <<'EOF'
#include <stdint.h>
// A line comment is C99, so it is not a C++ marker.
_Static_assert(sizeof(uint32_t) == 4, "the C11 spelling");
static inline uint32_t kos_probe_id(uint32_t v) { return (uint32_t)v; }
EOF
if ! compile_as_c ok.h "$TMP/p/ok.err"; then
  sed -n '1,4p' "$TMP/p/ok.err" >&2
  fail "$CC refuses a plain C11 header at $CFLAGS, so every finding below would be its own"
fi

# Each is valid C++ and invalid C11, one construct per probe so a compiler blind to one
# cannot hide behind the others. The last four are C23 keywords, so they also pin -std=c11.
probe_neg() { # <tag> <one line of C++>
  printf '%s\n' "$2" > "$TMP/p/neg.h"
  if compile_as_c neg.h "$TMP/p/neg.err"; then
    fail "$CC accepts \`$1\`, which is C++ only, so this arm is blind to it: the compiler is
      in the wrong mode or the -std=$CSTD in CFLAGS did not take"
  fi
}
probe_neg namespace     'namespace kos_probe { }'
probe_neg static_cast   'static inline unsigned f(unsigned v) { return static_cast<unsigned>(v); }'
probe_neg nullptr       'static inline void* f(void) { return nullptr; }'
probe_neg bool          'bool kos_probe_b(void);'
probe_neg static_assert 'static_assert(1, "the C++ spelling");'
probe_neg alignas       'struct s { alignas(8) unsigned char b[8]; };'

# Strictly conforming C11 and nothing else: a red at PEDFLAGS must never be the probe's own.
cat > "$TMP/p/ped_ok.h" <<'EOF'
#include <stdint.h>
_Static_assert(sizeof(uint32_t) == 4, "the C11 spelling");
EOF
compile_pedantic_c ped_ok.h "$TMP/p/ped_ok.err" || {
  sed -n '1,4p' "$TMP/p/ped_ok.err" >&2
  fail "$CC refuses strictly conforming C11 at $PEDFLAGS, so every pedantic finding below
      would be its own"
}

# Each is a GNU extension the first pass accepts, so the pedantic pass is the one that has to
# reject it.
probe_ped() { # <tag> <one line of GNU-extension C>
  printf '%s\n' "$2" > "$TMP/p/ped.h"
  if ! compile_as_c ped.h "$TMP/p/ped.err"; then
    sed -n '1,4p' "$TMP/p/ped.err" >&2
    fail "the first pass rejects \`$1\`, so it is no longer the extension-tolerant pass the
      pedantic one is meant to sit beside"
  fi
  if compile_pedantic_c ped.h "$TMP/p/ped.err"; then
    fail "$CC accepts \`$1\` at $PEDFLAGS, which is a GNU extension and not ISO C11, so the
      pedantic pass is blind to it and -pedantic-errors did not take"
  fi
}
probe_ped fixed_enum 'enum kos_probe_fe : unsigned { KOS_PROBE_FE = 0 };'
probe_ped zero_array 'struct kos_probe_za { unsigned n; int v[0]; };'

# --- the selector -----------------------------------------------------------
# The stripper blanks string literals as well as comments, so `extern "C"` survives it only
# as `extern `: the STRIPPED line proves the text was code, the RAW line supplies the
# literal. Not optional: a header naming both spellings in prose is a hit to a plain grep.
c_facing() { # <path>; 0 names __cplusplus and has a non-comment extern "C", 1 not, 2 refused
  LC_ALL=C awk -f "$STRIP" "$1" > "$TMP/stripped" 2>>"$TMP/strip.err" || return 2
  grep -q '__cplusplus' "$TMP/stripped" || return 1
  awk 'NR == FNR { s[FNR] = $0; next }
       /extern[[:space:]]*"C/ && s[FNR] ~ /extern/ { found = 1 }
       END { exit !found }' "$TMP/stripped" "$1"
}

includes_of() { # <path> -> its angled include targets that are CODE
  LC_ALL=C awk -f "$STRIP" "$1" 2>>"$TMP/strip.err" \
    | sed -n 's|^[[:space:]]*#[[:space:]]*include[[:space:]]*<\([^>]*\)>.*|\1|p'
}

: > "$TMP/strip.err"
: > "$TMP/unstrippable"

# --- corpus: the guarded seeds, then the include closure -------------------
: > "$TMP/seeds"
for h in $(cd "$INC" && find kickos -name '*.h' | sort); do
  c_facing "$INC/$h"
  case $? in
    0) printf '%s\n' "$h" >> "$TMP/seeds" ;;
    2) printf '%s\n' "$h" >> "$TMP/unstrippable" ;;
  esac
done
# A file the stripper could not finish is UNKNOWN, not clean: it may be a C-facing header
# this run never selected.
if [ -s "$TMP/unstrippable" ]; then
  sed -n '1,6p' "$TMP/strip.err" >&2
  sed 's/^/      /' "$TMP/unstrippable" >&2
  fail "the comment stripper could not finish the header(s) above, so whether they are
      C-facing is UNKNOWN and this run may have skipped them"
fi
[ -s "$TMP/seeds" ] || fail "not one installed header guards an extern \"C\" block with
      __cplusplus, so the C corpus is empty and every check below would pass vacuously"

sort -u "$TMP/seeds" > "$TMP/ccorpus"
cp "$TMP/ccorpus" "$TMP/todo"
: > "$TMP/added"
while [ -s "$TMP/todo" ]; do
  : > "$TMP/next"
  while IFS= read -r h; do
    includes_of "$INC/$h" | while IFS= read -r inc; do
      [ -f "$INC/$inc" ] || continue          # a freestanding/libc header, not ours to judge
      if ! grep -Fxq "$inc" "$TMP/ccorpus"; then
        printf '%s\n' "$inc" >> "$TMP/ccorpus"
        printf '%s\n' "$inc" >> "$TMP/added"
        printf '%s\n' "$inc" >> "$TMP/next"
      fi
    done
  done < "$TMP/todo"
  mv "$TMP/next" "$TMP/todo"
done

CSEEDS=$(wc -l < "$TMP/seeds" | tr -d ' ')
CADDED=$(wc -l < "$TMP/added" | tr -d ' ')
CN=$(wc -l < "$TMP/ccorpus" | tr -d ' ')
echo "== $CN C-facing installed header(s) of $n: $CSEEDS guard an extern \"C\" block, $CADDED reached by include =="
echo "== compiled standalone with $CC ($("$CC" -dumpversion 2>/dev/null)) at $CFLAGS =="
if [ -s "$TMP/added" ]; then
  echo "== in the C corpus by include only, not by a guard of their own =="
  sort "$TMP/added" | sed 's/^/   /'
fi

: > "$TMP/cbad"
: > "$TMP/crefused"
: > "$TMP/cbad.err"
: > "$TMP/crefused.err"
sort "$TMP/ccorpus" > "$TMP/ccorpus.s"
while IFS= read -r h; do
  compile_as_c "$h" "$TMP/c.err"
  case $? in
    0) ;;
    2) printf '%s\n' "$h" >> "$TMP/crefused"
       { printf '%s\n' "$h"; sed -n '1,4p' "$TMP/c.err"; } >> "$TMP/crefused.err" ;;
    *) printf '%s\n' "$h" >> "$TMP/cbad"
       { printf '%s\n' "$h"; sed -n '1,6p' "$TMP/c.err"; } >> "$TMP/cbad.err" ;;
  esac
done < "$TMP/ccorpus.s"

if [ -s "$TMP/crefused" ]; then
  cat "$TMP/crefused.err" >&2
  fail "an #include could not be found for $(wc -l < "$TMP/crefused" | tr -d ' ') C-facing
      header(s), so the compiler judged nothing and their verdict is UNKNOWN, not clean"
fi
if [ -s "$TMP/cbad" ]; then
  cat "$TMP/cbad.err" >&2
  echo "" >&2
  fail "$(wc -l < "$TMP/cbad" | tr -d ' ') C-facing installed header(s) are not valid C11.
      Each guards an extern \"C\" block with __cplusplus, or is included by one that does,
      which tells a consumer their C translation unit may include it. Either write the
      C-valid spelling of what it needs, or DROP the guard so the header declares itself
      C++ only and leaves this corpus."
fi

: > "$TMP/pedbad"
: > "$TMP/pedbad.err"
while IFS= read -r h; do
  if ! compile_pedantic_c "$h" "$TMP/ped.err"; then
    printf '%s\n' "$h" >> "$TMP/pedbad"
    { printf '%s\n' "$h"; sed -n '1,6p' "$TMP/ped.err"; } >> "$TMP/pedbad.err"
  fi
done < "$TMP/ccorpus.s"

if [ -s "$TMP/pedbad" ]; then
  cat "$TMP/pedbad.err" >&2
  echo "" >&2
  fail "$(wc -l < "$TMP/pedbad" | tr -d ' ') C-facing installed header(s) compile as C11 only
      with GNU extensions on. A consumer building strictly conforming C11 gets a hard error
      out of a shipped header, so write the ISO C11 spelling of what it needs."
fi

echo "PASS: $CN C-facing installed header(s) compile standalone at $CFLAGS, and at $PEDFLAGS"
