#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The rig side of the bench chain: the handful of values that describe THIS box and
# THIS bench rather than the project. Sourced by every script in tools/bench/.
#
# The boundary this file exists to hold: the flash-and-capture ORDER per board class,
# every refusal, the TAP validation and the service-list coverage derivation are
# project knowledge and live in the tracked scripts. Which physical cable is on which
# board, where the logs go, what the bench host is called and where a python carrying
# pyserial lives are not, and live in a gitignored file this reads.
#
# A TRACKED SCRIPT NEVER GUESSES A RIG VALUE. Where a value is required and absent it
# refuses, naming the key. In particular it does NOT fall back to a vendor-pattern
# glob for a console: this bench carries other people's FTDIs and CP210x cables, so
# `the first FT232` is a different cable on a different day and the capture that
# follows is a plausible-looking log of the wrong board.
#
# Resolution order:
#   $KICKOS_RIG                 explicit path, and the only form that works from a
#                               git worktree, which has no .session/ at all.
#   <root>/.session/rig.conf    <root> being the tree the caller is working in.
#
# tools/bench/rig.conf.example is the committed template.

# rig_find <root>  sets RIG_CONF to where the config is or would be, and sources it if
# it is there. Returns 1 when it is not, so a caller that needs nothing from it can carry
# on and still name the file in its own refusal.
rig_find() {
    _rig_root="${1:?rig_find needs the tree root}"
    RIG_CONF="${KICKOS_RIG:-$_rig_root/.session/rig.conf}"
    [ -f "$RIG_CONF" ] || return 1
    . "$RIG_CONF"
}

# rig_load <root>  rig_find, or refuse. For a caller that cannot run without it.
rig_load() {
    if ! rig_find "$1"; then
        {
            echo "REFUSING: no rig config at $RIG_CONF"
            echo "  The recipe is tracked; the rig values are not. Copy"
            echo "  tools/bench/rig.conf.example to $1/.session/rig.conf and fill it"
            echo "  in (.session/ is gitignored), or point KICKOS_RIG at one. A git worktree"
            echo "  has no .session/ of its own, so from one you must pass KICKOS_RIG."
        } >&2
        exit 2
    fi
}

# rig_need <VAR> <what it is>  refuses unless VAR is set and non-empty.
#
# It validates rather than echoes on purpose. Written as `X=$(rig_need ...)` the exit
# below would leave the command substitution's subshell and the caller would sail on
# with an empty value, which is the guess this whole file exists to prevent.
rig_need() {
    eval "_rig_v=\${$1:-}"
    if [ -z "$_rig_v" ]; then
        {
            echo "REFUSING: $1 is unset in $RIG_CONF: $2"
            echo "  That names something about this rig, so no tracked default can stand in"
            echo "  for it. See tools/bench/rig.conf.example."
        } >&2
        exit 2
    fi
}
