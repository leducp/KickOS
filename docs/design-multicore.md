<!-- SPDX-License-Identifier: CECILL-C -->
# Design: multicore (AMP / SMP) feasibility -- RP2040 + RP2350

Scope: a feasibility study + phased plan for running KickOS on more than one core,
grounded in the two dual-core SoCs in hand -- RP2040 (2x Cortex-M0+, ARMv6-M) and
RP2350 (2x Cortex-M33 ARMv8-M **or** 2x Hazard3 RV32IMAC(B), ISA-selectable). No
build/runtime code changes: this is a spike. Cites live code (file:line) and the
two datasheets by section. Aligns with the stated end goal in `roadmap.md` (M4 =
one SMP kernel image, Big-Kernel-Lock lineage) and refines the FIRST step.

## Verdict

**AMP first, on RP2040; then SMP-BKL, primarily on RP2350-M33.** The roadmap end
state is a single SMP image (`roadmap.md` M4) -- that does not change. But the
right first *prototype* is AMP (one kernel instance per core, message-passing over
the SIO FIFO/doorbell + a shared-memory partition), because it de-risks the three
hardware mechanics that SMP *also* needs -- core-1 bring-up, cross-core IPC, and
console/peripheral arbitration -- WITHOUT simultaneously forcing the invasive
mutual-exclusion refactor. AMP keeps each core's `IrqLock` (PRIMASK) valid as-is,
because an instance only ever touches its own state. You validate silicon bring-up
and the per-core MPU isolation win against known-good hardware, THEN do the
`IrqLock`-becomes-BKL rework on top. The AMP substrate is already ~80% present
(`KICKOS_MULTI_INSTANCE`, below).

RP2040 is chosen for the AMP prototype because it is silicon in hand with core-0
bring-up already done, and AMP dissolves its hard problem (no atomics) -- a
core-private scheduler needs no shared lock. The M0+ no-atomics wall (below) is
exactly why SMP's finer-grained future must land on RP2350, not RP2040.

## Invariant-first: what "single core" means in the code today

The whole kernel assumes ONE running core reached through unindexed global state.
Every item below is a place that fact is baked in.

INV-1  Mutual exclusion == mask local interrupts.
  `IrqLock` (kernel/include/kickos/irqlock.h:17-24) wraps `arch_irq_save` /
  `arch_irq_restore`. Per-arch mechanism:
    - armv6m: PRIMASK (`cpsid i`) -- arch/arm/armv6m/arch_armv6m.cc:99-111.
    - armv7m: BASEPRI band -- arch/arm/armv7m/arch_armv7m.cc:164-190.
    - rv32imac: mstatus.MIE (`csrrci`) -- arch/riscv/rv32imac/arch_rv32imac.cc:160-173.
  All THREE mask only the issuing core. On a second core they provide ZERO mutual
  exclusion. Every critical section in sched.cc / sync.cc / domain.cc / irq / time
  rests on this. This is the foundation break for SMP.

INV-2  One current thread + one run queue, held in the instance singleton.
  `Kernel::current`, `idle`, `live`, `ready[KICKOS_NUM_PRIO]`, `ready_bitmap`
  (kernel/include/kickos/instance.h:32-36), reached via `kernel()`
  (instance.h:89-96). `switch_to` writes `kernel().current` (sched.cc:24-39);
  `reschedule` picks one `pick_next` (sched.cc:89-98). One decision point, one
  running thread.

INV-3  The arch switch target is a bare global pair, no CPU index.
  `g_arch_current` / `g_arch_next` (`struct arch_context* volatile`):
    - ARM (shared v6m/v7m): arch/arm/common/arch_arm_common.cc:39-40; PendSV reads
      current/next and stores current (armv7m/switch.S:41,63,65; armv6m/switch.S:27,52-55).
    - rv32imac: arch_rv32imac.cc:78-79; msip switcher (switch.S:228,234,236).
  Plus per-arch single-hart companions: `g_isr_depth` (arch_rv32imac.cc:84, the
  IPSR!=0 analog) and `g_clint_msip` (arch_rv32imac.cc:90, a pointer to ONE hart's
  CLINT msip word). Two cores each need their own of every one of these.

INV-4  One MPU/PMP, programmed on the local unit each switch.
  `arch_mpu_apply` stashes the whole active set at the switch decision (sched.cc:34);
  `kickos_arch_mpu_commit` programs the hardware from the switch epilogue: ARM PMSA
  (arch_arm_common.cc), RISC-V PMP (arch_rv32imac.cc). Each core has its OWN MPU/PMP
  -- programming is inherently per-core. This is an isolation WIN (below), but the
  commit site assumes it drives the one-and-only MPU.

INV-5  Boot owns the whole machine on one core.
  `kmain` (kernel/init/kmain.cc:126-167) does all bring-up, creates idle+root, and
  calls `sched::start` (sched.cc:77-87) which never returns. No notion of "core 1
  also needs an idle thread / its own start". Confirmed: RP2040 parks core 1 in the
  bootrom forever today (arch/arm/chip/rp2040/chip_rp2040.cc:395; core-0 only).

INV-6  The console is a single unsynchronized producer.
  `kconsole_write` (kernel/init/console.cc:68-106) and the buffered ring
  (console_tx) are single-producer by construction (console.cc:50-60 is the choke
  point). Two cores writing race with no lock. The diag LED (chip_rp2040.cc:367-395)
  and any shared peripheral have the same exposure.

INV-7  Per-thread reentrancy / EH state is already thread-local -- and stays so.
  newlib `_impure_ptr` / EH context are swapped per thread at the context switch,
  not held in a core global, so they do NOT become a new cross-core hazard: they
  ride the TCB. This is the one axis multicore does NOT worsen.

Repo-wide sweep confirms ZERO multicore awareness today: no cpuid/coreid/mhartid/
mpidr, no SIO FIFO/spinlock/doorbell use (SIO touched only for the LED GPIO), no
sev/wfe, no LDREX/STREX/LR/SC/amo used for locking. Forward-looking notes only:
`TODO.md` (M4 block) and `roadmap.md` (M4) already name the BKL plan; slotpool.h:15
comments "the SMP kernel lock later".

## Hardware mechanics (from the datasheets)

### RP2040 (ARMv6-M, 2x Cortex-M0+) -- RP-008371-DS

SIO, the single-cycle IO block, is core-local hardware on each M0+ IOPORT
(0xd0000000..0xd000017c), reachable only by the processors (DS 2.3.1). It carries:

  - CPUID (DS 2.3.1.1): core 0 reads 0, core 1 reads 1. The per-core identity the
    per-CPU seam keys on. (Distinct from the M0+ PPB CPUID.)
  - 32 hardware spinlocks SPINLOCK0..31 (DS 2.3.1.3): READ = attempt-claim (nonzero
    = won, zero = already held); WRITE (any value) = release; core 0 wins a tie.
    Poll to acquire. Meant to guard the SHORT critical sections of higher primitives.
  - Two inter-processor FIFOs / mailboxes (DS 2.3.1.4): 32-bit x 8 deep, one per
    direction; FIFO_WR / FIFO_RD / FIFO_ST (VLD/RDY/ROE/WOF); a per-core FIFO IRQ
    (system IRQ 15 core-0, IRQ 16 core-1).

CRITICAL CONSTRAINT (M0+ no atomics): ARMv6-M has NO LDREX/STREX, and the SIO/
IOPORT itself "does not support atomic accesses at the bus level" (DS 2.1.2). So on
RP2040 the ONLY cross-core mutual-exclusion primitive is the SIO hardware spinlock.
You cannot write a C11-atomic CAS lock, nor any Treiber/Michael-Scott-style
lock-free structure, on M0+. Consequence: RP2040 SMP is BKL-or-nothing (all locks
built on SIO spinlocks); finer lock-free scheduling is unreachable on this part.

Core-1 launch (DS 2.8.2): after reset core 1 sleeps (WFE, SLEEPDEEP) in the bootrom
`wait_for_vector` until core 0 hands it, over the TX FIFO, the six-word sequence
`{0, 0, 1, VTOR, SP, PC}`, each word echoed back by core 1 before advancing; a `0`
is preceded by draining core-1's replies and a `__sev()`. Robust restartable
handshake. Per-core NVIC + per-core SysTick already exist on each M0+.

### RP2350 (ARMv8-M M33 / RV32IMAC Hazard3, 2x) -- RP-008373-DS

Homogeneous by default, ISA-selectable. Two "processor sockets" (core 0, core 1);
each socket is filled at reset by EITHER a Cortex-M33 (ARMv8-M Main) OR a Hazard3
(RV32IMAC), per the OTP ARCHSEL register; the unused processor is held in reset with
clocks gated (DS 3.9). Default = Arm (both allowed unless CRIT0/CRIT1 OTP flags
force one); set CRIT1_BOOT_ARCH for RISC-V. ARCHSEL is sampled ONLY at reset, so a
watchdog-reset can flip architecture in software. ARCHSEL has one bit PER socket, so
mixed Arm+RISC-V is *physically possible* but "practical applications are limited,
since this requires two separate program images" (DS 3.9.2) -- i.e. NON-default /
exotic. Confirmed: the SoC is homogeneous dual-M33 or dual-Hazard3 by default, not
ARM+RISC-V at once.

SIO v2 (DS 3.1): still 32 SPINLOCK0..31 (DS 3.1.4, kept for RP2040 compatibility)
and CPUID (0/1, DS 3.1.2). But BOTH processor types have native atomics -- M33 has
LDREX/STREX, Hazard3 has the RISC-V A extension (amoadd.w etc.) -- backed by a
global exclusive monitor (DS 2.1.6), so real shared-SRAM locks work across cores
(DS 3.9.2 notes an Arm ldrex/strex and a RISC-V amo.w can share one variable). New:
Doorbells (DS 3.1.6) -- 8 flags each direction, DOORBELL_OUT_SET raises the opposite
core's core-local SIO_IRQ_BELL (int 26); accumulative "ring once, answer once"
events, the clean AMP notify primitive. FIFOs are now 32-bit x 4 deep, core-local
SIO_IRQ_FIFO (int 25). Secure and Non-secure SIO banks are SEPARATE (own spinlocks,
FIFOs, doorbells) so NS code cannot starve S code (DS 3.1.1); this interacts with
TrustZone on the M33 build. Erratum RP2350-E2: writes above SIO +0x180 alias the
spinlocks (SDK uses atomic-memory workaround) -- prefer native atomics over SIO
spinlocks on M33/Hazard3 anyway. Core-1 launch (DS 5.3) is the same FIFO
wait_for_vector handshake, extended to carry the arch marker.

Backend cost note: RP2350-M33 currently reuses the `armv7m` arch layer (armv8-m is a
superset); the PMSAv8 MPU (RBAR/RLAR base+limit, MAIR attrs) is deferred design in
`docs/design-rp2350.md`. Dual-Hazard3 reuses `arch/riscv/rv32imac` + PMP. So RP2350
is uniquely a SINGLE board that can run KickOS as dual-ARMv8-M or dual-RV32IMAC --
a rare cross-ISA test vehicle for the same SMP/AMP code paths.

## Design space (concrete cost for KickOS)

### AMP -- two independent kernel instances, message-passing

Each core runs its own `Kernel` instance and its own scheduler; they share only an
explicit memory partition and a FIFO/doorbell channel.

  - Fit: `KICKOS_MULTI_INSTANCE` (instance.h:89-96) ALREADY selects a per-instance
    `Kernel` via a thread-local pointer (built for the KickCAT multi-slave sim: one
    MCU == one instance). AMP is the same shape with a per-CORE pointer instead of
    per-host-thread. The runtime bookkeeping is already instance-scoped; the residue
    is the file-static idle/root TCBs in kmain.cc:57-59 (already flagged there as the
    "invariant #7" scoping debt) and the arch globals in INV-3.
  - Mutual exclusion: each instance touches only its own state, so `IrqLock` ==
    PRIMASK/MIE stays CORRECT unchanged. This is why AMP sidesteps the M0+ no-atomics
    problem for scheduling entirely -- there is no shared run queue to lock.
  - Core-1 bring-up: core 0 boots normally, partitions SRAM (static: a linker
    region per core + a shared IPC region), owns the console/peripherals, then hands
    core 1 its {VTOR,SP,PC} via the FIFO handshake; core 1 runs a second `kmain`.
  - Shared console: one core owns the UART; the other posts log records into a
    shared ring and rings a doorbell (RP2350) or FIFO-IRQs (RP2040). One SIO
    spinlock guards the ring on RP2040; a native-atomic SPSC ring on RP2350.
  - Cost: an IPC transport (FIFO/doorbell driver + a shared-memory queue) and a
    static memory map. Modest, and mostly reusable by SMP later.
  - Matches KickOS's per-core-MPU isolation story: each instance programs its OWN
    MPU (INV-4) -- two hard isolation walls, no new sharing.

### SMP -- one scheduler across cores

  - Must become PER-CPU: current-thread, idle thread, the tickless next-event timer
    + SysTick state, `g_arch_current`/`g_arch_next`/`g_isr_depth`/`g_clint_msip`
    (INV-3), and the MPU program (already per-core hardware).
  - Must become SMP-LOCKED (a real cross-core lock, not just IRQ mask): the run
    queue + `ready_bitmap` (INV-2), every sync object (sync.cc), the semaphore/
    thread/domain pools (instance.h:64-72), the console (INV-6), and any future
    shared allocator.
  - Step 1 = Big Kernel Lock (per roadmap.md M4): redefine `IrqLock` as "mask local
    interrupts AND take one global spinlock." Centralised, so it is a redefinition
    of one class, not a 200-site audit; every existing critical section stays
    correct, coarsely. ~2x on a 2-core MCU is plausible (user threads run
    concurrently; only kernel entries serialise). Per-core run queues + finer locks
    are later optimisation.
  - The lock primitive is where the M0+ split bites: on RP2040 the BKL spinlock MUST
    be a SIO hardware spinlock (no atomics); on M33/Hazard3 it can be a native
    test-and-set over SRAM (global monitor). This forces an arch-abstracted SMP-lock
    seam (below) with two backends. And it caps RP2040 at BKL granularity forever.
  - IRQ affinity/rebalance: device lines land on whichever core the NVIC/PLIC routes
    them to; the first-level ISR must resolve "which core am I" (CPUID/mhartid) and
    act on that core's per-CPU state. New surface not present today.

### KickOS-fit angles

  - Per-core MPU/domain: an isolation WIN either way -- each core enforces its own
    domain set; the Domain model (domain.cc) is unchanged, only `kickos_arch_mpu_commit`
    gains a per-core "which MPU" (implicit: the caller's core).
  - Tickless per core: the next-event timer (time.cc, `ktime_rearm`, sched.cc:38)
    is already per-decision; it just needs per-CPU deadline state and a per-core
    hardware timer (both M0+ have their own SysTick; RP2350 adds a RISC-V platform
    timer, DS 3.1.8).
  - EH/reent state: already thread-local (INV-7) -- no new work.
  - Arch seam growth: today `arch/<isa>/...` names concepts, never mechanisms
    (arch.h). Multicore adds ONE sub-seam: a "cpu" identity + an "smp-lock". Small.

## Minimal new arch-seam surface

Two additions to the ISA-neutral seam (arch/include/kickos/arch/arch.h), each with
per-backend impls. Illustrative signatures (traditional include guards, spelled
operators, no ternary if a body is added later):

  - Per-CPU identity + count:
      unsigned arch_cpu_id(void);    // RP2040/RP2350 SIO CPUID; RISC-V mhartid
      unsigned arch_cpu_count(void); // 1 on every current single-core target
    Backed by: SIO CPUID (rp2040/rp2350), mhartid (generic RISC-V). Lets the kernel
    index per-CPU state and lets the first-level ISR self-identify.

  - Abstract SMP lock (the M0+ vs M33/Hazard3 divergence lives HERE, nowhere else):
      typedef ... arch_smp_lock_t;
      void arch_smp_lock_acquire(arch_smp_lock_t* lk);
      void arch_smp_lock_release(arch_smp_lock_t* lk);
    Backends: armv6m -> a SIO hardware spinlock (read-claim / write-release; the
    ONLY option, no atomics); armv8-m -> LDREX/STREX test-and-set over SRAM;
    rv32imac(Hazard3) -> amoswap.w / lr.w+sc.w. `IrqLock` (BKL build) becomes
    "arch_irq_save() then arch_smp_lock_acquire(&g_bkl)".

  - Core-1 launch + IPC (chip-level, not ISA-level): a `chip_smp_launch(core, entry,
    sp, vtor)` over the FIFO handshake, and a doorbell/FIFO notify primitive. Single-
    core chips leave `arch_cpu_count()==1` and never call it.

Per-CPU STATE change: `g_arch_current`/`g_arch_next`/`g_isr_depth`/`g_clint_msip`
(INV-3) move from scalars to `[arch_cpu_count()]` arrays indexed by `arch_cpu_id()`;
the switch asm gains one CPUID-scaled load. `Kernel::current`/`idle` + the tickless
deadline (INV-2) move into a per-CPU sub-struct. These are the shared prerequisite
for BOTH AMP and SMP.

## Phased plan

Phase 0 -- per-CPU arch seam (no second core yet). Introduce `arch_cpu_id/count`
  (returns 0/1 on every current target, so a no-op refactor) and index the INV-3
  globals by CPU. Land on the existing single-core fleet with zero behaviour change;
  this is the de-risking that keeps later phases small.

Phase 1 -- AMP on RP2040 (silicon in hand). Second `Kernel` instance per core via
  the existing `KICKOS_MULTI_INSTANCE` path; static SRAM partition (linker); core-1
  launch via the bootrom FIFO handshake (DS 2.8.2); a FIFO-IRQ IPC channel; shared
  console behind one SIO spinlock. Validates core-1 bring-up + IPC + per-core MPU on
  real hardware, with each scheduler core-private (PRIMASK stays valid -- no lock
  rework). Deliverable: two KickOS instances trading messages, both MPU-isolated.

Phase 2 -- SMP-BKL. Redefine `IrqLock` = local mask + `arch_smp_lock`. Two viable
  first targets, weighed:
    - RP2350-M33 (RECOMMENDED primary): real LDREX/STREX BKL, and the PMSAv8 backend
      it needs is already scoped (`docs/design-rp2350.md`). Real atomics keep the
      later finer-lock optimisation reachable. Cost: a new armv8-m MPU backend.
    - RP2040 (hard-mode validation): BKL on a SIO spinlock, no atomics. Proves the
      seL4-lineage big-lock on the hardest part and reuses Phase-1 bring-up. Cost:
      accept that RP2040 SMP is permanently BKL-granular.
  Do M33 first for headroom; run the RP2040 SIO-spinlock BKL as the portability
  proof that the `arch_smp_lock` seam actually abstracts the no-atomics case.

Phase 3 -- optimisation + cross-ISA test vehicle. Per-core run queues + finer locks
  (M33/Hazard3 only -- lock-free structures are impossible on M0+). Use the RP2350's
  ARCHSEL to run the SAME SMP code as dual-M33 AND dual-Hazard3 -- one board, two
  ISAs, one CI matrix. Flag ARM+RISC-V-at-once (DS 3.9.2) as explicitly out of scope
  (two images, no practical driver).

## Collides with an existing single-core invariant (required refactors)

  - INV-1 `IrqLock == arch_irq_save`: a SMP hard break. Redefinition (BKL), not a
    site audit, BECAUSE the project already centralised all masking in one class and
    forbade ad-hoc masking -- keep that discipline or Phase 2 becomes a rewrite.
  - INV-3 arch current/next globals: must be per-CPU before ANY second core (AMP or
    SMP). This is Phase 0 and gates everything.
  - INV-6 unsynchronized console + shared peripherals: needs a cross-core producer
    lock (SIO spinlock on RP2040, atomic ring on RP2350) the first time two cores
    run, in AMP already.

## Needs bench silicon to confirm

  - RP2040 core-1 FIFO handshake timing + the SLEEP/WFE behavior when BOTH cores are
    live (chip-wide SLEEP gates the debug bus only when both cores idle).
  - RP2350 core-1 launch + doorbell IRQ latency; the Secure/Non-secure SIO-bank split
    under a TrustZone M33 build; erratum-E2 spinlock aliasing on the actual stepping.
  - The AMP static SRAM partition sizes (per-core arena + shared IPC region) against
    real app footprints.
  - BKL contention / the ~2x claim: measured, not assumed, on a real 2-core workload.
  - Cross-ISA parity: the same SMP image built as dual-M33 vs dual-Hazard3 producing
    identical scheduler behaviour on one RP2350 board.
