#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The main checkout the gate sweeps read their GTest prefix from (tools/sweep-common.sh), over
# planted repositories: from the main tree, from a linked worktree, from a bare repository's
# worktree and from no repository, under this git and under one that predates
# `rev-parse --path-format` and echoes it back.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
. tools/sweep-common.sh

for sweep in tools/sweep_host_gates.sh tools/sweep_image_gates.sh; do
    # shellcheck disable=SC2016
    grep -qxF 'MAIN="$(main_checkout "$ROOT")"' "$sweep" \
        || bad "$sweep does not take its main checkout from main_checkout"
done

GIT_CONFIG_NOSYSTEM=1
GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM GIT_CONFIG_GLOBAL
REAL_GIT="$(command -v git)" || fail "no git on PATH"
g() {
    "$REAL_GIT" -c user.name=planted -c user.email=planted@invalid -c init.defaultBranch=main "$@" \
        > /dev/null 2>&1 || fail "planting failed: git $*"
}
g init -q "$TMP/main"
g -C "$TMP/main" commit -q --allow-empty -m planted
g -C "$TMP/main" worktree add -q "$TMP/linked"
g clone -q --bare "$TMP/main" "$TMP/bare.git"
g -C "$TMP/bare.git" worktree add -q "$TMP/barewt"
mkdir -p "$TMP/plain"
P="$(cd "$TMP" && pwd -P)"

mkdir -p "$TMP/oldgit"
cat > "$TMP/oldgit/git" <<STUB
#!/bin/sh
# rev-parse before git 2.31 prints an option it does not know as a revision.
for a in "\$@"; do
    case "\$a" in
        --path-format=*)
            echo "\$a"
            ;;
    esac
done
n=\$#
while [ "\$n" -gt 0 ]; do
    case "\$1" in
        --path-format=*)
            ;;
        *)
            set -- "\$@" "\$1"
            ;;
    esac
    shift
    n=\$((n - 1))
done
exec "$REAL_GIT" "\$@"
STUB
chmod +x "$TMP/oldgit/git"
PATH="$TMP/oldgit:$PATH" git -C "$TMP/main" rev-parse --path-format=absolute --git-common-dir \
    | grep -qx -- '--path-format=absolute' || fail "the old-git stub does not echo --path-format"

# <git label> <tree> <expected>
expect() {
    got="$(main_checkout "$2")"
    [ "$got" = "$3" ] || bad "under $1 git, the main checkout of $2 is [$got], not $3"
}
for label in this old; do
    if [ "$label" = old ]; then
        PATH="$TMP/oldgit:$PATH"
    fi
    expect "$label" "$P/main" "$P/main"
    expect "$label" "$P/linked" "$P/main"
    expect "$label" "$P/barewt" "$P/barewt"
    expect "$label" "$P/plain" "$P/plain"
done

[ "$rc" -eq 0 ] || exit 1
echo "PASS: the gate sweeps find the main checkout from every tree layout, on any git"
