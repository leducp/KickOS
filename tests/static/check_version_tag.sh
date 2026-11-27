#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The version cmake/kickos_version.cmake reads out of git, over planted repositories: the highest
# valid tag merged into HEAD wins over every invalid one, a tree with no tag or nested in another
# checkout is `unknown`, a version past the banner's width fails the configure, and HEAD and the
# branch it names are configure inputs.

set -u
. "$(dirname "$0")/../lib/gate.sh"

if [ ! -e .git ] || ! command -v git > /dev/null; then
    echo "SKIP: no git checkout here to read a version out of"
    exit 77
fi
require_repo_root
scratch_dir
rc=0
PROBE="$PWD/tests/static/version_probe.cmake"
export HOME="$TMP" GIT_CONFIG_NOSYSTEM=1

g() { # <repo> <git args>...
    _g_repo=$1
    shift
    git -C "$_g_repo" -c user.name=kickos -c user.email=kickos@invalid -c commit.gpgsign=false \
        -c tag.gpgsign=false "$@" > /dev/null 2>&1 || fail "git $* in $_g_repo"
}
repo() { # <dir>
    mkdir -p "$1"
    g "$1" init -q
    g "$1" commit -q --allow-empty -m one
}
probe() { # <source dir>
    cmake -DSOURCE="$1" -P "$PROBE" 2>&1
}
version_is() { # <source dir> <version> <package>
    _vi=$(probe "$1" | tail -n 1)
    case "$_vi" in
        "$2|$3|"*) ;;
        *) bad "$1 reads [$_vi], not version $2 package $3" ;;
    esac
}

repo "$TMP/none"
version_is "$TMP/none" unknown 0.0.0

repo "$TMP/tags"
g "$TMP/tags" tag v0.9.0
g "$TMP/tags" commit -q --allow-empty -m two
g "$TMP/tags" tag v0.10.0-rc1
version_is "$TMP/tags" 0.10.0-rc1 0.10.0
case "$(probe "$TMP/tags")" in
    *"/HEAD"*) ;;
    *) bad "HEAD is not a configure input of $TMP/tags" ;;
esac
case "$(probe "$TMP/tags")" in
    *"/refs/heads/"*) ;;
    *) bad "the branch HEAD names is not a configure input of $TMP/tags" ;;
esac

# Nearer than v0.10.0-rc1, every one invalid.
g "$TMP/tags" commit -q --allow-empty -m three
g "$TMP/tags" tag 'v1.2.3"x'
g "$TMP/tags" tag 'v1.2.4_x'
g "$TMP/tags" commit -q --allow-empty -m four
g "$TMP/tags" tag 'v1.2'
g "$TMP/tags" tag 'v1.2.x'
g "$TMP/tags" tag 'v1.2.6;x'
version_is "$TMP/tags" 0.10.0-rc1 0.10.0
# More invalid tags than any bounded retry would pass over.
for _n in $(seq 1 40); do
    g "$TMP/tags" tag "v2.0.${_n}_x"
done
version_is "$TMP/tags" 0.10.0-rc1 0.10.0

repo "$TMP/long"
g "$TMP/long" tag v0.10.0-abcdefghij
if _l=$(probe "$TMP/long"); then
    bad "a 17-character version configures: $_l"
elif ! tr -s '[:space:]' ' ' <<< "$_l" | grep -qF '16-character BANNER_IDENTITY_VERSION_MAX'; then
    bad "a 17-character version fails, but not naming the banner's width: $_l"
fi
repo "$TMP/fits"
g "$TMP/fits" tag v0.10.0-abcdefghi
version_is "$TMP/fits" 0.10.0-abcdefghi 0.10.0
# A tree nested in another checkout takes none of its tags.
mkdir -p "$TMP/fits/nested"
version_is "$TMP/fits/nested" unknown 0.0.0

if [ "$rc" -eq 0 ]; then
    echo "PASS: the version is the highest valid tag merged into the tree's own HEAD, refused past"
    echo "  the banner's width"
fi
exit "$rc"
