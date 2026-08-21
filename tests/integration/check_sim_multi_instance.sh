#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for KICKOS_MULTI_INSTANCE: build the sim with several independent kernels
# provisioned and require FIFTY of them, on fifty host threads, to run one app to
# completion without seeing each other. This is the one gate running more than one kernel,
# so it is where "two instances share a file-scope object" is caught.
#
# The comparison is against a ONE-instance run of the SAME image, not a fixed expected
# list: it asserts that co-residence changed nothing, and it needs no edit when the app
# changes. Fifty is also the top of the fleet size this feature exists to serve. A shared
# capability slab does not fail deterministically, co-resident kernels aliasing inside the
# free list rather than exhausting it, so lowering the count weakens that arm.
#
# What it does NOT cover, so read it narrowly:
#   - a SHARED SIGALTSTACK. That only corrupts when two host threads fault at the same
#     time, and this app takes no fault at all. Covering it needs an app that faults in
#     every instance.
#   - instances multiplexed on ONE host thread. That needs a yield-to-host in the
#     scheduler, which does not exist.
#   - the app's OWN file-scope state, which is one copy per process. sched_exit keeps no
#     such counter, which is why it is the app used.
#   - a build that writes to stdout by any path other than the tagged, line-atomic one.
#
# usage: check_sim_multi_instance.sh <kickos-source-dir> <cmake>

set -eu

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"
INSTANCES=50

fail() { echo "FAIL: $1"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== configuring the sim with KICKOS_MULTI_INSTANCE=ON =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_MULTI_INSTANCE=ON >/dev/null ) \
  || fail "configure with KICKOS_MULTI_INSTANCE=ON failed"

echo "== building sched_exit =="
"$CMAKE" --build "$TMP/build" --target sched_exit >/dev/null \
  || fail "sched_exit build failed"

APP="$TMP/build/user/apps/common/sched_exit/sched_exit"
[ -x "$APP" ] || fail "sched_exit binary not produced at $APP"

# The reference run: the same image at one instance, so any difference below is
# co-residence and nothing else.
set +e
KICKOS_SIM_INSTANCES=1 timeout "${SIM_TIMEOUT:-60}" "$APP" > "$TMP/one.log" 2>&1
RC_ONE=$?
KICKOS_SIM_INSTANCES="$INSTANCES" timeout "${SIM_TIMEOUT:-60}" "$APP" > "$TMP/many.log" 2>&1
RC_MANY=$?
set -e

if [ "$RC_ONE" -ge 124 ]; then
    fail "the one-instance reference run timed out (rc=$RC_ONE)"
fi
if [ "$RC_MANY" -ge 124 ]; then
    fail "the $INSTANCES-instance run timed out (rc=$RC_MANY)"
fi
if [ "$RC_MANY" -ne "$RC_ONE" ]; then
    fail "status $RC_MANY at $INSTANCES instances, $RC_ONE at one: the same image ended differently"
fi

# A shared arena, a shared capability slab and a shared sigaltstack all present as a fault
# rather than as missing output, so this comes before the content checks.
if grep -qE 'SIGSEGV|SIGILL|MPU FAULT|PANIC|KERNEL PANIC' "$TMP/many.log"; then
    echo "--- offending output ---"
    grep -E 'SIGSEGV|SIGILL|MPU FAULT|PANIC|KERNEL PANIC' "$TMP/many.log" | head -5
    fail "a fault or panic during the $INSTANCES-instance run"
fi

# Every line carries its instance, so the run splits cleanly even though the instances
# interleave. An untagged line means output escaped the per-instance path.
if grep -qvE '^\[[0-9]+\] ' "$TMP/many.log"; then
    echo "--- untagged output ---"
    grep -vE '^\[[0-9]+\] ' "$TMP/many.log" | head -5
    fail "output that no instance owns: the per-instance console path was bypassed"
fi

# A lone instance shares stdout with nobody and emits no tag at all, byte for byte what a
# build without the knob emits. That is what keeps every banner-exact and TAP-exact gate
# holding with the knob compiled in.
if grep -qE '^\[[0-9]+\] ' "$TMP/one.log"; then
    echo "--- tagged output ---"
    grep -E '^\[[0-9]+\] ' "$TMP/one.log" | head -5
    fail "a one-instance run tagged its output"
fi

cp "$TMP/one.log" "$TMP/ref.txt"
if [ ! -s "$TMP/ref.txt" ]; then
    fail "the one-instance reference run produced nothing to compare against"
fi

i=0
while [ "$i" -lt "$INSTANCES" ]; do
    sed -n "s/^\\[$i\\] //p" "$TMP/many.log" > "$TMP/inst.txt"
    if [ ! -s "$TMP/inst.txt" ]; then
        fail "instance $i emitted nothing: it never ran, or its output went to another instance"
    fi
    # Order between instances is not a property; order WITHIN one instance is, so this is
    # a plain diff and not a sort.
    if ! diff -u "$TMP/ref.txt" "$TMP/inst.txt" > "$TMP/delta.txt"; then
        echo "--- instance $i against the one-instance reference ---"
        head -30 "$TMP/delta.txt"
        fail "instance $i did not run the same as it does alone"
    fi
    i=$((i + 1))
done

echo "== $INSTANCES instances each ran identically to a one-instance run of the same image =="
echo "PASS: sim multi-instance"
