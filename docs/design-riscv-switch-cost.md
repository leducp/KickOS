<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# RISC-V context-switch cost: the cooperative fast-path, refused on its own numbers

> **Status: REFUSED (2026-09-18), with the numbers below.** Option A, the cooperative
> fast-path, is not built; Option B, Zcmp, falls with it (it has no frame to compress until A
> exists, and the board with the throughput problem has no Zcmp). Reopening needs new
> measurements, not a new argument: the reopening tests are at the end.

## What the item claimed, and what the silicon says

The item was entered as "the rv32 trap saves ~60 stack words a switch against armv7m's ~18,
a ~3.5x per-handoff cost". **That ratio is an arithmetic error and the 3.5x does not exist.**

`arch/arm/armv7m/switch.S` 153/171 pushes and pops `{r4-r11, lr}`, nine words, and the
hardware stacks eight more on exception entry and unstacks them on exit. armv7m therefore
moves **17 words each way, 34 a switch**, not 9 and 18. rv32 moves 30 each way, 60 a switch
(`arch/riscv/rv32imac/switch.S` `.Ltrap_regs` and `.Lrestore`, 28 GPRs plus `mepc` and
`mstatus`). The word ratio is **1.8x**, and the measured cycle ratio is smaller still,
because the armv7m bench window (switch.S 100 and 183) brackets the software push and pop
alone and so charges armv7m nothing for the hardware stacking it nonetheless pays for.

Measured this branch, enforcing, both on the corrected brackets:

| | `esp32c6-wroom` (rv32imac, 160 MHz) | `f411disco` (armv7m) |
| --- | --- | --- |
| `SWITCH` p50/p99/max | 144/144/159 (n=40001) | 80/80/232 |
| `MPU_COMMIT` | 75 | 247 |
| per-switch arch total | **219** | **327** |

**The rv32 switch is not the expensive one.** Counting the two arch terms a switch actually
pays, the C6 is a third cheaper per handoff than the armv7m board, and the armv7m column is
the optimistic one twice over: its `SWITCH` excludes the hardware stacking, and `STATE.md`
records that the PMSAv7 descriptor write sits in no bracket on any ARM backend, so 247 is a
floor. The register-file argument is real and it does not reach the conclusion drawn from it.

## The eligible population is nearly everything, and it still does not pay

A cooperative fast-path helps a switch taken in thread context. On rv32 `arch_switch`
(`arch/riscv/rv32imac/arch_rv32imac.cc` 300-305) pends msip unconditionally, so **every**
switch is a trap today; what a cooperative path could catch is the subset where
`arch_in_isr()` is false at the `arch_switch` call in `kernel/sched/sched.cc` 220.

- `g_isr_depth` is bumped only by the timer, soft, external and device demux arms
  (`switch.S` `.Ltimer`, `.Lssoft`, `.Lext`, `.Lextdev`). Neither `.Lswitch` nor the ecall
  path touches it, so a syscall dispatch and the switcher itself read thread context.
- Every `block_current` / `yield` / `wake`-from-dispatch switch is therefore eligible. The
  ineligible ones are RR slice expiry (`sched::tick_rr`, reached from `kickos_isr_timer`) and
  a wake raised by a device ISR.
- The n=40001 `SWITCH` population is the semaphore ping-pong window alone (20000 rounds x 2
  switches, `user/apps/common/bench/main.cc` 51 and 348; the reporter resets the row and
  prints inside the gate). Those threads never sleep and switch about every 19 us, far inside
  any RR slice, so the timer does not fire in the window. **That population is essentially
  100 percent eligible.**

So the item does not die of an empty population. It dies of its denominator.

**The register fastpath removes the wrong switch from the round trip, and leaves the eligible
one.** In the 8-byte call/reply round trip the caller's `kos_call` takes
`kickos_ipc_fastpath` (`switch.S` `.Lecall`), which swaps inline and is deliberately outside
`BD_SWITCH` (`kernel/bench/bench.cc` 389). The server's `kos_reply_recv` is
`KOS_SYS_REPLY_RECV` = 68, which has no fastpath: it runs the generic dispatch, wakes the
caller, parks, and takes **exactly one msip switch per round trip** -- and that one is
eligible. The cooperative path would catch it.

## What it is worth, end to end

The whole thing the lever can remove is the `SWITCH` bracket plus the msip pend it replaces:
144 + `ARCH_SWITCH` 23 = **167 cycles**. That is the ceiling, reached only by a cooperative
switch that costs nothing at all. Against the two real denominators:

| denominator | cycles | eligible switches | ceiling if the save were FREE |
| --- | --- | --- | --- |
| 8 B call/reply round trip (38325 ns at 160 MHz) | 6130 | 1 | **2.7 percent** |
| ping-pong handoff (53626 ctx-sw/s, `docs/archive/M8.8_meas.md` 201) | 2984 | 1 | **5.6 percent** |

A cooperative frame is 13 words each way against 30, and it still pays the `tp` derive and
the deferred PMP commit; it removes the `mscratch` swap, the `gp` anchor, the `mcause` demux,
the CLINT store on both the pend and the clear, and the `mret`. Call it two thirds of the
ceiling, which is where the "~2x on the bracket" claim lands. **The delivered win is about
1.8 percent of a round trip and about 3.7 percent of a switch-thrash handoff.**

**And it does not explain the soak that raised the item.** The entry cites the M3 C6
enforcement soak, C6 ~10.5k iterations against the XMC's ~33.9k in the same window, a 3.2x
gap. A term whose absolute ceiling is 5.6 percent of a handoff cannot produce a 3.2x gap.
Whatever the soak is measuring, it is not this, and the item was sized against a gap it does
not reach.

## What it would cost to build

`switch.S`'s header states the property the change deletes: "ONE frame format for every path,
so a preempted thread and a blocked-in-syscall thread resume identically", and
`docs/reference/porting.md` repeats it in the arch's own section as "ONE frame format for a
voluntary block and a preemptive wake -- the RX/PendSV property". The whole three-destination
stack argument in that header rests on one frame whose `F_SP` slot names the sp `.Lrestore`
leaves on, which is what lets a single epilogue serve all three.

Against `docs/reference/invariants.md`:

- **`switch-frame-matches-init`** binds directly. Two shapes force the xtensa wording: init
  always fabricates the PREEMPTIVE shape (a fresh thread enters through `mret`) and the
  context carries a `resume_kind` discriminator pinned by `static_assert` and hard-coded in
  the asm. `arch_ctx_redirect` must then overwrite `resume_kind` or the switcher takes the
  cooperative tail onto a trap frame -- a landmine this tree has already stepped on once and
  documented at `arch/xtensa/lx6/arch_xtensa.cc` 439.
- **`arch-switch-may-defer`** is the expensive one, because the change is semantic and not
  layout. rv32 moves from "always deferred, in ISR and thread context alike" to immediate in
  thread context, which crosses it into the class two other invariants describe as a live
  hazard: **`wake-from-a-thread-that-will-not-run-again-must-not-switch`** ("ARM, RISC-V and
  RX pend the switch ... and survive by luck, while the sim and Xtensa/LX6 take it
  immediately") and **`cap-teardown-sweep-is-preemptible-and-must-resume`** ("on a backend
  whose `arch_switch` is immediate ... it lands MID-chunk with the slot being closed still
  live"). Both are argued safe by construction on the immediate backends and both are covered
  by `tests/unit/schedwake/wake_dying.cc`, so this is survivable -- but it is precisely the
  silent, port-dependent ordering class, bought for single-digit percent.
- **`deferred-switch-lowest-band`** does NOT constrain the design: the cooperative path takes
  no exception, and the band keeps governing the surviving ISR-context msip path unchanged.
- The **`fp-*`** family does not bind today, rv32imac here being soft-float, and that is the
  debt rather than the relief: `fp-xtensa-caller-saved` is exactly the invariant a split
  creates ("the cooperative path relies on the compiler spilling caller-saved FP regs"), and
  the first rv32 target with F or D would owe a new one of that shape before it could run.

Two further costs outside the invariants file:

- **Four switch-in sites must dispatch on `resume_kind`**, not one: `.Lswitch`, the `.Lecall`
  fastpath tail, `.Ltrap_wild`'s containment resume, and `arch_start`. The round trip alone
  exercises the cross: the fastpath tail resumes a server this change would have parked
  cooperatively. `arch_switch` also stops being C, having to return through a stack it did
  not enter on and to enter `.Lrestore` and `mret` when the incoming frame is preemptive.
- **The kernel-stack depth chain forks.** `arch_rv32imac.cc` 97-114 derives
  `KICKOS_RV_TRAP_FRAME_SYS == 2 * KICKOS_RV_TRAP_FRAME` and
  `KICKOS_RV_TRAP_NEST_EXIT == KICKOS_RV_TRAP_FRAME` from "the msip frame a reschedule puts
  below it", and `tests/static/trap_redzone_roots.txt` scrapes those as plain immediates. The
  worst case does not shrink -- a timer preemption still takes the full frame -- so the
  asserts survive, but the derivation and the gate roots have to be restated for two paths.

## The verdict

A permanent second frame shape in the tree's most safety-load-bearing assembly, a fork of the
deferred-switch contract on one arch, and a fanned-out `resume_kind` across four resume sites,
for about 1.8 percent of a round trip and about 3.7 percent of a handoff, on the arch that is
already cheaper per switch than the armv7m comparator. **Refused.**

Option B follows: `cm.push`/`cm.pop`/`cm.popret` save only `{ra, s0-s11}` and never the
temporaries, `mepc` or `mstatus`, so they cannot compress the preemptive trap frame at all.
They fit exactly one thing, the cooperative frame that is not being built. The availability
fact is kept below because it cost a survey and outlives this decision.

### Extension availability (verified, kept)

- **RP2350 / Hazard3**: ships Zcmp, Zcb, Zca. Default ISA
  `rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp`. `cm.push`/`cm.pop`/`cm.popret`
  are available.
- **ESP32-C6 (HP core)**: plain `rv32imac` (IMAC only). NO Zcmp. The board with the actual
  throughput problem CANNOT use `cm.push`/`cm.pop`.
- **qemu-virt**: whatever `-march` is built; the shared soft-float rv32imac/ilp32 multilib
  does not include Zcmp.

The mnemonics fault on silicon without Zcmp, so any future gate must be a board/CPU knob
reflecting the silicon and never a toolchain capability.

### The bench bracket, and what a comparison owes

The instrument is in the tree and it is the one the numbers above were taken on. The rv32
`KICKOS_BENCH` SWITCH window opens in `trap_entry` above the register save and closes at the
end of the restore, so it prices the same software save and restore the armv7m window does
around its `stmdb`/`ldmia` -- with the standing caveat that armv7m executes eight more words
each way that no bracket charges it for. The deferred MPU commit and the telemetry hook sit
between the two halves and stay outside the window, as they do on armv7m; the MPU commit owns
a phase row of its own instead.

**A figure is comparable only against another taken on the same bracket**, and a number quoted
anywhere has to name the span it covers. The window closes a few instructions before `mret`,
where no register is left to host a call, so the two halves are banked at the NEXT switch: a
report names one sample fewer than the switches it observed, and the un-banked one is dropped
at reset rather than leaking into the next measurement window.

### What would reopen this

Numbers, not arguments. Any one of:

1. **A real workload where the msip switch is a double-digit share of the handoff.** Today it
   is 5.6 percent at its absolute ceiling. Measure the candidate on the bracket above and
   publish the share.
2. **An rv32 target with F or D registers**, where the trap frame grows by the whole FP file
   and the cooperative frame does not. That changes the ratio, not just the constant -- and it
   also brings the `fp-*` invariant the split owes.
3. **A round trip whose eligible switch count rises above one**, for instance if the register
   fastpath is refused for the dominant traffic, or a fused reply path is dropped.
4. **A corrected armv7m instrument that reverses the per-switch arch total.** If the ARM
   column turns out cheaper than 219 once the hardware stacking and the PMSAv7 descriptor
   write are both bracketed, the comparative claim above changes and the item may be re-argued
   on it.

The concept teaching stays at `docs/book/context-switching-and-the-silicon-contract.md` ("The
cost of a switch, and where an ISA pays it"). The exact contracts live in
`reference/invariants.md` (`switch-frame-matches-init`, `deferred-switch-lowest-band`,
`arch-switch-may-defer`, the `fp-*` family) and `reference/porting.md` (the rv32imac section).
