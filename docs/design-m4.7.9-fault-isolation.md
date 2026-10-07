<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M4.7.9 fault isolation

> **Status: LANDED.** The original spike, measurements and numbered reasoning are in
> the fault-isolation record (archived `M4_fault_isolation_record.md`). Current porting and safety
> rules are in [porting](reference/porting.md) and [invariants](reference/invariants.md).

## 3. The rule

A fault kills the faulting **task** when it happened in unprivileged thread context and the
thread is not already dying. Other faults panic. The backend identifies the privilege and
redirects a valid fault frame to the thread's exit stub; a backend unable to prove the condition
declines isolation. The core rechecks the conditions it can. Task teardown then releases its
capabilities and wakes observers through the normal exit path.

## 4. Fault exit and its limit

The fault handler captures a bounded record and does not print. The exit stub reports it in
thread context. A later fault may replace the record before that stub runs; the record's owner
check prevents printing another thread's PC. Re-faulting during death panics rather than
redirecting again. RXv3 also requires frame-validity checks before redirect.

## 5. Survivor and reporting

Surviving tasks continue at their assigned priorities; the proposed priority deflate was
rejected. A published console receives fault diagnostics through the fault route, without
reclaiming the driver or turning ordinary kernel debug output back on. Delivery is best effort
when the driver has no parked receiver. A join reports liveness, not fault cause.

Repeated crash-and-respawn can exhaust the non-freeing RAM arena. A dead driver can leave
peripheral state programmed even though its IRQ is detached. These are separate resource and
driver lifecycle questions.
