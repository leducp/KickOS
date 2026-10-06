# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sourced by the gate sweeps, never executed.

# main_checkout <root>: the main working tree of the repository <root> is in, or <root> itself
# where there is none (a bare repository's worktree, or no repository at all).
main_checkout() {
    _mc="$(LC_ALL=C git -C "$1" worktree list --porcelain 2>/dev/null | awk '
        /^$/ { exit }
        NR == 1 && sub(/^worktree /, "") { path = $0 }
        $0 == "bare" { path = "" }
        END { print path }')"
    if [ -n "$_mc" ] && [ -d "$_mc" ]; then
        printf '%s\n' "$_mc"
    else
        printf '%s\n' "$1"
    fi
}
