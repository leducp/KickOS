#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No image's link discards a constructor or destructor table entry. A table section the linker
# script names no selector for is an orphan, and --gc-sections drops it with the function it
# points at: the constructor never runs and nothing says so. Read off the `Discarded input
# sections` heading of each link map, matched in English as check_no_libnosys.sh's is.
#
#   check_ctors_kept.sh <map>...
#   check_ctors_kept.sh --images <list>
#   check_ctors_kept.sh --self-test

set -u
. "$(dirname "$0")/../lib/gate.sh"

# <map> <out>: each discarded table section the map lists, with the file it came from. Status 2
# when the map carries no discard list.
discarded_tables() {
    awk '/^Discarded input sections/ { on = 1; seen = 1; next }
         on && /^(Memory Configuration|Linker script and memory map)/ { exit }
         on && /^ \.(ctors|dtors|init_array|fini_array|preinit_array)([. ]|$)/ {
             line = $0
             if (NF < 4 && (getline nxt) > 0) { line = line " " nxt }
             n = split(line, f, /[ \t]+/)
             print f[2] " " f[n]
         }
         END { if (!seen) { exit 2 } }' "$1" > "$2"
}

scratch_dir

if [ "${1:-}" = --self-test ]; then
    # <name> <discarded row>...: a map whose discard list holds those rows beside a harmless one.
    plant() {
        _pl_out="$TMP/$1.map"
        shift
        {
            echo 'Archive member included to satisfy reference by file (symbol)'
            echo
            echo 'Discarded input sections'
            echo
            echo ' .text.unused   0x00000000       0x10 main.cc.obj'
            for _pl_r in "$@"; do
                printf '%s\n' "$_pl_r"
            done
            echo
            echo 'Memory Configuration'
            echo
            echo ' .ctors         0x40080000        0x4 kept.o'
        } > "$_pl_out"
    }
    plant clean
    discarded_tables "$TMP/clean.map" "$TMP/out" || fail "the planted clean map was not read"
    [ ! -s "$TMP/out" ] || fail "a map discarding no table reads as discarding: $(cat "$TMP/out")"
    plant ctors ' .ctors.65434   0x00000000        0x4 libkickos_cxx_throw.a(exception_report.cc.obj)'
    discarded_tables "$TMP/ctors.map" "$TMP/out"
    grep -qxF '.ctors.65434 libkickos_cxx_throw.a(exception_report.cc.obj)' "$TMP/out" \
        || fail "a discarded prioritised .ctors is not read: $(cat "$TMP/out")"
    plant wrapped ' .init_array.00101' '                0x00000000        0x4 app.o'
    discarded_tables "$TMP/wrapped.map" "$TMP/out"
    grep -qxF '.init_array.00101 app.o' "$TMP/out" \
        || fail "a discarded table whose name wraps onto its own line is not read: $(cat "$TMP/out")"
    plant dtors ' .dtors         0x00000000        0x4 eh_alloc.o'
    discarded_tables "$TMP/dtors.map" "$TMP/out"
    grep -qxF '.dtors eh_alloc.o' "$TMP/out" || fail "a discarded .dtors is not read"
    plant lookalike ' .ctorsx        0x00000000        0x4 a.o' ' .init_arrayz   0x00000000    0x4 b.o'
    discarded_tables "$TMP/lookalike.map" "$TMP/out"
    [ ! -s "$TMP/out" ] || fail "a section only named like a table reads as one: $(cat "$TMP/out")"
    echo 'Memory Configuration' > "$TMP/unread.map"
    if discarded_tables "$TMP/unread.map" "$TMP/out"; then
        fail "a map carrying no discard list was read as discarding nothing"
    fi
    echo "$TMP/ctors.map" > "$TMP/ctors.list"
    if sh "$0" --images "$TMP/ctors.list" > "$TMP/run.out" 2>&1; then
        fail "--images passes a map discarding a constructor"
    fi
    grep -qF 'discard a constructor or destructor table' "$TMP/run.out" \
        || fail "--images refuses a discarded constructor for another reason: $(cat "$TMP/run.out")"
    echo "PASS: a discarded .ctors, .dtors or .init_array entry is read, a wrapped name included,"
    echo "  and none out of a map discarding only code; --images refuses the first"
    exit 0
fi

if [ "${1:-}" = --images ]; then
    [ "$#" -eq 2 ] || fail "usage: check_ctors_kept.sh --images <list>"
    [ -f "$2" ] || fail "no image list at $2"
    grep -v '^$' "$2" > "$TMP/maps"
    [ -s "$TMP/maps" ] || fail "$2 names no image"
else
    [ "$#" -ge 1 ] || fail "usage: check_ctors_kept.sh <map>..."
    printf '%s\n' "$@" > "$TMP/maps"
fi
COUNT=0
BAD=0
while IFS= read -r MAP; do
    [ -f "$MAP" ] || fail "no link map at $MAP"
    discarded_tables "$MAP" "$TMP/tables" \
        || fail "$MAP carries no 'Discarded input sections' list, so it was not read"
    COUNT=$((COUNT + 1))
    if [ -s "$TMP/tables" ]; then
        echo "$(basename "$MAP"):" >&2
        sed 's/^/  /' "$TMP/tables" >&2
        BAD=$((BAD + 1))
    fi
done < "$TMP/maps"
if [ "$BAD" -ne 0 ]; then
    fail "$BAD of $COUNT image(s) discard a constructor or destructor table: the linker script
  names no selector for that section, so the function it registers never runs"
fi
echo "PASS: none of $COUNT image(s) discards a constructor or destructor table"
