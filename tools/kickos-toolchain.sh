#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Provision the KickOS toolchain (docs/design-m10-toolchain.md): build or restore every family
# for this host, then write the folder KICKOS_TOOLCHAIN names.
#
#   tools/kickos-toolchain.sh <output-folder> [family,family,...]
#   . <output-folder>/kickos-toolchain.sh
#
# The families default to every one conan/toolchains lists. KICKOS_TOOLCHAIN_SOURCES may name a
# folder holding the pinned archives, checked against their sha256 as a download is.
#
# POSIX sh (dash-clean).

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: kickos-toolchain.sh <output-folder> [family,family,...]" >&2
    exit 2
fi
OUT="$1"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALL=$(sed -n 's/^FAMILIES = (\(.*\))$/\1/p' "$ROOT/conan/toolchains/conanfile.py" \
    | tr -d '" ' | sed 's/,$//')
FAMILIES="${2:-$ALL}"

conan profile detect --exist-ok > /dev/null
for f in $(echo "$FAMILIES" | tr ',' ' '); do
    conan export "$ROOT/conan/toolchain" --name "kickos-toolchain-$f" > /dev/null
done
conan install "$ROOT/conan/toolchains" -o "&:families=$FAMILIES" --build=missing \
    --output-folder="$OUT"
echo "source $OUT/kickos-toolchain.sh, then configure"
