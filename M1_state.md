<!-- SPDX-License-Identifier: CECILL-C -->
# M1 validation -- final state

M1 is the uniformity / bring-up milestone: **every board boots, has a console, runs the
selftest, panics visibly, and runs at its true (or safely-degraded) clock.** This file records
the validated end state. Raw console captures are in `M1_raw_meas.md`; per-board pin/flash
details in `docs/reference/boards.md`.

**Result: M1 complete -- 10 boards validated on silicon (5 ISAs) + 3 emulator gates green.**
No open hardware dependency: nothing gating M1 waits on a board that isn't in hand.

## Fleet coverage

| ISA | Silicon boards | Emulated |
|---|---|---|
| armv7m (Cortex-M4F/M4/M3) | XMC4800, f411disco, blackpill, f302nucleo, bluepill (F103, clone retired post-M1) | qemu (mps2-an386) |
| armv6m (Cortex-M0/M0+) | picopi (RP2040) | microbit (nRF51) |
| RXv3 | RX72M | -- |
| Xtensa LX6 | ESP32-WROOM | -- |
| RV32IMAC | ESP32-C6 | qemu-riscv (virt) |
| host | -- | sim |

Every arch has a real console, a tickless timer, a fault-register dump, an inject-driven IRQ
path, and the M2 MPU no-op. The microbenchmark runs uniformly on all of them.

## 1. Host + emulated gates (ctest, in-CI)

| Target | Arch | Result | Tests |
|---|---|---|---|
| `sim` | host x86 | [x] 9/9 | build, telemetry_golden, hello_demo, selftest, oot_export, mpu_fault, sched_exit, stress, fault_dump |
| `qemu` (mps2-an386) | armv7m M4 | [x] 7/7 | build, hello, selftest, oot_export_mcu, sched_exit, fault_dump, fp_switch |
| `microbit` (nRF51) | armv6m M0 | [x] 5/5 | build, hello, selftest, sched_exit, fault_dump |
| `qemu-riscv` (virt) | rv32imac | [x] 5/5 | build, hello, selftest, sched_exit, fault_dump |

## 2. Silicon boards

App matrix per board. `mpu_fault` "passes" by completing the cross-domain write (no HW MPU in
M1 -- the M2 backend will trap it). `fault` deliberately executes an illegal instruction and the
kernel must emit the register dump. `--` = app not built for that board.

| Board | Chip @ clock / RAM | selftest | hello | sched_exit | stress | mpu_fault | fault (dump) | fp_switch | blink |
|---|---|---|---|---|---|---|---|---|---|
| **XMC4800-Relax** | XMC4800 M4F @144 MHz | [x] 14/14 | [x] | [x] | [x] | [x] | [x] `HARD FAULT` | [x] | [x] LED |
| **f411disco** | STM32F411 M4F @84 MHz / 128 K | [x] 14/14 | [x] | [x] | [x] | [x] | [x] `HARD FAULT` | -- | [x] LED |
| **blackpill** | STM32F411 M4F @84 MHz / 128 K | [x] 14/14 | [x] | [x] | [x] | [x] | [x] `HARD FAULT` | -- | -- |
| **f302nucleo** | STM32F302 M4 @64 MHz / 16 K | (!) 13/14^1 | [x] | [x] | [x] | [x] | [x] `HARD FAULT` | -- | [x] |
| **bluepill** (retired post-M1) | STM32F103 M3 / 10 K clone | (!) 13/14^1 | [x] | [no]^2 | [no]^2 | -- | -- | -- | -- |
| **picopi** | RP2040 M0+ @125 MHz | [x] 14/14 | [x] | [x] | [x] | [x] | [x] | -- | [x] |
| **RX72M** | RXv3 @240 MHz | [x] 14/14 | [x] | [x] | [x] | [x] | [x] `RX EXCEPTION` | -- | -- |
| **ESP32-WROOM** | Xtensa LX6 @240 MHz | [x] 14/14 | [x] | [x] | [x] | [x] | [x] `XTENSA EXCEPTION` | -- | [x] |
| **ESP32-C6** | RV32IMAC @160 MHz | [x] 14/14 | [x] | [x] | [x] | [x] | [x] `RISC-V TRAP` | -- | [x] |

^1 **13/14 is a board RAM-size limit, not a bug.** Selftest test 11 (`irq_as_event`) allocates a
4 KiB region via `kos_ram_alloc(4096)`; it doesn't fit the 16 K (f302) / 10 K (bluepill) arena
once the static kernel + stacks are placed. Tests 1-10 and 12-14 pass. Passes on every board with
more free RAM. The suite is pool-honest -- an early-return in test 11 no longer leaks its
semaphores (that leak previously wedged 12-14 on tiny parts).

^2 `stress`/`sched_exit`-heavy apps and `bench` don't fit the 10 K bluepill clone. `bluepill` also
runs notably slow, consistent with the clone's HSE not locking (HSI fallback) -- not confirmed by a
clock read, since `bench` (which reports `SystemCoreClock`) doesn't fit 10 K.

### Fault-dump coverage (silicon)

The unified kpanic/fault path emits a register dump + LED heartbeat on every ISA:

- **armv7m**: `=== HARD FAULT === ... CFSR=0x10000 HFSR=0x40000000` (UNDEFINSTR -> forced HardFault)
- **RXv3**: `=== RX EXCEPTION (trap) === PC=... PSW=...`
- **Xtensa LX6**: `=== XTENSA EXCEPTION (illegal instruction) === EXCCAUSE=... EPC1=... PS=...`
- **RV32IMAC**: `=== RISC-V TRAP (illegal instruction) === mcause=0x2 mepc=... mtval=... mstatus=...`

## 3. Microbenchmark (`bench`) -- uniform across the fleet

Two equal-prio players ping-pong through semaphores; the reporter runs as the **root thread** (so
only 2 pool slots -- fits boards down to `KICKOS_MAX_THREADS==2`) and is **woken by the workload**
(a gate post every N rounds), **not by a timer** -- so it cannot be starved (see section 3.1). It prints:

- **throughput** -- context switches/s, timed with `kos::clock_now`. Needs no cycle counter ->
  works on **every** target (incl. Cortex-M0 and the sim). The universal metric.
- **per-switch cost + IRQ-entry latency** in CPU cycles, where `switch.S` brackets the swap with a
  cycle counter: armv7m DWT, RXv3 CMTW1, RV32IMAC `rdcycle`/CLINT-MTIME, Xtensa CCOUNT. Absent on
  M0 / sim / QEMU-frozen-DWT -> those report throughput only.

Built with `-DKICKOS_BENCH=ON`. Full fleet:

| Target | Arch @ clock | Throughput | Switch cost (min) | IRQ-entry |
|---|---|---|---|---|
| **ESP32-C6** | rv32imac @160 MHz | 51 K sw/s | **26 cyc = 162 ns** | 304 cyc / 1900 ns |
| **RX72M** | RXv3 @240 MHz | 94 K sw/s | **64 cyc = 266 ns** | 160 cyc / 666 ns |
| **XMC4800** | armv7m @144 MHz | 39 K sw/s | **66 cyc = 458 ns** | 133 cyc / 923 ns |
| **f411disco** | armv7m M4F @84 MHz | 23 K sw/s | **66 cyc = 785 ns** | 119 cyc / 1416 ns |
| **blackpill** | armv7m M4F @84 MHz | 23 K sw/s | **70 cyc = 833 ns** | 119 cyc / 1416 ns |
| **f302nucleo** | armv7m M4 @64 MHz | 15 K sw/s | **84 cyc = 1312 ns** | 132 cyc / 2062 ns |
| **ESP32-WROOM** | Xtensa LX6 @240 MHz | 63 K sw/s | **124 cyc = 516 ns** (windowed-ABI spill) | 622 cyc / 2591 ns |
| **picopi (RP2040)** | armv6m M0+ @125 MHz | 34 K sw/s | -- (no cycle counter) | -- |
| micro:bit | armv6m M0 (QEMU) | 121 K sw/s | -- | -- |
| sim | host | 243 K sw/s | -- | -- |
| qemu (mps2) | armv7m M4 | 125 K sw/s | -- (DWT frozen in QEMU) | -- |
| qemu-riscv | rv32imac | 145 K sw/s | 460 cyc (`rdcycle` @10 MHz, not cycle-exact) | 880 cyc |

The two F411 boards (disco 66 cyc, blackpill 70 cyc) agree on switch *cycle* count and differ only
in wall-time by clock -- the uniformity the fleet is for. Throughput (semaphore-handoff rate, 2
switches/round, syscall-dominated) and the bracketed switch cost (bare register save/restore) are
different measurements; both are reported.

### 3.1 Starvation-proof by design

An earlier reporter slept on a **timer**; under 100 %-CPU zero-idle ping-pong the tickless timer is
starved (dense interrupt-masked critical sections + back-to-back syscalls leave no interrupt-enabled
window) and no report ever comes -- **cross-arch** (verified on RX: `wakes=0` over 200 k iterations),
**not** peripheral-vs-core (the C6 CLINT *core* timer starved too), **not** priority (a
scheduler-touching timer ISR can't be raised above the kernel lock). The design removes the
dependence: the reporter is woken by a semaphore post from the workload (a direct reschedule), so it
self-reports reliably on every board at full speed.

The *underlying* worst-case ISR latency under sustained syscall load is still worth bounding (shorter
/split critical sections) -- an M2 task; the IRQ-entry column above is the first fleet-wide measurement
of it. It does not affect real workloads (which idle/block).

### 3.2 Per-arch cycle sources

- **RV32IMAC**: a `g_bench_cycle_src` seam (mirrors `g_clint_msip`) -- null -> `rdcycle` (qemu-virt);
  the **C6 points it at CLINT MTIME** because its HP core has no Zicntr `cycle` CSR (`rdcycle` traps).
- **Xtensa LX6**: CCOUNT, bracketed in the windowed `switch.S`. The windowed exit can't host a call,
  so the cost is banked at the next switch's entry (a safe call site) -- one switch late, irrelevant
  over millions. The LX6 masks the injected logical line on delivery and expects an `irq_ack`, so the
  IRQ bench re-unmasks the line before each inject.
- **armv7m** DWT CYCCNT; **RXv3** CMTW1 (x32 scaling to ICLK cycles).

## 4. Scoping decisions

- **Arduino Due (SAM3X8E) -- retired unit.** The SAM3X *port* is proven on silicon (2026-07-09:
  selftest 14/14, 84 MHz PLL). The *physical unit* later developed a peripheral-I/O fault: core +
  flash-controller + native USB (SAM-BA) all verified working, but PIO output (PB27 "L" LED) won't
  toggle and the UART console is dead -- reproduced under a provably-correct **bare-metal** blink
  flashed via *two* independent paths (prog-port and native ROM SAM-BA). A correct program driving a
  pin that won't move is hardware, not software; likely marginal from the start (the MOSCXTST
  crystal-startup margin is a documented `GUESS`). Port stays proven; unit is not a reliable target.
- **K64F (FRDM-K64F) -- not an M1 gate; sign-off deferred to M2.** It's Cortex-M4F / armv7m -- the
  same path already silicon-proven on 4 boards, so it adds redundancy, not coverage. Its
  distinguishing feature is the **SYSMPU**, which is the M2 enforcement track, so K64F earns its
  formal HW re-confirm there. A prior silicon run is on record (120 MHz PLL, switch 77 cyc/641 ns,
  FP-switch 940 rounds). A deferral, not a coverage hole.

## 5. Open follow-ups (post-M1 -- do not gate M1)

Keyed to theme, not sequence. Only #3 is genuine M2 (MPU); the rest are orthogonal.

1. **[anytime perf -- not M2] Worst-case ISR latency** under sustained syscall load (section 3.1):
   shorten/split interrupt-masked critical sections. Ranked plan -- R1 thread a single `now`
   through the switch->re-arm path; R2 ARM skip redundant BASEPRI+barriers on nested locks
   (landed); R3 fold the min-delta read past the idempotency guard; R6 xtensa inline-masked
   switch (the one structural outlier). Needs a worst-case-latency probe (inject while a masked
   span is in flight) to demonstrate. Scheduler/switch-path work -- no MPU dependency.
2. **[M3] `sys_cpu_clock_hz()` syscall** -- expose the running core clock to userspace (bench ns
   math; read-side precursor to M3 user clock-select). Read-only, no MPU coupling.
3. **[M2] K64F re-confirm** -- folds into the M2 SYSMPU bring-up (section 4).
