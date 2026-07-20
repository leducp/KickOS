<!-- SPDX-License-Identifier: CECILL-C -->
# Design note: M3 -- user-selectable CPU clock / low-power mode (WRITE side)

**Status: design for fable review, then implementation. Not a contract.**

The READ side is landed: `sys_cpu_clock_hz()` (`KOS_SYS_cpu_clock_hz = 22`,
`user/include/kickos/sys/abi.h:36`) returns `arch_cpu_clock_hz()`
(`arch/include/kickos/arch/arch.h:79`), each backend returning its CMSIS
`SystemCoreClock`. This note settles the WRITE side: retuning the core/bus clock
at runtime WITHOUT breaking kernel time, in-flight sleeps, or the console.

Code truth consulted: `kernel/time/time.cc` (tickless core), `arch/arm/common/
arch_arm_common.cc:84-141` (`arch_timer_arm`, SysTick), `arch/arm/chip/xmc4800/
chip_xmc4800.cc` (SCU/PLL bring-up, CCU40 clock), `arch/arm/chip/mk64f/
chip_mk64f.cc` (MCG/PLL, PIT clock, UART baud), `arch/arm/chip/stm32f411/
chip_stm32f411.cc` (RCC/PLL, TIM2, USART BRR), `arch/arm/chip/rp2040/
chip_rp2040.cc` (clk_ref-decoupled TIMER), `kernel/sched/sched.cc:237-257`
(`next_timed_event` / RR slice), `kernel/syscall/syscall.cc:865` (privileged
gate pattern).

---

## 0. The one thing that makes this hard

On XMC4800, K64F, and STM32F411 the hardened wide monotonic timer that backs
`arch_clock_now` is clocked from the SAME domain the clock-select changes:

- XMC4800: CCU40 runs on `fCCU = fSYS = SystemCoreClock` (`chip_xmc4800.cc:391`).
- K64F: PIT runs on `bus = SystemCoreClock / BUS_DIV` (`chip_mk64f.cc:413`).
- STM32F411: TIM2 runs on APB1 == HCLK == `SystemCoreClock` (`chip_stm32f411.cc:339-341`).

Each `arch_clock_now` converts raw ticks to ns with a cached reciprocal `mult`
recomputed only `if (hz != cached_hz)` -- an explicit "changes once at boot"
assumption (see the comment at `chip_xmc4800.cc:387`). Runtime clock-select
violates that assumption two ways at once:

1. The counter's tick RATE changes going forward (correct after `mult` recompute).
2. `mult` is applied to the ENTIRE accumulated tick count, not just post-change
   ticks. So `now = raw_ticks * new_mult` reprices all of history at the new rate.
   At 144 -> 24 MHz the new `mult` is 6x larger; `now` JUMPS forward by roughly
   `elapsed_since_boot * (ratio - 1)` -- seconds-to-minutes of phantom time.

That is the hardest coherence hazard: a naive PLL change silently detonates the
monotonic clock the whole clock-hardening effort existed to make trustworthy, and
with it every ns-valued deadline compared against `now`.

The fix is section 2's epoch re-anchor. rp2040 (and the sim) are structurally
immune and need none of it.

---

## 1. The arch seam

Add one hook (weak default = unsupported):

```
// Retune the core/bus clock. target = a P-state selector, NOT a raw Hz (the
// achievable set is chip-specific and small). Returns the achieved core Hz, or
// 0 if unsupported / the retune failed and the previous clock is retained.
// MUST be called from privileged thread context with interrupts already masked
// by the caller (see the coherence sequence in section 2). MUST NOT be called
// from ISR context.
uint32_t arch_cpu_clock_set(kos_pstate_t target);
```

A small enum, not a free Hz, because no chip here can hit an arbitrary frequency:

```
typedef enum {
    KOS_PSTATE_MAX = 0,   // full PLL (today's boot clock: 144/120/84 MHz)
    KOS_PSTATE_MID,        // ~half: a locked PLL profile at reduced fVCO/divider
    KOS_PSTATE_LOW,        // crystal/RC direct, PLL bypassed (deep power saving)
} kos_pstate_t;
```

The weak default lives in `arch_arm_common.cc` (or a neutral TU) and returns 0.
The sim also returns 0 (`arch_cpu_clock_hz` there already returns 0). A backend
opts in by overriding the symbol.

Per-chip feasibility:

- **XMC4800 -- YES, cleanly.** `clock_init()` already ramps K2DIV through
  288/12/6/4/3/2 (`chip_xmc4800.cc:358-361`); the PLL stays locked while K2DIV
  changes. MID/LOW map to writing `pllcon1_value(k2div)` for a larger K2DIV, or
  `VCOBYP` for fOFI direct. Runtime-retunable.
- **STM32F411 -- FIXED SET only.** `PLLCFGR` is writable only while the PLL is
  OFF (`chip_stm32f411.cc:179-180`). A retune means: switch SYSCLK to HSI, stop
  the PLL, rewrite N/P, restart, wait `PLLRDY`, switch SYSCLK back -- doable but
  it briefly parks on HSI 16 MHz. MID/LOW = HSI-direct, or a second precomputed
  PLL profile.
- **K64F -- FIXED SET, staged.** MCG is a state machine (FEI/FBE/PBE/PEE,
  `chip_mk64f.cc:268-311`). Retune walks PEE->PBE, reprograms C6 VDIV, walks back,
  each step LOCK/status-polled. LOW = drop to FBE (external) or FEI (~20.97 MHz).
- **rp2040 -- YES for clk_sys.** PLL_SYS is independent; clk_ref (hence the 1 MHz
  TIMER) is deliberately NOT touched (`chip_rp2040.cc:256-259`), so the monotonic
  clock is immune by construction. The reference model.
- **Everything else (sam3x8e, nrf51, stm32f103/f302, mps2, esp32, rx72m,
  riscv) -- returns 0 for M3.** They keep the weak default until someone needs it.

---

## 2. The coherence problem -- concrete ordering

What must be updated, and in what order, so nothing tears:

| # | Item | Stored as | Action on change |
|---|------|-----------|------------------|
| a | `SystemCoreClock` | u32 | Rewritten by `arch_cpu_clock_set` to the achieved Hz -- the single source of truth every conversion below reads. |
| b | `arch_clock_now` mult | cached reciprocal of the timer clock | RE-ANCHOR, not just recompute (section 0). |
| c | Sleep/timer deadlines | **ns** (`Thread::deadline_ns`, `time.cc:108`) | SAFE -- ns is clock-invariant PROVIDED `now` does not jump (b). No rescale. |
| d | RR slice / `next_timed_event` | **ns** (`sched.cc:252`) | SAFE for the same reason. No rescale. |
| e | Armed hardware timer (SysTick) | cycles, `= delta_ns * SystemCoreClock / 1s` (`arch_arm_common.cc:126`) | MUST re-arm at the new rate. |
| f | Console baud divisor | derived from peripheral clock at init | MUST re-derive (section 2.2). |

Because (c) and (d) are already in ns, the delta-queue itself needs zero
rescaling -- the landed tickless design paid for this property up front. The work
is entirely in (b), (e), (f).

### 2.1 The re-anchor (fixes the jump)

Change each affected `arch_clock_now` from "reprice all ticks at current mult" to
a piecewise-linear accumulator with an epoch anchor:

```
now = base_ns + (raw_ticks - base_ticks) * mult   // 64x64 split as today
```

Add an arch hook the clock-select calls at the exact instant of the change:

```
void arch_clock_reanchor(void);   // base_ns = now()@old-mult; base_ticks = raw; recompute mult@new SystemCoreClock
```

- Called AFTER `SystemCoreClock` is updated but computing `base_ns` with the OLD
  mult first, so the seam is continuous: history keeps its old pricing, the future
  accrues at the new rate. `now` is monotonic across the change with no jump.
- rp2040 / nrf51 / sim: weak no-op (their timer clock does not move).
- This makes `arch_clock_now` re-entrancy-safe only under the section-2.3 mask;
  the anchor triple (`base_ns`, `base_ticks`, `mult`) must be updated with the
  timer ISR masked, else `ktime_on_timer` reading a half-updated anchor tears.

### 2.2 Baud re-derivation

The baud divisor is a function of the peripheral clock, which moves with the core
clock. K64F already has a re-derive helper (`chip_mk64f.cc:595-599`,
`sbr/brfa` from live `SystemCoreClock`); STM32 computes `usart_brr(pclk1, baud)`
(`chip_stm32f411.cc:296`); XMC's USIC constants are computed for fPERIPH=72 MHz
(`chip_xmc4800.cc:374-378`). The clock-select MUST re-run the baud derivation
against the new clock, or the console garbles. Two hazards:

- Re-derive only AFTER the TX FIFO/shift register is drained -- a baud change
  mid-character corrupts the byte in flight. Quiesce = flush console sync first.
- At LOW P-states some baud/clock pairs have no accurate divisor (e.g. 115200 off
  a 16 MHz HSI has >1% error on some parts). A P-state that cannot hold the
  console baud within tolerance should be rejected (return 0) OR the baud lowered
  in lockstep -- flagged as an open question.

### 2.3 The safe sequence

```
sys_cpu_clock_set(target):
  IrqLock                      # mask locally; single-core, so this quiesces timers
  flush console TX sync        # drain in-flight byte before baud moves (2.2)
  hz = arch_cpu_clock_set(t)   # PLL retune; parks on fallback internally if it fails
  if hz == 0: return -1        # unsupported or retune failed -> clock unchanged, nothing to undo
  # SystemCoreClock now == hz (set inside arch_cpu_clock_set)
  arch_clock_reanchor()        # (b) continuous now across the change (2.1)
  re-derive console baud       # (f) at the new peripheral clock (2.2)
  g_armed_deadline_ns = ~0     # (e) INVALIDATE the arm-dedup cache, else re-arm no-ops
  ktime_rearm()                # (e) reload SysTick against new SystemCoreClock
  # unlock
```

Hazards addressed by this ordering:

- **Timer IRQ mid-change.** The whole body runs under `IrqLock`; on this
  single-core kernel that masks the timer line, so no `ktime_on_timer` observes a
  half-updated anchor or a stale-rate SysTick. The change is atomic w.r.t. time.
- **The arm-dedup trap.** `arch_timer_arm` early-returns if `deadline_ns ==
  g_armed_deadline_ns` and SysTick is still enabled (`arch_arm_common.cc:86`).
  After a clock change the nearest deadline_ns is usually unchanged, so a plain
  `ktime_rearm()` would SKIP reloading SysTick and leave it counting at the old
  rate. The sequence explicitly clears `g_armed_deadline_ns` first. (`cached_f`
  inside `arch_timer_arm` self-heals `max_delta` because it already keys off
  `SystemCoreClock`.)
- **A sleeping thread's deadline.** Its `deadline_ns` is untouched (ns-invariant);
  it wakes at the correct wall time because `now` stayed continuous (2.1) and
  SysTick was re-armed (e). No per-sleeper walk needed -- the payoff of ns storage.
- **Retune failure.** Every backend already parks on a safe fallback clock and
  leaves `SystemCoreClock` reflecting it (`chip_mk64f.cc:250-257`,
  `chip_stm32f411.cc:189`, `chip_xmc4800.cc:322/339`). On `hz == 0` we return
  before re-anchor/re-arm, so a failed retune is a clean no-op.

---

## 3. Syscall shape

```
KOS_SYS_cpu_clock_set = 30,   // (kos_pstate_t) -> achieved core Hz (u32), or 0 on failure/unsupported/not-privileged
```

Next free after `console_publish = 29`. User stub `kos_cpu_clock_set(pstate)` in
`user/src/syscall_stubs.cc`, prototype in `user/include/kickos/sys.h`.

**Privileged-only.** It mutates a global (`SystemCoreClock`), retimes EVERY
thread's SysTick basis, and changes the console baud shared by all tasks -- the
same "disables/mutates global device state" reasoning that made `console_publish`
and `ram_alloc` privileged (`syscall.cc:868-873`). An unprivileged task retuning
the clock could denial-of-service every other task's timing. Gate with the exact
`current()->privileged` check; return 0 (== the failure sentinel) on the
unprivileged path so callers need only one error test.

Return value = achieved Hz (not a status), so a caller that asks for MID and gets
a coarser achievable point can observe what it actually got in one call, mirroring
the read-side `cpu_clock_hz` shape.

---

## 4. Low-power mode

Same seam, minimally. A deep low-power P-state IS "the lowest clock the console
can still hold", reached through the identical `arch_cpu_clock_set` path -- no
separate syscall for M3. WFI in the idle path already benefits: at a lower clock
the idle loop and any polled spins burn less. XMC even keeps fPLL through SLEEP
deliberately so a post-print WFI does not rescale baud (`chip_xmc4800.cc:363-365`)
-- that trick composes with a LOW P-state.

Deferred (NOT in M3): explicit STOP/STANDBY sleep states that gate peripheral
clocks or lose RAM; tickless deep-sleep that stops the monotonic counter (would
need an RTC wake + `now` catch-up on resume); DVFS/voltage scaling; per-peripheral
clock gating. Note them so the enum can grow without an ABI break (append states).

---

## 5. Interaction with the clock-hardening

The hardening's whole premise -- a wide peripheral monotonic counter that
`arch_clock_now` reads -- survives ONLY with the section-2.1 re-anchor, and only
per chip:

- **rp2040: immune.** TIMER on clk_ref, decoupled from clk_sys by design
  (`chip_rp2040.cc:256-259`). `now` is unaffected; reanchor is a no-op.
- **nrf51: immune.** `arch_clock_now` is semihosting-backed
  (`chip_nrf51.cc:76-99`), not on any core-derived counter.
- **sim: immune.** Host-clock backed (`sim.cc:843`).
- **XMC4800 / K64F / STM32F411: NOT immune.** Their counter clock moves with the
  core/bus clock (section 0). WITHOUT the re-anchor these are exactly the chips
  whose `now` would jump. WITH it, they stay monotonic and correct. These three
  are the only backends that must ship `arch_clock_reanchor` for M3 (they are also
  the only three shipping `arch_cpu_clock_set`, so the sets coincide).

No chip's `now` runs BACKWARD across the change under the re-anchor (base_ns is
computed before mult swaps, and raw_ticks only increases). Forward jump is
eliminated. The clock stays trustworthy.

---

## 6. Test / validation plan

Emulator-testable (QEMU mps2 / sim / rp2040 where modelled):

- Unit: `arch_clock_reanchor` continuity -- mock `raw_ticks` + `SystemCoreClock`,
  assert `now` monotonic and continuous (delta bounded by one sample) across a
  simulated 6x rate change. Pure math, no silicon.
- Unit: the arm-dedup invalidation -- assert `ktime_rearm` after a clock change
  actually reloads SysTick (i.e. `g_armed_deadline_ns` was cleared).
- Unsupported path: on sim/mps2 `sys_cpu_clock_set` returns 0 and leaves timing
  untouched; unprivileged caller returns 0.

Silicon-only (XMC Relax Kit, K64F FRDM, STM32F411 Disco):

- **Sleep-across-change:** thread sleeps N ms; a co-thread retunes the clock
  mid-sleep; assert wake lands within tolerance of the ORIGINAL wall-clock
  deadline (proves ns deadlines + re-anchor + re-arm compose). The direct
  regression for the section-0 hazard.
- **Console integrity:** print continuously while cycling MAX/MID/LOW; assert no
  garbled bytes on the host (proves flush-before + baud re-derive).
- **Monotonic `now`:** tight loop sampling `sys_clock_now()` across several
  retunes; assert strictly non-decreasing and no multi-second forward step (the
  jump this note exists to prevent).
- **Retune-failure:** force a PLL non-lock (e.g. no crystal); assert `set` returns
  0, clock/baud/`now` all unchanged.

---

## OPEN QUESTIONS (for fable review)

1. **P-state enum vs raw Hz.** Is a 3-point `kos_pstate_t` the right abstraction,
   or should the ABI carry a requested Hz with "nearest achievable" semantics
   (more flexible, but implies a per-chip frequency table)?
2. **Baud at LOW.** When a P-state cannot hold 115200 within tolerance: reject the
   P-state (return 0), silently lower the baud, or renegotiate the console baud as
   part of the transition (and how would userspace learn the new baud)?
3. **Re-anchor placement.** `arch_clock_reanchor` as a distinct hook, or fold the
   anchor logic INTO `arch_cpu_clock_set` so a backend cannot forget to call it?
   (Distinct hook keeps `arch_cpu_clock_set` a pure PLL op; folded is harder to
   misuse.)
4. **IrqLock span.** The transition holds `IrqLock` across a PLL relock +
   status-poll (K64F/STM32 can be tens of us). Is that masked span acceptable
   against the M3 ISR-latency budget, or should the retune run with only the
   timer line masked (finer, but more code per backend)?
5. **Multicore forward-proofing.** The safe sequence assumes single-core (IrqLock
   quiesces the only timer). Do we record an explicit "single-core only" invariant,
   or leave a hook shape that a future SMP port can extend (per-core SysTick
   re-arm + a barrier)?
6. **P-state observability.** Should there be a read-side `sys_cpu_pstate()` (which
   P-state am I in), or is `cpu_clock_hz()` enough to infer it?
7. **Failure atomicity on the staged chips.** K64F/STM32 pass THROUGH an
   intermediate clock (HSI / FBE) during retune. If the SECOND leg fails, we land
   on the intermediate, not the original. Is "achieved Hz reflects the
   intermediate, return it truthfully" acceptable, or must the transition be
   all-or-nothing back to the original?
