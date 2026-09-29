<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Task layer

> **Status: LANDED.** The original grouping diagnosis, staged plan, measured cost and
> corrections are in [the M4.8.3 record](archive/M4_task_layer_record.md). The current model
> is in [architecture](reference/architecture.md), [invariants](reference/invariants.md)
> and the code.

A task names a set of threads and gives their shared lifetime an explicit owner. Group kill,
fault death and driver teardown use that membership instead of a caller-maintained list of
thread handles. A plain spawn belongs to the caller's task; an explicit task creation starts
a separate group. Task-scoped state and thread-private grants remain distinct.

The address space belongs to `Domain`, not to `Task`: task identity and memory sharing are
different decisions. The MMU work extended this model to translated spaces. The archived
record retains the original one-thread default proposal and the changes made while landing.
