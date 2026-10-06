#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A constant the build declares in a cmake/*_geometry.cmake reaches C through the header the
# configure generates from its template, and is defined nowhere else, so CMake never reads a value
# back out of C and the two never disagree.
#
#   check_geometry_emitted.sh <kickos-build>
#
# Each row below is a name, the file declaring it, and the template its header is generated
# from. For each, the declaration is one `set(<name> <integer>)`, the template's one definition is
# `#define <name> @<name>@`, no other tracked C file defines it, and the build's generated header
# defines it to the declared value.

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_geometry_emitted.sh <kickos-build>"
BUILD="${1:?$USAGE}"
require_repo_root

ROWS="KICKOS_MPU_MAX_REGIONS cmake/mpu_geometry.cmake kernel/include/kickos/config/mpu_geometry.h.in
KOS_EXIT_FAULT cmake/exit_geometry.cmake user/include/kickos/sys/exit_status.h.in
KOS_EXIT_CANCELLED cmake/exit_geometry.cmake user/include/kickos/sys/exit_status.h.in"

scratch_dir
rc=0
printf '%s\n' "$ROWS" > "$TMP/rows"
git ls-files -- '*.h' '*.hh' '*.hpp' '*.c' '*.cc' '*.cpp' '*.h.in' > "$TMP/c"
require_nonempty "$TMP/c" "git ls-files listed no C file"
while read -r name declared template; do
    value="$(sed -n "s/^set($name \\([0-9][0-9]*\\))\$/\\1/p" "$declared")"
    if [ "$(printf '%s\n' "$value" | grep -c .)" -ne 1 ]; then
        bad "$declared does not declare $name once as an integer"
        continue
    fi
    [ "$(grep -c "^#define $name @$name@\$" "$template")" -eq 1 ] \
        || bad "$template does not define $name once as @$name@"
    others="$(grep -vxF "$template" "$TMP/c" | xargs grep -lE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$name([^A-Za-z0-9_]|\$)")"
    [ -z "$others" ] || bad "$name is defined beside $template, in: $others"
    generated="$BUILD/generated/include/${template#*/include/}"
    generated="${generated%.in}"
    if [ ! -f "$generated" ]; then
        bad "the build generated no $generated"
        continue
    fi
    [ "$(grep -c "^#define $name $value\$" "$generated")" -eq 1 ] \
        || bad "$generated does not define $name to $declared's $value"
done < "$TMP/rows"
[ "$rc" -eq 0 ] || exit 1
echo "PASS: each declared geometry constant reaches C through its generated header alone"
