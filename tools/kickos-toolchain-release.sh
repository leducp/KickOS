#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Assemble the files of the KickOS toolchain's GitHub release (docs/design-m10-toolchain.md
# section 1), for the maintainer to upload under the tag toolchain-<version>:
#
#   tools/kickos-toolchain-release.sh <output-folder> [family,family,...]
#
# Every source conan/toolchain/conandata.yml pins, each patch copied from conan/toolchain/patches
# and every archive taken from KICKOS_TOOLCHAIN_SOURCES where it holds the file, else from its
# upstream, else from the release itself, and checked against its sha256. Then one archive per family of the packages this host's Conan cache holds, which
# tools/kickos-toolchain.sh builds, named kickos-toolchain-<family>-<os>-<arch>.tgz; the families
# default to every one, and an empty list saves none. Each host that publishes runs it into the
# same folder, or leaves its archives there; SHA256SUMS lists every file there.
#
# POSIX sh (dash-clean).

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: kickos-toolchain-release.sh <output-folder> [family,family,...]" >&2
    exit 2
fi
OUT="$1"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ALL=$(sed -n '/^FAMILIES = (/,/)$/p' "$ROOT/conan/toolchains/conanfile.py" | tr -d '\n" ' \
    | sed 's/^FAMILIES=(\(.*\))$/\1/; s/,$//')
FAMILIES="${2-$ALL}"
VERSION=$(sed -n 's/^    version = "\(.*\)"$/\1/p' "$ROOT/conan/toolchain/conanfile.py")
RELEASE="https://github.com/leducp/KickOS/releases/download/toolchain-$VERSION"
mkdir -p "$OUT"

sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# One line per source: its archive's name, its upstream URL or "-", its sha256.
awk '
    function emit() { if (key != "") print (name != "" ? name : base), url, sha }
    /^  [A-Za-z0-9_-]+:$/ { emit(); key = $1; url = "-"; name = ""; base = ""; sha = "" }
    /^    url:/ { url = $2; gsub(/"/, "", url); base = url; sub(/.*\//, "", base) }
    /^    filename:/ { name = $2; gsub(/"/, "", name) }
    /^    sha256:/ { sha = $2; gsub(/"/, "", sha) }
    END { emit() }
' "$ROOT/conan/toolchain/conandata.yml" | while read -r name url sha; do
    if [ -f "$ROOT/conan/toolchain/patches/$name" ]; then
        cp "$ROOT/conan/toolchain/patches/$name" "$OUT/$name"
    elif [ -n "${KICKOS_TOOLCHAIN_SOURCES:-}" ] && [ -f "$KICKOS_TOOLCHAIN_SOURCES/$name" ]; then
        cp "$KICKOS_TOOLCHAIN_SOURCES/$name" "$OUT/$name"
    elif ! { [ "$url" != "-" ] && curl -fsSL -o "$OUT/$name" "$url"; } \
            && ! curl -fsSL -o "$OUT/$name" "$RELEASE/$name"; then
        echo "$name is neither upstream nor in the release: put it in KICKOS_TOOLCHAIN_SOURCES" >&2
        exit 1
    fi
    got=$(sha256 "$OUT/$name")
    if [ "$got" != "$sha" ]; then
        echo "$name has sha256 $got, conandata.yml pins $sha" >&2
        exit 1
    fi
done

HOST="$(uname -s)-$(uname -m)"
for f in $(echo "$FAMILIES" | tr ',' ' '); do
    conan cache save "kickos-toolchain-$f/$VERSION:*" --no-source \
        --file "$OUT/kickos-toolchain-$f-$HOST.tgz" > /dev/null
done

(cd "$OUT" && for file in *; do
    [ "$file" = SHA256SUMS ] || echo "$(sha256 "$file")  $file"
done > SHA256SUMS)
echo "upload every file in $OUT to the release toolchain-$VERSION"
