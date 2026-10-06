#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An image that creates no exception links none of the exception runtime: no libstdc++ or
# libsupc++ eh_* member and no libgcc unwinder member is extracted, read from the link map's
# list of the archive members it took. --control reads an image that throws and passes only where
# the same read finds the runtime, so a reader that matches nothing cannot pass.
#
#   check_no_eh_runtime.sh [--control] <map>

set -u
. "$(dirname "$0")/../lib/gate.sh"

CONTROL=0
if [ "${1:-}" = --control ]; then
    CONTROL=1
    shift
fi
[ "$#" -eq 1 ] || fail "usage: check_no_eh_runtime.sh [--control] <map>"
MAP="$1"
[ -f "$MAP" ] || fail "no link map at $MAP"
scratch_dir
awk '/^Archive member included/ { on = 1; next }
     on && /^(Discarded input sections|Memory Configuration|Allocating common symbols)/ { exit }
     on && /^[^ \t]/ { print $1 }' "$MAP" > "$TMP/members"
# An image takes kickos_user and the kernel from archives, so an empty list is a map this read
# no longer understands.
grep -q 'libkickos_kernel\.a(' "$TMP/members" \
    || fail "$MAP lists no member of the kernel archive taken, so the member list was not read"
grep -E '(libstdc\+\+|libsupc\+\+)(_nano)?\.a\(eh_[a-z0-9_-]*\.o\)|libgcc(_eh)?\.a\((unwind|libunwind|pr-support)[a-z0-9_-]*\.o\)' \
    "$TMP/members" > "$TMP/eh"
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
