<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# RISC-V context-switch cost: Zcmp vs the cooperative fast-path

> **Status: EXPLORATORY** -- an analysis spike. Neither lever is implemented and both are
> unscheduled (`../roadmap.md`); the bench prerequisite the measurement needs is in the tree.

The verdict and the soak evidence behind it are in `../roadmap.md` ("RISC-V
context-switch cost") and `../TODO.md` (the optimisation items): Option A, the software
cooperative fast-path, is the real and portable win; Option B, Zcmp, is a small Hazard3-only
follow-on. What only lives here is the Zcmp availability fact.

## Extension availability (verified)

- **RP2350 / Hazard3**: ships Zcmp, Zcb, Zca. Default ISA
  `rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp`. `cm.push`/`cm.pop`/
  `cm.popret` are available.
- **ESP32-C6 (HP core)**: plain `rv32imac` (IMAC only). NO Zcmp. The board with the
  actual throughput problem CANNOT use `cm.push`/`cm.pop`.
- **qemu-virt**: whatever `-march` is built; the shared soft-float rv32imac/ilp32
  multilib does not include Zcmp.

Which is why B depends on A rather than competing with it: `cm.push`/`cm.pop`/`cm.popret`
save and restore ONLY `{ra, s0-s11}`, never the caller-saved temporaries and never
`mepc`/`mstatus`, so they cannot compress the current preemptive full-file trap frame at
all. They fit exactly one thing, a cooperative switch frame, which does not exist until A
lands. The mnemonics also fault on silicon without Zcmp, so the gate must be a board/CPU
knob reflecting the silicon, not a toolchain capability.

## The bench bracket spans the save and restore

The instrument a go/no-go number needs is in place. The rv32 `KICKOS_BENCH` SWITCH window opens
in `trap_entry` above the register save and closes at the end of the restore, so it prices the
same software save and restore the armv7m window does around its `stmdb`/`ldmia`
(`arch/riscv/rv32imac/switch.S` bench blocks, `kernel/bench/bench.cc`). The deferred MPU commit
and the telemetry hook sit between the two halves and stay outside the window, as they do on
armv7m; the MPU commit owns a phase row of its own instead.

**A figure is comparable only against another taken on the same bracket.** An rv32 switch figure
that priced a narrower span is not a baseline this one can be subtracted from, because the two
measure different spans and a difference between them is not a cost. Both sides of any A-vs-A+B
comparison have to be taken on this bracket, and a number quoted anywhere has to name the span it
covers.

The window closes a few instructions before `mret`, where no register is left to host a call, so
the two halves are banked at the NEXT switch. A report therefore names one sample fewer than the
switches it observed, and the un-banked one is dropped at reset rather than leaking into the next
measurement window.

## Where the exact contracts live

- reference/invariants.md: `switch-frame-matches-init`, `deferred-switch-lowest-band`,
  `arch-switch-may-defer`, the `fp-*` family.
- The concept teaching: docs/book/context-switching-and-the-silicon-contract.md
  ("The cost of a switch, and where an ISA pays it").
