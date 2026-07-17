<!-- SPDX-License-Identifier: CECILL-C -->
# Design brief: task #9 -- unprivileged userspace driver + MMIO grant

**LANDED.** The MMIO-grant mechanism is implemented and committed:
`kos_thread_params.mmio_base/mmio_size` (Option A grant-at-spawn), the
`arch_mpu_region_encodable(base,size)` arch seam (exact-cover, no rounding: PMSA/PMP
pow2, SYSMPU 32 B, RX 16 B, sim fail-closed), `thread_spawn` boundary validation
(privileged-only, no-wrap, encodable), and `domain_for` appending the MMIO region as a
never-shared capability -- the stack-ownership refactor it sequenced behind also landed.
The GPIO/timer first-driver bring-up (k64drv, K64F PIT) answered this brief's top HW risk
on silicon: see "Per-backend feasibility -- K64F" below. This brief is retained as the
design record; the driver briefs (`design-spi-driver*.md`) build on the landed seam.

## Key finding: the seam is most of the way there
- `ARCH_MPU_DEV` (arch/include/kickos/arch/arch.h) already exists and is honored by the
  ARM PMSA encoder (`mpu_rasr` -> `AP_RW | XN | MEM_DEVICE`).
- `thread_create` already copies the whole `domain->regions[]` set into a thread's set,
  and the budget assert already covers it; `user_range_ok` iterates regions by attr.
- So an MMIO grant reduces to: append ONE `arch_mpu_region {R|W|DEV}` to a Domain, and
  validate it at the boundary. Modeling MMIO as a DOMAIN region means `thread.cc`'s
  composition loop needs no edit (keeps this out of the stack refactor's way).

## Load-bearing new invariant
**An MMIO grant is PRIVILEGED-ONLY** (gate on `sched::current()->privileged`, reject -1),
exactly like `ram_alloc`/`irq_attach`. Unlike the RAM `mem_base` grant (spawner-asserted,
trusted-until-M2), MMIO must NOT be self-grantable by an unprivileged caller -- else it
maps arbitrary peripheral space and defeats isolation.

## ABI (Option A -- grant at spawn; recommended minimal step)
Extend `kos_thread_params` with `void* mmio_base; uint32_t mmio_size;` (0 = none); attr is
implied `R|W|DEV` (device, RW, no-execute). One MMIO region per domain -- enough for the
minimal driver. Option B (a dedicated `mmio_grant` syscall for multiple/post-spawn regions)
is the documented follow-on; needs a domain/thread handle table + live re-apply.

Boundary validation in `thread_spawn` (before slot claim), when `mmio_base != 0`:
1. privileged caller, else -1;
2. `mmio_size != 0`, `base+size` no wrap;
3. ENCODABILITY -- reject (do not round) what one descriptor cannot cover: pow2-MPU archs
   (PMSA/PMP) require pow2 size >= min and base natural-aligned; byte-granular archs
   (SYSMPU/RX) require size >= min + page alignment. (Rounding MMIO up over-grants
   neighboring registers -- an isolation leak.)
4. attr fixed `R|W|DEV`, never X.

`domain_for` extended to append the MMIO region; **an MMIO-carrying domain is never shared**
(a capability -- do not auto-share with a sibling that only matched the data region).

## Per-backend feasibility
- ARM PMSA (XMC): DEV already implemented (device + XN + RW). Pow2/aligned window. Do the
  POC here first (enforcement-proven on silicon).
- K64F SYSMPU: **ANSWERED on silicon (k64drv PIT driver): SYSMPU does NOT gate AIPS
  peripheral-bridge accesses under user mode.** SYSMPU is a bus-slave-side unit (flash/SRAM
  crossbar ports); it never sees the peripheral bridge. Peripherals are gated by the AIPS
  bridge PACR -- by privilege+master, per 4 KB slot, NOT per-thread -- so a K64F MMIO grant is
  INERT for peripheral isolation (kernel-vs-user, per-slot only). Per-thread peripheral
  isolation is therefore impossible on K64F; it holds on the CPU-side-MPU chips (XMC PMSA,
  RISC-V PMP, RX MPU). See `reference/architecture.md` (Memory domains -- the peripheral-MMIO
  matrix) + `book/peripheral-isolation-and-the-hardware-ceiling.md`.
- RISC-V PMP: ordinary NAPOT RW-NX (no device type; that's PMA). Pow2/aligned.
- RX72M: UAC RW, clear X. 16 B page.
- sim: mprotect cannot map MMIO -> `grant_region_set` skips non-arena regions (fail-closed).
  The GPIO/LED half is HW-only; sim exercises the IRQ-as-event half against an arena-backed
  fake device (the existing `t_irqdrv` pattern).

## Minimal driver (XMC first, then K64F LED)
Privileged boot does the unsafe one-time setup (pin mux + direction; start a periodic
timer whose NVIC line the driver waits on). The UNPRIVILEGED driver is granted only: the
GPIO data-register block (toggle, but cannot re-mux), the timer control block (to W1C its
own interrupt flag), and the timer IRQ line via the existing tier-1 path. The IRQ path
needs NO kernel change (`irq_register`/`irq_wait`/`irq_event_isr`/`irq_ack`); the only
device-specific need is clearing the peripheral's interrupt flag, which is exactly what the
timer MMIO grant is for (else the line re-asserts on unmask -> storm). Region budget on
ARM PMSA: code + appdata + GPIO MMIO + timer MMIO + stack = 5 of 8.

## Risks
- SYSMPU peripheral gating (was top HW risk) -- RESOLVED: SYSMPU does not gate peripherals
  (AIPS PACR does, coarse/per-slot). Per-thread peripheral isolation lands only on the
  CPU-side-MPU chips (XMC PMSA, RISC-V PMP, RX MPU); lead there.
- Pow2 over-grant on PMSA/PMP -- reject non-pow2 windows, do not round (over-grants
  neighbors). Awkward blocks (e.g. PIT) may need a padded window or two descriptors.
- Fault-vs-grant: an ungranted MMIO access must be reported not escalated; a device access
  may surface as BusFault (not MemManage) on some Cortex-M / a bus error on SYSMPU -- the
  fault reporter must decode both + print the address.
- XN mandatory on every backend (never grant X to MMIO); keep a static check.
- Region budget (8): code+appdata+data+MMIO(xN)+stack. A 2-MMIO+data driver is 7 on
  ARMv7-M; that is the trigger for Option B / a params array.

## Sequencing vs the stack-ownership refactor
Land the stack/region refactor first; MMIO then rebases mechanically because it is a pure
ADDITION to `domain->regions[]` (no edit to `thread_create`'s composition loop). Both
streams touch the `KICKOS_MPU_MAX_REGIONS` budget assert and `domain_for` -- whoever lands
second rebases those. Do NOT add MMIO handling inside `thread_create`.
