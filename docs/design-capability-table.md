<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Capability table

> **Status: LANDED.** The design derivation, fleet sizing, old alternatives and staged
> implementation are in the capability-table record (archived `M4_capability_table_record.md`).
> For current behavior use [architecture](reference/architecture.md),
> [invariants](reference/invariants.md) and the code.

A userspace handle is a task-relative token: its index and generation name a table slot,
while the slot stores object type and rights. Resolution checks both the slot generation
and the target object's generation. Rights belong to the task's possession of a slot, not
to the handle bits or object identity. Object-less authority remains a separate word on the
caller, checked by its specific operation.

Capability storage is segmented and fully reserved when a task is created. The per-spawn
sizing path and unused size class were removed. The reservation law makes exhaustion and
lookup bounds explicit without requiring one contiguous table allocation. The archived
section 8 records the concurrency hazards considered for the later multicore work.

## 8. Multicore audit

The original section 8 (archived `M4_capability_table_record.md`) inventories cross-core
publication and lifetime hazards. The shared-kernel contract now resolves capability
references under one continuous lock; see [the multicore design](design-multicore.md).
