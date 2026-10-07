<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M7 SMP candidate spike

> **Status: EXPLORATORY.** This is the historical candidate ranking and staged-model spike.
> Its complete hardware analysis and numbered sections are in
> the M7 spike (archived `M7_smp_candidate_spike.md`). Later shared-kernel and AMP decisions are
> in [the multicore design](design-multicore.md) and [architecture](reference/architecture.md).

A shared kernel needs an inter-core exclusion primitive and a mature context-switch path.
The old ranking favored RP2350 as the first symmetric candidate, put RP2040 behind it, and
deferred LX6. A hardware spinlock can implement more than one independent lock even on a core
without atomic read-modify-write instructions; it does not provide every queue-lock algorithm.
The full spike records the per-chip register and erratum basis for that ranking.

The cross-core IPC sketch used a doorbell plus paired SPSC rings. Its ownership, ordering and
lifetime questions were resolved in later M7 to M9 designs. This page is a route to that earlier
reasoning, not a new implementation plan.
