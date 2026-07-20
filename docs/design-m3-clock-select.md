<!-- SPDX-License-Identifier: CECILL-C -->
# Design note: M3 -- user-selectable CPU clock / low-power mode (WRITE side)

**Status: fable review folded in. This is the corrected implementation spec.**
Rulings from the review are load-bearing; the old open-questions section is now
section 7 (resolved decisions).

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

Each `arch_clock_now` today converts raw ticks to ns with a cached reciprocal
`mult` recomputed lazily `if (hz != cached_hz)` -- an explicit "changes once at
boot" assumption (see the comment at `chip_xmc4800.cc:387`). Runtime clock-select
violates that assumption two ways at once:

1. The counter's tick RATE changes going forward (correct after `mult` recompute).
2. `mult` is applied to the ENTIRE accumulated tick count, not just post-change
   ticks. So `now = raw_ticks * new_mult` reprices all of history at the new rate.
   At 144 -> 24 MHz the new `mult` is 6x larger; `now` JUMPS forward by roughly
   `elapsed_since_boot * (ratio - 1)` -- seconds-to-minutes of phantom time.

That is the hardest coherence hazard: a naive PLL change silently detonates the
monotonic clock the whole clock-hardening effort existed to make trustworthy, and
with it every ns-valued deadline compared against `now`.

The fix is section 2's epoch re-anchor. But the fix has a REQUIRED PRECONDITION
(review B2): the lazy `if (hz != cached_hz) recompute mult` inside each affected
`arch_clock_now` MUST BE REMOVED. If it survives, any `now()` called in the window
between the `SystemCoreClock` update and the re-anchor recomputes `mult` itself
against the new Hz and bakes the phantom forward jump into `base_ns` PERMANENTLY.
After this change the re-anchor step (section 2.1) is the SOLE writer of `mult`;
`arch_clock_now` only ever READS the anchor triple, never recomputes it. This is a
concrete edit to `arch_clock_now` in `chip_xmc4800.cc`, `chip_mk64f.cc`, and
`chip_stm32f411.cc`.

rp2040's monotonic TIMER (on clk_ref) and the sim are structurally immune to the
`now` jump and need no re-anchor. rp2040's CONSOLE is NOT immune -- clk_peri
tracks clk_sys (section 1, section 5).

---

## 1. The arch seam

Add one hook (weak default = unsupported):

```
// Retune the core/bus clock. target = a P-state selector, NOT a raw Hz (the
// achievable set is chip-specific and small). Returns the ACTUALLY-LANDED core
// Hz -- ALWAYS the truth about where the clock now sits, never a status.
//   - A retune that fully succeeds returns the requested point's Hz.
//   - A retune that FAILS and parks the core on a safe fallback (e.g. K64F
//     fail_to_fei -> ~20.97 MHz) returns THAT fallback Hz -- non-zero, because
//     the clock DID move. The caller MUST then run the full coherence tail.
//   - 0 is returned ONLY for "this chip cannot change its clock at all"
//     (unsupported backend / weak default). 0 never means "failed but moved".
// The backend also performs the re-anchor and any flash-wait-state / voltage
// step INTERNALLY, bracketing the exact PLL/divider write (review S2, S3).
// MUST be called from privileged thread context with interrupts already masked
// by the caller (see the coherence sequence in section 2). MUST NOT be called
// from ISR context.
uint32_t arch_cpu_clock_set(kos_pstate_t target);
```

A small u32-backed enum, not a free Hz, because no chip here can hit an arbitrary
frequency (ruling 1):

```
typedef enum kos_pstate_e : uint32_t {   // fixed u32 width -> stable syscall ABI
    KOS_PSTATE_MAX = 0,   // full PLL (today's boot clock: 144/120/84 MHz)
    KOS_PSTATE_MID,        // ~half: a locked PLL profile at reduced fVCO/divider
    KOS_PSTATE_LOW,        // crystal/RC direct, PLL bypassed (deep power saving)
} kos_pstate_t;
```

The weak default lives in `arch_arm_common.cc` (or a neutral TU) and returns 0
(the "cannot change clock at all" value). The sim also returns 0
(`arch_cpu_clock_hz` there already returns 0). A backend opts in by overriding the
symbol.

Per-chip feasibility:

- **XMC4800 -- YES, cleanly, WITH the staircase (S3).** `clock_init()` already
  ramps K2DIV through 288/12/6/4/3/2 (`chip_xmc4800.cc:358-361`); the PLL stays
  locked while K2DIV changes. LOWERING the clock (MAX->MID/LOW) writes a larger
  K2DIV or `VCOBYP` for fOFI direct. RAISING it back toward 144 MHz is NOT a raw
  PLL bump: it MUST (a) pin the flash wait states for the high-frequency point
  BEFORE the rise, and any voltage/regulator step BEFORE the rise, then (b) walk
  K2DIV DOWN the divider STAIRCASE stepwise, never jump. A raw bump risks a fetch
  fault or supply droop, not merely wrong timing. On a DECREASE the order inverts:
  lower the frequency first, then relax flash wait states. General rule for ANY
  chip: flash-WS (and voltage) go UP before frequency goes up, DOWN after
  frequency goes down.
- **STM32F411 -- FIXED SET only.** `PLLCFGR` is writable only while the PLL is
  OFF (`chip_stm32f411.cc:179-180`). A retune means: switch SYSCLK to HSI, stop
  the PLL, rewrite N/P, restart, wait `PLLRDY`, switch SYSCLK back -- doable but
  it briefly parks on HSI 16 MHz. MID/LOW = HSI-direct, or a second precomputed
  PLL profile.
- **K64F -- FIXED SET, staged.** MCG is a state machine (FEI/FBE/PBE/PEE,
  `chip_mk64f.cc:268-311`). Retune walks PEE->PBE, reprograms C6 VDIV, walks back,
  each step LOCK/status-polled. LOW = drop to FBE (external) or FEI (~20.97 MHz).
- **rp2040 -- YES for clk_sys, but the console is NOT immune (S5).** PLL_SYS is
  independent; clk_ref (hence the 1 MHz TIMER) is deliberately NOT touched
  (`chip_rp2040.cc:256-259`), so the MONOTONIC CLOCK is immune by construction and
  its re-anchor is a no-op. But clk_peri TRACKS clk_sys (`chip_rp2040.cc:13`,
  `CLK_PERI_ENABLE_CLK_SYS = AUXSRC 0x0`), so the UART baud MOVES with clk_sys
  exactly like the other clock-affected consoles. rp2040 must re-derive its baud
  on retune (section 2.2). It is the reference model ONLY for the timer, not for
  the console.
- **Everything else (sam3x8e, nrf51, stm32f103/f302, mps2, esp32, rx72m,
  riscv) -- returns 0 for M3.** They keep the weak default until someone needs it.

---

## 2. The coherence problem -- concrete ordering

What must be updated, and in what order, so nothing tears:

| # | Item | Stored as | Action on change |
|---|------|-----------|------------------|
| a | `SystemCoreClock` | u32 | Rewritten by `arch_cpu_clock_set` to the achieved Hz -- the single source of truth every conversion below reads. |
| b | `arch_clock_now` mult | cached reciprocal of the timer clock | RE-ANCHOR is the SOLE writer of `mult`; the lazy in-`now()` recompute is REMOVED (B2, section 0). |
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

The re-anchor is FOLDED INTO THE CHIP BACKEND, applied AT THE RATE EDGE -- the
exact instruction where the PLL/divider actually changes -- NOT in generic code
after the fact (review S2, ruling 3). The re-anchor step is:

```
base_ns    = now()  computed with the OLD mult   // capture history at old pricing
base_ticks = raw_ticks now
mult       = reciprocal of the NEW timer clock    // sole writer of mult (B2)
```

- Because `base_ns` is computed with the OLD mult BEFORE the divider write, the
  seam is continuous: history keeps its old pricing, the future accrues at the new
  rate. `now` is monotonic across the change with no jump.
- Placing it AT the edge (inside the backend) rather than in generic code AFTER
  `arch_cpu_clock_set` returns BOUNDS the permanent skew. Every raw tick that
  elapses in the `SystemCoreClock`-update -> re-anchor window is priced wrong and
  that error is frozen into `base_ns`; folding the anchor to the divider write
  shrinks that window to a few instructions instead of a full syscall tail. Worst
  case is K64F, whose staged MCG walk otherwise leaves a ms-class gap.
- Backends whose timer clock does not move (rp2040 TIMER on clk_ref, nrf51
  semihosting, sim) simply do not re-anchor -- there is no generic hook to forget.
- The anchor triple (`base_ns`, `base_ticks`, `mult`) is updated with the timer
  ISR masked (section 2.3), else `ktime_on_timer` reading a half-updated anchor
  tears. Since the write now lives inside the backend, the backend relies on the
  caller's `IrqLock` being held around `arch_cpu_clock_set`.

### 2.2 Baud re-derivation

The baud divisor is a function of the peripheral clock, which moves with the core
clock. K64F already has a re-derive helper (`chip_mk64f.cc:595-599`,
`sbr/brfa` from live `SystemCoreClock`); STM32 computes `usart_brr(pclk1, baud)`
(`chip_stm32f411.cc:296`); XMC's USIC constants are computed for fPERIPH=72 MHz
(`chip_xmc4800.cc:374-378`). The clock-select MUST re-run the baud derivation
against the new clock, or the console garbles. Two hazards:

- Re-derive only AFTER the TX shift register is SHIFT-IDLE (transmission
  complete), not merely after the TX buffer is empty (review S6). Draining to
  buffer-empty (TXE/TDRE) still leaves one character CLOCKING OUT of the shift
  register; changing the baud then garbles that in-flight byte. The flush MUST
  poll transmission-complete (TC). NOTE: the STM32 sync writer currently polls
  only `SR_TXE` (`chip_stm32f411.cc:379`) and there is NO `SR_TC` constant defined
  -- adding a TC constant and a TC-poll to the flush path is a required edit.
  Analogous TC/shift-idle polls are required in each affected chip's sync flush.
- At LOW P-states some baud/clock pairs have no accurate divisor (e.g. 115200 off
  a 16 MHz HSI has >1% error on some parts). A P-state whose clock cannot produce
  a console baud within tolerance is REJECTED AT THE SEAM (ruling 2): the backend
  returns the previous Hz unchanged rather than landing a point that garbles the
  console. The baud is NOT silently lowered.

### 2.3 The safe sequence

```
sys_cpu_clock_set(target):
  if console_state != KERNEL_OWNED:      # S4: userspace owns the UART -> refuse
    return cpu_clock_hz()                #   previous Hz, unchanged, nothing masked
  previous = SystemCoreClock
  IrqLock                        # mask locally; single-core, so this quiesces timers
  arch_timer_disarm()            # S1: stop SysTick + clear g_armed_deadline_ns +
                                 #   clear a pending SysTick, so nothing fires mid-
                                 #   transition at the stale rate (arch_arm_common.cc:139)
  flush console TX to SHIFT-IDLE # drain the in-flight byte to TC, not just TXE (2.2, S6)
  hz = arch_cpu_clock_set(t)     # backend: flash-WS/voltage step, staircase, RE-ANCHOR
                                 #   at the rate edge (S2), writes SystemCoreClock,
                                 #   returns the LANDED Hz (fallback Hz on failure, 0
                                 #   only if the chip cannot change clock at all)
  if hz != 0 and hz != previous: # COHERENCE TAIL: run on ANY actual change, success
    re-derive console baud       #   OR staged fallback (B1). NOT gated on success.
  ktime_rearm()                  # (e) always re-arm: we disarmed above. Reloads SysTick
                                 #   against the current SystemCoreClock (unchanged if hz==0).
  # unlock
  return hz                      # truthful landed Hz; 0 == cannot-change/unsupported
```

Note: the re-anchor no longer appears as a generic step -- it is inside
`arch_cpu_clock_set`, at the rate edge (2.1, S2). Generic code owns only the baud
re-derive and the re-arm.

Hazards addressed by this ordering:

- **Staged-failure detonation (the model this rewrite fixes, B1).** The OLD design
  said "hz == 0 means the clock did not change, skip the tail". That is WRONG on
  staged chips. K64F's `fail_to_fei` parks the core at ~20.97 MHz AND truthfully
  rewrites `SystemCoreClock` -- the clock DID move. An early return on a
  "failure" would skip re-anchor/baud/re-arm and detonate `now()` and the console.
  The corrected contract: the backend returns the LANDED Hz (never 0-for-moved;
  0 only for cannot-change-at-all), and the coherence tail runs whenever
  `hz != previous`, independent of success vs fallback.
- **Timer IRQ mid-change.** The whole body runs under `IrqLock`; on this
  single-core kernel that masks the timer line, so no `ktime_on_timer` observes a
  half-updated anchor or a stale-rate SysTick. The change is atomic w.r.t. time.
- **The arm-dedup trap.** `arch_timer_arm` early-returns if `deadline_ns ==
  g_armed_deadline_ns` and SysTick is still enabled (`arch_arm_common.cc:86`).
  After a clock change the nearest deadline_ns is usually unchanged, so a plain
  `ktime_rearm()` would SKIP reloading SysTick and leave it at the old rate.
  `arch_timer_disarm()` (S1) already sets `g_armed_deadline_ns = ~0` and disables
  SysTick, so the trailing `ktime_rearm()` always reloads. Using the existing
  disarm primitive is preferred over poking the `g_armed_deadline_ns` static
  directly (it is a file-static, not externally pokeable) and it also kills a
  SysTick that pended while masked. (`cached_f` inside `arch_timer_arm` self-heals
  `max_delta` because it already keys off `SystemCoreClock`.)
- **A sleeping thread's deadline.** Its `deadline_ns` is untouched (ns-invariant);
  it wakes at the correct wall time because `now` stayed continuous (2.1) and
  SysTick was re-armed (e). No per-sleeper walk needed -- the payoff of ns storage.
- **Console owned by userspace.** If a userspace driver holds the UART (console
  handover active, `console_state == USER_OWNED`), the kernel cannot re-derive or
  relocate the driver's baud, so moving the peripheral clock would silently garble
  it. The transition is REFUSED before any masking (S4, section 2.4).
- **Masked span honesty (S6).** The whole transition holds `IrqLock` across a PLL
  relock + status-poll. This is NOT "tens of us" in general: PLL lock time
  dominates and is per-chip. Realistic bounds with IRQs masked: STM32F411 PLL
  relock is O(100-200 us) (HSI settle + `PLLRDY`); K64F's staged MCG walk plus
  per-leg LOCK polls is the worst, O(hundreds of us to ~1 ms); XMC K2DIV staircase
  stays PLL-locked so it is the cheapest, O(tens of us). These are ISR-blackout
  windows; see ruling 4.

### 2.4 Interaction with console handover (S4)

A retune is REJECTED while the console is `USER_OWNED`. Once a userspace driver
has taken the UART (the M3 console-handover path, `g_console_state` in
`kernel/init/console.cc`; `console_owner_is_kernel()` returns 0), the kernel does
not know the driver's baud parameters and cannot re-derive or relocate them across
a peripheral-clock change. Silently moving the clock would garble the userspace
console with no way for the kernel to fix it. So:

```
if (console_owner_is_kernel() == 0) return cpu_clock_hz();  // previous Hz, unchanged
```

The check runs FIRST, before any masking or disarm, so the refusal is a true
no-op. `RECLAIMED` (panic took the UART back, polled-only) is treated as
not-kernel-owned for this purpose -- a retune during a panic path is never wanted.
A cooperative userspace driver that wants a retune must relinquish the console
first (revert to `KERNEL_OWNED`), then request the P-state change.

---

## 3. Syscall shape

```
KOS_SYS_cpu_clock_set = 30,   // (kos_pstate_t as u32) -> landed core Hz (u32);
                              //   0 == cannot-change/unsupported/not-privileged
```

Next free after `console_publish = 29`. The argument is a `kos_pstate_t` carried
as a plain u32 in the syscall register (ruling 1) -- the fixed-width enum keeps the
ABI stable. User stub `kos_cpu_clock_set(pstate)` in `user/src/syscall_stubs.cc`,
prototype in `user/include/kickos/sys.h`.

No read-side `sys_cpu_pstate()` syscall (ruling 6). The observable state is the
landed frequency, already readable via `sys_cpu_clock_hz()`; a separate P-state
read would be a second source of truth that could disagree with the achieved Hz
(especially after a staged fallback). Userspace infers its P-state from the Hz.

**Privileged-only.** It mutates a global (`SystemCoreClock`), retimes EVERY
thread's SysTick basis, and changes the console baud shared by all tasks -- the
same "disables/mutates global device state" reasoning that made `console_publish`
and `ram_alloc` privileged (`syscall.cc:868-873`). An unprivileged task retuning
the clock could denial-of-service every other task's timing. Gate with the exact
`current()->privileged` check; return 0 (== the cannot-change sentinel) on the
unprivileged path so callers need only one error test.

Return value = LANDED Hz (not a status), truthful in all cases (ruling 7): a
caller that asks for MID and gets a coarser achievable point -- or a staged
fallback after a failed relock -- observes exactly where the clock sits in one
call, mirroring the read-side `cpu_clock_hz` shape. 0 is returned ONLY when the
clock did not and cannot move (unsupported chip, not privileged); every non-zero
return means the clock now runs at that Hz, so the caller compares it against the
requested point to learn whether it got what it asked for.

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

- **rp2040: monotonic clock immune, console NOT (S5).** TIMER on clk_ref,
  decoupled from clk_sys by design (`chip_rp2040.cc:256-259`); `now` is unaffected
  and reanchor is a no-op. But clk_peri tracks clk_sys (`chip_rp2040.cc:13`), so
  its UART baud MOVES with a clk_sys retune -- rp2040 must re-derive baud in the
  coherence tail exactly like the three not-immune chips. It is the reference
  model for the TIMER only, not for the console.
- **nrf51: immune.** `arch_clock_now` is semihosting-backed
  (`chip_nrf51.cc:76-99`), not on any core-derived counter.
- **sim: immune.** Host-clock backed (`sim.cc:843`).
- **XMC4800 / K64F / STM32F411: NOT immune.** Their counter clock moves with the
  core/bus clock (section 0). WITHOUT the re-anchor these are exactly the chips
  whose `now` would jump. WITH it, they stay monotonic and correct. These three
  are the only backends whose `arch_cpu_clock_set` must perform the re-anchor at
  the rate edge for M3 (they are also the only three shipping `arch_cpu_clock_set`,
  so the sets coincide).

No chip's `now` runs BACKWARD across the change under the re-anchor (base_ns is
computed before mult swaps, and raw_ticks only increases). Forward jump is
eliminated. The clock stays trustworthy.

---

## 6. Test / validation plan

Emulator-testable (QEMU mps2 / sim / rp2040 where modelled):

- Unit: re-anchor continuity -- exercise the backend's anchor math (mock
  `raw_ticks` + `SystemCoreClock`), assert `now` monotonic and continuous (delta
  bounded by one sample) across a simulated 6x rate change, and assert `mult` is
  written ONLY by the re-anchor, never lazily inside `arch_clock_now` (B2). Pure
  math, no silicon.
- Unit: the arm-dedup invalidation -- assert `arch_timer_disarm` before the retune
  plus the trailing `ktime_rearm` actually reloads SysTick at the new rate (i.e.
  `g_armed_deadline_ns` was cleared, so the re-arm does not no-op).
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
- **Staged-failure (the B1 regression):** on K64F force the relock to fail so the
  MCG parks on `fail_to_fei` (~20.97 MHz). Assert `set` returns the truthful ~21
  MHz (NOT 0), that the coherence tail RAN (baud re-derived to the fallback clock,
  `now` continuous, no jump), and that a sleep across the event still wakes on
  time. This is the direct test that the clock-DID-move-on-failure path is not
  treated as a no-op.
- **cannot-change vs moved:** on sim/mps2 assert `set` returns 0 and leaves
  timing/baud untouched (the only clean-no-op case).
- **Console-owned reject (S4):** hand the console to a userspace driver
  (`USER_OWNED`), then call `set`; assert it returns the previous Hz, nothing is
  masked, and the userspace console is uncorrupted.
- **rp2040 baud (S5):** retune clk_sys and assert the console stays clean (baud
  was re-derived), while `now` is unaffected (clk_ref TIMER).

---

## 7. Resolved decisions (was: open questions)

1. **P-state arg = u32 enum.** The ABI carries `kos_pstate_t` as a fixed-width
   u32 (section 1). No raw-Hz "nearest achievable" ABI and no per-chip frequency
   table: the achievable set is small and chip-specific, and the truthful landed
   Hz is the return value, which is enough to know what was obtained.
2. **A P-state that cannot hold a valid console baud is REJECTED at the seam.**
   The backend returns the previous Hz unchanged rather than landing a point whose
   peripheral clock has no in-tolerance divisor for the console baud (section 2.2).
   The baud is never silently lowered.
3. **Re-anchor is folded INTO the chip backend, at the rate edge** (section 2.1).
   There is no separate generic `arch_clock_reanchor` hook a backend could forget
   to call; the anchor write brackets the PLL/divider write, which also bounds the
   mispriced window (S2). The cost -- `arch_cpu_clock_set` is no longer a pure PLL
   op -- is accepted for the safety and the tighter skew bound.
4. **The IrqLock masked span is accepted, with honest per-chip bounds documented**
   (section 2.3): PLL lock dominates -- XMC O(tens of us) staircase, STM32
   O(100-200 us) relock, K64F up to ~1 ms staged. A retune is a rare, deliberate,
   privileged act, so a one-shot ISR-blackout of this size is acceptable; it is
   NOT on any hot path. Finer timer-line-only masking is rejected as premature.
5. **The transition is SINGLE-CORE ONLY** -- recorded as an explicit invariant.
   The safe sequence relies on `IrqLock` quiescing the one and only timer. An M4
   / SMP port will need a cross-core quiesce (per-core SysTick re-arm + a barrier
   so no other core reads a half-updated anchor). Flagged, NOT solved here.
6. **No `sys_cpu_pstate()` read syscall** (section 3). The read side is the landed
   `cpu_clock_hz()`; a separate P-state read would be a second, possibly
   disagreeing, source of truth (notably after a staged fallback).
7. **Staged-failure contract = return the truthful landed Hz + always run the
   coherence tail on an actual change** (B1, sections 1 and 2.3). A failed relock
   that parks on a fallback (K64F `fail_to_fei` ~20.97 MHz) DID move the clock and
   rewrote `SystemCoreClock`; it returns that non-zero Hz and the caller runs
   re-anchor/baud/re-arm because `hz != previous`. 0 is reserved for "this chip
   cannot change its clock at all". There is no all-or-nothing rollback to the
   original: the truthful landed frequency, coherently plumbed, is the contract.
