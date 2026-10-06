#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The chip and board descriptions under platform/ and the golden compositions, read by the host
# tool in tools/compose, the tool's own arms, and a kernel build's export manifest. Run from the
# repo root:
#   tests/static/check_platform.sh <build-dir> descriptions|compositions|arms|coverage
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
#   coverage      every board directory, from `git ls-files`, names a chip and has a board file
#                 beside that chip's chip file and a default composition, every chip backend under
#                 arch/ but a family directory has a chip file, every board file names a board on
#                 its chip and every chip file a chip some board names; each rule is run again with
#                 one file or one board's chip changed, and must refuse it
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

USAGE="usage: check_platform.sh <build-dir> descriptions|compositions|arms|coverage|manifest|golden|installed|tables|table [<arguments>]"
[ "$#" -ge 2 ] || fail "$USAGE"
BUILD="$1"
MODE="$2"
case "$BUILD" in
    /*) ;;
    *) fail "<build-dir> must be absolute, not $BUILD" ;;
esac
case "$MODE" in
    descriptions|compositions|arms|coverage) [ "$#" -eq 2 ] || fail "$USAGE" ;;
    manifest) [ "$#" -eq 3 ] || fail "$USAGE" ;;
    golden) [ "$#" -eq 4 ] || fail "$USAGE" ;;
    installed) [ "$#" -eq 4 ] || [ "$#" -eq 5 ] || fail "$USAGE" ;;
    tables) [ "$#" -eq 4 ] || fail "$USAGE" ;;
    table) [ "$#" -eq 6 ] || fail "$USAGE" ;;
    *) fail "unknown mode $MODE, expected descriptions, compositions, arms, coverage, manifest, golden, installed, tables or table" ;;
esac

require_repo_root
ROOT="$(pwd)"

# The findings of the coverage rules, one per line, over a list of tracked paths, the boards as
# `<board> <chip>` lines (`-` for a board naming none) and the chip family directories.
coverage_findings() { # <paths> <boards> <families>
    while read -r _cf_board _cf_chip; do
        if [ "$_cf_chip" = - ]; then
            echo "boards/$_cf_board/board.cmake names no chip"
            continue
        fi
        grep -qxF "platform/$_cf_chip/chip.yaml" "$1" \
            || echo "boards/$_cf_board/board.cmake names chip $_cf_chip, which has no platform/$_cf_chip/chip.yaml"
        grep -qxF "platform/$_cf_chip/$_cf_board.yaml" "$1" \
            || echo "board $_cf_board has no board file platform/$_cf_chip/$_cf_board.yaml"
        grep -qxF "boards/$_cf_board/composition.yaml" "$1" \
            || echo "board $_cf_board has no default composition boards/$_cf_board/composition.yaml"
    done < "$2"
    sed -n 's|^arch/[^/]*/chip/\([^/]*\)/.*|\1|p' "$1" | sort -u | while read -r _cf_backend; do
        grep -qxF "$_cf_backend" "$3" && continue
        grep -qxF "platform/$_cf_backend/chip.yaml" "$1" \
            || echo "chip backend $_cf_backend under arch/ has no platform/$_cf_backend/chip.yaml"
    done
    grep '^platform/[^/]*/[^/]*\.yaml$' "$1" | while IFS= read -r _cf_file; do
        _cf_chip="${_cf_file#platform/}"
        _cf_chip="${_cf_chip%%/*}"
        _cf_name="${_cf_file##*/}"
        _cf_name="${_cf_name%.yaml}"
        if [ "$_cf_name" = chip ]; then
            awk -v c="$_cf_chip" '$2 == c { found = 1 } END { exit !found }' "$2" \
                || echo "$_cf_file is the chip file of no board"
        elif ! awk -v b="$_cf_name" '$1 == b { found = 1 } END { exit !found }' "$2"; then
            echo "$_cf_file names board $_cf_name, which has no boards/$_cf_name/board.cmake"
        elif ! awk -v b="$_cf_name" -v c="$_cf_chip" '$1 == b && $2 == c { found = 1 } END { exit !found }' "$2"; then
            echo "$_cf_file sits beside chip $_cf_chip, and boards/$_cf_name/board.cmake names another chip"
        fi
    done
}

if [ "$MODE" = coverage ]; then
    scratch_dir
    corpus "$TMP/files" "board, chip backend, description or default composition" \
        'boards/*/board.cmake' 'boards/*/composition.yaml' 'platform/*/*.yaml' 'arch/*/chip/*/*'
    grep '^boards/[^/]*/board\.cmake$' "$TMP/files" | while IFS= read -r desc; do
        board="${desc#boards/}"
        chip=$(sed -n 's/^set(KICKOS_CHIP[[:space:]]*"\([^"]*\)").*/\1/p' "$desc")
        echo "${board%/board.cmake} ${chip:--}"
    done > "$TMP/boards"
    grep '/family\.cmake$' "$TMP/files" | while IFS= read -r family; do
        sed -n 's|^set(KICKOS_CHIP_FAMILY_DIR "${CMAKE_CURRENT_LIST_DIR}/\.\./\([^"]*\)").*|\1|p' "$family"
    done | sort -u > "$TMP/families"
    [ -s "$TMP/families" ] || fail "no family.cmake names a chip family directory, so every family would read as a chip"
    coverage_findings "$TMP/files" "$TMP/boards" "$TMP/families" > "$TMP/found"
    if [ -s "$TMP/found" ]; then
        sed 's/^/FAIL: /' "$TMP/found" >&2
        fail "a board or a chip is not described, see above"
    fi
    read -r board chip < "$TMP/boards"
    other=$(awk -v c="$chip" '$2 != c && $2 != "-" { print $2; exit }' "$TMP/boards")
    # Each arm: <what is changed>|<paths edit>|<boards edit>|<the finding it must produce>.
    arms=0
    while IFS='|' read -r label paths_edit boards_edit want; do
        cp "$TMP/files" "$TMP/arm_files"
        cp "$TMP/boards" "$TMP/arm_boards"
        case "$paths_edit" in
            -*) grep -vxF "${paths_edit#-}" "$TMP/files" > "$TMP/arm_files" ;;
            +*) echo "${paths_edit#+}" >> "$TMP/arm_files" ;;
        esac
        [ -n "$boards_edit" ] && awk -v b="$board" -v c="$boards_edit" '$1 == b { $2 = c } { print }' \
            "$TMP/boards" > "$TMP/arm_boards"
        coverage_findings "$TMP/arm_files" "$TMP/arm_boards" "$TMP/families" > "$TMP/arm_found"
        grep -qF "$want" "$TMP/arm_found" || fail "the coverage rules accept the tree with $label"
        arms=$((arms + 1))
    done <<ARMS
platform/$chip/chip.yaml dropped|-platform/$chip/chip.yaml||has no platform/$chip/chip.yaml
the board file of $board dropped|-platform/$chip/$board.yaml||has no board file
the default composition of $board dropped|-boards/$board/composition.yaml||has no default composition
a chip backend with no chip file|+arch/arm/chip/kickos_no_chip/chip_kickos_no_chip.cc||chip backend kickos_no_chip
a board file naming no board|+platform/$chip/kickos_no_board.yaml||names board kickos_no_board
a chip file no board uses|+platform/kickos_no_board/chip.yaml||is the chip file of no board
boards/$board/board.cmake naming chip $other||$other|names another chip
boards/$board/board.cmake naming no chip||-|names no chip
ARMS
    boards=$(wc -l < "$TMP/boards")
    echo "PASS: coverage ($boards board(s) described with a default, $arms arm(s) refused)"
    exit 0
fi
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
