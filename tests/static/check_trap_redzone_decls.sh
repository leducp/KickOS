#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the COMPLETENESS of the two static-callgraph declaration files. For
# tests/static/trap_redzone_roots.txt: every arch that has a trap-stack header is declared,
# every class that header prices is declared, and every configure preset of a declared arch is
# registered. For tests/static/console_reach_roots.txt: every preset whose chip TRANSLATES is
# registered, no record names a preset that is not there, and every registered preset carries
# the corpus floor its clause refuses without.
#
# Each of those two gates measures ONE preset per run and takes its set from whatever the file
# declares for that preset, so a DELETION is invisible to it: drop a class and the run measures
# one fewer and passes, drop a `preset` record and CMakeLists.txt registers no test for that
# board at all. This one is registered on EVERY board.
#
# It reads the tree through `git ls-files` plus the preset files and configures nothing.
#
# The invariant behind clause 2: a header's DEPTH macros ARE its class list, so a depth
# figure no class record names is either a deleted class or a figure nothing enforces.

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

# Byte-for-byte the filter check_trap_redzone.sh uses, so the two gates cannot disagree about
# what a record says. Comments stripped and trailing-backslash continuations joined.
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

    awk -F"$TAB" -v A="$a" '$1 == A { print $3 "\t" $2 }' "$TMP/classmacros" > "$TMP/want"
    while IFS="$TAB" read -r m cls; do
        if ! grep -qE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$m([[:space:]]|\$)" "$h"; then
            report "class $a $cls names $m, which $h does not define"
        fi
    done < "$TMP/want"

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
    # Only an arch this file gates is asked for; sim and lx6 have no trap-stack header. The
    # empty arch is tested FIRST: grep -xF "" matches every line and would put a board-less
    # preset in scope against the first declared arch.
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

# --- clause 4: every translating preset declares a console_reach record --------
# check_console_reach.sh is registered only on the presets tests/static/console_reach_roots.txt
# names, so a board added without a record there registers NOTHING and no run says so. The set
# is derived from arch/Kconfig: a chip that selects HAS_ASPACE translates, and a translating
# board is where the cap_console_deliver error paths survive the optimiser.
#
# A preset that is DECLARED without selecting HAS_ASPACE is not reported. qemu-x86_64 is the
# live case: q35 declines the select on purpose, its image being one flat link, and the clause
# still runs there because the console route is the same kernel C.
REACH="tests/static/console_reach_roots.txt"
[ -f "$REACH" ] || fail "no declaration file at $SRC/$REACH"

awk '
    /^config[[:space:]]+CHIP_/ { chip = tolower(substr($2, 6)); next }
    /^config[[:space:]]/ { chip = ""; next }
    chip != "" && $1 == "select" && $2 == "HAS_ASPACE" { print chip }
' arch/Kconfig | sort -u > "$TMP/aspacechips"
require_nonempty "$TMP/aspacechips" "no chip in arch/Kconfig selects HAS_ASPACE, so this clause would pass over an empty set"

awk '{ sub(/#.*/, "") } $1 == "preset" { printf "%s\t%s\n", $2, $3 }' "$REACH" > "$TMP/reachmap"
require_nonempty "$TMP/reachmap" "$REACH declares no preset record"
awk '{ sub(/#.*/, "") } $1 == "floor" { printf "%s\t%s\n", $2, $3 }' "$REACH" > "$TMP/reachfloor"
N_REACH_DECLARED="$(wc -l < "$TMP/reachmap" | tr -d ' ')"

N_TRANSLATING=0
while IFS="$TAB" read -r p b pa; do
    [ "$b" != "@none" ] || continue
    [ -f "boards/$b/board.cmake" ] || continue
    chip="$(sed -n 's/^[[:space:]]*set([[:space:]]*KICKOS_CHIP[[:space:]]\{1,\}"\([^"]*\)".*/\1/p' \
            "boards/$b/board.cmake" | head -1)"
    [ -n "$chip" ] || continue
    grep -qxF "$chip" "$TMP/aspacechips" || continue
    N_TRANSLATING=$((N_TRANSLATING + 1))
    if ! awk -F"$TAB" -v A="$pa" -v P="$p" '$1 == A && $2 == P { found = 1 } END { exit !found }' "$TMP/reachmap"; then
        report "configure preset $p (board $b, chip $chip) selects HAS_ASPACE and $REACH has no 'preset $pa $p' record; its board registers no console_reach test at all"
    fi
done < "$TMP/presetarch"

# The other direction, and the floor beside it: a record naming no visible preset is dead, and
# a preset declared with no floor record leaves that board's clause with no corpus floor.
while IFS="$TAB" read -r a p; do
    if ! awk -F"$TAB" -v A="$a" -v P="$p" '$1 == P && $3 == A { found = 1 } END { exit !found }' "$TMP/presetarch"; then
        report "preset record '$a $p' in $REACH names no visible configure preset of arch $a; the record is dead"
    fi
    if ! awk -F"$TAB" -v A="$a" -v P="$p" '$1 == A && $2 == P { found = 1 } END { exit !found }' "$TMP/reachfloor"; then
        report "$REACH declares preset $a $p and no floor record for it; that board's clause would report an absence over a corpus it never sized"
    fi
done < "$TMP/reachmap"

echo "== $N_HEADERS trap-stack header(s), $N_ARCHES declared arch(es), $N_CLASSES class record(s) over $N_DEPTHS depth figure(s), $N_INSCOPE of $N_PRESETS visible preset(s) in scope, $N_DECLARED registered =="
echo "== $N_TRANSLATING translating preset(s), $N_REACH_DECLARED registered in $REACH =="

if [ -s "$TMP/findings.txt" ]; then
    cat "$TMP/findings.txt" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings.txt" | tr -d ' ') finding(s) against $ROOTS and $REACH." >&2
    echo "      Declare the arch, the class or the preset, or drop a record whose subject is gone." >&2
    echo "      A class nothing declares is a red zone nothing measures, and a preset nothing" >&2
    echo "      registers is a board whose trap geometry, or whose console route, is never checked." >&2
    exit 1
fi

echo "PASS: every trap-stack arch, class and preset of a gated arch is declared, and every"
echo "      translating preset is registered with a corpus floor"
