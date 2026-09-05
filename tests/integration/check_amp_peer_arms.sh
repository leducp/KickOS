#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Runs the three arms that need a peer that is running against one: amp_far_call,
# amp_far_reply_guard, and the ring half of amp_window. Each decides at RUNTIME off the peer's
# own serviced count.
#
# usage: check_amp_peer_arms.sh <unused.elf> <cmake> <build-dir> <artefact>
#
# The own-image posture is ALLOWED to skip amp_far_call and amp_far_reply_guard
# (KICKOS_EXPECT_SKIPS), because the same image runs standalone and inside a merged partition
# and only the second has a peer. This gate is what bounds that permission: here the arms must
# report `ok` and must NOT be skipped, since a runtime skip that never stops skipping is a
# lapsed arm wearing a permission.
#
# A two-kernel gate draws fresh every run on the console interleaving, on which kernel reaches
# its first publication first, and on which is inside a masked handler when the other rings, so
# a change to this gate or to what it boots owes TEN green runs before it is believed:
#
#   for i in $(seq 1 10); do ctest --test-dir <build> -R 'amp_partition|amp_peer_arms' || break; done
#
# It may not join a `--repeat until-pass` set: a retry masks exactly the class of defect this
# vehicle exists to find.
#
# AND IT MAY NOT RUN UNDER `ctest -j`. It begins by running `cmake --build`, and two concurrent
# ninja invocations on ONE build directory race on the intermediates they share, leaving
# kernel/libkickos_kernel.a truncated mid-archive. The tell is a gate failing in about a tenth
# of a second, far too fast to have built or booted anything. RUN_SERIAL is set where these are
# registered (user/apps/common/ampping/CMakeLists.txt); a gate copied from this one owes the same.

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"
: "${QEMU_TIMEOUT:=120}"

_unused="${1:?usage: check_amp_peer_arms.sh <unused.elf> <cmake> <build> <artefact>}"
CMAKE="${2:?}"
BUILD="${3:?}"
ART="${4:?}"
: "$_unused"

need_qemu_machine

echo "== building the selftest partition artefact =="
"$CMAKE" --build "$BUILD" --target amp_partition_selftest >/dev/null 2>&1 \
    || fail "the amp_partition_selftest target did not build"
[ -f "$ART" ] || fail "no artefact at $ART"

run_image "$ART"

# The peer is witnessed through node 0's own reading and not through the peer's banner: both
# nodes write one console with no lock across the two kernels, interleaved at byte granularity
# by ruling, so a single line from the quieter node can be cut in half by the other's traffic.
printf '%s\n' "$OUT" | grep -qE 'amp window: [1-9][0-9]* of [0-9]+ peer node\(s\) answered' \
    || fail "no peer answered node 0 over the doorbell: the arms below would skip and mean
  nothing. This is node 0's own reading of the peer's counters, not a line the peer printed."

# `ok N - <arm>` with no SKIP directive: a skipped arm reports `ok` too, so the directive is
# what has to be excluded.
armed() { # <arm>
    printf '%s\n' "$OUT" | grep -E "^ok [0-9]+ - $1( |\$)" | grep -qv '# SKIP'
}
skipped() { # <arm>
    printf '%s\n' "$OUT" | grep -E "^ok [0-9]+ - $1 # SKIP" >/dev/null
}

for arm in amp_far_call amp_far_reply_guard amp_window; do
    if skipped "$arm"; then
        fail "$arm SKIPPED against a live peer.
  This artefact exists to give it one, so a skip here means the arm cannot see the peer and the
  permission the standalone run carries has quietly become permanent."
    fi
    armed "$arm" || fail "$arm did not report ok against a live peer"
    echo "== $arm: ok, not skipped"
done

echo "PASS: 3 arm(s) that needed a running peer ran against one and none of them skipped"
