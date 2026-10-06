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
# FIRST peer the partition names and no other.
#
# The peer's app announcement is also a console-contract witness: a normal claimant must
# deliver one intact line even while other images print their own banners. The partition region
# every node's task maps, `/shm/ampbook`, remains the authority for app liveness and crossings;
# the console can drop a line if its bounded claim fails, so its contents cannot prove a peer ran.
#
# The order in which kernels reach the UART changes each run, so a change to this gate or its
# image owes ten green runs:
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

need_qemu_machine

echo "== building the partition artefact =="
"$CMAKE" --build "$BUILD" --target amp_partition >/dev/null 2>&1 \
    || fail "the amp_partition target did not build"
[ -f "$ART" ] || fail "no artefact at $ART after building amp_partition"

run_image "$ART"

# Each peer holds the shared UART through its line, so its announcement must arrive whole
# beside node 0's banner on the actual device.
peer=1
while [ "$peer" -lt "$NODES" ]; do
    printf '%s\n' "$OUT" | grep -qE "^ampping: node $peer serves (port [0-9]+|no port)$" \
        || fail "node $peer's console announcement is absent or torn"
    peer=$((peer + 1))
done
echo "== console: $((NODES - 1)) peer announcement(s) arrived as whole lines"

# EVERY NODE'S APP RAN, on node 0's own reading of the partition region. Each node's task writes
# the port the partition names it, biased by one, into its own row; node 0 derives that port from
# its own copy of the list and counts the rows that agree, its OWN row included as the
# known-value control. Which index a node carries is the partition's, so this counts rather
# than naming one, and the total is compared against the width the partition states.
#
alive="$(printf '%s\n' "$OUT" \
    | sed -n 's/^ampping: \([0-9]*\) of \([0-9]*\) node app(s) alive on the port the partition names, own row \([0-9]*\)$/\1 \2 \3/p' \
    | tail -1)"
[ -n "$alive" ] || fail "node 0 never reported which nodes' apps published their own port.
  This line is node 0's own reading of the partition region and is the only witness that an app
  ran on a node this demo never calls."
alive_ok="$(echo "$alive" | cut -d' ' -f1)"
alive_of="$(echo "$alive" | cut -d' ' -f2)"
alive_own="$(echo "$alive" | cut -d' ' -f3)"
[ "$alive_own" = "1" ] || fail "node 0 did not read back its OWN app mark, so this sweep is an
  artefact rather than a report on the peers: the reader and the writer disagree about the row
  or about the port the partition names this node."
[ "$alive_of" -eq "$NODES" ] || fail "node 0 swept $alive_of node(s) where the partition states
  $NODES: the app and this gate are reading different widths"
[ "$alive_ok" -eq "$NODES" ] || fail "$alive_ok of $NODES node app(s) published the port the
  partition names them. A node missing here reached no app at all, or reached one that bound a
  port the partition does not name it."
echo "== node apps: $alive_ok of $NODES published the port the partition names, own row read back"

# THE FAR PORT THE PARTITION HANDED NODE 0, announced by node 0 once the sweep above says the
# peers have published their app rows. The value is read out of this node's own copy of the partition
# list, which the kernel seats before main and nothing alters, so announcing it after the sweep
# announces the same crossing the rounds below then run on.
printf '%s\n' "$OUT" | grep -qE '^ampping: node [0-9]+ calls node [0-9]+ port [0-9]+$' \
    || fail "node 0 never announced the far port the partition handed it"

# THE ANSWER AND NOT MERELY A WAKE, READ OUT OF THE PARTITION REGION AND NOT OFF THIS CONSOLE.
# A peer is witnessed through a counter the OTHER node reads, never through what it printed,
# and the per-round lines both apps still print are read by nothing here. Node 0 checks every
# round's answer itself, the peer's own transformation of the payload, and refuses before this
# line where one is wrong; what the line adds is the peer's OWN row, which its serving task
# stores ahead of every reply it sends. That row is the same region the app-alive sweep reads,
# under the same one-writer-per-row rule.
cross="$(printf '%s\n' "$OUT" \
    | sed -n 's/^ampping: node \([0-9]*\) answered \([0-9]*\) round(s), its own record says \([0-9]*\)$/\1 \2 \3/p' \
    | tail -1)"
[ -n "$cross" ] || fail "node 0 never reported what the answering node's own row says.
  The crossing is UNKNOWN and not absent: no clause here reads a round off the console, so
  without this line there is nothing to read. A node the partition hands no far port refuses
  earlier and by its own name."
cross_node="$(echo "$cross" | cut -d' ' -f1)"
cross_rounds="$(echo "$cross" | cut -d' ' -f2)"
cross_served="$(echo "$cross" | cut -d' ' -f3)"
# A FIELD MAY MATCH EMPTY, `[0-9]*` accepting none, so a torn line reaches the arithmetic below
# as a blank rather than as a number. Refused here by name: a record this gate cannot read is
# UNKNOWN and not a partition that never crossed.
for field in "$cross_node" "$cross_rounds" "$cross_served"; do
    case "$field" in
        ''|*[!0-9]*) fail "node 0's crossing line carries a field that is not a number: [$cross].
  The record is UNKNOWN and not a crossing that failed." ;;
    esac
done
[ "$cross_node" != "0" ] || fail "node 0 named ITSELF as the node that answered its rounds, so
  this reports a local call and not a crossing"
[ "$cross_rounds" -ge 1 ] || fail "node 0 holds answers for $cross_rounds round(s), so nothing
  crossed for the row below to have counted"
[ "$cross_served" -ge "$cross_rounds" ] || fail "node $cross_node's own row counts $cross_served
  call(s) its APP answered, where node 0 holds answers for $cross_rounds round(s). A row short
  of the rounds means those answers came from something other than a thread in that kernel: the
  kernel answers a call no thread received with an empty reply of its own, and that moves TOOK
  and SENT but not this row."
echo "== crossing: node $cross_node's row counts $cross_served answered call(s) for the $cross_rounds round(s) node 0 holds"

printf '%s\n' "$OUT" | grep -q 'ampping: node 0 done' \
    || fail "node 0 never completed its rounds"

# A publication outlives the doorbell that would have announced it: node 0 publishes with the
# target's seat forced unseated, so the raise is skipped, then makes an ordinary call carrying
# the next notice. That node's OWN take counter must have moved by TWO; one means the deferred
# message was lost. Which node the kernel published at is the kernel's own choice, so the node
# fields are compared rather than discarded: a notice sent to any other node leaves the
# deferred publication with none.
#
defer="$(printf '%s\n' "$OUT" | sed -n 's/^ampping: deferred \([0-9]*\) raise(s) skipped at node \([0-9]*\), notice to node \([0-9]*\) port [0-9]*, took \([0-9]*\) message(s).*$/\1 \2 \3 \4/p' | tail -1)"
[ -n "$defer" ] || fail "node 0 never reported the deferred publication.
  A node the partition names no port refuses this clause by name instead; that is a partition
  this demo cannot carry a notice on, not a lost message. The app also asserts the notice call
  itself before printing this line, so its own refusal line above stands in place of it."
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

# Keep the app ordering witness: node 0 reports the crossing after it has read all peer rows.
at_line() { # <extended regex>: the capture line it first matched on, empty where it did not
    printf '%s\n' "$OUT" | grep -nE "$1" | head -1 | cut -d: -f1
}
barrier_at="$(at_line 'ampping: [0-9]+ of [0-9]+ node app\(s\) alive')"
[ -n "$barrier_at" ] || fail "the app-alive sweep line is not in the capture"
for pat in 'ampping: node [0-9]+ calls node [0-9]+ port' \
           'ampping: node [0-9]+ answered [0-9]+ round\(s\), its own record says' \
           'ampping: node 0 done' \
           'ampping: deferred [0-9]+ raise\(s\) skipped at node'; do
    at="$(at_line "$pat")"
    [ -n "$at" ] || fail "nothing matched /$pat/ here, where a clause above already read it"
    [ "$at" -gt "$barrier_at" ] || fail "node 0 printed /$pat/ at capture line $at, ahead of its
  app-alive sweep at line $barrier_at. Print it after the sweep."
done
echo "== ordering: every line read above arrives after the app-alive sweep"

echo "PASS: one artefact of $NODES node(s), $alive_ok node app(s) alive, $cross_rounds round(s)
  answered by a thread in node $cross_node's kernel on that node's own row"
