<!-- SPDX-License-Identifier: CECILL-C -->
# The KickOS Book

**Focus.** The Book is the *durable, code-synced reference* for what KickOS **is** and how it
works — as opposed to the throwaway design *spikes* (deleted once their code lands) and the
*state/roadmap* files (`TODO.md`, `M1_state.md`, which record status, not how-it-works). If a
chapter and the code disagree, the code wins and the chapter is a bug. Written to be read by
someone evaluating or porting KickOS, not to narrate how we got here.

**What KickOS is.** A minimal, seL4-principled microkernel RTOS: a small trusted kernel
(scheduler, IPC/sync, tickless time, interrupt plumbing, memory protection) with everything
else in userspace. One uniform design across five ISAs — armv7m, armv6m, RXv3, RV32IMAC,
Xtensa LX6 — so the *same* semantics hold on every target and divergence is a bug, not a
port quirk. North star: capability-based authority (M3) over MPU-enforced isolation (M2),
eventually MMU-alongside-MPU. (See the `kickos-vision` note.)

**Second purpose — teach how a microkernel works.** The Book doubles as a learning resource:
a reader who knows minimal C/C++ and the basics of the compile/link/flash pipeline should be
able to follow how a real µkernel is built, using KickOS as the worked example. That means a
few *concept* chapters (what an OS/kernel is, monolithic vs micro, scheduling, interrupts,
memory protection) that stand on their own before the KickOS-specific reference. **Main
further-reading reference: Andrew S. Tanenbaum, *Modern Operating Systems* / *Operating
Systems: Design and Implementation*** — cite the relevant Tanenbaum chapter where a concept
is introduced, so a learner who wants the full theory has the canonical pointer. Concept
chapters teach the idea *and* show how KickOS realises it (and where it deliberately differs,
e.g. tickless + no MMU + capability endgame).

## Chapters (outline — fill along the path)

Source column = the existing doc/material a chapter absorbs; migrate + de-narrate into here.

*Part 0 (concept chapters) teaches the ideas for a learner — prereq: minimal C/C++ +
compile/link/flash basics — citing Tanenbaum for the full theory. Parts 1+ are the KickOS
reference. A reader who just wants the reference can start at chapter 1.*

| # | Chapter | Covers | Source / status |
|---|---|---|---|
| 0.1 | What is an OS / a kernel? | user vs privileged, syscalls, what a kernel is *for*; Tanenbaum ch.1 | **draft me** (concept) |
| 0.2 | Monolithic vs microkernel | why a small trusted kernel + userspace services; the seL4 lineage; trade-offs; Tanenbaum ch.1 (§1.7 structure) | **draft me** (concept) |
| 0.3 | How to read this book + toolchain | prerequisites, the compile→link→flash pipeline, how to build/run KickOS, the arch/chip layout | new; **draft me** (concept) |
| 1 | Overview & principles | vision, the uniform-fleet thesis, milestone map (M1 done → M2 MPU → M3 caps → M4 SMP) | new + `kickos-vision`; **draft me** |
| 2 | Kernel model | scheduler (tickless, RR/FIFO, reporter-as-root), sync (semaphores + generational handles), time, `IrqLock` critical-section model | `docs/architecture.md` |
| 3 | Interrupt model | two-tier IRQ (direct ISR vs IRQ-as-event), the arch dispatch seam, real-peripheral demux (per arch), the buffered console ring | `docs/console.md` + `console_tx.h` + `arch_*` |
| 4 | Per-ISA reference | armv7m / armv6m / RXv3 / RV32IMAC / Xtensa LX6: context switch, trap entry, timer, IRQ controller | `arch/*/` code + inline docs; **per-ISA pages** |
| 5 | Per-chip reference | XMC4800, STM32F4/F3/F1, RP2040, SAM3X, ESP32(-C6/WROOM), K64F, RX72M, nRF51: clocks, console, LED, quirks | `docs/boards.md` + `arch/*/chip/*` |
| 6 | Porting guide | how to add a board/chip/ISA; the arch seam contract | `docs/porting.md` |
| 7 | Memory protection (M2) | domains, per-thread private/caller-owned stacks, region placement, PMP/PMSA/SYSMPU backends, syscall-arg validation | `docs/architecture.md` §MPU + `docs/m2-readiness.md`; **future** |
| 8 | Capabilities (M3) | the object/handle table, authenticated grants, clock-select | **future** |

## Conventions
- **Keep state, not path.** Describe the current design; do not narrate the sequence of
  fixes/commits that produced it (that lives in git). No "we first did X, then changed to Y".
- **Terse, invariant-first.** Same rule as code comments: explain hidden constraints and
  contracts, not what the code plainly does.
- Per-ISA / per-chip pages stay next to the concepts they instantiate, so a reader sees the
  uniform model (ch. 2-3) then how each target realises it (ch. 4-5).
