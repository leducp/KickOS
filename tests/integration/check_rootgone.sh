#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# ROOT-IS-NOT-INHERITABLE gate: boot the `rootgone` image, let root fault, and assert that
# the thread the pool hands out afterwards is NOT root.
#
# Registered on the thread-kill posture only (see the app's CMakeLists): on the panic posture
# root's violation ends the system and there is no surviving thread to ask.
#
# Five things have to hold, and each one covers a different way the arm could go green for
# the wrong reason:
#   * root reached its deliberate write (so the fault is this one and not an earlier trap);
#   * the kernel credited a thread-kill to 'root' (so root really died: without this the
#     grandchild would never have been offered root's slot, and the refusal below would be
#     trivially satisfied);
#   * the grandchild's kos_wait_last was REFUSED (the identity did not move);
#   * ROOTGONE PASS (the system carried on, and the grandchild's exit ended ONE thread
#     rather than reaching kickos_terminate);
#   * the image exited, and exited 0.
#
# Native for the sim, QEMU when QEMU_MACHINE is set.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_rootgone.sh <rootgone.elf>}"

run_image "$elf"

if has "ERROR"; then
    fail "rootgone reported a failed setup or control arm"
fi
if has "NOT confined"; then
    fail "root's cross-domain write was NOT trapped (enforcement off?)"
fi
if ! has "child: alive, releasing root"; then
    fail "the child never ran (setup failed before the test)"
fi

want="$(printf '%s\n' "$OUT" \
    | sed -n "s/.*root: writing the child's granted region at 0x\([0-9a-fA-F]*\).*/\1/p" \
    | head -n1)"
if [ -z "$want" ]; then
    fail "root never reached the deliberate write (faulted earlier?)"
fi
if ! has_e "$(thread_fault_re root)"; then
    fail "no thread-kill for 'root': root did not die, so the slot was never offered"
fi
assert_no_panic "root's death escalated instead of killing the thread"
if [ "$RC" -eq 124 ]; then
    fail "the image did not exit within ${QEMU_TIMEOUT:-20}s: a granted wait_last parks the
  grandchild until last-out, and the child's own shutdown follows its PASS, so this is the
  run failing to terminate"
fi
got="$(reported_fault_addr)"
if [ -z "$got" ]; then
    fail "the fault report carries no address (KICKOS_PANIC_DUMP off?)"
fi
if [ "$((0x$got))" -ne "$((0x$want))" ]; then
    fail "root trapped at 0x$got, not at the child's region 0x$want"
fi

if ! has "child: outlived root"; then
    fail "the child did not survive root's death"
fi
if has "wait_last GRANTED"; then
    fail "the thread in root's slot answered is_root: root's authority was INHERITED"
fi
if ! has "grandchild: wait_last refused"; then
    fail "the grandchild never reported on kos_wait_last (spawn or join failed?)"
fi
require_on_wire "ROOTGONE PASS" "the run never reached PASS"
# PASS is printed by the surviving child, which then calls kos_shutdown: the console flush
# and arch_shutdown all run after the last line this gate can read, so the status is the
# only thing that covers them.
if [ "$RC" -ne 0 ]; then
    fail "the image printed PASS and then exited $RC: the teardown behind the verdict failed"
fi

echo "PASS: root died, its slot was retired, and no successor inherited its identity"
exit 0
