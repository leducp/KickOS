<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Kill and slay

> **Status: LANDED.** The original mechanism, mutation record and corrections are in
> [the M4.8.4 record](archive/M4_kill_and_slay_record.md). Read its section 14 before using
> an earlier proposal there. Current behavior is in [the ABI](../user/include/kickos/sys/abi.h),
> [invariants](reference/invariants.md) and code.

`kos_thread_kill` requests cooperative cancellation. `slay` forcibly redirects an eligible
target to its own teardown on a rebuilt context. It reuses the normal exit path and has no
reaper. The task form waits for the group to become empty. `kos_task_kill` slays every member
as `kos_task_slay` does and does not wait. Self, idle and privileged targets are
refused before checking the caller's relation to the target.

A slay request may take effect even when the waiting caller returns `-KOS_ECANCELED` because
that caller was cancelled. A zero timeout arms the request and returns at the first timer
opportunity; the unbounded timeout retains the ordinary join convention. The forced exit
uses the cancelled exit cause.

The archived original was scoped to one core. In a shared kernel, a running peer is re-seated
for its own scheduler pass to claim the slay; the victim still performs its own teardown.
The archived section 14.6 is the original SMP debt, not the current implementation status.
