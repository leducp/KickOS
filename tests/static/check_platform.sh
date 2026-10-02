#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The chip and board descriptions under platform/ and the golden compositions, read by the host
# tool in tools/compose, the tool's own arms, and a kernel build's export manifest. Run from the
# repo root:
#   tests/static/check_platform.sh <build-dir> descriptions|compositions|arms
#   tests/static/check_platform.sh <build-dir> manifest <manifest>
#   tests/static/check_platform.sh <build-dir> golden <manifest> <composition>
#   tests/static/check_platform.sh <build-dir> installed <tool-dir> <manifest> [<composition>]
#   tests/static/check_platform.sh <build-dir> tables <host-c-compiler> <generated-include-dir>
#   tests/static/check_platform.sh <build-dir> table <manifest> <composition> <c-compiler> <generated-include-dir>
#
#   descriptions  every tracked file under platform/, from `git ls-files`
#   compositions  every tracked file under examples/composition/systems/ and every board's default
#                 composition, admitted against platform/
#   arms          tools/compose/tests: each rule's minimal pair, built from copies of those files
#   manifest      one export manifest, and the chip and board files it names
#   golden        one composition, admitted against a build's manifest and the descriptions it names
#   installed     the tool an installed package carries, run from <tool-dir> on the manifest the
#                 package installs and, when given, the default composition installed beside it
#   tables        tools/compose/tests/round_trip.py: each golden's table emitted twice, compiled
#                 with the host compiler and walked, its dump the tool's own
#   table         one composition's table, emitted against that build's manifest and compiled with
#                 that build's C compiler, so the layout's assertions hold on its architecture
#
# uv runs the tool against its uv.lock as committed (`--locked` refuses a stale lock rather than
# rewriting it), with its environment, scratch and bytecode in <build-dir> and its downloads in
# uv's own cache, so a run writes nothing into the source tree. A missing uv FAILS: the
# descriptions are then unchecked, not clean.

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_platform.sh <build-dir> descriptions|compositions|arms|manifest|golden|installed|tables|table [<arguments>]"
[ "$#" -ge 2 ] || fail "$USAGE"
BUILD="$1"
MODE="$2"
case "$BUILD" in
    /*) ;;
    *) fail "<build-dir> must be absolute, not $BUILD" ;;
esac
case "$MODE" in
    descriptions|compositions|arms) [ "$#" -eq 2 ] || fail "$USAGE" ;;
    manifest) [ "$#" -eq 3 ] || fail "$USAGE" ;;
    golden) [ "$#" -eq 4 ] || fail "$USAGE" ;;
    installed) [ "$#" -eq 4 ] || [ "$#" -eq 5 ] || fail "$USAGE" ;;
    tables) [ "$#" -eq 4 ] || fail "$USAGE" ;;
    table) [ "$#" -eq 6 ] || fail "$USAGE" ;;
    *) fail "unknown mode $MODE, expected descriptions, compositions, arms, manifest, golden, installed, tables or table" ;;
esac

require_repo_root
ROOT="$(pwd)"
TOOL="$ROOT/tools/compose"
if [ "$MODE" = installed ]; then
    TOOL="$3"
    shift
fi
[ -f "$TOOL/uv.lock" ] || fail "no $TOOL/uv.lock; the tool runs against its lock"
command -v uv >/dev/null 2>&1 \
    || fail "uv not found on PATH; tools/compose runs under uv (https://docs.astral.sh/uv/)"

scratch_dir

STATE="$BUILD/compose/$MODE"
if [ "$MODE" = manifest ]; then
    MANIFEST="$3"
    [ -f "$MANIFEST" ] || fail "no manifest at $MANIFEST"
    # Its own environment per manifest, so two manifests checked at once share no venv.
    STATE="$BUILD/compose/manifest-$(printf '%s' "$MANIFEST" | cksum | cut -d ' ' -f 1)"
fi
if [ "$MODE" = golden ] || [ "$MODE" = installed ] || [ "$MODE" = table ]; then
    MANIFEST="$3"
    COMPOSITION="${4:-}"
    [ -f "$MANIFEST" ] || fail "no manifest at $MANIFEST"
    [ -z "$COMPOSITION" ] || [ -f "$COMPOSITION" ] || fail "no composition at $COMPOSITION"
fi
if [ "$MODE" = tables ]; then
    CC="$3"
    GENERATED="$4"
fi
if [ "$MODE" = table ]; then
    CC="$5"
    GENERATED="$6"
fi
if [ "$MODE" = tables ] || [ "$MODE" = table ]; then
    command -v "$CC" >/dev/null 2>&1 || fail "not an executable C compiler: $CC"
    [ -f "$GENERATED/kickos/sys/table_version.h" ] || fail "no generated kickos/sys/table_version.h under $GENERATED"
fi
mkdir -p "$STATE/tmp" || fail "cannot create $STATE in the build directory"
UV_PROJECT_ENVIRONMENT="$STATE/venv"
UV_PYTHON_DOWNLOADS=never
PYTHONPATH="$TOOL"
PYTHONDONTWRITEBYTECODE=1
TMPDIR="$STATE/tmp"
export UV_PROJECT_ENVIRONMENT UV_PYTHON_DOWNLOADS PYTHONPATH PYTHONDONTWRITEBYTECODE TMPDIR

tree_state() { # <outfile>
    git status --porcelain --ignored --untracked-files=all -- tools/compose platform examples/composition > "$1" \
        || fail "git status failed; whether this run wrote into the source tree is UNKNOWN"
}
tree_state "$TMP/before"

if [ "$MODE" = descriptions ] || [ "$MODE" = compositions ]; then
    if [ "$MODE" = descriptions ]; then
        corpus "$TMP/files" "file under platform/" platform
    else
        corpus "$TMP/files" "file under examples/composition/systems/ or default composition" \
            examples/composition/systems 'boards/*/composition.yaml'
    fi
    set --
    while IFS= read -r f; do
        set -- "$@" "$f"
    done < "$TMP/files"
    if [ "$MODE" = descriptions ]; then
        uv run --project "$TOOL" --locked --quiet python -m kickos_compose platform "$@"
    else
        uv run --project "$TOOL" --locked --quiet \
            python -m kickos_compose admit "$@" --platform "$ROOT/platform"
    fi
    rc=$?
elif [ "$MODE" = manifest ]; then
    uv run --project "$TOOL" --locked --quiet python -m kickos_compose manifest "$MANIFEST"
    rc=$?
elif [ "$MODE" = golden ]; then
    uv run --project "$TOOL" --locked --quiet python -m kickos_compose admit "$COMPOSITION" --manifest "$MANIFEST"
    rc=$?
elif [ "$MODE" = installed ]; then
    uv run --project "$TOOL" --locked --quiet python -m kickos_compose manifest "$MANIFEST"
    rc=$?
    if [ "$rc" -eq 0 ] && [ -n "$COMPOSITION" ]; then
        uv run --project "$TOOL" --locked --quiet python -m kickos_compose admit "$COMPOSITION" --manifest "$MANIFEST"
        rc=$?
    fi
elif [ "$MODE" = tables ]; then
    uv run --project "$TOOL" --locked --quiet \
        python "$TOOL/tests/round_trip.py" "$CC" "$GENERATED"
    rc=$?
elif [ "$MODE" = table ]; then
    uv run --project "$TOOL" --locked --quiet \
        python -m kickos_compose emit "$COMPOSITION" --manifest "$MANIFEST" -o "$TMP/table.c"
    rc=$?
    if [ "$rc" -eq 0 ]; then
        "$CC" -std=c11 -pedantic-errors -Wall -Wextra -Werror -I"$ROOT/user/include" -I"$ROOT/system/include" \
            -I"$GENERATED" -c "$TMP/table.c" -o "$TMP/table.o"
        rc=$?
    fi
else
    uv run --project "$TOOL" --locked --quiet \
        python -m unittest discover -s "$TOOL/tests"
    rc=$?
fi

tree_state "$TMP/after"
if ! cmp -s "$TMP/before" "$TMP/after"; then
    diff "$TMP/before" "$TMP/after" >&2
    fail "the run changed the source tree under tools/compose, platform or examples/composition"
fi
[ "$rc" -eq 0 ] || fail "tools/compose refused (exit $rc), see above"
echo "PASS: $MODE"
