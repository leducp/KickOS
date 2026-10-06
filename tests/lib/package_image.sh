#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A consumer project built against a KickOS build's installed package, for a caller that is not
# a gate (tools/bench/bench.sh). It runs gate.sh's package_image, so the steps are the gates':
#   package_image.sh <kickos-build> <cmake> <project-dir> <target> <out-dir> [<cmake-arg>...]
# <out-dir> must not exist; it is created and kept. The one line on stdout is the image.

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: package_image.sh <kickos-build> <cmake> <project-dir> <target> <out-dir> [<cmake-arg>...]"
[ "$#" -ge 5 ] || fail "$USAGE"
_build="$1"
_cmake="$2"
_project="$3"
_target="$4"
TMP="$5"
shift 5
[ ! -e "$TMP" ] || fail "$TMP exists; a reused prefix would install over another build's package"
mkdir -p "$TMP" || fail "cannot create $TMP"
exec 3>&1 1>&2
package_image "$_build" "$_cmake" "$_project" "$_target" "$@"
printf '%s\n' "$IMAGE" >&3
