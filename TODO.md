<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS TODO

**M1 VALIDATION COMPLETE (2026-07-14)** — 10 boards on silicon (5 ISAs) + 3 emulator gates
green; every board boots, has a console, runs the selftest, panics visibly, and runs at its
true (or safely-degraded) clock. Full record in `M1_state.md`. The items still open below are
either optional perf, deferred to M2, or non-gating HW-unverified notes — none block M1.

Living checklist for **M1** (uniformity / bring-up). Check items off as they land — this file,
not memory, is the source of truth for "where are we". M2 (MPU enforcement) and M3
(capabilities + clock-select) items are parked at the bottom so they aren't lost.

Durable background lives in the auto-memory (`kickos-clock-audit`,
`kickos-panic-console-review`, `m2-m3-roadmap-split`, per-board `*-hw-*`) and in
`docs/m2-readiness.md`; this file is just the actionable status.

## M1 — clocks (fleet audit 2026-07-09; see `kickos-clock-audit` memory)

Every board's timing math is ACCURATE (no ESP32-C6-class constant bug survived the
audit). Remaining work is boards that never raise their PLL, so they run far below
capability and their benchmarks reflect a slow core. Each fix = raise PLL **and**
update `SystemCoreClock` in the same step so the ns↔tick math stays coherent.

- [x] **ESP32-WROOM: PLL bring-up 40 → 240 MHz** — DONE, validated on silicon 2026-07-09.
      6× confirmed by a SystemCoreClock-independent host-wall-clock spin (2203 ms @240 vs
      13020 ms @40); selftest 14/14, console clean at the recomputed 80 MHz-APB baud, 0.4 s
      beat coherent. No BBPLL lock bit on this chip → hardened with a bounded RTC-slow-cycle
      barrier (esp-idf's mechanism) around the power-up + before the source switch.
- [x] **RP2040: PLL_SYS bring-up 12 → 125 MHz** — DONE, validated on FIRST SILICON
      2026-07-09 (the RP2040 port had never run on HW). selftest 14/14 at 125 MHz over
      UART0/GP0; 125 MHz confirmed by a fixed-spin interval (2573 ms/20M = 16 cyc/iter @125,
      physically impossible at 12); XIP survives the clk_sys switch (boot2 SCK=31.25 MHz
      risk resolved — code runs from flash at 125). Watchdog `/12` tick kept on clk_ref=XOSC
      so the 1 MHz TIMER stays correct.
- [x] **SAM3X8E / Arduino Due — port validated on silicon 2026-07-09** (selftest 14/14,
      84 MHz PLL, `-b` GPNVM1 boot-from-flash + physical-RESET flashing flow). Crystal-race
      fix (bounded `pmc_wait` + MOSCXTST margin + RC fallback) landed as part of bring-up.
      **UNIT RETIRED 2026-07-14** (removed from the available-HW list): the physical board
      developed a peripheral-I/O fault — core + flash-controller + native USB (SAM-BA) all
      verified working, but PIO output (PB27 LED) won't toggle and the UART console is dead,
      even under a provably-correct bare-metal blink flashed via two independent paths → HW,
      not KickOS. Likely marginal all along (the MOSCXTST margin is a documented `GUESS`).
      Port stays proven; this unit is not a reliable target. See `docs/boards.md`.
- [x] **XMC4800 120 → 144 MHz** — DONE, validated on silicon 2026-07-09: selftest 14/14
      at 144 over the J-Link VCOM (ttyACM0); 144 confirmed by the spin ratio (1938 ms @144
      vs 2306 ms @120 = 1.19 ≈ 144/120). VCO 288/K2DIV=2; flash WS=4 unchanged (already
      correct); baud recomputed for fPERIPH 72 MHz. 144 was not a hard sweet-spot after
      all: the USB PLL is separate/untouched and WS=4 already covers 144.
- [ ] *(optional perf)* STM32F411 84 → 96/100 — deliberate sweet-spot today; only if we
      want the true ceiling. F302 is HW-capped (Nucleo has no HSE crystal);
      C6/K64F/RX72M/F103 already at max; ESP32/RP2040/XMC now at max (silicon-validated).

## M1 — ESP32-C6

- [x] **Diag-LED (WS2812B on GPIO8) via RMT.** DONE @d76d187 — RMT ch0 (20 MHz tick),
      routed to GPIO8, RGB-ordered (red = 0xFF0000), blinks the panic heartbeat;
      validated on silicon. (Bit-bang was infeasible — GPIO write latency > the bit
      high-time; `rdcycle` traps on the C6.)
- [x] **selftest 10-14 pass/fail on silicon.** DONE — all 14 PASS on silicon. Two real
      bugs fixed: (1) console rerouted from the native USB-Serial-JTAG (never delivers
      app output — CDC host-draining gating + reset re-enumeration) to **UART0**, exposed
      as a stable COM port by the board's **CH343P bridge** (ttyACM0); (2) the inject
      doorbell programmed enable/type/prio/thresh into the **vestigial INTC/INTPRI block
      (0x600C5000)** — the C6's real interrupt controller is the **PLIC (0x2000_1000)**;
      moved the config there and 10-14 deliver. (INTPRI keeps only the FROM_CPU trigger.)
- [x] **PMP NAPOT verified on silicon.** DONE — a locked, no-permission BOUNDED 4 KiB
      NAPOT region correctly took a store-access fault (mcause=7, mtval=page) on the C6.
      So the M2 RISC-V NAPOT track is safe: only the *all-ones whole-space* NAPOT special
      case is unhonored (the M1 bootstrap already avoids it via TOR). Probe was throwaway.

## M1 — hardware validation (batch when units are connected)

- [x] **blackpill** (F411 25 MHz HSE) + **f411disco** (F411 84 MHz) + **f302nucleo** (F302 16 K) +
      **bluepill** (F103 10 K clone) — all HW-validated on silicon 2026-07-14 (blackpill/f411disco
      14/14 + bench; f302/bluepill 13/14, test 11 = RAM-size limit). Only **bluepill-c8** (genuine
      20 K F103) stays build-only — a linker variant of the already-validated F103.
- [ ] **K64F — NOT an M1 gate; sign-off deferred to M2** (decided 2026-07-14). It's M4F/armv7m,
      the same path proven on silicon across XMC/f411disco/blackpill/f302 (4 boards) — redundant
      arch coverage, not new. Its distinguishing feature is the **SYSMPU**, which is the M2
      enforcement track, so K64F gets its formal HW re-confirm there (unit unavailable for a few
      days). Prior silicon run exists (`k64f-hw-baseline`: 120 MHz PLL, switch 77 cyc/641 ns,
      FP-switch 940 rounds) — a deferral, not an untested board.
- [ ] micro:bit / nRF51 as a real silicon target: needs an **RTC-based timer** (the
      nRF51 M0 has no SysTick); today it's a QEMU vehicle only.
- [ ] Panic/console review HW-checklist: RP2040 PL011 `TXRIS`-at-rest with FEN=0;
      ESP32 UART FIFO DPORT-vs-AHB alias; RX72M `SCR.TIE`-while-`TDRE` fires TXI. (All
      flagged HW-unverified in-code.)

## M1 — fleet parity (audit 2026-07-09)

Capability audit across all arch/chip. Fleet is broadly uniform (every arch has a real
console, tickless timer, fault-register dump, inject-driven IRQ path, M2 MPU no-op).
Divergences worth closing for M1, most impactful first:

- [~] **mk64f diag-LED backend ADDED build-only @b5c5665** (RED PTB22 active-low) — code gap
      closed; HW confirm folds into the M2 K64F SYSMPU bring-up (K64F is not an M1 gate, see above).
      **esp32(lx6) DONE** — GPIO2 (silkscreen D2), validated with `blink` on hardware.
- [x] **IRQ default-mask posture unified** — DONE @5da8a38: riscv/xtensa/sim now init their
      mask all-MASKED (matching ARM/RX); the reset contract is documented in `arch.h` (all
      lines masked at reset; a driver unmasks/irq_register-arms before use). Validated:
      selftest 14/14 on sim/qemu/qemu-riscv, no regressions.
- [x] **`arch_console_write_sync` uniformly bounded** — DONE @9fd9623: stm32f103/f302/f411,
      rp2040, mk64f, esp32(lx6), sam3x8e all wrapped their unbounded TX-ready poll in a
      spin-then-drop guard (ceiling ~40-140 ms; esp32 200000). A wedged UART now drops bytes
      instead of hanging the panic path (the Due's solid-LED hang). fault_dump gates confirm
      a drained console still emits the full dump. (esp32c6/rx72m were already bounded.)
- [x] **ESP32-C6 real peripheral-IRQ path + buffered (ring) console — DONE** (@cc4b236,
      silicon-validated). The C6 was inject-doorbell only; added its first real device-interrupt
      path: UART0 TX-empty → interrupt-matrix source (0x600100AC) → a dedicated CPU int (30) →
      `switch.S` `.Lextdev` → `kickos_rv_ext_dispatch_dev` → the console line's ISR. Level source,
      NO PLIC claim (clears by de-assert, like the doorbell). selftest 14/14 over the buffered
      console (2048-byte ring > total output ⇒ proves the ISR drains it), inject path intact.
      *(anytime coherence — was mislabeled "M2"; it's interrupt plumbing, no MPU dependency.)*
- [ ] *(driver-era, anytime — NOT M2)* RX `kickos_rx_default_irq` real-peripheral-IRQ demux —
      still a stub (RXv3, a different arch than the C6, so its own work; same concept). Injected
      lines pass selftest but a real peripheral IRQ drops. The C6 `.Lextdev` design is the riscv
      reference pattern. **When the 2nd real device line lands** (fable review finding 5): the
      arch IRQ mask must reach the controller for real lines — add an `arch_rv_hw_mask` twin (or
      gate `.Lextdev` dispatch on `g_irq_masked` + disable the source), else a tier-1 driver's
      mask-until-ack and the spurious-handler mask silently fail to stop a level source (storm).
      Unreachable today: the C6 console (line 16) is permanently owned + self-gates via INT_ENA.

## M1 — misc

- [x] RX72M `arch_irq_unmask`: replaced the `IPR index == vector` assumption with a
      vector→IPR source table (`vector_to_ipr` + `kIprMap`); IR/IER stay 1:1, only the
      shared IPR is remapped. Byte-identical for the vectors used today (SWINT/CMTW/SCI6),
      so no runtime change now; correct for driver-era device lines. RX72M re-validated on
      silicon 2026-07-09 (selftest 14/14, rfp-cli/E2 Lite flash, SCI6 console on ttyUSB0).

---

## Later — not M1

**Milestones are keyed to their THEME, not sequence** (audit 2026-07-14). **M2 = MPU /
memory-protection enforcement**, specifically. Work that merely follows M1 is not "M2" unless
it needs the MPU — the object-pool refactor, worst-case-ISR-latency perf, `sys_cpu_clock_hz`,
and the real-peripheral-IRQ demux are orthogonal (anytime coherence / M3-substrate), tagged
below where they were previously mislabeled.

- **M2 — MPU enforcement** fan-out: reference pair (RISC-V PMP/NAPOT + XMC v7-M PMSA) →
  K64F SYSMPU → RX → tail; + the arch-independent security model (domains, per-thread
  private stacks, syscall-arg/user-pointer validation, pow2 region placement). See
  `docs/architecture.md` / `docs/m2-readiness.md`.
- **[anytime coherence — NOT M2] object-pool mutualisation** — DONE (step 1). The semaphore
  pool is a generational `SlotPool<T,N>` (slotpool.h); the thread pool is grouped into a
  tailored `ThreadPool` struct (thread.h) — deliberately **not** SlotPool: thread liveness is
  intrinsic (`state==EXITED`) and its generation bumps at *reclaim* (so a future join-by-handle
  can still resolve a just-exited thread), genuinely different from the sem pool, so forcing
  one pool would be false-DRY. Full unification (a shared handle codec across sems + the M3
  capability store) waits for that genuine second SlotPool-shaped case. (No MPU dependency —
  was mislabeled "M2 handle table"; it's the M3-caps substrate + anytime coherence.)
- **M3 — capabilities + authenticated grants** (seL4-principled object model), **and
  user-selectable CPU clock / low-power mode** (needs explicit per-chip clock bring-up
  first, from the audit above).
  - **`sys_cpu_clock_hz()` syscall** — expose the running core clock (Hz) to userspace.
    Today the bench hardwires the kernel's `SystemCoreClock` for its cycle→ns math; a
    read syscall lets any app convert cycle counts / interpret timings itself, and is the
    natural read-side precursor to the user clock-select above. (Small; not an M1 blocker.)
- **[anytime perf — NOT M2] worst-case ISR latency (shorten interrupt-masked critical
  sections).** Scheduler/switch-path timing, gated on a worst-case-latency probe — no MPU
  dependency (was mislabeled "M2"). The uniform bench surfaced that under sustained syscall
  load the kernel spends too long masked. Ranked plan (see `M1_state.md` §3.1):
  - [x] **R2** — armv7m: skip the redundant BASEPRI raise + DSB/ISB on nested IrqLocks
        (only the outer raise needs them). Landed `5ba57fd`. Correct (ctests green) but
        **below the current bench's noise floor** — see the measurement gap below.
  - [ ] **R1** — thread a single `now` through switch_to->ktime_rearm->arch_timer_arm +
        arm_slice (kills the 3x arch_clock_now pileup per RR switch; on RX each is a
        nested lock + two 64-bit divides). Cross-arch signature change.
  - [ ] **R3** — fold the min-delta clock read past arch_timer_arm's idempotency guard
        (so an unchanged-deadline re-arm reads the clock zero times). Combine with R1.
        R3b: add the idempotent-arm guard to xtensa.
  - [ ] **R6** — xtensa: its cooperative switch runs INLINE under RSIL (masked), unlike
        the 4 other arches that defer the register save/restore to an unmasked handler.
        The one structural outlier; **high risk** (touches windowed-switch atomicity).
  - **Measurement gap (do first):** the current bench measures throughput + *best-case*
    IRQ entry (reporter injects while uncontended), NOT masked-span delay — so R1/R2/R6
    are not demonstrable with it. Need a worst-case-ISR-latency probe (inject while a
    masked syscall span is in flight) to justify + validate these before landing R1/R6.
  - Note: the earlier **bench self-report starvation is already FIXED** by the
    reporter-as-root/woken-by-workload redesign (not a timer sleep).
- **Driver era (12b)** — userspace UART/console driver takes the peripheral as a
  capability; kernel relinquishes it (`console_tx_deinit`), panic path moves to a
  kernel-retained transport. See `docs/console.md` "Future".
- **M4 — SMP (one kernel image across cores; NOT two AMP instances).** Motivation: run the
  dual-core RP2040 (picopi) at 100% under a single KickOS. Biggest architectural axis on the
  roadmap — it reworks the *foundation*, not a feature: the whole kernel's mutual exclusion is
  `IrqLock == arch_irq_save` ("interrupts off ⇒ exclusive"), which is a single-core-only
  guarantee (masking IRQs on one core does nothing to another). Plan:
  - **Step 1 — Big Kernel Lock.** Redefine `IrqLock` as "disable *local* interrupts + take one
    global spinlock." Centralised, so it's a redefinition of one class, not a 200-site audit;
    every existing critical section keeps working, kernel is SMP-*correct* (coarsely). For a
    2-core MCU this likely already gives ~2× (user threads run concurrently; only syscalls
    serialise on the BKL). Per-core run-queues + finer locks come later as *optimisation*.
  - **RP2040 specifics:** M0+ has **no atomics** (no LDREX/STREX) → use the **SIO hardware
    spinlocks** (32 in the SIO block) for the lock; launch core 1 via bootrom/SIO-FIFO
    (`chip_rp2040.cc` already notes the core-1 milestone + the single-core `TIMELR/TIMEHR`
    latch); per-core SysTick + per-core tickless state.
  - **Already seam-ready:** the `KICKOS_*_BARRIER` publish seams (console_tx / rtt) are the
    fence-injection points — flip to real fences on the SMP build. Keep centralising `IrqLock`,
    structs-over-globals, no ad-hoc masking → keeps this a redefinition, not a rewrite.
  - Fits the seL4 endgame (seL4 ships a big-lock SMP variant). See `kickos-smp-roadmap` memory.
