<!-- SPDX-License-Identifier: CECILL-C -->
# RISC-V context-switch cost: Zcmp vs the cooperative fast-path

> **Status: EXPLORATORY** -- an analysis spike; no `switch.S` or kernel change. Both levers are
> post-M6 and unscheduled (`../roadmap.md`).

The verdict, the ~3.5x ratio and the soak evidence behind it are in `../roadmap.md` ("RISC-V
context-switch cost") and `../TODO.md` (post-M6 optimizations): Option A, the software
cooperative fast-path, is the real and portable win; Option B, Zcmp, is a small Hazard3-only
follow-on. What only lives here is the Zcmp availability fact and the bench-bracket defect.

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

## Prerequisite: fix the bench bracket, then measure

Any go/no-go number needs a bracket that spans the register save+restore. Today the
rv32 `KICKOS_BENCH` bracket stamps `g_bench_sw_start` at the TOP of `.Lswitch` (after
`trap_entry` already did the 30 stores) and ends BEFORE `.Lrestore` does the 30 loads
(switch.S bench blocks; kernel/bench/bench.cc). So it excludes the entire save/restore
-- the dominant RISC-V cost -- and is not comparable to the armv7m bracket, which does
span its `stmdb`/`ldmia`. Fix: move the rv32 start stamp to the first instruction of
the save path and the end stamp to the end of the restore. Until that lands, no measured
A-vs-A+B delta means anything.

## Where the exact contracts live

- reference/invariants.md: `switch-frame-matches-init`, `deferred-switch-lowest-band`,
  `arch-switch-may-defer`, the `fp-*` family.
- The concept teaching: docs/book/context-switching-and-the-silicon-contract.md
  ("The cost of a switch, and where an ISA pays it").
