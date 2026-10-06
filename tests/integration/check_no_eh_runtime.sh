#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An image that creates no exception links none of the exception runtime: no libstdc++ or
# libsupc++ eh_* member and no libgcc unwinder member is extracted, read from the link map's
# list of the archive members it took. --control reads an image that throws and passes only where
# the same read finds the runtime, so a reader that matches nothing cannot pass. --self-test reads
# planted maps of each runtime spelling, nano and full, the ARM unwinder and the DWARF one, both
# ways.
#
#   check_no_eh_runtime.sh [--control] <map>
#   check_no_eh_runtime.sh --self-test

set -u
. "$(dirname "$0")/../lib/gate.sh"

# <map> <members out> <eh out>: the archive members the map lists taken, and the runtime's among
# them; status 2 when the member list was not read.
eh_members() {
    awk '/^Archive member included/ { on = 1; next }
         on && /^(Discarded input sections|Memory Configuration|Allocating common symbols)/ { exit }
         on && /^[^ \t]/ { print $1 }' "$1" > "$2"
    # An image takes kickos_user and the kernel from archives, so an empty list is a map this read
    # no longer understands.
    grep -q 'libkickos_kernel\.a(' "$2" || return 2
    grep -E '(libstdc\+\+|libsupc\+\+)(_nano)?\.a\(eh_[a-z0-9_-]*\.o\)|libgcc(_eh)?\.a\((unwind|libunwind|pr-support)[a-z0-9_-]*\.o\)' \
        "$2" > "$3"
    return 0
}

scratch_dir

if [ "${1:-}" = --self-test ]; then
    # <name> <member line>...: a map taking the kernel archive's member and each line's.
    plant() {
        _pl_out="$TMP/$1.map"
        shift
        {
            echo 'Archive member included to satisfy reference by file (symbol)'
            echo
            echo '/opt/kickos/lib/libkickos_kernel.a(kmain.cc.obj)'
            echo '                              main.cc.obj (kmain)'
            for _pl_m in "$@"; do
                echo "$_pl_m"
                echo '                              main.cc.obj (__cxa_throw)'
            done
            echo
            echo 'Discarded input sections'
            echo
            echo '/opt/kickos/lib/libstdc++.a(eh_personality.o) .text.unused'
        } > "$_pl_out"
    }
    plant none
    plant nano '/opt/gcc/lib/thumb/v6-m/nofp/libstdc++_nano.a(eh_personality.o)'
    plant supc '/opt/gcc/lib/libsupc++.a(eh_personality.o)' '/opt/gcc/lib/libgcc_eh.a(unwind-arm.o)'
    plant dwarf '/opt/gcc/lib/libstdc++.a(eh_personality.o)' '/opt/gcc/lib/libgcc.a(unwind-dw2.o)'
    plant unwinder '/opt/gcc/lib/libgcc.a(unwind-dw2-fde.o)'
    for _st in none nano supc dwarf unwinder; do
        eh_members "$TMP/$_st.map" "$TMP/members" "$TMP/eh" \
            || fail "the planted $_st map's member list was not read"
        _st_found=0
        if [ -s "$TMP/eh" ]; then
            _st_found=1
        fi
        _st_want=1
        if [ "$_st" = none ]; then
            _st_want=0
        fi
        [ "$_st_found" -eq "$_st_want" ] || fail "the read finds the runtime in the planted $_st map: $_st_found, not $_st_want"
        _st_pers=0
        if grep -q 'eh_personality' "$TMP/eh"; then
            _st_pers=1
        fi
        case "$_st" in
            nano|supc|dwarf) [ "$_st_pers" -eq 1 ] || fail "--control finds no personality in the planted $_st map" ;;
            *) [ "$_st_pers" -eq 0 ] || fail "--control finds a personality in the planted $_st map" ;;
        esac
    done
    grep -v 'libkickos_kernel' "$TMP/none.map" > "$TMP/unread.map"
    if eh_members "$TMP/unread.map" "$TMP/members" "$TMP/eh"; then
        fail "a map taking no kernel member was read as a member list"
    fi
    echo "PASS: the read finds each planted runtime member, nano and full, ARM and DWARF unwinders, and none in a map without them"
    exit 0
fi

CONTROL=0
if [ "${1:-}" = --control ]; then
    CONTROL=1
    shift
fi
[ "$#" -eq 1 ] || fail "usage: check_no_eh_runtime.sh [--control] <map>"
MAP="$1"
[ -f "$MAP" ] || fail "no link map at $MAP"
eh_members "$MAP" "$TMP/members" "$TMP/eh" \
    || fail "$MAP lists no member of the kernel archive taken, so the member list was not read"
if [ "$CONTROL" -eq 1 ]; then
    grep -q 'eh_personality' "$TMP/eh" \
        || fail "$(basename "$MAP") throws, and the read found no exception runtime in it"
    echo "PASS: control: $(basename "$MAP"), which throws, links the exception runtime the read finds"
    exit 0
fi
if [ -s "$TMP/eh" ]; then
    sed 's/^/  /' "$TMP/eh" >&2
    fail "$(basename "$MAP") links the exception runtime, which an image creating no exception must not"
fi
echo "PASS: $(basename "$MAP") links no member of the exception runtime"
