#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boots the merged partition artefact and asserts that its kernels talk over the shared window.
#
# usage: check_amp_partition.sh <node0.elf> <cmake> <build-dir> <artefact> <nodes>
#        <node0.elf> is what kickos_add_qemu_test hands every script and is unused here: the
#        artefact is the subject. <nodes> is KICKOS_AMP_NODES, the width the PARTITION states,
#        and every count below is derived from it, so a literal 2 appears nowhere.
#
# The counts are keyed on the width, the round-trip clauses are not: node 0's app calls the
# FIRST peer the partition names and no other. The banner and announcement counts also read
# lines the quieter nodes printed, which N6h says a gate may not rest on.
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
: "${QEMU_TIMEOUT:=90}"

_unused_elf="${1:?usage: check_amp_partition.sh <node0.elf> <cmake> <build> <artefact> <nodes>}"
CMAKE="${2:?}"
BUILD="${3:?}"
ART="${4:?}"
NODES="${5:?}"
: "$_unused_elf"

case "$NODES" in
    ''|*[!0-9]*) fail "the node count '$NODES' is not a number" ;;
esac
[ "$NODES" -ge 2 ] || fail "a partition of $NODES node(s) has no crossing to witness"
PEERS=$((NODES - 1))

need_qemu_machine

echo "== building the partition artefact =="
"$CMAKE" --build "$BUILD" --target amp_partition >/dev/null 2>&1 \
    || fail "the amp_partition target did not build"
[ -f "$ART" ] || fail "no artefact at $ART after building amp_partition"

run_image "$ART"

# Every clause below reads a line only a live partition prints.
banners="$(printf '%s\n' "$OUT" | grep -c 'microkernel RTOS' || true)"
[ "$banners" -eq "$NODES" ] \
    || fail "expected $NODES kernel banner(s) from the merged artefact, saw $banners"

# Which index a peer carries is the partition's, so this counts rather than naming one.
announced="$(printf '%s\n' "$OUT" | grep -c 'ampping: node [0-9][0-9]* serves port' || true)"
[ "$announced" -eq "$PEERS" ] || fail "$announced of $PEERS peer(s) announced the port the
  partition bound for them"
printf '%s\n' "$OUT" | grep -qE 'ampping: node [0-9]+ calls node [0-9]+ port' \
    || fail "node 0 never announced the far port the partition handed it"

served="$(printf '%s\n' "$OUT" | grep -c '^  serve ' || true)"
[ "$served" -ge 1 ] || fail "no peer's service thread took a call: a far call reached no thread"

# The ANSWER and not merely a wake: a peer replies with the request byte plus one. The
# answering node is left unnamed and only required not to be node 0.
printf '%s\n' "$OUT" | grep -qE 'ping 1 -> pong 2 from node [1-9][0-9]*' \
    || fail "node 0's first round did not come back carrying a peer's own answer"

printf '%s\n' "$OUT" | grep -q 'ampping: node 0 done' \
    || fail "node 0 never completed its rounds"

# A publication outlives the doorbell that would have announced it: node 0 publishes with the
# target's seat forced unseated, so the raise is skipped, then makes an ordinary call carrying
# the next notice. That node's OWN take counter must have moved by TWO; one means the deferred
# message was lost. Which node the kernel published at is the kernel's own choice, so the node
# fields are compared rather than discarded: a notice sent to any other node leaves the
# deferred publication with none.
defer="$(printf '%s\n' "$OUT" | sed -n 's/^ampping: deferred \([0-9]*\) raise(s) skipped at node \([0-9]*\), notice to node \([0-9]*\) port [0-9]*, took \([0-9]*\) message(s).*/\1 \2 \3 \4/p' | tail -1)"
[ -n "$defer" ] || fail "node 0 never reported the deferred publication.
  A node the partition names no port refuses this clause by name instead; that is a partition
  this demo cannot carry a notice on, not a lost message."
skipped="$(echo "$defer" | cut -d' ' -f1)"
at="$(echo "$defer" | cut -d' ' -f2)"
notice="$(echo "$defer" | cut -d' ' -f3)"
took="$(echo "$defer" | cut -d' ' -f4)"
[ "$skipped" -eq 1 ] || fail "expected exactly 1 skipped raise, node 0 reported $skipped:
  without a skipped raise the publication carried its own notice and this witnesses nothing"
[ "$at" != "0" ] || fail "the kernel reported publishing at node 0, which is the node running
  this: its own ring is the one no service drains, so this witnesses nothing"
[ "$at" = "$notice" ] || fail "the publication went to node $at and the notice to node $notice.
  The count below is node $at's, so a notice sent elsewhere leaves the deferred publication
  with none and this clause measures a crossing it never made."
[ "$took" -eq 2 ] || fail "node $at took $took message(s) across the deferred publication and the
  call that followed it, and the claim needs 2. One means the publication whose raise was
  skipped was LOST, which is the clause this arm exists for."
echo "== deferred delivery: $skipped raise(s) skipped at node $at, which took $took message(s)"

rounds="$(printf '%s\n' "$OUT" | grep -c ' -> pong ' || true)"
echo "PASS: one artefact, $banners kernel(s), $rounds round(s) answered by a thread in another
  kernel"
