<!-- SPDX-License-Identifier: CECILL-C -->
# RISC-V context-switch cost: Zcmp vs the cooperative fast-path

Analysis spike (design only; no switch.S/kernel change here). Question from the
maintainer: is closing the RISC-V context-switch gap worth supporting Zcmp
(`cm.push`/`cm.pop`/`cm.popret`), and can it be compile-time-gated with a plain
`sw`/`lw` fallback?

Short answer: **the software cooperative fast-path is the real win (~2x, portable);
Zcmp is at best a small follow-on that only helps Hazard3 and only AFTER the
cooperative path exists. Zcmp cannot be applied to the current unified trap frame at
all.** Do the cooperative path first; treat Zcmp as an optional, low-priority,
compile-gated micro-optimization for the RP2350.

## Background: why the switch is expensive

The rv32imac switch (arch/riscv/rv32imac/switch.S) defers every swap to a CLINT
`msip` trap. `trap_entry` software-saves the FULL integer file -- 28 GPRs + `mepc` +
`mstatus` = ~30 word stores -- before it can demux `mcause`; `.Lrestore` reverses it
(~30 word loads). armv7m PendSV software-saves only the callee-saved half (9 words
each way) because Cortex-M hardware-stacks the caller-saved 8 on exception entry. The
software-word ratio is ~60/18 = 3.3x, matching the observed ~3.5x per-handoff
deficit. The workload that exposes it (two threads ping-ponging through a semaphore)
is a VOLUNTARY switch: it reaches the switcher through an ordinary call, so by the
psABI the caller-saved registers are already dead.

## Extension availability (verified)

- **RP2350 / Hazard3**: ships Zcmp, Zcb, Zca. Default ISA
  `rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp`. `cm.push`/`cm.pop`/
  `cm.popret` are available.
- **ESP32-C6 (HP core)**: plain `rv32imac` (IMAC only). NO Zcmp. The board with the
  actual throughput problem CANNOT use `cm.push`/`cm.pop`.
- **qemu-virt**: whatever `-march` is built; the shared soft-float rv32imac/ilp32
  multilib does not include Zcmp.

## The decisive constraint on Zcmp

`cm.push`/`cm.pop`/`cm.popret` can save/restore ONLY `{ra, s0-s11}` (up to 13
registers, callee-saved plus the return address), with a stack adjust from a fixed
set (16..112 bytes on RV32). They CANNOT save caller-saved temporaries (`t0-t6`,
`a0-a7`), and cannot touch `mepc`/`mstatus`.

Consequence: **Zcmp is a callee-saved-frame instruction.** It cannot compress the
current preemptive full-file trap save (which must save `a0-a7`/`t0-t6` because it
interrupts arbitrary code). It fits exactly ONE thing -- a cooperative switch frame
of `{ra, s0-s11}`. So Zcmp is not an alternative to the cooperative fast-path; it is
an accelerator that only becomes applicable once that path exists.

## The two options, quantified

### Option A -- software cooperative fast-path (the real lever)

A voluntary switch needs to preserve only `{ra, s0-s11}` (13 words) plus `sp`, not
the full 28-GPR frame. Doing the voluntary swap inline (a plain callee-saved
switch routine) instead of through the full-file `msip` trap roughly halves the
voluntary switch's memory traffic and instruction count.

- Memory traffic: ~26 word accesses (13 save + 13 restore) vs ~60 today.
- Expected soak effect: ~2x, since the soak is nearly pure voluntary switching.
- Portability: helps EVERY rv32 target -- crucially the C6, which is the board with
  the problem.
- Cost: gives up the single-frame simplicity (one restore path, one fabricated
  init frame). The restore must distinguish a cooperative frame from a preemptive
  one. Fable-gated; touches the switch model + `switch-frame-matches-init`.

### Option B -- Zcmp (`cm.push`/`cm.pop`) on the cooperative frame

Replaces the 13 explicit `sw` + `addi sp` on save with ONE `cm.push {ra,s0-s11},-64`,
and the 13 `lw` + `addi sp` + `ret` on restore with ONE `cm.popret {ra,s0-s11},64`.

- Instruction count: ~14 -> 1 on save, ~14 -> 1 on restore (~26 fewer instructions
  issued per switch); I-fetch / code-size drops correspondingly.
- **Memory traffic is UNCHANGED**: `cm.push` still performs 13 stores, `cm.pop` 13
  loads -- the same 52 bytes each way. On an implementation that micro-sequences the
  push/pop (one access per cycle) the DATA-movement cycles are essentially the same;
  the win is the eliminated fetch/decode/PC-increment/`addi`/`ret` overhead and
  pipeline slots.
- Expected magnitude: ~10-20% of the (already-halved) cooperative switch on Hazard3
  -- i.e. roughly a further single-digit percent of the ORIGINAL switch cost. On an
  SRAM-store-bound part the wall-clock win is small; the code-size win is real but
  orthogonal to throughput.
- Scope: Hazard3 only. Zero benefit on the C6 (no Zcmp) or virt (stock multilib).

### Do they compose?

Yes, but dependently: B requires A (Zcmp can only compress a callee-saved frame,
which only exists after A). B alone is not applicable to the current frame. So the
ladder is: A -> ~2x everywhere; A+B -> A's 2x plus a small Hazard3-only extra.

### Verdict

The cooperative fast-path (A) is strictly the better investment: bigger, portable,
and it fixes the board that actually has the problem. Zcmp (B) is a cheap,
optional follow-on for the RP2350 once A lands -- worth it mainly for code size, not
throughput. Do not pursue Zcmp as a standalone; it closes the armv7m gap only
marginally and does nothing for the C6.

## Compile-gated design sketch (for B, layered on A)

The frame layout `cm.push`/`cm.pop` produce is FIXED by the instruction (`ra` at the
highest slot, then `s0, s1, ...` descending). The cooperative `arch_context_init`
frame must fabricate exactly that order (`switch-frame-matches-init`).

Multilib: the shared multilib is `rv32imac`/`ilp32` (soft-float, no Zcmp), so C/C++
objects never emit `cm.push` -- fine. switch.S is hand-asm, so a local
`.option arch, +zca, +zcmp` makes the ASSEMBLER accept the mnemonics regardless of
the file's `-march`; NO new multilib is required. But the mnemonics fault on silicon
that lacks Zcmp, so the gate must be a BOARD/CPU knob (reflecting silicon), not just a
toolchain capability. Derive `KICKOS_RISCV_HAS_ZCMP` from `KICKOS_MCPU` (set only for
rp2350-hazard3).

```
/* Cooperative (voluntary) switch save/restore. Preemptive trap_entry still
   saves the full file and does NOT use these -- Zcmp cannot cover caller-saved. */
#if KICKOS_RISCV_HAS_ZCMP
    .option push
    .option arch, +zca, +zcmp
    .macro COOP_SAVE            /* frame: ra @ top, s0..s11 descending, sp -= 64 */
    cm.push {ra, s0-s11}, -64
    .endm
    .macro COOP_RESTORE_RET     /* reverse, sp += 64, ret to ra */
    cm.popret {ra, s0-s11}, 64
    .endm
    .option pop
#else
    .macro COOP_SAVE
    addi sp, sp, -64
    sw   ra,  60(sp)
    sw   s0,  56(sp)
    sw   s1,  52(sp)
    /* s2..s11 ... */
    sw   s11,  4(sp)
    .endm
    .macro COOP_RESTORE_RET
    lw   ra,  60(sp)
    lw   s0,  56(sp)
    /* ... */
    lw   s11,  4(sp)
    addi sp, sp, 64
    ret
    .endm
#endif
```

Both branches MUST produce the identical frame layout so the fabricated first-resume
frame and the restore agree on either build. Fable-gated (switch.S + frame contract).

## Prerequisite: fix the bench bracket, then measure

Any go/no-go number needs a bracket that spans the register save+restore. Today the
rv32 `KICKOS_BENCH` bracket stamps `g_bench_sw_start` at the TOP of `.Lswitch` (after
`trap_entry` already did the 30 stores) and ends BEFORE `.Lrestore` does the 30 loads
(switch.S bench blocks; kernel/bench/bench.cc). So it excludes the entire save/restore
-- the dominant RISC-V cost -- and is not comparable to the armv7m bracket, which does
span its `stmdb`/`ldmia`. Fix: move the rv32 start stamp to the first instruction of
the save path and the end stamp to the end of the restore.

Micro-benchmark to settle Zcmp (Hazard3 silicon; easy-flash, print-debug):

1. Land Option A (cooperative fast-path). Fix the bracket to span COOP_SAVE..
   COOP_RESTORE_RET.
2. Cycle source: `mcycle` (verify Hazard3 exposes the counter; RP2350 has Zicsr) or
   the RP2350 system timer at its known rate. On the C6 use CLINT MTIME (core-clocked
   at 160 MHz) -- but the C6 cannot run Zcmp, so the A-vs-A+B comparison is
   Hazard3-only.
3. Build two firmwares: `KICKOS_RISCV_HAS_ZCMP=0` and `=1`. Run the bench ping-pong
   (user/apps/bench), compare min/avg switch cycles. The delta is the pure Zcmp win.
4. Go/no-go: if the delta is below ~10% of the switch, Zcmp is not worth the build
   knob for throughput (keep it only if code size matters).

## Where the exact contracts live

- reference/invariants.md: `switch-frame-matches-init`, `deferred-switch-lowest-band`,
  `arch-switch-may-defer`, the `fp-*` family.
- The concept teaching: docs/book/context-switching-and-the-silicon-contract.md
  ("The cost of a switch, and where an ISA pays it").
