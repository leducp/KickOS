#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Assemble the files of the KickOS toolchain's GitHub release (docs/design-m10-toolchain.md
# section 1), for the maintainer to upload under the tag toolchain-<version>:
#
#   tools/kickos-toolchain-release.sh <output-folder> [family,family,...]
#
# One archive of the sources, kickos-toolchain-sources-<version>.tar.xz at xz -9e: every source
# conan/toolchain/conandata.yml pins, taken from KICKOS_TOOLCHAIN_SOURCES where it holds the
# file, else from its upstream, and checked against its sha256, and each patch from
# conan/toolchain/patches. A source that conandata.yml gives a tar_sha256 and a `stored` name is
# stored under that name, as xz, checked by the hash of its uncompressed tar, which the upstream
# file's gzip stream reproduces; the recipe reads the same `stored`.
# TAR-SHA256 and SHA256SUMS inside the archive list them. Then one
# archive per family of the packages this host's Conan cache holds, which tools/kickos-toolchain.sh
# builds, named kickos-toolchain-<family>-<os>-<arch>.tgz; the families default to every one, and
# an empty list saves none. Each host that publishes runs it into the same folder, or leaves its
# archives there; SHA256SUMS lists every file there.
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
mkdir -p "$OUT"

sha256() {
    if command -v sha256sum > /dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
TOP="kickos-toolchain-sources-$VERSION"
mkdir -p "$STAGE/$TOP"

# One line per source: its archive's name, its upstream URL or "-", its sha256, its tar sha256 or
# "-", the name the sources archive holds it under (conandata.yml's `stored`, else the archive's).
awk '
    function emit() {
        if (key == "") { return }
        if (name == "") { name = base }
        if (stored == "") { stored = name }
        print name, url, sha, tsha, stored
    }
    /^  [A-Za-z0-9_-]+:$/ {
        emit(); key = $1; url = "-"; name = ""; base = ""; sha = ""; tsha = "-"; stored = ""
    }
    /^    url:/ { url = $2; gsub(/"/, "", url); base = url; sub(/.*\//, "", base) }
    /^    filename:/ { name = $2; gsub(/"/, "", name) }
    /^    sha256:/ { sha = $2; gsub(/"/, "", sha) }
    /^    tar_sha256:/ { tsha = $2; gsub(/"/, "", tsha) }
    /^    stored:/ { stored = $2; gsub(/"/, "", stored) }
    END { emit() }
' "$ROOT/conan/toolchain/conandata.yml" > "$STAGE/sources.txt"

: > "$STAGE/$TOP/SHA256SUMS"
: > "$STAGE/$TOP/TAR-SHA256"
while read -r name url sha tsha stored; do
    file="$STAGE/$name"
    if [ -f "$ROOT/conan/toolchain/patches/$name" ]; then
        cp "$ROOT/conan/toolchain/patches/$name" "$file"
    elif [ -n "${KICKOS_TOOLCHAIN_SOURCES:-}" ] && [ -f "$KICKOS_TOOLCHAIN_SOURCES/$name" ]; then
        cp "$KICKOS_TOOLCHAIN_SOURCES/$name" "$file"
    elif ! { [ "$url" != "-" ] && curl -fsSL -o "$file" "$url"; }; then
        echo "$name is not upstream: put it in KICKOS_TOOLCHAIN_SOURCES" >&2
        exit 1
    fi
    got=$(sha256 "$file")
    if [ "$got" != "$sha" ]; then
        echo "$name has sha256 $got, conandata.yml pins $sha" >&2
        exit 1
    fi
    if [ "$tsha" != "-" ]; then
        gzip -dc "$file" > "$STAGE/tar"
        got=$(sha256 "$STAGE/tar")
        if [ "$got" != "$tsha" ]; then
            echo "$name holds a tar of sha256 $got, conandata.yml pins $tsha" >&2
            exit 1
        fi
        xz -9e -T0 -c "$STAGE/tar" > "$STAGE/$TOP/$stored"
        rm -f "$STAGE/tar"
        echo "$tsha  $stored" >> "$STAGE/$TOP/TAR-SHA256"
    else
        mv "$file" "$STAGE/$TOP/$stored"
    fi
    rm -f "$file"
    echo "$(sha256 "$STAGE/$TOP/$stored")  $stored" >> "$STAGE/$TOP/SHA256SUMS"
done < "$STAGE/sources.txt"

# Not a pipe: a failing tar must fail the script.
tar -C "$STAGE" -cf "$STAGE/sources.tar" "$TOP"
xz -9e -T0 -c "$STAGE/sources.tar" > "$OUT/$TOP.tar.xz"
xz -t "$OUT/$TOP.tar.xz"
expected=$(cd "$STAGE" && find "$TOP" | wc -l)
listed=$(xz -dc "$OUT/$TOP.tar.xz" | tar -tf - | wc -l)
if [ "$listed" -ne "$expected" ]; then
    echo "$TOP.tar.xz lists $listed entries, expected $expected" >&2
    exit 1
fi
rm -f "$STAGE/sources.tar"

HOST="$(uname -s)-$(uname -m)"
for f in $(echo "$FAMILIES" | tr ',' ' '); do
    conan cache save "kickos-toolchain-$f/$VERSION:*" --no-source \
        --file "$OUT/kickos-toolchain-$f-$HOST.tgz" > /dev/null
done

(cd "$OUT" && for file in *; do
    [ "$file" = SHA256SUMS ] || echo "$(sha256 "$file")  $file"
done > SHA256SUMS)
echo "upload every file in $OUT to the release toolchain-$VERSION"
