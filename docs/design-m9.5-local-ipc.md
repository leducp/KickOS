<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M9.5 x86 owner-local IPC experiment

> **Status: experimental path removed.** The shared kernel uses the single
> BKL path. The pinned pure-local KVM results are in
> [M9.5_x86_ipc_local.md](archive/M9.5_x86_ipc_local.md).

The [mixed-workload decision](archive/M9.5_x86_ipc_mixed.md) keeps the BKL and
removes the x86 local path to avoid maintaining two exclusion protocols. The
sections below describe the measured experiment, not the current kernel.

M9.4's lock-escape refusal was based on dual-core LX6. It is not evidence
against a many-core x86 path. The [pinned x86 baseline](archive/M9.5_x86_ipc_pinned.md)
showed independent call/reply pairs flattening at roughly one pair's aggregate
rate while BKL wait rose with core count. In the one-core control, wake and park
dominated the locked spans; extracting only the payload copy could not address
that limit. M9.5 therefore reopened the PARK switch rule for the experiment.

## Exclusion and publication

The first local attempt activates the barrier once under the ticket BKL. Before
activation, global spans use only the BKL and no local span can exist. The
activation waits for any earlier global span to finish; every later global
span reads the enabled flag after taking the BKL. Once enabled, the ordinary
global path publishes a writer flag, then waits for each core's active local
span to end. A local path waits for the writer flag to clear, marks its own core
active, and checks the flag again. These flag and active accesses are
sequentially consistent. If the local path's second check sees no writer, a
writer that follows must see that
core active. If the writer published first, the local path's second check sees
it and withdraws. The ticket BKL admits one writer, and the writer flag stops
new local spans while the writer waits. Neither side can enter concurrently
after both checks. The x86 wait loops service doorbells, including TLB requests,
while interrupts are masked.

`IrqLock` has a probe constructor in this build. It enters the local gate,
runs a read-only eligibility probe, and either keeps that gate or drops it
before taking the global path. Nested `IrqLock` brackets inherit the current
mode. No local span upgrades while holding its gate. The probe does not retain
any pointer across the fallback gap; the generic body resolves again under the
global lock.

`klock_detach` carries the mode and original core with the bracket depth.
The local active mark remains set through the switch until
`kickos_switch_unlock` runs after the outgoing frame is saved, exactly where
the BKL was released before. This preserves the parked-frame boundary without
a second TCB reclaim state. If a control operation changes a parked thread's
affinity and it resumes on another core, `klock_attach` takes global exclusion
before finishing the old bracket. A new core's gate alone cannot protect the
old core's endpoint.

## Local transactions admitted

The probes deliberately choose a narrow, checkable case:

- The running thread is pinned to its current core and alone at the top of
  that core's ready queue. Its selected priority equals the seated level.
- A fast `CALL` has a waiting receiver on that same core, which is also the
  endpoint's recorded server. Both peers have the same effective priority;
  neither wait uses a deadline or has a pending cancellation.
- A fused `REPLY_RECV` has a live local reply caller parked on that server,
  no queued sender on the receive endpoint, no notification admission or
  timeout, and no active priority donation. Its server is the endpoint's
  recorded server. The no-notification post-resume tail uses the local gate
  too, including its notification-state close.

The endpoint's one `server` pointer makes different cores' local probes
mutually exclusive for that endpoint. Calls from a peer core, a second server,
timed or notified operations, donation, and queued-send cases use the global
path. Global exclusion covers cap close and teardown, timeout and cancellation,
mapping changes, and thread reconfiguration. Thus a local copy into a parked
peer's buffer cannot race a global unmap, and a local reply-cap mint cannot
race global TCB or cap-table reuse. Two local spans on separate cores touch
different pinned TCBs, endpoint queues and ready queues. Neither admitted
path edits the global timer list.

The local path still executes the existing IPC transaction and scheduler
functions under an `IrqLock`; this experiment changes the exclusion mode, not
the call/reply ABI or the reply lifetime rules. The unchanged generic path is
used whenever a probe fails. The register-only trap fastpath remains disabled
for shared-kernel SMP.

## Scope and remaining gate

The pinned 2, 4, 8 and 12-core IPC sweep passes with checked payloads and
overlapping pairs. Four- and twelve-core KVM selftests pass, including a new
arm that remasks a caller onto another core while its local call frame is
parked. The x86 trap-depth gate and relevant tree checks pass. The matched
single-pair four-core control loses 2.7-2.9% throughput, below M9.4's 5%
single-flow budget. The four-core global-only yield control loses 4.4% despite
never activating the barrier; the extra enabled check and changed BKL path
still have a cost. An earlier eager barrier lost 15% on that control. The
mixed-workload gate found a 2.06x gain with
one of twelve pairs cross-core, a 1.20x gain with six, and a 6.35% loss when
all twelve pairs crossed cores. On host CPUs 0-3, one cross-core pair left
only a 3.39% gain. These results favor the local mode for strongly local,
wide workloads, but do not justify a second maintained kernel path.

This is x86 KVM evidence on a heterogeneous 12-core host. It neither overturns
the LX6 silicon measurement nor claims the same gain on a dual-core MCU.
