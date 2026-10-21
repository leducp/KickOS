#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The CI jobs a change can affect, as name=true|false lines for $GITHUB_OUTPUT, and each changed
# path with its class on stderr.
#
# usage: ci-scope.sh all | <a range git diff takes, A..B or A...B>
#
# Run it at the head of the range: a board's or a chip's family is read from the tree checked out.
#
#   tree       code-quality, for any change that is not to the docs alone
#   doc_gates  the tree gates that read the docs, for a change to the docs alone
#   host       sim and sim-ubsan, whose unit tests compile every family's arch code
#   arm, arm64, riscv, x86, xtensa, rx   the jobs that build that family's boards
#   bench      the microbenchmark job, which builds arm, arm64 and riscv boards

set -eu

FAMILIES="arm arm64 riscv x86 xtensa rx"

# The family directory under arch/ holding <isa>, or nothing.
family_of_isa() { # <isa>
    [ -n "$1" ] || return 0
    _f=$(git ls-files -- "arch/*/$1/*" | head -n1 | cut -d/ -f2)
    if [ -z "$_f" ] && [ -n "$(git ls-files -- "arch/$1/*" | head -n1)" ]; then
        _f="$1"
    fi
    printf '%s' "$_f"
}

# The class of one changed path: docs, toolchain, full, or the family whose jobs it reaches.
classify() { # <path>
    case "$1" in
        docs/*|*.md|mkdocs.yml|.github/workflows/docs.yml)
            echo docs; return ;;
        conan/toolchain/*|conan/toolchains/*|tools/kickos-toolchain.sh|tools/kickos-toolchain-release.sh|tools/check-x86_64-*.sh)
            echo toolchain; return ;;
        arch/*/*)
            _fam="${1#arch/}"
            _fam="${_fam%%/*}" ;;
        boards/*/*)
            _b="${1#boards/}"
            _b="${_b%%/*}"
            _isa=""
            if [ -f "boards/$_b/board.cmake" ]; then
                _isa=$(sed -n 's/^set(KICKOS_ARCH[[:space:]][[:space:]]*"\([^"]*\)").*/\1/p' "boards/$_b/board.cmake")
            fi
            _fam=$(family_of_isa "$_isa") ;;
        platform/*/*)
            _c="${1#platform/}"
            _c="${_c%%/*}"
            _isa=""
            if [ -f "platform/$_c/chip.yaml" ]; then
                _isa=$(sed -n 's/^arch:[[:space:]]*\([A-Za-z0-9_]*\).*/\1/p' "platform/$_c/chip.yaml")
            fi
            _fam=$(family_of_isa "$_isa") ;;
        *)
            echo full; return ;;
    esac
    case " sim $FAMILIES " in
        *" $_fam "*) echo "$_fam" ;;
        *) echo full ;;
    esac
}

[ "$#" -eq 1 ] || { echo "usage: ci-scope.sh all | <range>" >&2; exit 2; }

full=false
tree=false
host=false
families=" "
paths=$(mktemp)
trap 'rm -f "$paths"' EXIT

if [ "$1" = all ]; then
    echo "every job runs: no range to classify" >&2
    full=true
elif ! git -c core.quotepath=off diff --name-only --no-renames "$1" > "$paths"; then
    echo "every job runs: git cannot diff $1" >&2
    full=true
elif [ ! -s "$paths" ]; then
    echo "every job runs: $1 changes no file" >&2
    full=true
else
    while IFS= read -r p; do
        c=$(classify "$p")
        printf '%-9s %s\n' "$c" "$p" >&2
        case "$c" in
            docs) ;;
            full) full=true ;;
            toolchain)
                tree=true
                families=" $FAMILIES " ;;
            sim)
                tree=true
                host=true ;;
            *)
                tree=true
                host=true
                case "$families" in
                    *" $c "*) ;;
                    *) families="$families$c " ;;
                esac ;;
        esac
    done < "$paths"
fi

if [ "$full" = true ]; then
    tree=true
    host=true
    families=" $FAMILIES "
fi

doc_gates=true
if [ "$tree" = true ]; then
    doc_gates=false
fi
echo "tree=$tree"
echo "doc_gates=$doc_gates"
echo "host=$host"
bench=false
for f in $FAMILIES; do
    v=false
    case "$families" in
        *" $f "*) v=true ;;
    esac
    echo "$f=$v"
    case "$f" in
        arm|arm64|riscv)
            if [ "$v" = true ]; then
                bench=true
            fi ;;
    esac
done
echo "bench=$bench"
