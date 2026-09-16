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
# NOT ONE CLAUSE BELOW READS A LINE A QUIETER NODE PRINTED, and none may be added: the nodes
# share one console, no lock spans two kernels, and N6h rules the stream interleaves at byte
# granularity, so any such line can arrive cut in half and an exact count of them fails on a
# draw. The announcement count that used to sit here was measured failing 1 run in 10 on a
# partition of three, and the rate rises with the width.
#
# What replaced it is a per-node mark each node's own app publishes into the shared record,
# which NODE 0 reads and reports in one line of its own. `amp_window` witnesses each peer's
# KERNEL servicing a doorbell and the round trip witnesses the first peer's app alone, so that
# mark is the only witness a node this demo never calls has.
#
# AND READING NODE 0'S OWN LINE IS NOT SUFFICIENT ON ITS OWN. Node 0 writes the same shared
# console, so a line it prints WHILE THE PEERS ARE BOOTING is cut by their banners exactly as a
# peer's line would be. Every clause below therefore reads a line node 0 prints after its
# app-alive sweep has returned, which is the point at which every node has queued its whole BOOT
# output. The ordering clause at the foot of this file is what holds that, so a print moved back
# ahead of the sweep reddens every run instead of one in ten.
#
# THAT SWEEP DOES NOT MAKE THE PEERS SILENT, AND NOTHING BELOW MAY ASSUME IT DOES. It closes the
# boot window and nothing further: a serving node publishes the mark swept here and then prints
# one line for every round it answers, ahead of its kos_reply, and those bytes leave that node's
# own ring under that node's own interrupt, so they can reach the UART while node 0 is writing
# the lines these clauses read. What carries the clauses past that is not silence. Not one of
# them counts a peer's lines, and not one pattern anchors at line start, so a peer's fragment
# sitting in front of a line node 0 printed is accepted and the ordering loop reads a line
# NUMBER, which a prepend does not move. A peer's bytes landing INSIDE a clause's own matched
# text is what that leaves, and nothing here measures its rate: TODO.md's M8.8 review residue
# carries the options for closing it.
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

need_qemu_machine

echo "== building the partition artefact =="
"$CMAKE" --build "$BUILD" --target amp_partition >/dev/null 2>&1 \
    || fail "the amp_partition target did not build"
[ -f "$ART" ] || fail "no artefact at $ART after building amp_partition"

run_image "$ART"

# EVERY NODE'S APP RAN, on node 0's own reading of the shared record. Each node's app asks its
# own kernel to publish the port the partition names it, biased by one; node 0 derives that port
# from its own copy of the list and counts the rows that agree, its OWN row included as the
# known-value control. Which index a node carries is the partition's, so this counts rather
# than naming one, and the total is compared against the width the partition states.
#
# THE LEADING `.*` IS NOT DECORATION, and a '^' may not replace it. A peer's bytes can sit ahead
# of this line's first byte, and an anchor reports such a capture as a line node 0 never printed;
# without the `.*` the substitution leaves those bytes in front of the three fields it extracts.
alive="$(printf '%s\n' "$OUT" \
    | sed -n 's/.*ampping: \([0-9]*\) of \([0-9]*\) node app(s) alive on the port the partition names, own row \([0-9]*\).*/\1 \2 \3/p' \
    | tail -1)"
[ -n "$alive" ] || fail "node 0 never reported which nodes' apps published their own port.
  This line is node 0's own reading of the shared record and is the only witness that an app
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
# peers have stopped writing. The value is read out of this node's own copy of the partition
# list, which the kernel seats before main and nothing alters, so announcing it after the sweep
# announces the same crossing the rounds below then run on.
printf '%s\n' "$OUT" | grep -qE 'ampping: node [0-9]+ calls node [0-9]+ port' \
    || fail "node 0 never announced the far port the partition handed it"

# The ANSWER and not merely a wake: a peer replies with the request byte plus one. The answering
# node is left unnamed and only required not to be node 0.
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
#
# THE LEADING `.*` FOR THE REASON THE SWEEP CLAUSE ABOVE CARRIES ONE: a '^' refuses a capture a
# peer's bytes reached first, and without the `.*` those bytes survive into the first field, where
# the arithmetic below reads them.
defer="$(printf '%s\n' "$OUT" | sed -n 's/.*ampping: deferred \([0-9]*\) raise(s) skipped at node \([0-9]*\), notice to node \([0-9]*\) port [0-9]*, took \([0-9]*\) message(s).*/\1 \2 \3 \4/p' | tail -1)"
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

# THE BARRIER HELD, AND THIS IS WHAT HOLDS IT. Every clause above reads a line node 0 printed
# after its sweep had read every node's mark, which is the point at which every node has queued
# its whole boot output. A line printed before that sweep sits among the peers' banners and is
# cut in half on a draw, so the capture is required to carry the sweep's own line FIRST.
at_line() { # <extended regex>: the capture line it first matched on, empty where it did not
    printf '%s\n' "$OUT" | grep -nE "$1" | head -1 | cut -d: -f1
}
# NOT ONE PATTERN HERE ANCHORS AT LINE START, the reference line included, because not one clause
# above does: a peer's bytes prepended to a line are the tear this file reads for, so an anchor
# refuses a capture a clause above has already accepted and reports it as nothing matching. A
# prepend moves no line NUMBER, which is all this reads.
barrier_at="$(at_line 'ampping: [0-9]+ of [0-9]+ node app\(s\) alive')"
[ -n "$barrier_at" ] || fail "the app-alive sweep line is not in the capture"
for pat in 'ampping: node [0-9]+ calls node [0-9]+ port' \
           'ping 1 -> pong 2 from node [1-9][0-9]*' \
           'ampping: node 0 done' \
           'ampping: deferred [0-9]+ raise\(s\) skipped at node'; do
    at="$(at_line "$pat")"
    [ -n "$at" ] || fail "nothing matched /$pat/ here, where a clause above already read it"
    [ "$at" -gt "$barrier_at" ] || fail "node 0 printed /$pat/ at capture line $at, ahead of its
  app-alive sweep at line $barrier_at. That is the boot window: the peers are still writing
  their banners into this same console, so the line is cut in half on a draw. Print it after
  the sweep."
done
echo "== ordering: every line read above arrives after the app-alive sweep"

rounds="$(printf '%s\n' "$OUT" | grep -c ' -> pong ' || true)"
echo "PASS: one artefact of $NODES node(s), $alive_ok node app(s) alive, $rounds round(s)
  answered by a thread in another kernel"
