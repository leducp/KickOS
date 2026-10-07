#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A constant a cmake/*_geometry.cmake hands kickos_emit_geometry reaches C through the header
# that writes, and no tracked source defines it. A copy in a translation unit that never
# includes that header compiles, and keeps the old value when the declaration changes.
#
# Run from the repo root, no arguments: tests/static/check_geometry_emitted.sh

set -u
. "$(dirname "$0")/../lib/gate.sh"
require_repo_root
scratch_dir
rc=0

defines() { # <name> <list of files>: those defining <name>
    xargs grep -lE "^[[:space:]]*#[[:space:]]*define[[:space:]]+$1([^A-Za-z0-9_]|\$)" < "$2"
}
printf '#  define KOS_EXIT_FAULT 139\n' > "$TMP/planted.h"
echo "$TMP/planted.h" > "$TMP/planted"
[ -n "$(defines KOS_EXIT_FAULT "$TMP/planted")" ] || fail "the reader missed a planted #define,
  so finding none in the tree witnesses nothing"

corpus "$TMP/decl" "geometry declaration" 'cmake/*_geometry.cmake'
xargs sed -n 's/^kickos_emit_geometry([^ ]* \(.*\))$/\1/p' < "$TMP/decl" | tr ' ' '\n' > "$TMP/names"
require_nonempty "$TMP/names" "no cmake/*_geometry.cmake hands kickos_emit_geometry a name"
corpus_sources "$TMP/c"
while read -r name; do
    others="$(defines "$name" "$TMP/c")"
    [ -z "$others" ] || bad "$name is defined beside its generated header, in: $others"
done < "$TMP/names"
[ "$rc" -eq 0 ] || exit 1
echo "PASS: $(wc -l < "$TMP/names") geometry constant(s) reach C through their generated header alone"
