#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the COMPLETENESS of the two static-callgraph declaration files, and on the SHAPE
# of the bindings file both gates share. For
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
# A `preset` record is matched against the REGISTRATION KEY and not against the configure
# preset name; tests/static/preset_boards.cmake says what that is and why the two differ.
#
# The invariant behind clause 2: a header's DEPTH macros ARE its class list, so a depth
# figure no class record names is either a deleted class or a figure nothing enforces.
#
# EVERY CLAUSE READS FILES AND NOTHING ELSE, which is why judge() below takes the whole world
# as arguments: the self-test hands it a planted one. The one thing outside that world is the
# preset table, which comes from the cmake flattener and is read once here.
#
# EVERY READER IS STATUS-CHECKED, through tool_out. An awk or a sed that dies writes an empty
# file, and a clause reading that file concludes "nothing declared" rather than "nothing read",
# which is the wrong cause with the right colour. `#!/bin/sh` is dash on the CI images and dash
# has no `pipefail`, so the reader's own exit status is what gets checked and a reader is never
# piped into the thing that sizes its output.

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
REACH="tests/static/console_reach_roots.txt"
INDIRECT="tests/static/trap_redzone_indirect.txt"
FLATTEN="tests/static/preset_boards.cmake"
[ -f "$ROOTS" ] || fail "no declaration file at $SRC/$ROOTS"
[ -f "$REACH" ] || fail "no declaration file at $SRC/$REACH"
[ -f "$INDIRECT" ] || fail "no bindings file at $SRC/$INDIRECT"
[ -f "$FLATTEN" ] || fail "no preset flattener at $SRC/$FLATTEN"

# Which macro spellings ARE a header's class list, handed to judge() so the controls and the
# tree are read by the same rule.
DEPTH_RE='KICKOS_[A-Z0-9_]*DEPTH[A-Z0-9_]*'
# The flattener's sentinel for a preset that names no board, and so carries no arch.
NONE_BOARD='@none'

scratch_dir

# Byte-for-byte the filter check_trap_redzone.sh uses, so the two gates cannot disagree about
# what a record says. Comments stripped and trailing-backslash continuations joined.
decl() { # <kind> <declaration-file> <outfile>
    tool_out "$3" '' awk -v KIND="$1" '
        { sub(/#.*/, "") }
        /\\[[:space:]]*$/ { sub(/\\[[:space:]]*$/, " "); acc = acc $0; next }
        { line = acc $0; acc = "" }
        { n = split(line, f, /[[:space:]]+/) }
        n >= 2 && f[1] == KIND { print line }
    ' "$2"
}

judge() { # <root> <roots> <reach> <indirect> <presets> <headers> <kconfig> <depth-re> <out>
    _root="$1"
    _roots="$2"
    _reach="$3"
    _indirect="$4"
    _presets="$5"
    _headers="$6"
    _kconfig="$7"
    _depth="$8"
    _out="$9"

    report() { echo "$1" >> "$_out"; }

    # --- the arch records ------------------------------------------------------
    decl arch "$_roots" "$TMP/arch.txt"
    require_nonempty "$TMP/arch.txt" "$_roots declares no arch record; every clause below would pass vacuously"
    # <arch>\t<header path>
    tool_out "$TMP/archmap" '' awk '{ h = ""
           for (i = 3; i <= NF; i++) { if ($i ~ /^header=/) { h = substr($i, 8) } }
           printf "%s\t%s\n", $2, h }' "$TMP/arch.txt"

    while IFS="$TAB" read -r a h; do
        [ -n "$h" ] || fail "$_roots: arch record for '$a' carries no header= field"
        [ -f "$_root/$h" ] || report "arch $a names header=$h, which is not a file in this tree"
    done < "$TMP/archmap"

    # --- clause 1: the trap-stack headers the tree actually has ----------------
    require_nonempty "$_headers" "git ls-files matched no *_trap_stack.h; the arch clause would pass vacuously"
    N_HEADERS="$(wc -l < "$_headers" | tr -d ' ')"

    cut -f2 "$TMP/archmap" > "$TMP/declared_headers"
    while IFS= read -r h; do
        if ! grep -qxF "$h" "$TMP/declared_headers"; then
            report "$h is a trap-stack header in this tree and $_roots declares no arch record for it; its whole arch is ungated"
        fi
    done < "$_headers"

    # --- clause 2: every class the header prices is declared -------------------
    decl class "$_roots" "$TMP/class.txt"
    require_nonempty "$TMP/class.txt" "$_roots declares no class record"
    N_CLASSES="$(wc -l < "$TMP/class.txt" | tr -d ' ')"

    # <arch>\t<CLASS>\t<macro> for each of frame= and depth=
    tool_out "$TMP/classmacros" '' awk '{ for (i = 4; i <= NF; i++) {
               if ($i ~ /^frame=/) { printf "%s\t%s\t%s\n", $2, $3, substr($i, 7) }
               if ($i ~ /^depth=/) { printf "%s\t%s\t%s\n", $2, $3, substr($i, 7) }
           } }' "$TMP/class.txt"
    require_nonempty "$TMP/classmacros" "no class record names a frame= or depth= macro"

    N_ARCHES=0
    N_DEPTHS=0
    while IFS="$TAB" read -r a h; do
        N_ARCHES=$((N_ARCHES + 1))
        [ -f "$_root/$h" ] || continue

        tool_out "$TMP/want" '' awk -F"$TAB" -v A="$a" '$1 == A { print $3 "\t" $2 }' \
            "$TMP/classmacros"
        while IFS="$TAB" read -r m cls; do
            if ! grep -qE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$m([[:space:]]|\$)" "$_root/$h"; then
                report "class $a $cls names $m, which $h does not define"
            fi
        done < "$TMP/want"

        tool_out "$TMP/have.raw" '' sed -n \
            "s/^[[:space:]]*#[[:space:]]*define[[:space:]]\{1,\}\($_depth\).*/\1/p" "$_root/$h"
        sort -u "$TMP/have.raw" -o "$TMP/have"
        tool_out "$TMP/named.raw" '' awk -F"$TAB" -v A="$a" '$1 == A { print $3 }' \
            "$TMP/classmacros"
        sort -u "$TMP/named.raw" -o "$TMP/named"
        while IFS= read -r m; do
            N_DEPTHS=$((N_DEPTHS + 1))
            if ! grep -qxF "$m" "$TMP/named"; then
                report "$h defines $m and no class record of arch $a measures it; a class was deleted, or the figure enforces nothing"
            fi
        done < "$TMP/have"
    done < "$TMP/archmap"

    # --- clause 3: every preset of a declared arch is registered ---------------
    require_nonempty "$_presets" "the preset flattener produced no table"
    N_PRESETS="$(wc -l < "$_presets" | tr -d ' ')"

    decl preset "$_roots" "$TMP/preset.txt"
    require_nonempty "$TMP/preset.txt" "$_roots declares no preset record"
    tool_out "$TMP/presetmap" '' awk '{ printf "%s\t%s\n", $2, $3 }' "$TMP/preset.txt"
    N_DECLARED="$(wc -l < "$TMP/presetmap" | tr -d ' ')"

    # <preset>\t<board>\t<arch>\t<registration key>, the arch read from the board's own
    # board.cmake the way the root CMakeLists reads it, and the key as the flattener rebuilds
    # it. A preset with no board carries no arch and is out of scope.
    : > "$TMP/presetarch"
    while IFS="$TAB" read -r p b k; do
        pa=""
        if [ "$b" != "$NONE_BOARD" ] && [ -f "$_root/boards/$b/board.cmake" ]; then
            tool_out "$TMP/onearch" '' sed -n \
                's/^[[:space:]]*set([[:space:]]*KICKOS_ARCH[[:space:]]\{1,\}"\([^"]*\)".*/\1/p' \
                "$_root/boards/$b/board.cmake"
            pa="$(head -1 "$TMP/onearch")"
        fi
        printf '%s\t%s\t%s\t%s\n' "$p" "$b" "$pa" "$k" >> "$TMP/presetarch"
    done < "$_presets"

    # --- the registration key, which every preset record below is matched against ---
    # A `preset` record does NOT name a configure preset: CMake names none, so the root
    # CMakeLists REBUILDS a name out of the board, the variant and, on an own-image AMP node,
    # the node index, and registers the gate under that. Two facts have to hold for the key to
    # be usable, and neither is visible from a record: it must key to exactly one preset, and
    # it must be a preset at all, both gates handing it to `cmake --preset`.
    cut -f1 "$_presets" > "$TMP/presetnames"
    tool_out "$TMP/keys" '' awk -F"$TAB" -v NONE="$NONE_BOARD" '$2 != NONE { print $4 }' \
        "$TMP/presetarch"
    sort "$TMP/keys" -o "$TMP/keys.sorted"
    uniq -d "$TMP/keys.sorted" > "$TMP/keydup"
    while IFS= read -r k; do
        [ -n "$k" ] || continue
        who="$(awk -F"$TAB" -v K="$k" '$4 == K { printf "%s ", $1 }' "$TMP/presetarch")"
        report "configure presets [ $who] all register under the name '$k'; a record naming it keys to more than one of them and measures whichever was configured"
    done < "$TMP/keydup"
    N_KEYS=0
    while IFS="$TAB" read -r p b pa k; do
        [ "$b" != "$NONE_BOARD" ] || continue
        N_KEYS=$((N_KEYS + 1))
        if ! grep -qxF "$k" "$TMP/presetnames"; then
            report "configure preset $p registers under the name '$k', which is no configure preset; the gate registered there hands that name to cmake --preset and dies before it measures anything"
        fi
    done < "$TMP/presetarch"

    cut -f1 "$TMP/archmap" > "$TMP/archnames"
    N_INSCOPE=0
    while IFS="$TAB" read -r p b pa k; do
        # Only an arch this file gates is asked for; sim and lx6 have no trap-stack header. The
        # empty arch is tested FIRST: grep -xF "" matches every line and would put a board-less
        # preset in scope against the first declared arch.
        [ -n "$pa" ] || continue
        grep -qxF "$pa" "$TMP/archnames" || continue
        N_INSCOPE=$((N_INSCOPE + 1))
        if ! awk -F"$TAB" -v A="$pa" -v K="$k" '$1 == A && $2 == K { found = 1 } END { exit !found }' "$TMP/presetmap"; then
            report "configure preset $p (board $b, arch $pa) registers under '$k' and $_roots has no 'preset $pa $k' record; its board registers no trap_redzone test at all"
        fi
    done < "$TMP/presetarch"

    # The other direction: a record naming a key no visible preset of that arch rebuilds.
    while IFS="$TAB" read -r a p; do
        if ! awk -F"$TAB" -v A="$a" -v P="$p" '$4 == P && $3 == A { found = 1 } END { exit !found }' "$TMP/presetarch"; then
            report "preset record '$a $p' names a registration key no visible configure preset of arch $a rebuilds; the record is dead"
        fi
    done < "$TMP/presetmap"

    # --- clause 4: every translating preset declares a console_reach record ----
    # check_console_reach.sh is registered only on the presets
    # tests/static/console_reach_roots.txt names, so a board added without a record there
    # registers NOTHING and no run says so. The set is derived from arch/Kconfig: a chip that
    # selects HAS_ASPACE translates, and a translating board is where the cap_console_deliver
    # error paths survive the optimiser.
    #
    # A preset that is DECLARED without selecting HAS_ASPACE is not reported. qemu-x86_64 is
    # the live case: q35 declines the select on purpose, its image being one flat link, and the
    # clause still runs there because the console route is the same kernel C.
    tool_out "$TMP/aspacechips.raw" '' awk '
        /^config[[:space:]]+CHIP_/ { chip = tolower(substr($2, 6)); next }
        /^config[[:space:]]/ { chip = ""; next }
        chip != "" && $1 == "select" && $2 == "HAS_ASPACE" { print chip }
    ' "$_kconfig"
    sort -u "$TMP/aspacechips.raw" -o "$TMP/aspacechips"
    require_nonempty "$TMP/aspacechips" "no chip in $_kconfig selects HAS_ASPACE, so this clause would pass over an empty set"

    tool_out "$TMP/reachmap" '' awk '{ sub(/#.*/, "") } $1 == "preset" { printf "%s\t%s\n", $2, $3 }' "$_reach"
    require_nonempty "$TMP/reachmap" "$_reach declares no preset record"
    tool_out "$TMP/reachfloor" '' awk '{ sub(/#.*/, "") } $1 == "floor" { printf "%s\t%s\n", $2, $3 }' "$_reach"
    N_REACH_DECLARED="$(wc -l < "$TMP/reachmap" | tr -d ' ')"

    N_TRANSLATING=0
    while IFS="$TAB" read -r p b pa k; do
        [ "$b" != "$NONE_BOARD" ] || continue
        [ -f "$_root/boards/$b/board.cmake" ] || continue
        tool_out "$TMP/onechip" '' sed -n \
            's/^[[:space:]]*set([[:space:]]*KICKOS_CHIP[[:space:]]\{1,\}"\([^"]*\)".*/\1/p' \
            "$_root/boards/$b/board.cmake"
        chip="$(head -1 "$TMP/onechip")"
        [ -n "$chip" ] || continue
        grep -qxF "$chip" "$TMP/aspacechips" || continue
        N_TRANSLATING=$((N_TRANSLATING + 1))
        if ! awk -F"$TAB" -v A="$pa" -v K="$k" '$1 == A && $2 == K { found = 1 } END { exit !found }' "$TMP/reachmap"; then
            report "configure preset $p (board $b, chip $chip) selects HAS_ASPACE, registers under '$k' and $_reach has no 'preset $pa $k' record; its board registers no console_reach test at all"
        fi
    done < "$TMP/presetarch"

    # The other direction, and the floor beside it: a record naming a key no visible preset
    # rebuilds is dead, and a preset declared with no floor record leaves that board's clause
    # with no corpus floor.
    while IFS="$TAB" read -r a p; do
        if ! awk -F"$TAB" -v A="$a" -v P="$p" '$4 == P && $3 == A { found = 1 } END { exit !found }' "$TMP/presetarch"; then
            report "preset record '$a $p' in $_reach names a registration key no visible configure preset of arch $a rebuilds; the record is dead"
        fi
        if ! awk -F"$TAB" -v A="$a" -v P="$p" '$1 == A && $2 == P { found = 1 } END { exit !found }' "$TMP/reachfloor"; then
            report "$_reach declares preset $a $p and no floor record for it; that board's clause would report an absence over a corpus it never sized"
        fi
    done < "$TMP/reachmap"

    # --- clause 5: the shared bindings file is well formed, WHOLE --------------
    # trap_redzone.py and console_reach.py each read only the records matching the one arch and
    # preset they were handed, so a malformed record for an arch no board on this box
    # configures reaches neither. This shape-checks every record in the file and builds
    # nothing.
    if ! SHAPE="$(python3 tests/static/trap_redzone.py --check-file "$_indirect" 2>&1)"; then
        printf '%s\n' "$SHAPE" >&2
        fail "$_indirect has a malformed record"
    fi
    echo "== $SHAPE =="

    echo "== $N_KEYS of $N_PRESETS visible preset(s) rebuild a registration key that is a preset =="
    echo "== $N_HEADERS trap-stack header(s), $N_ARCHES declared arch(es), $N_CLASSES class record(s) over $N_DEPTHS depth figure(s), $N_INSCOPE of $N_PRESETS visible preset(s) in scope, $N_DECLARED registered =="
    echo "== $N_TRANSLATING translating preset(s), $N_REACH_DECLARED registered in $_reach =="
}

# --- self-test: one control per clause, each a minimal pair -------------------
# The world is PLANTED, so nothing here is tracked and no clause reads the real tree. Each
# control is one edit away from the clean world, and the finding COUNT is asserted rather than
# a colour.
#
# The negatives are proven to be NEAR MISSES the same way round: each has a twin edit that
# turns the tolerated property into the reported one, and both counts are asserted. That is
# what a set-comparison clause can offer in place of an ERE to withdraw.
world() { # <dir>
    _w="$1"
    rm -rf "$_w"
    mkdir -p "$_w/arch" "$_w/boards/bb"
    printf '#define KICKOS_AA_IRQ_FRAME 64\n#define KICKOS_AA_IRQ_DEPTH 128\n' \
        > "$_w/arch/aa_trap_stack.h"
    {
        printf 'arch aa header=arch/aa_trap_stack.h\n'
        printf 'class aa IRQ frame=KICKOS_AA_IRQ_FRAME depth=KICKOS_AA_IRQ_DEPTH\n'
        printf 'preset aa pboard\n'
    } > "$_w/roots.txt"
    {
        printf 'preset aa pboard\n'
        printf 'floor aa pboard files=1 nodes=1 reason: planted\n'
    } > "$_w/reach.txt"
    printf 'site aa * planted_caller@1/1 planted.cc:kos_callee\n' > "$_w/indirect.txt"
    printf 'pboard\tbb\tpboard\n' > "$_w/presets"
    printf 'arch/aa_trap_stack.h\n' > "$_w/headers"
    printf 'config CHIP_CC\n\tselect HAS_ASPACE\n' > "$_w/kconfig"
    printf 'set(KICKOS_ARCH "aa")\nset(KICKOS_CHIP "cc")\n' > "$_w/boards/bb/board.cmake"
}

# A second board, so a control can add a preset without disturbing the first. Its chip decides
# whether that preset TRANSLATES, which is what separates clause 3's scope from clause 4's.
board2() { # <dir> <arch> <chip>
    mkdir -p "$1/boards/b2"
    printf 'set(KICKOS_ARCH "%s")\nset(KICKOS_CHIP "%s")\n' "$2" "$3" > "$1/boards/b2/board.cmake"
    printf 'p2\tb2\tp2\n' >> "$1/presets"
}

W="$TMP/world"
run_judge() { # <expect-rc>; leaves the findings in $TMP/jf and the output in $TMP/jo
    : > "$TMP/jf"
    ( judge "$W" "$W/roots.txt" "$W/reach.txt" "$W/indirect.txt" "$W/presets" \
        "$W/headers" "$W/kconfig" "$DEPTH_RE" "$TMP/jf" ) > "$TMP/jo" 2>&1
    _rc=$?
    [ "$_rc" -eq "$1" ] || {
        cat "$TMP/jo" >&2
        fail "the planted world exits $_rc, expected $1"
    }
}
findings() {
    wc -l < "$TMP/jf" | tr -d ' '
}
one() { # <label> <expect-ere>
    run_judge 0
    [ "$(findings)" -eq 1 ] || {
        cat "$TMP/jf" >&2
        fail "control '$1' reports $(findings) finding(s), expected exactly 1"
    }
    grep -qE "$2" "$TMP/jf" || {
        cat "$TMP/jf" >&2
        fail "control '$1' reports, but not for its own clause; expected /$2/. A control
      another clause catches proves nothing about the clause it plants for"
    }
}
none() { # <label>
    run_judge 0
    [ "$(findings)" -eq 0 ] || {
        cat "$TMP/jf" >&2
        fail "negative control '$1' reports; the gate would redden a declaration set that is correct"
    }
}
refuses() { # <label> <expect-ere>
    run_judge 1
    grep -qE "$2" "$TMP/jo" || {
        cat "$TMP/jo" >&2
        fail "control '$1' refused for the wrong reason; expected /$2/"
    }
}
edit() { # <file> <sed-script>
    sed "$2" "$1" > "$1.new" || fail "the control editor failed on $1"
    mv "$1.new" "$1"
}

# The clean world, which every count below is measured against.
world "$W"; none clean

# Clause 0: an arch record whose header is not in the tree. The tree list still names the
# path, so clause 1 cannot be what reports.
world "$W"; rm "$W/arch/aa_trap_stack.h"
one header_absent 'names header=arch/aa_trap_stack.h, which is not a file'

# Clause 1: a trap-stack header the tree has and no arch record declares.
world "$W"
printf '#define KICKOS_ZZ_IRQ_DEPTH 8\n' > "$W/arch/zz_trap_stack.h"
printf 'arch/zz_trap_stack.h\n' >> "$W/headers"
one header_undeclared 'zz_trap_stack.h is a trap-stack header in this tree and .* declares no arch record'

# Clause 2, first half: a class record naming a macro the header does not define. The FRAME
# macro is the one edited, because a missing DEPTH macro would also leave the header's own
# depth figure unmeasured and report twice.
world "$W"; edit "$W/roots.txt" 's/frame=KICKOS_AA_IRQ_FRAME/frame=KICKOS_AA_IRQ_NOSUCH/'
one class_macro_absent 'class aa IRQ names KICKOS_AA_IRQ_NOSUCH, which .* does not define'

# Clause 2, second half: a depth figure in the header that no class record measures.
world "$W"; printf '#define KICKOS_AA_FIQ_DEPTH 32\n' >> "$W/arch/aa_trap_stack.h"
one depth_unmeasured 'defines KICKOS_AA_FIQ_DEPTH and no class record of arch aa measures it'

# Clause 3: two configure presets rebuilding the SAME registration key.
world "$W"; printf 'pother\tbb\tpboard\n' >> "$W/presets"
one key_collision "all register under the name 'pboard'"

# Clause 3: a key that is no configure preset. The declaration files name the same key, so the
# missing-record leg cannot be what reports.
world "$W"
edit "$W/presets" 's/pboard$/pkey/'
edit "$W/roots.txt" 's/preset aa pboard/preset aa pkey/'
edit "$W/reach.txt" 's/ pboard/ pkey/'
one key_not_a_preset "registers under the name 'pkey', which is no configure preset"

# Clause 3: an in-scope preset with no record. Its chip does not translate, so clause 4 is
# silent and this leg is alone.
world "$W"; board2 "$W" aa dd
one preset_unregistered "arch aa\) registers under 'p2' and .* has no 'preset aa p2' record"

# Clause 3: a record naming a key nothing rebuilds.
world "$W"; printf 'preset aa ghost\n' >> "$W/roots.txt"
one preset_record_dead "preset record 'aa ghost' names a registration key no visible configure preset"

# Clause 4: a TRANSLATING preset with no console_reach record. It is registered in roots.txt,
# so clause 3 is silent.
world "$W"; board2 "$W" aa cc; printf 'preset aa p2\n' >> "$W/roots.txt"
one reach_unregistered 'selects HAS_ASPACE, registers under .p2. and .* has no .preset aa p2. record'

# Clause 4: a dead record in the reach file, and separately a declared preset with no floor.
world "$W"
printf 'preset aa ghost\nfloor aa ghost files=1 nodes=1 reason: planted\n' >> "$W/reach.txt"
one reach_record_dead "preset record 'aa ghost' in .* names a registration key no visible configure preset"
world "$W"; edit "$W/reach.txt" '/^floor /d'
one reach_floor_absent 'declares preset aa pboard and no floor record for it'

# The refusals, which are not findings: an unreadable posture is not a clean one.
world "$W"; edit "$W/roots.txt" 's/ header=arch.*$//'
refuses arch_no_header "arch record for 'aa' carries no header= field"
world "$W"; edit "$W/roots.txt" '/^arch /d'
refuses no_arch_record 'declares no arch record'
# Both refusals come out of the shape checker, so each control asserts the CHECKER's own
# message and not just this gate's wrapper: the two would otherwise be indistinguishable and
# either could be satisfied by the other's cause.
world "$W"; printf 'notasite aa\n' > "$W/indirect.txt"
refuses bindings_malformed 'unknown record .notasite.'
# A site key carrying no caller and no ordinal. Its own refusal, because a key the shape
# checker lets through is one the graph readers would then have to guess an ordinal for.
world "$W"; printf 'site aa * planted.cc:1:1 planted.cc:kos_callee\n' > "$W/indirect.txt"
refuses bindings_key_shape 'is not <caller>@<ordinal>/<count>'
# An ordinal past its own count, which no graph can satisfy and which a digits-only check
# would accept.
world "$W"; printf 'site aa * planted_caller@3/2 planted.cc:kos_callee\n' > "$W/indirect.txt"
refuses bindings_key_range 'is not <caller>@<ordinal>/<count>'
world "$W"; : > "$W/indirect.txt"
refuses bindings_empty 'carries no site record'
world "$W"; : > "$W/headers"
refuses no_trap_stack_header 'matched no .*_trap_stack.h'
world "$W"; edit "$W/kconfig" 's/HAS_ASPACE/HAS_NOTHING/'
refuses no_aspace_chip 'selects HAS_ASPACE, so this clause would pass over an empty set'

# The negatives, each read on its own, and each with the twin edit that makes it report: a
# control silent for the WRONG reason is what the pair rules out.
world "$W"; printf 'phost\t%s\tphost\n' "$NONE_BOARD" >> "$W/presets"; none board_less_preset
world "$W"; board2 "$W" sim dd; none arch_not_gated
world "$W"; board2 "$W" aa dd; edit "$W/roots.txt" 's/^preset aa pboard$/preset aa pboard\npreset aa p2/'
none preset_registered
world "$W"; board2 "$W" aa cc
printf 'preset aa p2\n' >> "$W/roots.txt"
printf 'preset aa p2\nfloor aa p2 files=1 nodes=1 reason: planted\n' >> "$W/reach.txt"
none reach_registered
# A chip that does not translate, DECLARED in the reach file anyway. Deliberately not a
# finding, so a flat-image board can carry the clause.
world "$W"; board2 "$W" aa dd
printf 'preset aa p2\n' >> "$W/roots.txt"
printf 'preset aa p2\nfloor aa p2 files=1 nodes=1 reason: planted\n' >> "$W/reach.txt"
none reach_declared_untranslating
# Comments and a trailing-backslash continuation, which the record filter has to survive.
world "$W"
{
    printf '# a comment naming preset aa nothing\n'
    printf 'arch aa header=arch/aa_trap_stack.h\n'
    printf 'class aa IRQ frame=KICKOS_AA_IRQ_FRAME \\\n'
    printf '    depth=KICKOS_AA_IRQ_DEPTH\n'
    printf 'preset aa pboard  # trailing comment\n'
} > "$W/roots.txt"
none comments_and_continuation
# A depth-shaped macro OUTSIDE the KICKOS_ namespace, which is not a class figure.
world "$W"; printf '#define AA_DEPTH_LIMIT 5\n' >> "$W/arch/aa_trap_stack.h"
none foreign_depth_macro

# The one reading this gate takes as a pattern, withdrawn: widen it past the KICKOS_ prefix
# and the foreign macro must newly report, so that control is a near miss.
world "$W"; printf '#define AA_DEPTH_LIMIT 5\n' >> "$W/arch/aa_trap_stack.h"
: > "$TMP/jf"
( judge "$W" "$W/roots.txt" "$W/reach.txt" "$W/indirect.txt" "$W/presets" "$W/headers" \
    "$W/kconfig" '[A-Z0-9_]*DEPTH[A-Z0-9_]*' "$TMP/jf" ) > "$TMP/jo" 2>&1 \
    || { cat "$TMP/jo" >&2; fail "the widened depth pattern refused the planted world"; }
[ "$(findings)" -eq 1 ] || {
    cat "$TMP/jf" >&2
    fail "with the KICKOS_ prefix withdrawn the depth clause reports $(findings) finding(s),
      expected 1; the foreign-macro control is not a near miss and proves nothing"
}

# THE DEAD READER, which is the case a planted world cannot reach: a reader that dies writes
# an empty file, and the clause above it then reports the absence it was testing for. The world
# here is CLEAN, so only the tool death can redden it, and the message has to NAME the tool.
world "$W"
mkdir -p "$TMP/deadbin"
printf '#!/bin/sh\nexit 3\n' > "$TMP/deadbin/awk"
chmod +x "$TMP/deadbin/awk"
: > "$TMP/jf"
( PATH="$TMP/deadbin:$PATH"; export PATH
  judge "$W" "$W/roots.txt" "$W/reach.txt" "$W/indirect.txt" "$W/presets" "$W/headers" \
      "$W/kconfig" "$DEPTH_RE" "$TMP/jf" ) > "$TMP/jo" 2>&1 && \
    fail "a reader that cannot run read the whole planted world and said nothing; every count
      this gate prints would be a corpus size it never measured"
grep -q '^FAIL: exit ' "$TMP/jo" \
    || {
        cat "$TMP/jo" >&2
        fail "a dead reader reddened the gate without naming the tool failure, so the run
      reports an empty declaration set instead of an unread one"
    }

# --- the tree ----------------------------------------------------------------
tool_out "$TMP/flatten.log" '' "$CMAKE" "-DSRC=$SRC" "-DOUT=$TMP/presets" -P "$FLATTEN"
git ls-files -- 'arch/*_trap_stack.h' > "$TMP/headers" || fail "git ls-files failed"

: > "$TMP/findings.txt"
judge . "$ROOTS" "$REACH" "$INDIRECT" "$TMP/presets" "$TMP/headers" arch/Kconfig \
    "$DEPTH_RE" "$TMP/findings.txt"

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
