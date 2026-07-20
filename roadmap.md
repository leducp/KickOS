<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS roadmap

The milestone-level plan: the general idea to tackle per milestone. **No granular items** --
those live in `TODO.md` (the actionable checklist); the design behind them lives in
`docs/reference/architecture.md`; validated end-state lives in `M1_state.md`.

**Milestones are keyed to THEME, not sequence.** A milestone names a *capability the kernel
gains*, not a date. Work that merely follows M1 is not "M2" unless it needs the MPU; orthogonal
work (perf, a real-peripheral-IRQ demux, a userspace driver) is **anytime coherence** and lands
whenever it is ready, tagged as such in `TODO.md`.

## Done

- **M0 -- x86 sim.** The real kernel + an unprivileged userspace app in one Linux process:
  tickless scheduler, semaphores, syscalls across the SVC boundary, `mprotect`-emulated MPU,
  IRQ-as-event, OS-agnostic `main`, the pooled-object pattern. Runs in CI. (Detail in git.)
- **M1 + M1.x -- the MCU fleet.** First silicon, then breadth: **10 boards across 5 ISAs**
  (armv7m, armv6m, RXv3, RV32IMAC, Xtensa LX6) up on hardware, privilege + SVC (no HW MPU yet),
  each with a console, tickless timer, fault dump, and inject-driven IRQ path; plus telemetry,
  the buffered console, and per-chip clock bring-up. Full record in `M1_state.md`.

## Next

### M2 -- hardware MPU enforcement
Make per-task isolation real on silicon. **Status:** the enforcement mechanism has landed on
silicon across the reference set -- K64F SYSMPU, XMC PMSA, RX72M MPU, ESP32-C6 PMP -- each with
selftest under enforcement plus a cross-domain `mpu_fault` trap; the arch-independent floor
(memory domains, per-thread private stacks, pow2 region placement, confused-deputy out-pointer
copy-in) is in. Remaining tail (STM32/RP2040 silicon, C6 peripheral APM open, RX region-skip
fail-closed fix, the deferred syscall-buffer bounds) is tracked in `TODO.md` / `docs/m2-readiness.md`.
Two halves:
- **Mechanism, per chip** -- `arch_mpu_apply()` backends wired into the task-switch hook, one
  distinct mechanism class at a time (the discipline: prove `{base,size,attr}` is sufficient,
  never leak a per-arch field). Reference pair first: **RISC-V PMP/NAPOT** (traps in CI without
  silicon) + **XMC v7-M PMSA**; then **K64F SYSMPU** (byte-granular), **RX72M** (non-ARM), then
  the same-mechanism tail.
- **Security model, arch-independent** -- the memory-domain object (a shared region set) with
  **per-thread private stacks**, power-of-two region placement, and **syscall-argument /
  user-pointer validation** (the soundness floor). Design in `docs/reference/architecture.md`;
  readiness matrix in `docs/m2-readiness.md`.

### M3 -- capabilities & object model (and user clock-select)
The object/credential model on top of M2's enforcement:
- **Per-task typed handle table** (Zircon/seL4 shape) replacing global object ids -- rights bits,
  refcounted, destroy-on-last-close; **authenticated grant ownership** as its memory-side twin.
  **LANDED -- the first M3 capability:** the semaphore syscall ABI migrated from global object ids
  to a per-task `CapEntry` table with a single `cap_resolve` chokepoint, silicon-validated under
  enforcement on all four M2 mechanism classes, plus authenticated-grant spawn delegation
  (subset-only rights narrowing, deterministic B1 placement). The items below stay open.
- **One blocking primitive**, not an object zoo -- a cap-named wait/wake object; richer sync
  built in userspace; the sole justified typed object is a priority-inheritance mutex.
- **Console *device* handover** -- a userspace UART driver takes the peripheral as a capability;
  the kernel relinquishes it and the panic path reclaims + re-inits it.
- **Low-barrier hard constraint** -- a plain app never writes a capability manifest; the runtime
  wires a sane default cap set (never resurrect CapDL-to-boot friction).
- **User-selectable CPU clock / low-power mode** -- the `sys_cpu_clock_hz()` read syscall has
  landed; the write side (clock-select / low-power) stays open.

### M4 -- SMP (one kernel image across cores)
Run a multi-core part (dual-core RP2040) at 100% under a single KickOS -- not two AMP instances.
Reworks the foundation: `IrqLock` ("interrupts off => exclusive") is single-core-only. Plan: a
**Big Kernel Lock** first (redefine `IrqLock` = local-IRQ-off + one global spinlock -- coarse but
correct), then per-core run-queues + finer locks as optimisation. Fits the seL4 big-lock lineage.

## Later
Multi-domain isolation + cross-domain shared-memory IPC; message-passing IPC + userspace drivers;
**service publication** (naming/discovery, capability delegation, badged endpoints, an interface
convention); runloops + multi-object waiting; timed wait (`sem_timedwait`) as one unified wait
primitive; introspection; a HAL/driver model; pluggable EDF / rate-monotonic policies; loadable
MPU-isolated user modules; POSIX / CMSIS-RTOS2 compat; TLSF heap; RP2040 SMP; Renode CI; and
**the Book** as the durable how-&-why reference (see `docs/book/`).

### Userspace init service (driver-era; not hardware-gated -- anytime-coherence)
Today the user's `main` doubles as pid-1: it IS the init entry, holds full userspace
rights, and spawns every task -- so (1) the app's `main` is really the SYSTEM init wearing
the app's name, and (2) that init pattern (create endpoint, publish console, spawn the
driver with caps, close the parent cap, spawn apps -- exactly the M3 handover choreography)
gets re-hand-rolled by every root task. Idea: rename the entry (`kos_init_entry` /
`kos_init_userspace`) to separate init from the app, and ship a DEFAULT init service that
does configurable bring-up then calls the real user `main` with a configurable capability
set. A power user links their OWN init service instead. Constraints: keep the LOW-BARRIER
zero-config default (a plain app still writes no manifest -- the default init wires the sane
cap set; never reintroduce CapDL-to-boot friction), and the entry RENAME is a consumer-facing
breaking change -- settle the entry-point seam EARLY (a cheap-now-vs-break-later quick-win)
rather than after consumers bake in `main`. Formalizes the implicit root task. Its NATURAL
home is the **driver era** -- spawning drivers-with-caps + a proper driver API is what turns
"KickOS runs on one board" into "any app builds on KickOS," and the init service is a gating
enabler for that. It is when real user apps can actually land: today KickCAT is the only
consumer and it is a POC (one board, a driver more demo than proper API), not evidence the
real-app story exists yet. Not gated by any hardware capability, so it pairs with the
driver-era workstream (whatever milestone number that carries) rather than a fixed M4/M6.

### Userspace power-manager service (driver-era; mechanism/policy split)
The M3 clock-select syscall (`arch_cpu_clock_set`) is deliberately a MECHANISM seam: change
the CPU/bus clock COHERENTLY (re-anchor the monotonic clock, re-derive baud, re-arm timers)
and return the landed Hz. POLICY -- which P-state when, DVFS, idle/low-power governors --
belongs in userspace, in a dedicated **power-manager driver/service**, exactly as the console
DEVICE moved to a userspace UART driver. Like the console, the privileged steps a userspace
driver cannot safely touch (flash wait states, voltage/regulator scaling, PLL relock) stay
kernel-side behind the seam; the power manager drives policy through it. Pairs with the driver
era + the init service; the M3 seam is the stepping stone, not the final home.

### The MMU / new-platform horizon (post-M6, foundational)
The biggest axis beyond the MCU fleet: today the whole memory model is **one physical address
space + per-thread MPU regions**. A real **MMU (VMSA / page tables)** adds virtual address spaces
-- a foundational change, not a port, and its own milestone-class effort. The **Domain seam** is
deliberately shaped to absorb it (a domain becomes a page-table root instead of an MPU region set).
Concrete targets that motivate it:
- **x86_64 PC target** -- KickOS as an actual OS on a PC (QEMU-first, then bare metal): MMU
  paging, a different boot/privilege/interrupt model (long mode, ring0/3, APIC). This is where
  `__KickOS__` earns its name.
- **i.MX8MP -- heterogeneous AMP across profiles** -- an **MMU KickOS on the Cortex-A53(s)**
  (VMSA) alongside an **MPU KickOS on the Cortex-M7**, one per core-cluster, over cross-core IPC.
  Extends the M4 AMP/IPC work from homogeneous cores to a heterogeneous application-core +
  MCU-core split under a shared IPC contract.
Captured, not scheduled -- an exploratory design/research spike scopes feasibility (the MMU memory
model, the two boot models, the A53/M7 IPC seam) when M3/M4 have settled.
