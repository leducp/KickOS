<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Design note: M3 -- user-selectable CPU clock / low-power mode (WRITE side)

> **Status: LANDED** -- the write side shipped: `arch_cpu_clock_set` plus the coherence tail
> (epoch re-anchor as sole rate-writer, baud re-derive, timer re-arm, console-ownership refusal).
> Silicon-proven on XMC (144/48 MHz) and K64F (120/20.97 MHz): monotonic `now` across a retune,
> ratio-correct timing, no fault. XMC does a full retune, K64F a staged one; every other
> chip keeps the explicit fallback. Fleet-wide rollout and the userspace power-manager /
> clock-tree policy service stay open (`../roadmap.md`).
> **The contract now lives in `reference/invariants.md`**: `clock-retune-coherence-tail` (the
> ordered sequence and the "run the tail on an actual move" gate),
> `clock-anchor-sole-writer-at-rate-edge` (the epoch anchor), `timer-arm-dedup-needs-disarm`.
> This note is the decision record behind them.

The READ side landed first: `sys_cpu_clock_hz()` (`KOS_SYS_CPU_CLOCK_HZ`,
`user/include/kickos/sys/abi.h`) returns `arch_cpu_clock_hz()`, each backend reporting its CMSIS
`SystemCoreClock`. This note settles the WRITE side.

---

## 0. The hazard the decisions answer

On both retuning backends the hardened wide monotonic counter behind `arch_clock_now` is clocked
from the domain the retune moves (XMC CCU40 on fCCU = fSYS; K64F PIT on bus = core/BUS_DIV). A
ns-per-tick rate prices the ENTIRE accumulated tick count, not just post-change ticks, so a naive
PLL change reprices all of history: at XMC's 144 -> 48 MHz the rate triples and `now` jumps forward
by roughly `elapsed_since_boot * (ratio - 1)`, seconds to minutes of phantom time, taking every
ns-valued deadline compared against it (K64F's 120 -> 20.97 MHz is the wider ratio, about 5.7x).
STM32F411's TIM2 is same-domain and WOULD jump, but F411 does not retune. rp2040's TIMER (clk_ref)
and the sim are immune by construction; rp2040's CONSOLE is not (clk_peri tracks clk_sys).

## 1. The arch seam

**DECIDED: one hook, `arch_cpu_clock_set(kos_pstate_t target)`, returning the ACTUALLY-LANDED
core Hz** rather than a status. A full success returns the requested point's Hz; a failed relock
that parked the core on a fallback returns THAT Hz, because the clock did move; `0` is returned
only for "this chip cannot change its clock at all". REJECTED: a status return with the Hz read
back separately, which cannot distinguish a staged fallback from a no-op (see ruling 7).

**DECIDED: the backend owns the re-anchor, the flash-wait-state/voltage step and the staircase
INTERNALLY**, called from privileged thread context with interrupts already masked by the caller,
never from an ISR. Ruling 3 carries the re-anchor half.

**DECIDED: flash wait-states (and any voltage step) go UP before frequency goes up, and DOWN
after frequency goes down** -- the general rule for any chip, not an XMC detail. A raw frequency
bump past the current wait-state's access window returns invalid fetch data, which is a fetch
fault rather than merely wrong timing.

**DECIDED: raise the clock by walking the divider STAIRCASE, never by a jump** where the silicon
offers one. XMC's `clock_init` already ramps K2DIV and the PLL stays locked across it, so the
staircase is free; a raw bump risks a supply droop.

Per-chip opt-in, and why the rest do not:

- **XMC4800: full retune, three locked-PLL points (144/96/48 MHz)** via the K2DIV staircase. The
  silicon harness cycled MAX and LOW only, so the 96 MHz MID point ships unexercised.
- **K64F: a staged FIXED SET.** MCG is a state machine; the retune walks PEE->PBE, reprograms C6
  VDIV, walks back, each leg status-polled. The achievable set is `{120 MHz PEE, 20.97 MHz FEI}`
  and MID rounds UP to MAX, which is what makes B1's gate unexercisable here (section 6).
- **STM32F411: DEFERRED, fallback only.** `PLLCFGR` is writable only while the PLL is OFF, so a
  retune must park SYSCLK on HSI 16 MHz, stop the PLL, rewrite N/P, restart and switch back. It
  carries the sole-rate-writer cleanup regardless, as uniform hygiene.
- **rp2040: feasible for clk_sys, but it is the reference model for the TIMER ONLY.** clk_ref is
  deliberately untouched so the monotonic clock is immune, yet clk_peri TRACKS clk_sys, so its
  UART baud moves exactly like the not-immune chips and it would still need the baud re-derive.
- **Everything else (sam3x8e, nrf51, stm32f103/f302, mps2, esp32, rx72m, riscv): the fallback
  TU**, which returns 0, until someone needs otherwise.

## 2. The coherence problem

**DECIDED: store deadlines in ns and rescale NOTHING.** `Thread::deadline_ns` and the RR slice /
`next_timed_event` are clock-invariant, so the delta queue needs zero walk on a retune and a
sleeping thread wakes at the correct wall time untouched. This property was paid for up front by
the tickless design; it holds ONLY while `now` stays continuous, which is 2.1's job. The work is
then confined to the anchor, the armed hardware timer, and the baud divisor.

### 2.1 The re-anchor

**DECIDED: a piecewise-linear accumulator with an epoch anchor** (`now = base_ns + (raw_ticks -
base_ticks) * rate`), re-anchored at the rate edge: capture `base_ns` under the OLD rate, then
move the clock, then commit the new rate. History keeps its old pricing and the future accrues at
the new rate, so `now` never jumps and never runs backward.

**REQUIRED PRECONDITION (B2): the lazy `if (hz != cached_hz) recompute` inside each affected
`arch_clock_now` is REMOVED.** If it survives, any `now()` in the window between the
`SystemCoreClock` write and the re-anchor recomputes the rate itself and bakes the phantom jump
into `base_ns` permanently. That makes the re-anchor the SOLE rate-writer, which is now
`clock-anchor-sole-writer-at-rate-edge`.

### 2.2 Baud re-derivation

**DECIDED: re-derive the baud divisor from the live clock in the tail, and only after the TX
shift register is SHIFT-IDLE.** Draining to buffer-empty (TXE/TDRE) still leaves one character
clocking out of the shifter, and changing the baud then garbles that in-flight byte, so the flush
must poll transmission-complete.

**DECIDED: a P-state whose clock cannot produce the console baud within tolerance is REJECTED at
the seam** (ruling 2): the backend returns the previous Hz unchanged rather than landing a point
that garbles the console. The baud is never silently lowered.

### 2.3 The safe sequence

The ordered sequence and the gating rule are the Reference contract
(`reference/invariants.md`, `clock-retune-coherence-tail`); `kernel/time/clock_select.cc` is the
code. The decisions inside it:

- **The tail runs on an ACTUAL move, never on a success flag (B1).** The superseded model said
  "hz == 0 means the clock did not change, skip the tail", which is WRONG on a staged chip:
  K64F's `fail_to_fei` parks the core at ~20.97 MHz AND truthfully rewrites `SystemCoreClock`, so
  an early return would skip re-anchor/baud/re-arm and detonate `now()` and the console.
- **`arch_timer_disarm()` before the retune, not a bare `ktime_rearm()` after it.**
  `arch_timer_arm` deduplicates an unchanged deadline, and after a clock change the nearest
  deadline_ns usually IS unchanged, so the re-arm would silently keep SysTick at the old rate.
  REJECTED: poking the `g_armed_deadline_ns` static directly; it is file-static on purpose, and
  the disarm primitive also kills a SysTick that pended while masked.
- **The whole body under one `IrqLock`.** Single-core, so this masks the one timer line and the
  change is atomic with respect to time: no `ktime_on_timer` can observe a half-updated anchor or
  a stale-rate SysTick.

### 2.4 Interaction with console handover

**DECIDED: REFUSE the retune while the console is not KERNEL_OWNED**, checked FIRST so the
refusal is a true no-op (nothing masked, previous Hz returned). Once a userspace driver holds the
UART the kernel does not know its baud parameters and cannot re-derive them, so moving the
peripheral clock would garble a channel the kernel cannot fix. `RECLAIMED` counts as
not-kernel-owned: a retune on a panic path is never wanted. A cooperative driver that wants a
retune relinquishes the console first.

## 3. Syscall shape

**DECIDED: one syscall, `KOS_SYS_CPU_CLOCK_SET`, argument a `kos_pstate_t` carried as a plain
u32** (ruling 1), return value the landed Hz (ruling 7). `0` means cannot-change / unsupported /
not-permitted, so a caller needs exactly one error test, and this syscall stays OUT of the
`-KOS_E*` scheme by return type.

**DECIDED: no read-side P-state syscall** (ruling 6). The observable state is the landed
frequency, already readable; a separate P-state read would be a second source of truth that could
disagree with it, especially after a staged fallback.

**Gated, and the gate MOVED.** This note wrote the gate as `current()->privileged`, on the same
"mutates or disables global device state" reasoning that gates the console publish and the arena
allocation. The gate that SHIPPED is the `KOS_AUTH_PSTATE` authority bit, its own bit rather than
the console or shutdown one, because root is unprivileged on every board now and a privilege check
would gate nothing. The reasoning stands unchanged: an ungated retune mutates `SystemCoreClock`,
retimes every thread's SysTick basis and moves the shared console baud, so it denies service to
every other task's timing.

## 4. Low-power mode

**DECIDED: no separate low-power syscall for M3.** A deep low-power P-state IS "the lowest clock
the console can still hold", reached through the identical seam; the idle WFI already benefits.

DEFERRED, and named so the enum can grow by appending: explicit STOP/STANDBY states that gate
peripheral clocks or lose RAM; tickless deep-sleep that stops the monotonic counter (needs an RTC
wake plus a `now` catch-up on resume); DVFS/voltage scaling; per-peripheral clock gating.

## 5. Interaction with the clock-hardening

The hardening's premise is a wide peripheral monotonic counter behind `arch_clock_now`. It
survives only per chip, and the split is a decision to record rather than a symmetry to expect:

- **Immune:** rp2040's TIMER (clk_ref, decoupled by design), nrf51 (semihosting-backed), sim
  (host-clock backed). Their re-anchor is a no-op or absent.
- **NOT immune:** XMC4800 and K64F, whose counter clock moves with the core/bus clock. They are
  also the only two backends shipping `arch_cpu_clock_set`, so the not-immune set and the
  retuning set coincide; F411 is same-domain but never retunes.
- **The console is a SEPARATE question from the clock.** rp2040 is immune for `now` and NOT for
  the console, which is the case that stops "rp2040 is the model" from being read too far.

## 6. Test / validation plan

Emulator-testable: re-anchor continuity against a mocked 6x rate change (assert `now` monotonic
and continuous, and that the rate is written ONLY at the edge, never lazily in the read); the
arm-dedup invalidation (assert the disarm plus trailing re-arm really reloads SysTick at the new
rate); the unsupported and unpermitted paths return 0 and leave timing untouched.

Silicon-only (XMC Relax Kit, K64F FRDM), the direct regressions for section 0:
**sleep-across-change** (a co-thread retunes mid-sleep; wake must land within tolerance of the
ORIGINAL wall-clock deadline); **console integrity** while cycling MAX/MID/LOW; **monotonic
`now`** across several retunes with no multi-second forward step; the **console-owned reject**
(2.4) returning the previous Hz with nothing masked; **rp2040 baud** re-derived while `now` is
unaffected.

**FALSIFIER, untested path accepted: B1's gate is NOT exercisable on the K64F today.** The gating
logic (run the tail whenever `hz != previous`, never on a success flag) is correct and would fire
for a staged fallback landing on a DISTINCT intermediate Hz. But K64F's achievable set is binary
(MAX = 120 MHz PEE, LOW = ~20.97 MHz FEI, MID rounding UP to MAX), and `fail_to_fei` runs only on
a RISE, where `previous` is ALREADY the ~20.97 MHz FEI point. A failed relock parks right back
on that same Hz, so `hz == previous` and the tail is correctly skipped: a real staged fallback is
indistinguishable from a plain no-op here. Exercising the gate needs a genuine THIRD K64F staged
point at a distinct intermediate Hz. **DECIDED: do NOT add one just to test the gate** --
silicon-risky, and out of scope. The gate ships unexercised on purpose.

## 7. Resolved decisions (was: open questions)

1. **P-state arg = u32 enum.** No raw-Hz "nearest achievable" ABI and no per-chip frequency
   table: the achievable set is small and chip-specific, and the truthful landed Hz is the return
   value, which is enough to know what was obtained.
2. **A P-state that cannot hold a valid console baud is REJECTED at the seam** (2.2). The
   backend returns the previous Hz rather than landing a point whose peripheral clock has no
   in-tolerance divisor. The baud is never silently lowered.
3. **Re-anchor is folded INTO the chip backend, at the rate edge** (2.1). REJECTED: a generic
   `arch_clock_reanchor` hook called after `arch_cpu_clock_set` returns, which a backend could
   forget and which widens the mispriced window from a few instructions to a full syscall tail
   (worst case K64F, whose staged MCG walk would otherwise leave a ms-class gap). The cost is
   that `arch_cpu_clock_set` is no longer a pure PLL op; accepted for the tighter skew bound.
4. **The masked span is accepted, with honest per-chip bounds.** PLL lock time dominates and is
   per-chip: XMC's staircase stays PLL-locked and is O(tens of us), an STM32 relock is
   O(100-200 us), K64F's staged MCG walk plus per-leg LOCK polls is the worst at up to ~1 ms.
   These are ISR-blackout windows on a rare, deliberate, privileged act, never on a hot path.
   REJECTED as premature: finer timer-line-only masking.
5. **The transition is SINGLE-CORE ONLY**, recorded as an explicit invariant. It relies on
   `IrqLock` quiescing the one and only timer. An SMP port needs a cross-core quiesce (per-core
   SysTick re-arm plus a barrier so no other core reads a half-updated anchor). Flagged, NOT
   solved here.
6. **No P-state read syscall** (section 3). A second source of truth could disagree with the
   landed Hz, notably after a staged fallback.
7. **Staged-failure contract = return the truthful landed Hz and always run the tail on an actual
   change** (B1). There is no all-or-nothing rollback to the original point: the truthful landed
   frequency, coherently plumbed, is the contract. `0` is reserved for cannot-change-at-all.
