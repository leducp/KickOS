<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS TODO

Living checklist of what's left for **M1** (the current uniformity / bring-up
milestone: every board boots, has a console, runs the selftest, panics visibly, and
runs at its true clock). Check items off as they land — this file, not memory, is the
source of truth for "where are we". M2 (MPU enforcement) and M3 (capabilities +
clock-select) items are parked at the bottom so they aren't lost.

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
- [x] **SAM3X8E / Arduino Due — WORKING on silicon 2026-07-09** (selftest 14/14, console
      alive, 84 MHz PLL). The "dead console / experimental" status was NOT the crystal or a
      HW fault: the board kept booting the ROM SAM-BA monitor because flashing used a soft
      reset. Two fixes: (1) `flash-bossac.sh` now passes `-b` (set GPNVM1 boot-from-flash);
      (2) the SAM3X latches boot mode at NRST/power-on — press RESET / power-cycle after
      flashing (a soft `-R` isn't enough). Crystal-race fix (bounded `pmc_wait` + MOSCXTST
      margin + RC fallback) also landed as part of the bring-up. Validated via J-Link SWD.
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

- [ ] Build-only boards still needing silicon: STM32F411/F103/F302, **K64F** — LED + console
      + selftest (`<board>-st` presets exist). (RP2040 ✓, SAM3X ✓, ESP32/C6 ✓, XMC ✓, RX72M ✓
      all validated on HW 2026-07-09.)
- [ ] micro:bit / nRF51 as a real silicon target: needs an **RTC-based timer** (the
      nRF51 M0 has no SysTick); today it's a QEMU vehicle only.
- [ ] Panic/console review HW-checklist: RP2040 PL011 `TXRIS`-at-rest with FEN=0;
      ESP32 UART FIFO DPORT-vs-AHB alias; RX72M `SCR.TIE`-while-`TDRE` fires TXI. (All
      flagged HW-unverified in-code.)

## M1 — fleet parity (audit 2026-07-09)

Capability audit across all arch/chip. Fleet is broadly uniform (every arch has a real
console, tickless timer, fault-register dump, inject-driven IRQ path, M2 MPU no-op).
Divergences worth closing for M1, most impactful first:

- [~] **mk64f diag-LED backend ADDED build-only @b5c5665** (RED PTB22 active-low) — closes
      the code gap; still `[ ]` only because it needs an FRDM-K64F to confirm the LED lights.
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
- [ ] *(driver-era)* RX `kickos_rx_default_irq` is a stub while ARM/riscv/xtensa demux real
      device lines; injected lines work (selftest passes) but a real peripheral IRQ drops.

## M1 — misc

- [x] RX72M `arch_irq_unmask`: replaced the `IPR index == vector` assumption with a
      vector→IPR source table (`vector_to_ipr` + `kIprMap`); IR/IER stay 1:1, only the
      shared IPR is remapped. Byte-identical for the vectors used today (SWINT/CMTW/SCI6),
      so no runtime change now; correct for driver-era device lines. RX72M re-validated on
      silicon 2026-07-09 (selftest 14/14, rfp-cli/E2 Lite flash, SCI6 console on ttyUSB0).

---

## Later — not M1

- **M2 — MPU enforcement** fan-out: reference pair (RISC-V PMP/NAPOT + XMC v7-M PMSA) →
  K64F SYSMPU → RX → tail; + the arch-independent security model (domains, per-thread
  private stacks, syscall-arg/user-pointer validation, pow2 region placement). See
  `docs/architecture.md` / `docs/m2-readiness.md`.
- **M3 — capabilities + authenticated grants** (seL4-principled object model), **and
  user-selectable CPU clock / low-power mode** (needs explicit per-chip clock bring-up
  first, from the audit above).
- **Driver era (12b)** — userspace UART/console driver takes the peripheral as a
  capability; kernel relinquishes it (`console_tx_deinit`), panic path moves to a
  kernel-retained transport. See `docs/console.md` "Future".
