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

### M4 -- the driver era (make M3 real, fleet-wide)
M3 proved the mechanisms (endpoints/IPC, console handover, panic reclaim, clock-select) but each
on ONE or TWO chips. The driver era reuses them and makes them REAL ACROSS THE FLEET, then grows
a driver framework on top. Single-core throughout. Full gap list + sequencing in
`docs/design-driver-era-scope.md`.
- **The objective: driver support is the VEHICLE that validates the service APIs against real
  hardware variation** -- prove the console/UART, gpio, pinmux, clock/power, and bus (SPI/I2C)
  service APIs are genuinely vendor-neutral, not accidentally shaped around one vendor. Full
  driver support is the means; a hardware-hardened, vendor-neutral service API is the end.
- **The API-neutrality matrix -- FOUR easy-to-flash boards, diverse across BOTH vendor AND
  arch/MPU family:** c6 (ESP32-C6, rv32imac / PMP / Espressif), xmc (XMC4800, armv7m / PMSAv7 /
  Infineon), k64f (FRDM-K64F, armv7m / SYSMPU / NXP), rx72m (RXv3 / RX-MPU / Renesas). Four
  vendors x four arch/MPU families is a far stronger neutrality test than adding another ARM
  board -- a bias baked around one vendor cannot survive all four.
- **Scope guard -- the ASPIRATION is FULL per-board peripheral coverage, PRIORITIZED not
  restricted.** Supporting *all* of a board's peripherals is how we discover hardware that needs
  a dedicated / NEW service API -- that discovery IS the point -- so coverage is NOT bounded
  a-priori to the service-defined classes (console/UART, gpio, pinmux, clock/power, SPI/I2C).
  The bound is a COMPLEXITY-vs-GAIN weighting per candidate (api-discovery value + real
  usefulness + cross-vendor coverage, against bring-up effort + spec depth + DMA/IRQ plumbing +
  the no-probe penalty on c6/rx72m), NOT a class restriction. That weighting -- the per-board
  backlog + the cross-board neutrality shortlist in `docs/design-m4-driver-matrix.md` -- is what
  keeps scope from ballooning (analog-in, PWM, the event fabric, the on-chip EtherCAT SC are
  exactly the high-gain non-class peripherals it surfaces). Still prefer MAPPING different
  drivers to different boards over duplicating one driver on all four.
- **Scope guard -- the no-probe constraint is a design INPUT:** rx72m and c6 are easy-flash but
  have NO usable debug probe, so bring-up there is print-debug only. The services must therefore
  be console-observable, or two of the four matrix boards cannot be brought up at all.
- **Fleet userspace UART / console drivers + per-chip `arch_console_reclaim` + handover
  validation** -- `user/driver/xmcuart` is the only console driver today; every other board is
  still kernel-owned, and only XMC + K64F ship a reclaim body (the fault-funnel porting invariant:
  no real reclaim => a driver-garbled UART silently eats the panic banner). One driver per chip
  family, silicon-available first; isolation is real only where the MPU gates peripherals.
- **Clock-select fleet-wide** -- extend `arch_cpu_clock_set` per opt-in chip, or keep the weak
  default explicitly.
- **The enabling services** -- **init** (separate init from the app; spawn drivers-with-caps in
  dependency order; settle the entry-point rename EARLY), **clock-tree / power-manager**, **pinmux**
  (one-shot init-time config), **gpio** (a pin allocator that mints per-pin caps -- cold IPC to
  allocate, direct MMIO to toggle). Deep-dive prose under "## Later" below.
- **The driver framework** -- a call/reply (reply-cap) IPC layer on CAP_ENDPOINT for synchronous
  SPI/I2C drivers; the driver-lib / demo split; multi-instance = thread-per-instance. The
  class/service duality (driver-lib class as the primitive, service composed on top; the consumer
  picks the coupling and pays only for what it uses) is designed in `docs/design-m4-driver-model.md`.

### M5 -- SMP (one kernel image across cores)
Run a multi-core part at 100% under a single KickOS -- not two AMP instances. Reworks the
foundation: `IrqLock` ("interrupts off => exclusive") is single-core-only. Plan: a **Big Kernel
Lock** first (redefine `IrqLock` = local-IRQ-off + one global spinlock -- coarse but correct, and
byte-identical on single-core builds), then per-core run-queues + finer locks as optimisation only
where real atomics exist. Fits the seL4 big-lock lineage. Candidate ranking by the real gate
(inter-core atomic + arch-switch maturity: RP2350 M33/Hazard3 best -> RP2040 big-lock-only ->
ESP32 LX6 last), the staged model, and the SMP-is-per-chip-capability constraint are spiked in
`docs/design-m5-smp.md` (with the AMP-vs-SMP feasibility in `docs/design-multicore.md` and the
cross-core IPC in `docs/design-multicore-ipc.md`).

## Later
Multi-domain isolation + cross-domain shared-memory IPC; message-passing IPC + userspace drivers;
**service publication** (naming/discovery, capability delegation, badged endpoints, an interface
convention); runloops + multi-object waiting; timed wait (`sem_timedwait`) as one unified wait
primitive; introspection; a HAL/driver model; pluggable EDF / rate-monotonic policies; loadable
MPU-isolated user modules; POSIX / CMSIS-RTOS2 compat; TLSF heap; RP2040 SMP; Renode CI; and
**the Book** as the durable how-&-why reference (see `docs/book/`).

### RISC-V context-switch cost -- optimization (post-M6, not scheduled)
The rv32 trap-based switch software-saves the full integer file (~60 stack words/switch vs
armv7m's ~18 with hardware exception stacking) -- a ~3.5x per-handoff cost, general to RISC-V
(not C6-specific; Hazard3 shares it). Two levers, both fable-gated: (a) a **cooperative
fast-path** (voluntary switches save only callee-saved regs, ~2x, portable to every rv32 target
incl. the C6), and (b) an **optional Zcmp `cm.push`/`cm.pop`** compile-gated path (Hazard3-only;
compresses instruction count, not memory traffic; single-digit % on top of (a), mostly code
size). Full analysis, the compile-gate design, and the prerequisite bench-bracket fix in
`docs/design-riscv-switch-cost.md`.

### ARMv8-M TrustZone kernel-confinement backend (post-M6, opt-in, per-chip)
The armv8-M-with-Security-Extension MECHANISM for kernel confinement: kernel/TCB in Secure state,
apps in Non-secure. Not a per-task isolation mechanism and NOT an MPU replacement -- NS tasks are
still isolated from each other by MPU_NS at the same per-switch cost. The framing that makes it fit:
kernel confinement is already arch-dependent (PMSAv7 background-drop, PMP locked entries, RX-MPU,
SYSMPU RGD0); uniformity lives in the GOAL (confined TCB + per-task isolation + capability
authority), not the register mechanism. So TrustZone is simply the strongest armv8-M realization of
the parked "confine the kernel / drop PRIVDEFENA" goal (Option B), layered on top of Option B rather
than replacing it; chips without the extension use Option B alone. Buys a hardware TCB boundary
(NS-privileged still cannot touch Secure memory) + a PSA-style secure-services partition that fits
the capability-gated-services model. A security/assurance play, not a performance one. Post-M6
(needs the M4 service model + M5 SMP settled); per-chip capability (M23/M33/M55/M85 MAY have it,
detect + fall back); RP2350's M33 is a concrete target. Spike: `docs/design-armv8m-trustzone.md`.

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
real-app story exists yet. Not gated by any hardware capability; its home is the
driver-era workstream (now **M4**), not a later hardware-gated milestone.

### Userspace power-manager service (driver-era; mechanism/policy split)
The M3 clock-select syscall (`arch_cpu_clock_set`) is deliberately a MECHANISM seam: change
the CPU/bus clock COHERENTLY (re-anchor the monotonic clock, re-derive baud, re-arm timers)
and return the landed Hz. POLICY -- which P-state when, DVFS, idle/low-power governors --
belongs in userspace, in a dedicated **power-manager driver/service**, exactly as the console
DEVICE moved to a userspace UART driver. Like the console, the privileged steps a userspace
driver cannot safely touch (flash wait states, voltage/regulator scaling, PLL relock) stay
kernel-side behind the seam; the power manager drives policy through it. Pairs with the driver
era + the init service; the M3 seam is the stepping stone, not the final home.

**The fuller vision -- a clock-tree service.** The service is really the OWNER of the whole
clock TREE: the PLL, dividers/muxes, and the tree-level clock gates (which live in the shared
SCU/RCC/SIM block, NOT per-peripheral windows, so they are refcounted CENTRALLY -- a branch
feeding two peripherals gates off only when both are idle; per-driver gating covers only a
peripheral's LOCAL enable). Because peripheral clocks are DERIVED from the shared PLL, a rate
change CASCADES: every derived-clock consumer must re-derive (a UART re-derives baud, an SPI
its prescaler) -- a rate-change-notifier fan-out (Linux Common-Clock-Framework shape). The
kernel is itself such a consumer (its monotonic clock + timer), so it can never fully leave:
the irreducible KERNEL RESIDUE is (a) re-anchor its own clock atomically on a rate change, and
(b) gate the safety-critical privileged steps (flash wait-states, voltage/regulator, PLL
relock) so a service BUG is wrong policy (restartable), not a flash-controller hard-fault.
Authority is a delegatable clock-control CAPABILITY (the service holds it like a driver holds
an MMIO grant), not full privilege. This is the console-handover pattern applied to the clock:
machinery -> userspace service, kernel keeps only the re-anchor + privileged-step residue.

### M6 -- the MMU / new-platform horizon (foundational)
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
  Extends the M5 AMP/IPC work from homogeneous cores to a heterogeneous application-core +
  MCU-core split under a shared IPC contract.
Captured, not scheduled -- an exploratory design/research spike scopes feasibility (the MMU memory
model, the two boot models, the A53/M7 IPC seam) when M4 (driver era) / M5 (SMP) have settled.
