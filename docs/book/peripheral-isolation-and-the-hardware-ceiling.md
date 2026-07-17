<!-- SPDX-License-Identifier: CECILL-C -->
# Peripheral isolation and the hardware ceiling

> **Status: DRAFT.** A worked example of the rule that governs every isolation
> claim KickOS makes: *you can only enforce what the silicon enforces.* The same
> microkernel design -- an unprivileged userspace driver granted just its own
> device window -- is a genuine per-thread capability on one chip and physically
> impossible on another, and nothing in the OS can close that gap. The lesson is to
> read the hardware honestly and state the per-chip ceiling, not a portable promise.
> Points into `../reference/architecture.md` (Memory domains -- the peripheral-MMIO
> matrix) for the exact contract.

## The promise, and its limit

A microkernel's pitch is that a driver is just an unprivileged program: it is handed
the registers of *its* device and nothing else, so a bug in it cannot corrupt the
kernel or another driver. KickOS makes exactly this promise for memory -- a thread
sees its own code, its own data, its own stack, and any region deliberately shared
with it, and the MPU faults everything else (Chapter 7). The natural next step is to
make a *peripheral* one more region in that set: grant the driver thread the device's
register block, deny it to everyone else.

The catch is that "deny it to everyone else" is not a decision the OS gets to make on
its own. Memory protection is a hardware unit, and whether that unit can gate a
*peripheral* access -- and at what granularity -- is a property of the chip, not of
the kernel. On some chips the promise holds exactly. On others the hardware simply
cannot express it, and no amount of clever kernel code recovers it. This chapter is
the concrete story of discovering that boundary on real silicon.

*Further reading: Tanenbaum, Modern Operating Systems, ch.1 (protection, user vs
kernel mode) and ch.3 (memory management / protection hardware -- why the MMU or MPU,
not the OS, is the thing that actually enforces a boundary).*

## The one question that decides it

Every per-task protection unit answers the same question -- "may *this* execution
context touch *this* address?" -- but the units live in different places, and that
placement is everything:

- A **CPU-side** unit sits on the processor's own access path. It inspects the
  address of every load, store, and instruction fetch the core issues, *before* the
  access leaves the core. ARM's v7-M PMSA MPU, RISC-V's PMP, and the Renesas RX MPU
  are all of this kind. Because they see the raw address stream, they see accesses to
  the peripheral aperture (0x4000_0000-ish) exactly as they see accesses to RAM.
- A **bus-slave-side** unit sits out on the interconnect, guarding particular slave
  ports (flash, the SRAM banks). Freescale/NXP's Kinetis "SYSMPU" is this kind. It
  guards the memories hanging off the crossbar -- but the peripherals hang off a
  *separate* bridge that is not one of its guarded ports, so it never sees a
  peripheral access at all.

For RAM the distinction does not matter: both kinds guard the SRAM the stacks and
data live in, so per-thread memory isolation is uniform across the fleet. For
*peripherals* it is the whole game. A CPU-side unit can carve a per-thread device
window; a bus-slave-side unit is blind to devices and something else must gate them.

## The K64F experiment: what the datasheet did not say

KickOS's first userspace driver was built on the FRDM-K64F to answer precisely this
question on hardware. A privileged bring-up shim starts a periodic timer (the PIT);
the unprivileged driver thread is granted *only* the PIT's register window and its
interrupt, and its job each tick is to write-1-clear the timer's own flag through that
granted window -- the direct unprivileged device write that would prove the grant.

The reference manual already hinted at trouble: the SYSMPU's guarded slave ports are
flash and the SRAM banks; the peripheral bridge is listed as "protection built into
the bridge," a different mechanism entirely. But a manual reading is a hypothesis, not
a result -- so we flashed it.

The board answered in one clean run. The *privileged* shim's writes to the PIT
succeeded (the timer ran and fired its interrupt). The *unprivileged* driver's very
first write to a PIT register -- an address *inside* its granted SYSMPU window --
took a bus fault. And the SYSMPU's own error register latched **nothing**: it had not
denied the access, because it never saw it. Same peripheral, same clock, same run;
the only difference was privilege. The conclusion is forced:

1. **SYSMPU does not gate peripherals.** It latched no error; the fault came from
   elsewhere. (This settled the open question the SPI design brief was blocked on.)
2. **But peripherals are gated anyway** -- by the peripheral bridge itself. That bridge
   (Kinetis calls it AIPS) enforces access per *privilege level and bus master*, and
   its per-peripheral control (a `PACR` field) resets to *supervisor-only*. An
   unprivileged store therefore never reaches the register; the bridge rejects it with
   the bus fault we saw.

The fix that made the driver run confirmed the mechanism: the privileged shim clears
the supervisor-protect bit for the timer's peripheral slot, and the unprivileged
driver then ticks cleanly. So user-mode peripheral access on K64F *is* controllable
-- just not by the MPU, and not the way the design assumed.

## Why the fix is not the isolation we wanted

Opening the AIPS slot got the driver working, but read what the knob actually is. The
bridge decides by *privilege and master* -- "may user-mode code touch this peripheral
slot?" -- and there is exactly one user privilege level and one CPU master. It has no
notion of *which* unprivileged thread, because a thread is a software idea the bus has
never heard of. So the moment the slot is opened for the driver, it is open to **every**
unprivileged thread in the system. And the granularity is a whole 4 KB peripheral slot,
coarser than the MPU's byte-level windows (we watched the driver read a register
outside its fine SYSMPU window but inside the same slot, and it succeeded).

That is the ceiling. On K64F you get a *kernel-vs-user, per-peripheral* boundary:
peripherals you leave closed are unreachable by any user code, and peripherals you
open are reachable by all of it. What you cannot get is *driver-A-but-not-driver-B*
peripheral isolation, because the hardware that gates peripherals cannot tell the two
drivers apart. If you truly need that on K64F, the only route is to keep the peripheral
supervisor-only and mediate every access through a kernel syscall -- at which point it
is no longer an unprivileged driver poking its own registers. Memory isolation between
those drivers is untouched; it is specifically the *peripheral* boundary that the
silicon will not draw.

## The fleet: same design, different ceilings

Surveying the other MPU-capable targets against the same question shows the K64F is
the outlier, and shows *why*:

| Chip / unit | Unit sits | Sees MMIO? | Per-thread peripheral window? |
|---|---|---|---|
| XMC4800 -- ARM v7-M PMSA | CPU-side | yes | **yes** -- and no second gate to fight |
| ESP32-C6 -- RISC-V PMP | CPU-side | yes | **yes** -- PMP discriminates per thread; a coarse APM gate must be opened once |
| RX72M -- RX MPU | CPU-side | yes | **yes** (architecturally; this backend is not yet silicon-proven) |
| K64F -- SYSMPU | bus-slave-side | **no** | **no** -- AIPS gates per privilege, not per thread |

Two chips are worth pairing. The ESP32-C6 has the *same shape* as the K64F -- a
separate bus-side permission unit (its "APM") that defaults to denying user-mode
peripheral access -- yet the *opposite* outcome. The difference is that on the C6 the
*per-thread* unit is the PMP, and the PMP *does* see peripheral addresses. So the APM
becomes a one-time "open the peripheral aperture to user mode" switch, after which the
PMP still draws the per-thread line. On the K64F the per-thread unit (SYSMPU) is blind
to peripherals, so its second gate (AIPS) is the *only* peripheral authority -- and it
cannot draw a per-thread line. Same architectural pattern, and the capability turns on
one detail: whether the unit that knows about threads is also the unit that sees
devices.

## The lesson

The design brief that started this work assumed a portable model: grant a driver its
device window, done. That model is real -- on CPU-side-MPU chips (XMC, PMP, RX) it is
exactly right, and it is where KickOS should demonstrate the unprivileged-driver
story. But it is a *CPU-side-MPU* model, not a universal one, and writing it as a
portable guarantee would have been a promise the K64F cannot keep.

So the rule KickOS follows, in the reference and in the code, is to state the ceiling
per chip rather than a single portable claim: memory isolation is uniform; peripheral
isolation is per-thread where the hardware sees devices from the CPU side, and coarser
(kernel-vs-user, or syscall-mediated) where it does not. The honest sentence is the
title of this chapter -- the OS cannot enforce a boundary the silicon will not draw,
and the design's job is to know, per target, where that ceiling sits. The exact
per-chip matrix lives in `../reference/architecture.md` (Memory domains); this chapter
is why it reads the way it does.
