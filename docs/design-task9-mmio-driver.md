<!-- SPDX-License-Identifier: CECILL-C -->
# Design brief: task #9 -- unprivileged userspace driver + MMIO grant

> **Status: LANDED** -- the current contract is `reference/architecture.md` (Memory domains) and
> `reference/invariants.md` (`grant-refuses-kernel-reserved-blocks`).
> See `design/README.md` for the marker taxonomy.

Decision record. Grant admissibility, the encodability seam and the peripheral-MMIO matrix belong
to the Reference. The driver briefs (`design-spi-driver*.md`) build on the same seam.

## Key finding: the seam is most of the way there
An MMIO grant reduces to appending ONE `arch_mpu_region {R|W|DEV}` to a Domain, plus boundary
validation. `ARCH_MPU_DEV` already existed and was honored by the PMSA encoder, `thread_create`
already copied the whole `domain->regions[]` set, and `user_range_ok` already iterated regions by
attr.

## Load-bearing new invariant
An MMIO grant is PRIVILEGED-ONLY, like `ram_alloc`/`irq_attach`. REJECTED: making it
self-grantable by an unprivileged caller, which maps arbitrary peripheral space and defeats
isolation. The RAM `mem_base` grant was different: spawner-asserted, trusted until M2.

## ABI (Option A -- grant at spawn; recommended minimal step)
DECIDED: `kos_thread_params.mmio_base/mmio_size`, one MMIO region per domain, attr implied
`R|W|DEV`, never X on any backend.

Option B (a dedicated `mmio_grant` syscall for multiple or post-spawn regions) is DEFERRED rather
than rejected. It needs a domain/thread handle table plus a live re-apply. Its trigger is the
region budget: a two-MMIO-plus-data driver is 7 of 8 on ARMv7-M.

DECIDED: model MMIO as a DOMAIN region, so `thread_create`'s composition loop needs no edit.
REJECTED: MMIO handling inside `thread_create`, which would have collided with the
stack-ownership refactor landing in the same window.

DECIDED: reject a non-encodable window at the boundary. REJECTED: rounding it up, which
over-grants the neighboring registers, an isolation leak. An awkward block (PIT) takes a padded
window or two descriptors instead.

DECIDED: an MMIO-carrying domain is never shared, not even with a sibling that matched only the
data region. The grant is a capability.

## Per-backend feasibility
The top HW risk was whether SYSMPU gates peripheral accesses under user mode. ANSWERED on silicon
by the k64drv PIT driver: it does not. SYSMPU is a bus-slave-side unit (flash/SRAM crossbar ports)
and never sees the peripheral bridge. The AIPS bridge PACR is what gates, by privilege plus
master, per 4 KB slot, not per thread. So a K64F MMIO grant is INERT for peripheral isolation,
per-thread peripheral isolation is impossible on K64F, and it holds only on the CPU-side-MPU chips
(XMC PMSA, RISC-V PMP, RX MPU). Lead there. Matrix: `reference/architecture.md` (Memory domains).
Narrative: `book/peripheral-isolation-and-the-hardware-ceiling.md`.

sim: no real peripheral window is encodable there, so the GPIO/LED half of a driver has no sim
twin. sim exercises the IRQ-as-event half against an arena-backed fake device (the `t_irqdrv`
pattern).

## Risks
- Fault-vs-grant: an ungranted MMIO access must be reported, not escalated. A device access may
  surface as BusFault rather than MemManage on some Cortex-M, and as a bus error on SYSMPU, so the
  fault reporter must decode both and print the address.
- Region budget (8 on ARMv7-M PMSA): code + appdata + data + MMIO(xN) + stack.
