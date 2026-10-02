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
# The families default to every one conan/toolchains lists. A family whose package the release
# toolchain-<version> holds for this host is restored from it, and built otherwise;
# KICKOS_TOOLCHAIN_NO_RELEASE set skips the release, and KICKOS_TOOLCHAIN_NO_BUILD set fails where
# neither the cache nor the release holds a family. KICKOS_TOOLCHAIN_SOURCES may name a folder
# holding the pinned archives, checked against their sha256 as a download is.
#
# POSIX sh (dash-clean).

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: kickos-toolchain.sh <output-folder> [family,family,...]" >&2
    exit 2
fi
OUT="$1"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALL=$(sed -n '/^FAMILIES = (/,/)$/p' "$ROOT/conan/toolchains/conanfile.py" | tr -d '\n" ' \
    | sed 's/^FAMILIES=(\(.*\))$/\1/; s/,$//')
FAMILIES="${2:-$ALL}"

VERSION=$(sed -n 's/^    version = "\(.*\)"$/\1/p' "$ROOT/conan/toolchain/conanfile.py")
RELEASE="https://github.com/leducp/KickOS/releases/download/toolchain-$VERSION"
HOST="$(uname -s)-$(uname -m)"

conan profile detect --exist-ok > /dev/null
# Restore before export: the export then makes this tree's recipe the latest revision, and a
# release package built from another one is left unused rather than taken for it.
for f in $(echo "$FAMILIES" | tr ',' ' '); do
    archive="$OUT/kickos-toolchain-$f-$HOST.tgz"
    mkdir -p "$OUT"
    if [ -z "${KICKOS_TOOLCHAIN_NO_RELEASE:-}" ] \
            && curl -fsSL -o "$archive" "$RELEASE/kickos-toolchain-$f-$HOST.tgz"; then
        conan cache restore "$archive" > /dev/null
    fi
    rm -f "$archive"
    conan export "$ROOT/conan/toolchain" --name "kickos-toolchain-$f" > /dev/null
done
BUILD=missing
[ -z "${KICKOS_TOOLCHAIN_NO_BUILD:-}" ] || BUILD=never
conan install "$ROOT/conan/toolchains" -o "&:families=$FAMILIES" --build="$BUILD" \
    --output-folder="$OUT"
echo "source $OUT/kickos-toolchain.sh, then configure"
