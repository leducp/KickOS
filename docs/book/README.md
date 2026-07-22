<!-- SPDX-License-Identifier: CECILL-C -->
# The KickOS Book

**Focus.** The Book is the *how & why* of KickOS: what it is, why it is built this way, and --
as a second purpose -- a teach-how-a-microkernel-works text. It is durable narrative, written
to be read by someone evaluating, learning from, or porting KickOS. It is **not** the exact
technical contract: that is the *Reference* (`../reference/`), which is code-synced and wins on
any disagreement with the code. The Book *explains* and links into the Reference for the precise
contract; a concept here does not become a bug when the code is refactored. (Roadmap/task status
lives in `../../roadmap.md` + `../../TODO.md`; validated state in `../../M1_state.md`. The Book
does not narrate how we got here.)

**What KickOS is.** A minimal, seL4-principled microkernel RTOS: a small trusted kernel
(scheduler, IPC/sync, tickless time, interrupt plumbing, memory protection) with everything
else in userspace. One uniform design across five ISAs -- armv7m, armv6m, RXv3, RV32IMAC,
Xtensa LX6 -- so the *same* semantics hold on every target and divergence is a bug, not a
port quirk. North star: capability-based authority (M3) over MPU-enforced isolation (M2),
eventually MMU-alongside-MPU.

**seL4's paradigm, without its burden.** KickOS takes seL4's *design principles* -- a kernel
small by nature; capability-based authority; drivers/FS/net as unprivileged userspace servers
reached by IPC -- but deliberately does NOT pursue formal verification / CapDL / proof
engineering. The usability benchmark is the sibling project **KickCAT**: *write a `main`, that's
it.* Low barrier is a hard constraint on two axes: (1) **app authors** never write a capability
manifest -- the runtime wires sane default caps; (2) **porting a CPU** is the small arch/chip
seam, not a kernel restructure. The design choices (capabilities over ambient global ids; a
minimal syscall surface, with `read`/`open`/`socket` as userspace stubs over IPC and the
debug-console `write` the sole sanctioned kernel exception) are downstream of this goal.

**Second purpose -- teach how a microkernel works.** The Book doubles as a learning resource:
a reader who knows minimal C/C++ and the basics of the compile/link/flash pipeline should be
able to follow how a real microkernel is built, using KickOS as the worked example. That means a
few *concept* chapters (what an OS/kernel is, monolithic vs micro, scheduling, interrupts,
memory protection) that stand on their own before the KickOS-specific chapters. **Main
further-reading reference: Andrew S. Tanenbaum, *Modern Operating Systems* / *Operating
Systems: Design and Implementation*** -- cite the relevant Tanenbaum chapter where a concept
is introduced, so a learner who wants the full theory has the canonical pointer. Concept
chapters teach the idea *and* show how KickOS realises it (and where it deliberately differs,
e.g. tickless + no MMU + capability endgame).

## Chapters (outline -- fill along the path)

Each KickOS chapter *explains* its topic and points into the Reference for the exact contract
(cross-link, do not restate -- one fact, one home). The "Points into" column names the Reference
page / code a chapter narrates over.

*Part 0 (concept chapters) teaches the ideas for a learner -- prereq: minimal C/C++ +
compile/link/flash basics -- citing Tanenbaum for the full theory. Parts 1+ are the KickOS
guided tour. A reader who just wants the exact contract can go straight to `../reference/`.*

| # | Chapter | Covers | Points into (Reference / code) |
|---|---|---|---|
| 0.1 | The machine underneath: CPU, memory, peripherals | what a CPU core is (PC/SP/regs/flags/privilege bit); MCU vs application processor (one flat space, usually no MMU); MMIO (a register is an address, read side effects); buses + the interconnect (where a guard can sit); memory types (flash/SRAM/TCM/external) + the memory map; how to read a memory map + a register table; Tanenbaum ch.1, Hennessy & Patterson ch.2 | draft: [`the-machine-underneath.md`](the-machine-underneath.md) (concept); grounds ch.3.7, 7.6; binds to `reference/architecture.md` ("Memory domains") + `reference/boards.md` |
| 0.2 | Interrupts and traps | what an interrupt is; the vector table (hardware-indexed jump table, VTOR / `mtvec`); the interrupt controller (NVIC / CLINT / PLIC); synchronous vs asynchronous traps (SVC / ecall / RX INT, `mcause` demux); masking + critical sections (BASEPRI / `mstatus.MIE`); Tanenbaum ch.1, ch.5 | draft: [`interrupts-and-traps.md`](interrupts-and-traps.md) (concept); grounds ch.3, 3.8; binds to `reference/invariants.md` (`irqlock-nesting-safe`, `basepri-write-needs-barrier`) + `reference/porting.md` (`arch_irq_*`) |
| 0.3 | What is an OS / a kernel? | user vs privileged, syscalls, what a kernel is *for*; Tanenbaum ch.1 | **draft me** (concept) |
| 0.4 | Monolithic vs microkernel | why a small trusted kernel + userspace services; the seL4 lineage; trade-offs; Tanenbaum ch.1 (section 1.7 structure) | **draft me** (concept) |
| 0.5 | How to read this book + toolchain | prerequisites, the compile->link->flash pipeline, how to build/run KickOS, the arch/chip layout | new; **draft me** (concept) |
| 0.6 | What's under #include: the C library and the C++ runtime | what a C library IS (a bag of functions + a bottom-edge syscall seam); hosted vs freestanding; the three C++ runtime layers (libgcc/libsupc++/libstdc++); freestanding vs full C++ and its cost; the "toolchain C++ on your own libc" trap; global-ctor boot order on a freestanding OS; full C++ under MPU (deep dives: 7.2, 7.3); Tanenbaum ch.1 | draft: [`whats-under-include-libc-and-the-cxx-runtime.md`](whats-under-include-libc-and-the-cxx-runtime.md); binds to `reference/architecture.md` ("C++ decisions", "Toolchain-libc lesson") |
| 1 | Overview & principles | vision, the uniform-fleet thesis, the milestone themes (M2 MPU -> M3 caps -> M4 SMP) | `../../roadmap.md`; **draft me** |
| 2 | Kernel model | scheduler (tickless, RR/FIFO, reporter-as-root), sync (semaphores + generational handles), time, `IrqLock` critical-section model | `reference/architecture.md` |
| 2.1 | The time base: a monotonic clock you can trust | what a monotonic `now` + one-shot is and why a tickless kernel's every deadline rests on it; the design space (core cycle counter / periodic tick / peripheral timer); the narrow-counter + software-wrap fragility pattern and the porter's vetting checklist; the `arch_clock_now` weak-override seam | draft: [`the-time-base-a-monotonic-clock-you-can-trust.md`](the-time-base-a-monotonic-clock-you-can-trust.md); binds to `reference/architecture.md` (`arch_clock_now`/`arch_timer_arm`, "Tickless") + `arch/*/chip/*` clock code |
| 2.2 | The blocking substrate: one wait/wake primitive | what "blocking" is (park/wake) and the lost-wakeup; why one shared primitive beats a fork per object; the waitq (`wq_block`/`wq_pop_highest`, the lazy at-pop scan) + the pop-transfer-wake protocol whose step 2 is the only extension point; the `wait_result` status channel; the deferred-switch stale-read hazard and the `wq_confirm_resume` barrier | draft: [`the-blocking-substrate-one-wait-wake-primitive.md`](the-blocking-substrate-one-wait-wake-primitive.md); binds to `reference/invariants.md` (deferred-switch, switch-frame) + `kernel/sync/sync.cc`, `kernel/sched/sched.cc` |
| 2.3 | Priority inheritance: lending a thread its blocker's urgency | priority inversion (the Pathfinder bug); why a PI mutex is the one sync object beyond the semaphore (PI is a scheduler action userspace cannot do); plain PI vs priority ceiling; the effective-vs-base invariants + `sched::set_prio` as sole writer; the two-pass boost (cycle detect then boost) and the chain walk; revert-by-recompute over the held list; the PI-stops-at-semaphores boundary; owner-died via `wait_result` | draft: [`priority-inheritance-lending-urgency.md`](priority-inheritance-lending-urgency.md); binds to `reference/architecture.md` ("Synchronization surface") + `kernel/sched/sched.cc` |
| 3 | Interrupt model | two-tier IRQ (direct ISR vs IRQ-as-event), the arch dispatch seam, real-peripheral demux (per arch), the buffered console ring | `reference/console.md` + `console_tx.h` + `arch_*` |
| 3.5 | Context switching & the silicon contract | what a thread's saved state is; why the switch (and minimal startup) must be assembly; the per-arch contract axes (register file, trap entry, deferred-switch trigger, FP, privilege) | draft: [`context-switching-and-the-silicon-contract.md`](context-switching-and-the-silicon-contract.md); binds to `reference/invariants.md` |
| 3.6 | Thread stacks & the KISS tension | who owns a thread's stack under a no-free bump arena; why static pools burn RAM and bump-on-demand leaks; constraining to one size class so the free-list stays trivial; userspace-owned stacks | draft: [`thread-stacks-and-the-kiss-tension.md`](thread-stacks-and-the-kiss-tension.md); binds to `reference/architecture.md` |
| 3.7 | Peripheral isolation & the hardware ceiling | why per-thread MMIO isolation depends on where the protection unit sits (CPU-side vs bus-slave-side); the fleet matrix; the K64F SYSMPU-vs-AIPS silicon lesson | draft: [`peripheral-isolation-and-the-hardware-ceiling.md`](peripheral-isolation-and-the-hardware-ceiling.md); binds to `reference/architecture.md` (Memory domains) |
| 3.8 | A masked interrupt is latched, not lost | the masked window inherent to handling IRQs in thread context (ISR masks -> driver services -> next wait re-arms); drop-vs-latch and why drop silently voids the lossless-`wait` promise; why level sources hide the bug (re-latch) and edge/pulse sources expose it (permanent loss); latch as what the NVIC/PLIC hardware already does (drop is unimplementable on a claim/complete controller); the hardware-pend + semaphore two-latch chain; one-deep coalescing; discard as its own explicit primitive (first-arm garbage, future level path) so `unmask` means preserve | draft: [`a-masked-interrupt-is-latched-not-lost.md`](a-masked-interrupt-is-latched-not-lost.md); binds to `reference/invariants.md` (`isr-mask-then-wake-wait-rearm`) + `reference/porting.md` (`arch_irq_*`) + `kernel/irq/irq.cc` |
| 4 | Per-ISA guided tour | armv7m / armv6m / RXv3 / RV32IMAC / Xtensa LX6, one section each: how that ISA *realises* the ch.3.5 contract -- context switch, trap entry, timer, IRQ controller | `arch/*/` code; worked example drafted: [`porting-a-new-isa-riscv.md`](porting-a-new-isa-riscv.md) |
| 5 | Per-chip guided tour | XMC4800, STM32F4/F3/F1, RP2040, SAM3X, ESP32(-C6/WROOM), K64F, RX72M, nRF51: clocks, console, LED, quirks | `reference/boards.md` + `arch/*/chip/*` |
| 6 | Porting guide | how to add a board/chip/ISA; the arch seam contract | `reference/porting.md` |
| 7 | Memory protection (M2) | domains, per-thread private/caller-owned stacks, region placement, PMP/PMSA/SYSMPU backends, syscall-arg validation | `reference/architecture.md` (MPU) + `../m2-readiness.md`; overview **draft me** (deep-dives 7.1-7.4 drafted below) |
| 7.1 | Alignment across the syscall boundary | why the kernel cannot assume a caller pointer's alignment; `alignof(T)` is arch-specific (4 on RXv3, 8 elsewhere); the too-strict (silent no-op) vs too-lax (kernel trap) failure modes; alignment as one facet of "never trust a user pointer" | draft: [`alignment-across-the-syscall-boundary.md`](alignment-across-the-syscall-boundary.md); binds to `reference/architecture.md` (syscall-arg validation) |
| 7.2 | Exceptions and RTTI under memory protection | the four things a full-C++ runtime touches at runtime (heap, EH tables, the unwinder's WRITABLE state, RTTI); the three EH models (EHABI / SjLj / DWARF) and how each is reached from an unprivileged thread; why read-only tables ride the code grant while writable state must ride the data grant (+0 regions); the DWARF `__register_frame` boot hook; the RISC-V `gp` fix; 4-arch throw/catch/unwind/RTTI payoff | draft: [`exceptions-and-rtti-under-memory-protection.md`](exceptions-and-rtti-under-memory-protection.md); binds to `reference/architecture.md` ("Memory domains", "C++ decisions") + `reference/porting.md` (RISC-V `gp` anchor) |
| 7.3 | Where your RAM goes: the full-C++ memory floor and the linker split | the ~32K writable floor and why (heap + runtime tax); EH cost is per-toolchain not per-board; the `KICKOS_HEAP_SIZE`/`KICKOS_APPDATA_SIZE` knobs; then the linker -- the inverted `archive:member` colon selector, the `.init_array` ctor split, EH-table homing, the copy/zero init tables, the RISC-V `gp` anchor; how to read and author such a script (mk64f.ld worked example) | draft: [`where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md`](where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md); binds to `reference/architecture.md`, `reference/porting.md` (RISC-V `gp` anchor), `arch/*/chip/*.ld` |
| 7.4 | Proving memory protection: coexistence vs confinement | why "the MPU is on" is not "the MPU protects"; the privileged root thread runs your `main()` (so an inline test bypasses the unit); the two independent claims -- confined execution (unprivileged worker, `nPRIV=1`/`MPP=0`/`PSW.PM=1`) AND proving the negative (a wild access must FAULT); the `cxxtest` near-miss and the `mpu_fault` negative test | draft: [`proving-memory-protection-coexistence-vs-confinement.md`](proving-memory-protection-coexistence-vs-confinement.md); binds to `kernel/init/kmain.cc` + `user/apps/{mpu_fault,cxxtest,selftest}/main.cc` + `reference/architecture.md` |
| 7.5 | Protection follows the CPU, not the scheduler's intent | why the per-thread MPU region set must be committed at the *physical* context switch, not the scheduler's logical decision; the deferred-switch window where `current`+MPU already name `next` while the outgoing thread still runs (faults on its own stack under enforcement, silent cross-thread access without it); why a narrow window hides on fast cores and only a slow core / heavy churn reveals the structural race; enforcement as a bug-surfacer; the stash-at-decision / commit-in-epilogue fix, generalised to any context-keyed hardware resource (MMU page-table root next) | draft: [`protection-follows-the-cpu-not-the-schedulers-intent.md`](protection-follows-the-cpu-not-the-schedulers-intent.md); binds to `reference/invariants.md` (`mpu-apply-on-every-switch-in`, `arch-switch-may-defer`, `deferred-switch-lowest-band`) + `kernel/sched/sched.cc`, `arch/arm/armv6m/` |
| 7.6 | The CPU reads ahead: memory types and speculative access | the memory map as a contract with the *core* (what it may touch on its own), not only a permission list; speculative access on an in-order core with caches/prefetch; why Normal memory is speculatable and Device/Strongly-ordered is not (read side effects); the unbacked-but-Normal external window that stalls the core fault-lessly (a speculative access to a bus slave that never responds); the design rule -- bound real memory Normal, wrap everything else Device/SO + XN; the MPU corollary that a kernel cannot lean on a permissive privileged background because it re-exposes the architecture's default Normal typing | draft: [`memory-types-and-speculative-access.md`](memory-types-and-speculative-access.md); binds to `reference/architecture.md` (Memory domains -- background region + region-set contract) + `reference/boards.md` (per-chip memory-map quirks) |
| 8 | Capabilities (M3) | the object/handle table, authenticated grants, clock-select | `reference/architecture.md` ("Object model, capabilities & IPC"); **draft me** |
| 8.1 | Naming a kernel object: the handle and the resolve chokepoint | what a per-task handle is (typed/rights-bearing/refcounted, vs ambient global ids); the validate-then-use-under-one-lock resolve chokepoint; WRAP not replace (global liveness vs per-task possession); the boundary (bounds+liveness+type+rights) vs the detector (generation) split; ABA/pigeonhole and the fd analogy; the ISR fail-safe drop | draft: [`handles-and-the-resolve-chokepoint.md`](handles-and-the-resolve-chokepoint.md); binds to `reference/architecture.md` ("Object model, capabilities & IPC") + `slotpool.h`, `kernel/syscall/cap.cc`, `kernel/syscall/syscall.cc` |
| 8.2 | Adding a kernel object type: the additive recipe | the object-side plumbing behind resolve; why the naive per-site hardcoding stops being additive; three explicit switches (`obj_ref_inc`/`obj_ref_drop`/`obj_close_protocol`) over the type enum vs templating (house-rule: monomorphic pools); the close-protocol-before-ref-drop ordering + the teardown flag; the leak-don't-strand refcount discipline and its unreachability argument; the per-pool `uint8_t` refs bound; the one-pool + one-refs[] + one-resolve-case recipe | draft: [`adding-a-kernel-object-type-the-additive-recipe.md`](adding-a-kernel-object-type-the-additive-recipe.md); binds to `reference/architecture.md` ("Object model, capabilities & IPC") + `kernel/syscall/cap.cc` |
| 8.3 | Endpoints: synchronous IPC by rendezvous | why IPC is the microkernel's central primitive; buffered channel vs synchronous rendezvous; the parked side's own buffer as the storage; the arriving-thread-copies-under-one-lock invariant (both buffers stable) + kernel-copied bounded payload + datagram truncation; two waitqs; `recv_holders` vs the refcount (dead-endpoint gate + EPIPE); the send/receive rights split; why no PI / no call-reply / no timed ops; the deliberate lifecycle asymmetries; the cross-domain privileged-write porting contract | draft: [`endpoints-synchronous-ipc-by-rendezvous.md`](endpoints-synchronous-ipc-by-rendezvous.md); binds to `reference/architecture.md` ("Object model, capabilities & IPC") + `reference/porting.md` (cross-domain write) |

## How a chapter is shaped

The teaching spine, applied per chapter so the pedagogy is uniform: teach the problem
before the solution, show the alternatives so the choice is meaningful, then land it in
real code.

- **Main concepts up front.** The foundational ideas a section leans on come first, so a
  reader meets a concept before the KickOS-specific section that uses it (the Part-0
  concept chapters, and the concept framing at the head of a KickOS chapter).
- Then each section, in order:
  1. **Why** we are doing this -- the problem, grounded in the underlying concept.
  2. **How one *could*** do it -- the design space, the options laid out with trade-offs.
  3. **What KickOS chose, and why** -- the decision, argued against the options in (2).
  4. **How it is done in the code** -- the real implementation, linking into the
     Reference / code for the exact contract.

## Conventions
- **How & why, not the contract.** Explain design and teach concepts; for the exact,
  code-synced contract link into `../reference/` rather than restating it here (restated detail
  rots). One fact, one home.
- **Keep state, not path.** Describe the current design; do not narrate the sequence of
  fixes/commits that produced it (that lives in git). No "we first did X, then changed to Y".
- **Timeless teaching, no status.** The Book teaches how a microkernel works and why KickOS
  is built this way; it carries no implementation-status or milestone-completion language
  (no "landed", "not yet landed", "DRAFT", "proven on silicon X", "will land in Mx"). What
  currently exists and is validated lives in `../../roadmap.md` / `../../TODO.md` / the
  Reference; a chapter that needs the exact current contract links into `../reference/`.
- **Terse, invariant-first.** Same rule as code comments: explain hidden constraints and
  contracts, not what the code plainly does.
- **ASCII only.** `--` not em-dash, `->` not arrow, straight quotes, "section"/"microkernel".
