<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS TODO

**M1 VALIDATION COMPLETE (2026-07-14)** -- 10 boards on silicon (5 ISAs) + 3 emulator gates
green; every board boots, has a console, runs the selftest, panics visibly, and runs at its
true (or safely-degraded) clock. Full record in `M1_state.md`. The items still open below are
either optional perf, deferred to M2, or non-gating HW-unverified notes -- none block M1.

Living checklist for **M1** (uniformity / bring-up). Check items off as they land -- this file,
not memory, is the source of truth for "where are we". M2 (MPU enforcement) and M3
(capabilities + clock-select) items are parked at the bottom so they aren't lost.

This file is the **granular, actionable** status. The milestone-level plan (the general idea
per milestone) is `roadmap.md`; validated end-state + per-board detail is `M1_state.md`; the
board/console readiness matrix is `docs/m2-readiness.md`.

## M3 -- landed so far (2026-07-20)
- [x] `sys_cpu_clock_hz()` read syscall; [x] per-task capability handle table (sem ABI) +
      authenticated-grant delegation; [x] priority-inheritance mutex (CAP_MUTEX). All on master,
      silicon-validated UNDER ENFORCEMENT on K64F (SYSMPU) + C6 (PMP). Design in Book ch.8.1 +
      `reference/architecture.md`.

Remaining M3 (to finish the milestone) -- gated flow (fable design review -> branch -> silicon):
- [ ] **Writable user-pointer bound-check** at the syscall boundary (arch-neutral). Prerequisite:
      a recv into an unchecked out-buffer is a privileged write oracle. (See the parked syscall-arg
      validation item lower in this file.)
- [ ] **Endpoint/IPC object (CAP_ENDPOINT)** -- additive per `docs/design-m3-substrate.md`:
      `SlotPool<Endpoint,N>` + `endpoint_refs` + one `cap_resolve` case + obj_ref_inc/drop and
      obj_close_protocol (EPIPE-wake) arms; synchronous rendezvous, kernel-copied bounded payload,
      parks on the shared `wq_block`/`wq_pop_highest` primitive; badging object-side; send/recv
      syscalls (recv gated on the bound-check above). CapEntry stays frozen (CAP_ENDPOINT reserved).
- [ ] **Console device handover** (`docs/design-m3-console-handover.md`) -- a userspace UART driver
      takes the console as a capability. Publish-BEFORE-spawn: `console_tx_deinit` under one IrqLock
      (state=USER_OWNED set last), THEN spawn the driver with the MMIO grant + a console endpoint.
      Seat a stdout endpoint cap at index 0 (this makes `cap_install_defaults` / the low-barrier #5
      real); `_write` probes `kos_send(0)` then falls back to `kconsole_write`.
- [ ] **Panic-path console reclaim** -- kernel re-seizes + re-inits the UART on panic
      (`arch_console_reclaim` per chip); driver-death = EPIPE-wake parked senders + root respawn/
      re-publish (NO kernel auto-adoption). Needs a scramble-then-force-panic HW test per backend
      (a wrong reclaim looks exactly like silent panic loss -- the worst failure mode).
- [ ] **User-selectable CPU clock / low-power mode** -- per-chip clock-select syscall; keep the
      scheduler ns<->tick math coherent across a clock change (the clock-hardening timers make `now`
      trustworthy for this). Read side (`sys_cpu_clock_hz`) already landed.
Silicon target for the handover: the CPU-side-MPU boards (XMC/RX/C6) where per-thread peripheral
isolation is real; K64F is coarse-AIPS (documentation, not enforcement).

## Clock hardening (2026-07-20) -- clock off the debug-domain / narrow counters
Root cause: v7-M `arch_clock_now` used DWT_CYCCNT (core DEBUG power domain), sw-extended 32->64.
On K64F+XMC silicon DWT intermittently returns aliased garbage -> phantom 2^32 wrap -> clock
leaps ~35 s -> every timed wait strands (intermittent ~50-75%, silicon-only). Masqueraded as a
"test-5 stall" and invalidated this session's earlier single-run silicon claims. Fragility class
= narrow counter + sw wrap-extension (fails via a bad read OR a missed wrap). Fix = a wide,
reliably-readable, NON-debug free-running peripheral counter. Book ch.2.1 teaches it.
- [x] **K64F** 64-bit PIT -- SILICON 20/20 (+ mutex 10/10, under enforcement).
- [x] **XMC4800** 64-bit CCU4 (4 slices concat) -- SILICON 18/18; fixed fCCU WFI-gating (SLEEPCR).
- [~] **F411/F302** TIM2(32b), **F103** TIM2->TIM3 chained, **SAM3X** TC0 ch0(32b) -- on master,
      reviewed+fixed (f103 tear-discriminator; per-timer overflow-IRQ wrap observer; f411 APB1LPENR).
      **BUILD-ONLY, SILICON PENDING.**
- [~] **ESP32 (Xtensa)** 64-bit TIMG0 (UPDATE-latch) -- also fixes a latent CCOUNT WAITI-freeze.
      **BUILD-ONLY, SILICON PENDING.**
- RISC-V (CLINT mtime) + RX (CMTW): already sound, unchanged.

Silicon-test-later (fleet+Xtensa; `.session/*-clock*.patch` are backups):
1. idle-wrap observer: quiescent > 1 wrap period (51/67/59/102 s) -> clock still correct.
2. f103: soak across chain wraps -> no +59.6 s leap, no backward stall.
3. rate/monotonicity vs wall clock (2x error = wrong Hz); no backward step under IRQ load.
4. WFI keeps counting (f411 APB1LPENR; sam3x FSMR Sleep-not-Wait; Xtensa TIMG UPDATE-latch settle +
   DPORT ungate assumption -- the two things unverifiable build-only).
5. overflow lands in the chip clock ISR (NVIC TIM2=28/TIM3=29/TC0=27, RM-sourced).
6. debug-halt > 1 wrap period loses a wrap (DBGMCU freeze unset) -- bench artifact, not a bug.

Clock follow-ups (not blocking): arch_trace_now + KICKOS_BENCH still read raw DWT/CCOUNT (telemetry
may glitch on K64F/XMC -- tolerable, NOT the scheduler clock); ticks->ns epilogue duplicated ~7x
(hoist an arch/arm/common helper). ENV (this box): sim/kickcat_slave build needs `tinyxml2`
reinstalled -- a 2026-07-20 system change dropped it; unrelated to KickOS.

## M1 -- clocks (fleet audit 2026-07-09; detail in `M1_state.md`)

Every board's timing math is ACCURATE (no ESP32-C6-class constant bug survived the
audit). Remaining work is boards that never raise their PLL, so they run far below
capability and their benchmarks reflect a slow core. Each fix = raise PLL **and**
update `SystemCoreClock` in the same step so the ns<->tick math stays coherent.

- [x] **ESP32-WROOM: PLL bring-up 40 -> 240 MHz** -- DONE, validated on silicon 2026-07-09.
      6x confirmed by a SystemCoreClock-independent host-wall-clock spin (2203 ms @240 vs
      13020 ms @40); selftest 14/14, console clean at the recomputed 80 MHz-APB baud, 0.4 s
      beat coherent. No BBPLL lock bit on this chip -> hardened with a bounded RTC-slow-cycle
      barrier (esp-idf's mechanism) around the power-up + before the source switch.
- [x] **RP2040: PLL_SYS bring-up 12 -> 125 MHz** -- DONE, validated on FIRST SILICON
      2026-07-09 (the RP2040 port had never run on HW). selftest 14/14 at 125 MHz over
      UART0/GP0; 125 MHz confirmed by a fixed-spin interval (2573 ms/20M = 16 cyc/iter @125,
      physically impossible at 12); XIP survives the clk_sys switch (boot2 SCK=31.25 MHz
      risk resolved -- code runs from flash at 125). Watchdog `/12` tick kept on clk_ref=XOSC
      so the 1 MHz TIMER stays correct.
- [x] **SAM3X8E / Arduino Due -- port validated on silicon 2026-07-09** (selftest 14/14,
      84 MHz PLL, `-b` GPNVM1 boot-from-flash + physical-RESET flashing flow). Crystal-race
      fix (bounded `pmc_wait` + MOSCXTST margin + RC fallback) landed as part of bring-up.
      **UNIT RETIRED 2026-07-14** (removed from the available-HW list): the physical board
      developed a peripheral-I/O fault -- core + flash-controller + native USB (SAM-BA) all
      verified working, but PIO output (PB27 LED) won't toggle and the UART console is dead,
      even under a provably-correct bare-metal blink flashed via two independent paths -> HW,
      not KickOS. Likely marginal all along (the MOSCXTST margin is a documented `GUESS`).
      Port stays proven; this unit is not a reliable target. See `docs/reference/boards.md`.
- [x] **XMC4800 120 -> 144 MHz** -- DONE, validated on silicon 2026-07-09: selftest 14/14
      at 144 over the J-Link VCOM (ttyACM0); 144 confirmed by the spin ratio (1938 ms @144
      vs 2306 ms @120 = 1.19 ~ 144/120). VCO 288/K2DIV=2; flash WS=4 unchanged (already
      correct); baud recomputed for fPERIPH 72 MHz. 144 was not a hard sweet-spot after
      all: the USB PLL is separate/untouched and WS=4 already covers 144.
- [ ] *(optional perf)* STM32F411 84 -> 96/100 -- deliberate sweet-spot today; only if we
      want the true ceiling. F302 is HW-capped (Nucleo has no HSE crystal);
      C6/K64F/RX72M/F103 already at max; ESP32/RP2040/XMC now at max (silicon-validated).

## M1 -- ESP32-C6

- [x] **Diag-LED (WS2812B on GPIO8) via RMT.** DONE @d76d187 -- RMT ch0 (20 MHz tick),
      routed to GPIO8, RGB-ordered (red = 0xFF0000), blinks the panic heartbeat;
      validated on silicon. (Bit-bang was infeasible -- GPIO write latency > the bit
      high-time; `rdcycle` traps on the C6.)
- [x] **selftest 10-14 pass/fail on silicon.** DONE -- all 14 PASS on silicon. Two real
      bugs fixed: (1) console rerouted from the native USB-Serial-JTAG (never delivers
      app output -- CDC host-draining gating + reset re-enumeration) to **UART0**, exposed
      as a stable COM port by the board's **CH343P bridge** (ttyACM0); (2) the inject
      doorbell programmed enable/type/prio/thresh into the **vestigial INTC/INTPRI block
      (0x600C5000)** -- the C6's real interrupt controller is the **PLIC (0x2000_1000)**;
      moved the config there and 10-14 deliver. (INTPRI keeps only the FROM_CPU trigger.)
- [x] **PMP NAPOT verified on silicon.** DONE -- a locked, no-permission BOUNDED 4 KiB
      NAPOT region correctly took a store-access fault (mcause=7, mtval=page) on the C6.
      So the M2 RISC-V NAPOT track is safe: only the *all-ones whole-space* NAPOT special
      case is unhonored (the M1 bootstrap already avoids it via TOR). Probe was throwaway.

## M1 -- hardware validation (batch when units are connected)

- [x] **blackpill** (F411 25 MHz HSE) + **f411disco** (F411 84 MHz) + **f302nucleo** (F302 16 K) +
      **bluepill** (F103 10 K clone) -- all HW-validated on silicon 2026-07-14 (blackpill/f411disco
      14/14 + bench; f302/bluepill 13/14, test 11 = RAM-size limit). Only **bluepill-c8** (genuine
      20 K F103) stays build-only -- a linker variant of the already-validated F103.
- [x] **K64F revalidated on silicon 2026-07-15** (OpenSDA/J-Link): full selftest streamed
      in-order over the buffered console ring; bench re-confirmed 77 cyc / 641 ns switch (=> 120
      MHz), 160 cyc / 1333 ns IRQ-entry; fault-dump verified (UsageFault UNDEFINSTR -> HardFault).
      Its distinguishing feature -- the **SYSMPU** -- is the M2 enforcement backend, so K64F's
      formal M2 sign-off (per-task MPU trap) still lands there. Not an M1 gate; M1 was never a hole.
- micro:bit / nRF51 -- **QEMU-only; silicon bring-up not planned.** The nRF51 is discontinued
      (no silicon obtainable), so it stays an armv6m QEMU vehicle (`-M microbit`). A real-silicon
      port would also have needed an **RTC-based timer** (the nRF51 M0 has no SysTick).
- [ ] Panic/console review HW-checklist: RP2040 PL011 `TXRIS`-at-rest with FEN=0;
      ESP32 UART FIFO DPORT-vs-AHB alias; RX72M `SCR.TIE`-while-`TDRE` fires TXI. (All
      flagged HW-unverified in-code.)

## M1 -- fleet parity (audit 2026-07-09)

Capability audit across all arch/chip. Fleet is broadly uniform (every arch has a real
console, tickless timer, fault-register dump, inject-driven IRQ path, M2 MPU no-op).
Divergences worth closing for M1, most impactful first:

- [~] **mk64f diag-LED backend ADDED build-only @b5c5665** (RED PTB22 active-low) -- code gap
      closed; HW confirm folds into the M2 K64F SYSMPU bring-up (K64F is not an M1 gate, see above).
      **esp32(lx6) DONE** -- GPIO2 (silkscreen D2), validated with `blink` on hardware.
- [x] **IRQ default-mask posture unified** -- DONE @5da8a38: riscv/xtensa/sim now init their
      mask all-MASKED (matching ARM/RX); the reset contract is documented in `arch.h` (all
      lines masked at reset; a driver unmasks/irq_register-arms before use). Validated:
      selftest 14/14 on sim/qemu/qemu-riscv, no regressions.
- [x] **`arch_console_write_sync` uniformly bounded** -- DONE @9fd9623: stm32f103/f302/f411,
      rp2040, mk64f, esp32(lx6), sam3x8e all wrapped their unbounded TX-ready poll in a
      spin-then-drop guard (ceiling ~40-140 ms; esp32 200000). A wedged UART now drops bytes
      instead of hanging the panic path (the Due's solid-LED hang). fault_dump gates confirm
      a drained console still emits the full dump. (esp32c6/rx72m were already bounded.)
- [x] **ESP32-C6 real peripheral-IRQ path + buffered (ring) console -- DONE** (@cc4b236,
      silicon-validated). The C6 was inject-doorbell only; added its first real device-interrupt
      path: UART0 TX-empty -> interrupt-matrix source (0x600100AC) -> a dedicated CPU int (30) ->
      `switch.S` `.Lextdev` -> `kickos_rv_ext_dispatch_dev` -> the console line's ISR. Level source,
      NO PLIC claim (clears by de-assert, like the doorbell). selftest 14/14 over the buffered
      console (2048-byte ring > total output => proves the ISR drains it), inject path intact.
      *(anytime coherence -- was mislabeled "M2"; it's interrupt plumbing, no MPU dependency.)*
- [ ] *(driver-era, anytime -- NOT M2)* RX `kickos_rx_default_irq` real-peripheral-IRQ demux --
      still a stub (RXv3, a different arch than the C6, so its own work; same concept). Injected
      lines pass selftest but a real peripheral IRQ drops. The C6 `.Lextdev` design is the riscv
      reference pattern. **When the 2nd real device line lands** (fable review finding 5): the
      arch IRQ mask must reach the controller for real lines -- add an `arch_rv_hw_mask` twin (or
      gate `.Lextdev` dispatch on `g_irq_masked` + disable the source), else a tier-1 driver's
      mask-until-ack and the spurious-handler mask silently fail to stop a level source (storm).
      Unreachable today: the C6 console (line 16) is permanently owned + self-gates via INT_ENA.

## M1 -- misc

- [x] RX72M `arch_irq_unmask`: replaced the `IPR index == vector` assumption with a
      vector->IPR source table (`vector_to_ipr` + `kIprMap`); IR/IER stay 1:1, only the
      shared IPR is remapped. Byte-identical for the vectors used today (SWINT/CMTW/SCI6),
      so no runtime change now; correct for driver-era device lines. RX72M re-validated on
      silicon 2026-07-09 (selftest 14/14, rfp-cli/E2 Lite flash, SCI6 console on ttyUSB0).
- [ ] *(dev ergonomics, small)* **debug-in-sleep**: set `DBGMCU` `DBG_SLEEP`/`DBG_STOP` under a
      `KICKOS_DEBUG` gate so SWD survives the idle `WFI` (no connect-under-reset dance to reflash
      a running board). A per-chip one-liner in `arch_init`.

---

## Later -- not M1

**Milestones are keyed to their THEME, not sequence** (audit 2026-07-14). **M2 = MPU /
memory-protection enforcement**, specifically. Work that merely follows M1 is not "M2" unless
it needs the MPU -- the object-pool refactor, worst-case-ISR-latency perf, `sys_cpu_clock_hz`,
and the real-peripheral-IRQ demux are orthogonal (anytime coherence / M3-substrate), tagged
below where they were previously mislabeled.

- **M2 -- MPU enforcement** fan-out: reference pair (RISC-V PMP/NAPOT + XMC v7-M PMSA) ->
  K64F SYSMPU -> RX -> tail; + the arch-independent security model (domains, per-thread
  private stacks, syscall-arg/user-pointer validation, pow2 region placement). See
  `docs/reference/architecture.md` / `docs/m2-readiness.md`.
- **Driver era -- unprivileged MMIO drivers + peripheral-isolation ceiling** (needs the M2
  grant seam; the drivers themselves are anytime coherence). Status in `docs/m2-readiness.md`
  (Driver era subsection) + the fleet peripheral-isolation matrix in
  `docs/reference/architecture.md`.
  - [x] **MMIO-grant mechanism (task #9)** -- DONE + committed 2026-07-16.
        `kos_thread_params.mmio_base/mmio_size` (grant-at-spawn), the
        `arch_mpu_region_encodable` arch seam (exact-cover, no rounding), privileged-only
        `thread_spawn` validation, `domain_for` appends MMIO as a never-shared capability.
        PLUS a Critical fix: an unprivileged `mem_base` grant is now arena-bounds-checked
        (closed a peripheral/kernel-SRAM self-grant escalation). See `docs/design-task9-mmio-driver.md`.
  - [x] **K64F first unprivileged driver (k64drv, PIT)** -- DONE on silicon 2026-07-16;
        added the weak `arch_fault_report_extra` hook (K64F decodes SYSMPU CESR/EARn/EDRn).
  - [x] **SYSMPU peripheral-gating question -- ANSWERED on silicon 2026-07-16:** SYSMPU does
        NOT gate AIPS peripheral-bridge accesses under user mode; the AIPS bridge PACR does
        (per privilege+master, per 4 KB slot, NOT per-thread). So **per-thread peripheral
        isolation is IMPOSSIBLE on K64F**; it holds on the CPU-side-MPU chips (XMC PMSA,
        RISC-V PMP, RX MPU). Hardware-ceiling docs DONE (`reference/architecture.md` matrix +
        `book/peripheral-isolation-and-the-hardware-ceiling.md`).
  - [~] **F411 canonical per-thread PMSA driver (f411spi, SPI1 loopback)** -- BUILT +
        fable-reviewed; **silicon-validation PENDING** a bench swap to the 32F411E-DISCO. It
        first-proves granted-SPI-works AND ungranted-peripheral-faults per thread on PMSA
        silicon -- the fleet's one honest peripheral-isolation gap. `docs/design-spi-driver-stm32f411.md`.
  - [x] **K64F/DSPI driver (k64dspi, DSPI0 for the KickCAT ESC SPI PDI)** -- DONE on silicon:
        the polled-FIFO transport (~10 MHz) reached OPERATIONAL against a real LAN9252. Exported
        as the `kickos_k64dspi` lib (`<kickos/driver/k64dspi.h>`, source `user/driver/k64dspi`)
        so an out-of-tree consumer links it. Within the K64F coarse-peripheral ceiling (window
        grant is documentation, not enforcement); microkernel invariant kept (driver in userspace).
  - [x] **C6 PMP SRAM enforcement DONE on silicon** (18/18 selftest under enforcement +
        mpu_fault cross-domain trap, 2026-07-17) -- the earlier blockers (all-SRAM image /
        gp-relative small-data / code-from-RAM) were resolved. REMAINING (peripheral side,
        follow-on -- NOT needed for SRAM enforcement): a **separate APM/PMS bus permission
        unit** defaults deny-user on peripheral targets and needs a one-time global open (not
        per-thread) on top of the PMP grant before a C6 userspace driver reaches a peripheral.
        See the C6 row in `docs/m2-readiness.md` + `docs/design-c6-driver.md`.
  - [x] **RX72M MPU DONE on silicon** (selftest 20/20 under enforcement + mpu_fault
        cross-domain trap, 2026-07-17). REMAINING: m2-review-followup #5 (RX rounds
        misaligned regions instead of skipping -- fail-closed drift, build-robustness).
        See `docs/m2-review-followups.md`.
- **[anytime coherence -- NOT M2] object-pool mutualisation** -- DONE (step 1). The semaphore
  pool is a generational `SlotPool<T,N>` (slotpool.h); the thread pool is grouped into a
  tailored `ThreadPool` struct (thread.h) -- deliberately **not** SlotPool: thread liveness is
  intrinsic (`state==EXITED`) and its generation bumps at *reclaim* (so a future join-by-handle
  can still resolve a just-exited thread), genuinely different from the sem pool, so forcing
  one pool would be false-DRY. Full unification (a shared handle codec across sems + the M3
  capability store) waits for that genuine second SlotPool-shaped case. (No MPU dependency --
  was mislabeled "M2 handle table"; it's the M3-caps substrate + anytime coherence.)
- **[anytime coherence -- NOT M2] general freeing allocator (M4).** `arch_ram_alloc` is a
  wholesale bump allocator (freed only at reset). Default thread stacks now reclaim via a
  single-size-class intrusive free list in `ThreadPool` (thread.h) -- the special case that needs
  no size metadata (one class == `KICKOS_USER_STACK_SIZE`, link stored in the dead block). A
  GENERAL multi-size-class freeing allocator for `arch_ram_alloc`/`kos_ram_alloc` at large is M4;
  it would subsume this free list. Until then, only default stacks are reclaimable.
- **[anytime coherence -- NOT M2] user-pointer validation at the syscall boundary.** M2 is MPU
  *enforcement*; validating a user pointer is arch-neutral kernel logic that matters MORE at M1
  (no MPU to contain an OOB access -- see the `user-args-validated-at-boundary` invariant).
  Cheap parts DONE (fable code review): thread name copied into a bounded TCB buffer (fault path
  never derefs/`%s` a user pointer); `clock_now` out-pointer null+8-byte-alignment checked;
  `thread_spawn` stack `base+size` wrap checked; `SlotPool::resolve` rejects a dirty handle top
  byte. Remaining: copy-in the `kos_thread_params` struct via a checked read, and bound-check
  writable out-pointers (`clock_now`) + the `write()` buffer against the caller's granted region
  -- this last part wants the M1 region-ownership model pinned (privileged = whole arena,
  unprivileged = `mem_base`) so it rejects bad pointers without rejecting legit threads.
- **M3 -- capabilities + authenticated grants** (seL4-principled object model), **and
  user-selectable CPU clock / low-power mode** (needs explicit per-chip clock bring-up
  first, from the audit above).
  - [x] **Per-task capability handle table (sem ABI: global ids -> per-task caps)** -- DONE,
        silicon-validated under enforcement on ALL FOUR M2 mechanism classes: K64F SYSMPU,
        XMC4800 PMSA, RX72M RX-MPU, ESP32-C6 PMP -- each 21/21 selftest under enforcement
        (incl. domain_share / mmio_grant / confused_deputy + the close-while-parked sem test).
        `CapEntry` table embedded in the TCB (`cap.h`), single `cap_resolve` chokepoint
        (per-task cap-gen then global object-gen), rights WAIT/SIGNAL/TRANSFER each enforced at a
        real site, refcounted destroy-on-last-close, `KOS_SYS_handle_close` (renamed from
        `sem_destroy`), authenticated-grant spawn delegation (subset-only rights narrowing,
        validate-before-claim, B1 handle==index-on-a-fresh-table deterministic placement).
        Reference: `docs/reference/architecture.md` + `invariants.md`; teaching: `docs/book` ch 8.1.
  - [x] **`sys_cpu_clock_hz()` syscall** -- DONE @638620d, already on master (build+sim/qemu verified). Read-only
    `KOS_SYS_cpu_clock_hz` via the `arch_cpu_clock_hz()` seam (mirrors `clock_now`), value
    returned in-register (no out-pointer), each backend reuses its CMSIS `SystemCoreClock`;
    sim returns 0. selftest `t_cpu_clock_hz` covers both branches; all 5 ISAs + sim build,
    runtime green on sim/armv7m/rv32imac. Read-side precursor to user clock-select below.
- **[anytime perf -- NOT M2] worst-case ISR latency (shorten interrupt-masked critical
  sections).** Scheduler/switch-path timing, gated on a worst-case-latency probe -- no MPU
  dependency (was mislabeled "M2"). The uniform bench surfaced that under sustained syscall
  load the kernel spends too long masked. Ranked plan (see `M1_state.md` section 3.1):
  - [x] **R2** -- armv7m: skip the redundant BASEPRI raise + DSB/ISB on nested IrqLocks
        (only the outer raise needs them). Landed `5ba57fd`. Correct (ctests green) but
        **below the current bench's noise floor** -- see the measurement gap below.
  - [ ] **R1** -- thread a single `now` through switch_to->ktime_rearm->arch_timer_arm +
        arm_slice (kills the 3x arch_clock_now pileup per RR switch; on RX each is a
        nested lock + two 64-bit divides). Cross-arch signature change.
  - [ ] **R3** -- fold the min-delta clock read past arch_timer_arm's idempotency guard
        (so an unchanged-deadline re-arm reads the clock zero times). Combine with R1.
        R3b: add the idempotent-arm guard to xtensa.
  - [ ] **R6** -- xtensa: its cooperative switch runs INLINE under RSIL (masked), unlike
        the 4 other arches that defer the register save/restore to an unmasked handler.
        The one structural outlier; **high risk** (touches windowed-switch atomicity).
  - **Measurement gap (do first):** the current bench measures throughput + *best-case*
    IRQ entry (reporter injects while uncontended), NOT masked-span delay -- so R1/R2/R6
    are not demonstrable with it. Need a worst-case-ISR-latency probe (inject while a
    masked syscall span is in flight) to justify + validate these before landing R1/R6.
  - Note: the earlier **bench self-report starvation is already FIXED** by the
    reporter-as-root/woken-by-workload redesign (not a timer sleep).
- **Console device handover (driver era)** -- userspace UART/console driver takes the
  peripheral as a capability; kernel relinquishes it (`console_tx_deinit`), panic path moves
  to a kernel-retained transport. See `docs/reference/console.md` "Future".
- **M4 -- multicore (AMP first on RP2040, SMP-BKL endgame on RP2350).** Design spikes
  2026-07-19: `docs/design-multicore.md` (AMP-vs-SMP feasibility on rp2040 + rp2350) and
  `docs/design-multicore-ipc.md` (the RP2040 cross-core IPC). The spike REVISED the earlier
  "SMP-only, NOT AMP" call below: ARMv6-M (M0+) has no atomics (no LDREX/STREX; the SIO bus is
  non-atomic too), so RP2040 SMP is capped at coarse Big-Kernel-Lock forever -- AMP (two
  core-private kernels + IPC) is the better FIRST step there, and fine-grained lock-free SMP is
  reachable only on RP2350 (M33 exclusives / Hazard3 A-ext). AMP + IPC and the invariant
  refactors are the near-term items; the SMP-BKL plan (one kernel image across cores) stays the
  endgame. Motivation: run the
  dual-core RP2040 (picopi) at 100% under a single KickOS. Biggest architectural axis on the
  roadmap -- it reworks the *foundation*, not a feature: the whole kernel's mutual exclusion is
  `IrqLock == arch_irq_save` ("interrupts off => exclusive"), which is a single-core-only
  guarantee (masking IRQs on one core does nothing to another). Plan:
  - **Step 1 -- Big Kernel Lock.** Redefine `IrqLock` as "disable *local* interrupts + take one
    global spinlock." Centralised, so it's a redefinition of one class, not a 200-site audit;
    every existing critical section keeps working, kernel is SMP-*correct* (coarsely). For a
    2-core MCU this likely already gives ~2x (user threads run concurrently; only syscalls
    serialise on the BKL). Per-core run-queues + finer locks come later as *optimisation*.
  - **RP2040 specifics:** M0+ has **no atomics** (no LDREX/STREX) -> use the **SIO hardware
    spinlocks** (32 in the SIO block) for the lock; launch core 1 via bootrom/SIO-FIFO
    (`chip_rp2040.cc` already notes the core-1 milestone + the single-core `TIMELR/TIMEHR`
    latch); per-core SysTick + per-core tickless state.
  - **Already seam-ready:** the `KICKOS_*_BARRIER` publish seams (console_tx / rtt) are the
    fence-injection points -- flip to real fences on the SMP build. Keep centralising `IrqLock`,
    structs-over-globals, no ad-hoc masking -> keeps this a redefinition, not a rewrite.
  - Fits the seL4 endgame (seL4 ships a big-lock SMP variant). See `roadmap.md` (M4).
  - **AMP-first on RP2040 (spike verdict, the recommended near-term step).** Two core-private
    `Kernel` instances -- the `KICKOS_MULTI_INSTANCE` per-instance seam (`instance.h:89`, built
    for the KickCAT multi-slave sim) is the ~80% substrate; re-key it on SIO CPUID instead of
    host-TLS. Each core keeps its own run queue + `IrqLock`==PRIMASK, so NO mutual-exclusion
    refactor: AMP de-risks the shared mechanics (core-1 launch, IPC, console arbitration) that
    SMP also needs, and sidesteps the no-atomics problem entirely.
  - **Cross-core IPC -- required for AMP; none exists today** (`Semaphore`/`Mutex` are intra-core
    only). Design in `docs/design-multicore-ipc.md`: a per-direction SPSC ring in a shared-SRAM
    window (one writer per index + `DMB` ordering -> no lock, no atomics needed on M0+) with the
    SIO 8x32 FIFO used only as a doorbell (write a tag, raise `SIO_IRQ_PROCn`). API = a `Channel`
    (ring + a `Semaphore` in the receiver's kernel) exposed as `KOS_SYS_chan_{open,send,recv}`;
    blocking `recv` parks on the local run queue via `sem_wait`, the peer's SIO ISR drains + wakes
    via the already-ISR-safe `sem_post`. New arch surface is small: `arch_cpu_id`, `arch_dmb`, and
    an `arch_ipc_notify`/`arch_ipc_drain` doorbell pair (so RP2350 SIO-v2 doorbells back the same
    API). The one genuinely-new isolation decision: a fixed `.shared_ipc` region (pow2 for PMSA)
    granted R|W in BOTH cores' MPU sets -- the ONLY cross-core-writable memory; everything else
    stays per-core-private, preserving the per-core-MPU isolation the AMP verdict rests on.
  - **Three single-core invariants to refactor (either path)** -- `IrqLock`==PRIMASK (local-only
    masking; -> BKL or per-core), the single global current-thread/run-queue (per-CPU), and the
    unsynchronised console + boot-on-one-core + single `arch_mpu_apply`. The arch globals
    `g_arch_current`/`g_arch_next` (+ rv32imac `g_isr_depth`/`g_clint_msip`) are the shared
    prerequisite that gates even AMP.
