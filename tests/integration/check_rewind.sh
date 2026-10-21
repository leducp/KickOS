#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The rewind witness: a thread the switcher takes off the core inside kickos_user_cas's window
# resumes at the routine's load. A kernel built with KICKOS_WITNESS_REWIND counts each rewind
# that moves a stacked PC and answers the count (KOS_SYS_WITNESS_REWINDS); this gate builds that
# kernel in a scratch tree of the preset and boots `ucasrewind`, whose two round-robin adders on
# one word must see at least one such rewind and end with the word equal to their adds. Each of
# two faults turns it red:
#   - a switcher that skips the rewind: the count stays at zero;
#   - a rewind to the wrong place: a resumed store lands over the peer's adds, or the thread
#     runs off into the routine's caller.
# On QEMU an interrupt is taken at a translation-block start, so the only PC in the window a
# switch finds is the store; a lower bound made wrong stays green there.
#
# The knob is refused without the marker only this script passes. The control below holds that
# refusal.
#
#   check_rewind.sh <kickos-source-dir> <cmake> <preset> <scratch-dir>
#   KOS_CAPTURE=<log> check_rewind.sh <board-build> <kickos-source> <cmake>
set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

if judging_capture; then
    judge_capture ucasrewind
else
    _usage="usage: check_rewind.sh <kickos-source-dir> <cmake> <preset> <scratch-dir>"
    SRC="${1:?$_usage}"
    CMAKE="${2:?$_usage}"
    PRESET="${3:?$_usage}"
    SCRATCH="${4:?$_usage}"
    need_qemu_machine
    scratch_dir
    MARKER=check_rewind.sh

    if "$CMAKE" -S "$SRC" -B "$TMP/refused" --preset "$PRESET" -DKICKOS_WITNESS_REWIND=ON \
           -DKICKOS_BUILD_TESTS=OFF > "$TMP/refused.log" 2>&1; then
        fail "a configure with KICKOS_WITNESS_REWIND and no marker was accepted"
    fi
    if ! grep -q 'WITNESS_REWIND_REFUSED' "$TMP/refused.log"; then
        sed -n '1,30p' "$TMP/refused.log" >&2
        fail "the unmarked configure failed, but not on the refusal"
    fi

    # Kept under the caller's build directory. A cache that names another source tree, or lacks
    # the knob, is not this witness's kernel.
    _cache="$SCRATCH/CMakeCache.txt"
    if ! { [ -f "$_cache" ] && grep -q '^KICKOS_WITNESS_REWIND:BOOL=ON$' "$_cache" \
           && grep -qxF -e "KICKOS_WITNESS_REWIND_SCRATCH:UNINITIALIZED=$MARKER" "$_cache" \
           && grep -qxF -e "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$_cache"; }; then
        rm -rf "$SCRATCH"
        "$CMAKE" -S "$SRC" -B "$SCRATCH" --preset "$PRESET" -DKICKOS_WITNESS_REWIND=ON \
            -DKICKOS_WITNESS_REWIND_SCRATCH="$MARKER" -DKICKOS_BUILD_TESTS=OFF \
            > "$TMP/cfg.log" 2>&1 \
            || { sed -n '1,40p' "$TMP/cfg.log" >&2; fail "configure of the witness kernel failed for $PRESET"; }
    fi
    "$CMAKE" --build "$SCRATCH" --target ucasrewind > "$TMP/build.log" 2>&1 \
        || { grep -E 'error' "$TMP/build.log" | sed -n '1,20p' >&2; fail "build of ucasrewind failed in $SCRATCH"; }
    run_image "$SCRATCH/user/apps/common/ucasrewind/ucasrewind"
fi

if has_f "[rewind] ERROR"; then
    printf '%s\n' "$OUT" | grep -F -e '[rewind] ERROR' >&2
    cfail rewind "the run reported the failure above"
fi
assert_no_panic "the run panicked"
require_on_wire "   witness rewind" "the banner does not say this is the witness kernel" banner
require_on_wire "REWIND PASS" "the run never reached PASS" pass
status_clause "the system exited 0 once main returned past PASS" 0
echo "PASS: a switch inside the user compare-and-swap's window resumed the thread at its load"
exit 0
