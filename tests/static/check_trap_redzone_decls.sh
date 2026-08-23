#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the COMPLETENESS of tests/static/trap_redzone_roots.txt: every arch that has a
# trap-stack header is declared, every class that header prices is declared, and every
# configure preset of a declared arch is registered.
#
# WHY THIS IS A GATE OF ITS OWN. check_trap_redzone.sh measures ONE preset per run and takes
# its class set from whatever the file declares for that preset's arch, so a DELETION is
# invisible to it in every direction: drop the rv32imac EXIT class and the run measures six
# classes instead of seven and passes; drop a `preset` record and CMakeLists.txt registers no
# trap_redzone test for that board at all, and a fleet sweep reports one fewer green test
# rather than a failure. The completeness of the declaration set is a property of the
# DECLARATIONS against the tree, which no single-preset measurement can see. This gate is
# that cross-check, and it is why the enumeration rule the file states in prose is now
# executable.
#
# Source-tree gate: reads the tree through `git ls-files` plus the preset files, builds
# nothing and configures nothing, so it needs no cross toolchain and registers on every
# board including sim.
#
# WHAT IT PINS, in both directions:
#   - every tracked *_trap_stack.h is named by an `arch` record, and every `arch` record's
#     header exists. Without this leg, deleting an arch record AND its presets together
#     un-gates that whole arch silently.
#   - every DEPTH macro the declared header defines is measured by a `class` record of that
#     arch. The header's DEPTH macros ARE the class list: a depth figure exists to be
#     compared against a measurement, so one that no class names is either a class that was
#     deleted or a figure nothing enforces.
#   - every `frame=` and `depth=` macro a class record names is defined in that arch's
#     header. check_trap_redzone.sh resolves these through the compiler, so it catches a
#     rename only for the arch it is measuring.
#   - every visible configure preset whose board's arch is a declared arch has a `preset`
#     record, and every `preset` record names a visible preset of that same arch.
#
# SCOPE. An arch with no trap-stack header is not gated and is not asked to be: sim and lx6
# have no such header, so their presets are outside every clause here by construction rather
# than by exemption. What this gate cannot see is whether a declared figure is RIGHT; that is
# check_trap_redzone.sh's measurement, and this gate only pins that the measurement is asked
# for at all.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

if [ "$#" -ne 2 ]; then
    fail "usage: check_trap_redzone_decls.sh <cmake> <src-dir>"
fi
CMAKE="$1"
SRC="$2"

[ -x "$CMAKE" ] || fail "no cmake at $CMAKE"
[ -d "$SRC" ] || fail "no source directory at $SRC"
cd "$SRC" || fail "cannot enter $SRC"
[ -f CMakeLists.txt ] || fail "$SRC is not the repo root"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "$SRC is not the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

ROOTS="tests/static/trap_redzone_roots.txt"
FLATTEN="tests/static/preset_boards.cmake"
[ -f "$ROOTS" ] || fail "no declaration file at $SRC/$ROOTS"
[ -f "$FLATTEN" ] || fail "no preset flattener at $SRC/$FLATTEN"

# The declaration reader, byte-for-byte the filter check_trap_redzone.sh uses, so this gate
# and the measuring gate cannot disagree about what a record says. Comments stripped and
# trailing-backslash continuations joined; no arch filter, because completeness is a question
# about the whole file.
decl() { # <kind>
    awk -v KIND="$1" '
        { sub(/#.*/, "") }
        /\\[[:space:]]*$/ { sub(/\\[[:space:]]*$/, " "); acc = acc $0; next }
        { line = acc $0; acc = "" }
        { n = split(line, f, /[[:space:]]+/) }
        n >= 2 && f[1] == KIND { print line }
    ' "$ROOTS"
}

scratch_dir
: > "$TMP/findings.txt"
report() { echo "$1" >> "$TMP/findings.txt"; }

# --- the arch records ----------------------------------------------------------
decl arch > "$TMP/arch.txt"
require_nonempty "$TMP/arch.txt" "$ROOTS declares no arch record; every clause below would pass vacuously"
# <arch>\t<header path>
awk '{ h = ""
       for (i = 3; i <= NF; i++) { if ($i ~ /^header=/) { h = substr($i, 8) } }
       printf "%s\t%s\n", $2, h }' "$TMP/arch.txt" > "$TMP/archmap"

while IFS="$TAB" read -r a h; do
    [ -n "$h" ] || fail "$ROOTS: arch record for '$a' carries no header= field"
    [ -f "$h" ] || report "arch $a names header=$h, which is not a file in this tree"
done < "$TMP/archmap"

# --- clause 1: the trap-stack headers the tree actually has --------------------
git ls-files -- 'arch/*_trap_stack.h' > "$TMP/headers" || fail "git ls-files failed"
require_nonempty "$TMP/headers" "git ls-files matched no *_trap_stack.h; the arch clause would pass vacuously"
N_HEADERS="$(wc -l < "$TMP/headers" | tr -d ' ')"

cut -f2 "$TMP/archmap" > "$TMP/declared_headers"
while IFS= read -r h; do
    if ! grep -qxF "$h" "$TMP/declared_headers"; then
        report "$h is a trap-stack header in this tree and $ROOTS declares no arch record for it; its whole arch is ungated"
    fi
done < "$TMP/headers"

# --- clause 2: every class the header prices is declared -----------------------
decl class > "$TMP/class.txt"
require_nonempty "$TMP/class.txt" "$ROOTS declares no class record"
N_CLASSES="$(wc -l < "$TMP/class.txt" | tr -d ' ')"

# <arch>\t<CLASS>\t<macro> for each of frame= and depth=
awk '{ for (i = 4; i <= NF; i++) {
           if ($i ~ /^frame=/) { printf "%s\t%s\t%s\n", $2, $3, substr($i, 7) }
           if ($i ~ /^depth=/) { printf "%s\t%s\t%s\n", $2, $3, substr($i, 7) }
       } }' "$TMP/class.txt" > "$TMP/classmacros"
require_nonempty "$TMP/classmacros" "no class record names a frame= or depth= macro"

N_ARCHES=0
N_DEPTHS=0
while IFS="$TAB" read -r a h; do
    N_ARCHES=$((N_ARCHES + 1))
    [ -f "$h" ] || continue

    # Every macro this arch's classes name must be defined in its own header.
    awk -F"$TAB" -v A="$a" '$1 == A { print $3 "\t" $2 }' "$TMP/classmacros" > "$TMP/want"
    while IFS="$TAB" read -r m cls; do
        if ! grep -qE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$m([[:space:]]|\$)" "$h"; then
            report "class $a $cls names $m, which $h does not define"
        fi
    done < "$TMP/want"

    # And every DEPTH figure the header defines must be measured by one of them.
    sed -n 's/^[[:space:]]*#[[:space:]]*define[[:space:]]\{1,\}\(KICKOS_[A-Z0-9_]*DEPTH[A-Z0-9_]*\).*/\1/p' \
        "$h" | sort -u > "$TMP/have"
    awk -F"$TAB" -v A="$a" '$1 == A { print $3 }' "$TMP/classmacros" | sort -u > "$TMP/named"
    while IFS= read -r m; do
        N_DEPTHS=$((N_DEPTHS + 1))
        if ! grep -qxF "$m" "$TMP/named"; then
            report "$h defines $m and no class record of arch $a measures it; a class was deleted, or the figure enforces nothing"
        fi
    done < "$TMP/have"
done < "$TMP/archmap"

# --- clause 3: every preset of a declared arch is registered -------------------
tool_out "$TMP/flatten.log" '' \
    "$CMAKE" "-DSRC=$SRC" "-DOUT=$TMP/presets" -P "$FLATTEN"
require_nonempty "$TMP/presets" "the preset flattener produced no table"
N_PRESETS="$(wc -l < "$TMP/presets" | tr -d ' ')"

decl preset > "$TMP/preset.txt"
require_nonempty "$TMP/preset.txt" "$ROOTS declares no preset record"
awk '{ printf "%s\t%s\n", $2, $3 }' "$TMP/preset.txt" > "$TMP/presetmap"
N_DECLARED="$(wc -l < "$TMP/presetmap" | tr -d ' ')"

# <preset>\t<board>\t<arch>, the arch read from the board's own board.cmake the way the root
# CMakeLists reads it. A preset with no board carries no arch and is out of scope.
: > "$TMP/presetarch"
while IFS="$TAB" read -r p b; do
    pa=""
    if [ "$b" != "@none" ] && [ -f "boards/$b/board.cmake" ]; then
        pa="$(sed -n 's/^[[:space:]]*set([[:space:]]*KICKOS_ARCH[[:space:]]\{1,\}"\([^"]*\)".*/\1/p' \
              "boards/$b/board.cmake" | head -1)"
    fi
    printf '%s\t%s\t%s\n' "$p" "$b" "$pa" >> "$TMP/presetarch"
done < "$TMP/presets"

cut -f1 "$TMP/archmap" > "$TMP/archnames"
N_INSCOPE=0
while IFS="$TAB" read -r p b pa; do
    # Only an arch this file gates is asked for; sim and lx6 have no trap-stack header. An
    # empty arch is tested for first: grep -xF "" matches every line and would put a
    # board-less preset in scope against the first declared arch.
    [ -n "$pa" ] || continue
    grep -qxF "$pa" "$TMP/archnames" || continue
    N_INSCOPE=$((N_INSCOPE + 1))
    if ! awk -F"$TAB" -v A="$pa" -v P="$p" '$1 == A && $2 == P { found = 1 } END { exit !found }' "$TMP/presetmap"; then
        report "configure preset $p (board $b, arch $pa) has no 'preset $pa $p' record in $ROOTS; its board registers no trap_redzone test at all"
    fi
done < "$TMP/presetarch"

# The other direction: a record naming a preset that is not a visible preset of that arch.
while IFS="$TAB" read -r a p; do
    if ! awk -F"$TAB" -v A="$a" -v P="$p" '$1 == P && $3 == A { found = 1 } END { exit !found }' "$TMP/presetarch"; then
        report "preset record '$a $p' names no visible configure preset of arch $a; the record is dead"
    fi
done < "$TMP/presetmap"

echo "== $N_HEADERS trap-stack header(s), $N_ARCHES declared arch(es), $N_CLASSES class record(s) over $N_DEPTHS depth figure(s), $N_INSCOPE of $N_PRESETS visible preset(s) in scope, $N_DECLARED registered =="

if [ -s "$TMP/findings.txt" ]; then
    cat "$TMP/findings.txt" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings.txt" | tr -d ' ') finding(s) against $ROOTS." >&2
    echo "      Declare the arch, the class or the preset, or drop a record whose subject is gone." >&2
    echo "      A class nothing declares is a red zone nothing measures, and a preset nothing" >&2
    echo "      registers is a board whose trap geometry is never checked." >&2
    exit 1
fi

echo "PASS: every trap-stack arch, class and preset of a gated arch is declared"
