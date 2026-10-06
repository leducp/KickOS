#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A system target whose composition names no packaged driver links the walk's trivial driver
# path and not the driver path itself, read from the objects its link map loads. --control reads
# a system naming a driver, which must link the driver path and not the trivial one.
#
#   check_no_driver_path.sh [--control] <map>

set -u
. "$(dirname "$0")/../lib/gate.sh"

CONTROL=0
if [ "${1:-}" = --control ]; then
    CONTROL=1
    shift
fi
[ "$#" -eq 1 ] || fail "usage: check_no_driver_path.sh [--control] <map>"
MAP="$1"
[ -f "$MAP" ] || fail "no link map at $MAP"
loads() { # <object basename, as an ERE>
    grep -Eq "^LOAD .*/init/compose/$1\\.cc\\.o(bj)?\$" "$MAP"
}
loads walk || fail "$(basename "$MAP") loads no init walk, so it is no system target's image"
WANT=driver_path_none
NOT=driver_path
if [ "$CONTROL" -eq 1 ]; then
    WANT=driver_path
    NOT=driver_path_none
fi
loads "$WANT" || fail "$(basename "$MAP") loads no $WANT"
if loads "$NOT"; then
    fail "$(basename "$MAP") loads $NOT as well"
fi
echo "PASS: $(basename "$MAP") links $WANT alone"
