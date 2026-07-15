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
Make per-task isolation real on silicon. Two halves:
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
- **One blocking primitive**, not an object zoo -- a cap-named wait/wake object; richer sync
  built in userspace; the sole justified typed object is a priority-inheritance mutex.
- **Console *device* handover** -- a userspace UART driver takes the peripheral as a capability;
  the kernel relinquishes it and the panic path reclaims + re-inits it.
- **Low-barrier hard constraint** -- a plain app never writes a capability manifest; the runtime
  wires a sane default cap set (never resurrect CapDL-to-boot friction).
- **User-selectable CPU clock / low-power mode** + a `sys_cpu_clock_hz()` read syscall.

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
MPU-isolated user modules; POSIX / CMSIS-RTOS2 compat; TLSF heap; RP2040 SMP; Renode CI; the
**A-profile / MMU (VMSA)** horizon (application-class cores behind the same Domain seam); and
**the Book** as the durable how-&-why reference (see `docs/book/`).
