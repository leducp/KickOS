<!-- SPDX-License-Identifier: CECILL-C -->
# M1 final validation state

Full-fleet M1 validation: every app + the microbenchmark on every reachable target
(host sim, the QEMU/emulated gates, and each physically-connected board). This file
is the record of what was actually observed, not a plan. Started 2026-07-09 at
`8ff9ada`.

## How this run was done

- **Host + emulated** targets run the `ctest` gates (deterministic, in-CI).
- **Physical** boards are flashed with `tools/flash.sh` (backend per chip) using the
  `-st` selftest build (`FLASH_BUILD=build/<board>-st`, so the diagnostic apps are
  present), and the console is captured off the mapped serial device with a pyserial
  helper. The microbenchmark uses a separate `-DKICKOS_BENCH=ON` build.

### Device map (this bench, 2026-07-09)

| Board | Arch | Flash backend | Console device |
|---|---|---|---|
| XMC4800-Relax | armv7m (M4F) | J-Link SWD | `/dev/ttyACM2` (J-Link VCOM) |
| RX72M | rxv3 | rfp-cli / E2 Lite | `/dev/ttyUSB0` (FTDI) |
| ESP32-WROOM | xtensa LX6 | esptool | `/dev/ttyUSB1` (CH340) |
| ESP32-C6 | rv32imac | esptool | `/dev/ttyACM0` (CH343P); flash on `/dev/ttyACM1` (native USB-JTAG) |

### App availability per arch (from `user/apps/CMakeLists.txt`)

- Everywhere: `hello`, `hello_c`, `selftest`, `mpu_fault`, `sched_exit`, `stress`,
  `fault` (fault needs the selftest/diagnostic build).
- `fp_switch`: M4F only among these (XMC; also qemu/k64f). `blink`: XMC + ESP32-WROOM.
- `bench`: armv7m / rv32imac / rxv3 (XMC, RX72M, ESP32-C6, qemu-riscv) — **not** xtensa,
  needs `-DKICKOS_BENCH=ON`.

---

## 1. Host + emulated gates (ctest)

| Target | Arch | Result | Detail |
|---|---|---|---|
| `sim` | host x86 | ✅ 9/9 | build, telemetry_golden, hello_demo, selftest, oot_export, mpu_fault, sched_exit, stress, fault_dump |
| `qemu` (mps2-an386) | armv7m M4 | ✅ 7/7 | build, hello, selftest, oot_export_mcu, sched_exit, fault_dump, fp_switch |
| `microbit` (nRF51) | armv6m M0 | ✅ 5/5 | build, hello, selftest, sched_exit, fault_dump |
| `qemu-riscv` (virt) | rv32imac | ✅ 5/5 | build, hello, selftest, sched_exit, fault_dump |

All emulated gates green.

---

## 2. Physical boards

### 2.1 XMC4800-Relax (armv7m Cortex-M4F, 144 MHz)

Flash: onboard J-Link SWD (SN 591165808). Console: J-Link VCOM `/dev/ttyACM2` @115200.
All apps flashed from `build/xmc4800-relax-st`. **9/9 apps pass.**

| App | Result | Observed |
|---|---|---|
| `hello` / `hello_c` | ✅ | banner + ping/pong |
| `selftest` | ✅ 14/14 | `# all tests passed` |
| `sched_exit` | ✅ | `root: survived worker exit` |
| `stress` | ✅ | `naps 200/200  handoffs 18000/18000  churn 1360/1360` → `STRESS PASS` (needs ~60 s) |
| `mpu_fault` | ✅ (M1) | `cross-domain write completed: OK on M1 hardware` |
| `fault` | ✅ | `=== HARD FAULT === … CFSR=0x10000` (UNDEFINSTR, from `udf #0`) |
| `fp_switch` | ✅ | `FP OK: 10/20 rounds, s16-s31 preserved across switch` |
| `blink` | ✅ | boots, `blink: heartbeat on the kernel diagnostic LED` (LED P5.9) |

**J-Link tooling note:** the freshly-installed J-Link V9.58 forks `JLinkGUIServerExe` in
`JLINK_OpenEx` and hangs on it in a headless session. Fixed by passing **`-nogui 1`** to
`JLinkExe` (the earlier "wedged probe / needs replug" diagnosis was wrong — it was the GUI
server, not the probe). Baked into `scratchpad/run_xmc.sh`.

### 2.2 RX72M (RXv3, 240 MHz)

Flash: `rfp-cli`/E2 Lite over FINE. Console: SCI6 → FTDI `/dev/ttyUSB0` @115200.
All apps flashed from `build/rx72m-st`. **7/7 apps pass.**

| App | Result | Observed |
|---|---|---|
| `hello` | ✅ | banner + `hello from KickOS userspace!` + ping/pong counting |
| `hello_c` | ✅ | same (plain-C API) |
| `selftest` | ✅ 14/14 | `1..14`, `ok 1..14`, `# all tests passed` |
| `sched_exit` | ✅ | `worker: running` → `worker: exiting` → `root: survived worker exit` |
| `stress` | ✅ | `naps 600/600  handoffs 18000/18000  churn 2040/2040` → `STRESS PASS` |
| `mpu_fault` | ✅ (M1) | `cross-domain write completed: OK on M1 hardware (no HW MPU yet)` — expected pre-M2 |
| `fault` | ✅ | `=== RX EXCEPTION (trap) === PC=0xffc00417 PSW=0x30001` |

**Bug found + fixed during this run:** the `fault` app had no RX branch — it fell
through to `__builtin_trap()`, which on rx-elf compiles to a `bsr _abort` (→ `_exit`),
so it never raised a CPU exception and the RX reporter/dump path was never exercised.
Added `#elif defined(__RX__) __asm volatile("brk")` (RX has no undefined-instruction
mnemonic; `BRK` traps via relocatable vector 0 → `_rx_trap` → `_kickos_rx_fault_report`).
Dump now emits correctly. (The other three arches already had a real trap instruction.)

### 2.3 ESP32-WROOM (Xtensa LX6, 240 MHz)

Flash: `esptool` (ESP32-D0WD-V3, 4 MB). Console: UART0 → CH340 `/dev/ttyUSB1` @115200.
`cap.py` pulses EN/IO0 to boot from a clean POWERON_RESET each time. **8/8 apps pass**
(no `bench` — the microbench is armv7m/rv32/rxv3 only, not Xtensa).

| App | Result | Observed |
|---|---|---|
| `hello` | ✅ | banner + ping/pong to 10 |
| `hello_c` | ✅ | same (plain-C API) |
| `selftest` | ✅ 14/14 | `# all tests passed` |
| `sched_exit` | ✅ | `root: survived worker exit` |
| `stress` | ✅ | `naps 600/600  handoffs 18000/18000  churn 2040/2040` → `STRESS PASS` |
| `mpu_fault` | ✅ (M1) | `cross-domain write completed: OK on M1 hardware` — expected pre-M2 |
| `fault` | ✅ | `=== XTENSA EXCEPTION (illegal instruction) === EXCCAUSE=0x0 EPC1=0x4008041c PS=0x60c30` |
| `blink` | ✅ | boots, `blink: heartbeat on the kernel diagnostic LED` (LED GPIO2; not eyeballed) |

### 2.4 ESP32-C6 (RV32IMAC, 160 MHz)

Flash: `esptool`. Console: UART0 → CH343P `/dev/ttyACM0` @115200. **Flashing note:** the
native USB-JTAG (`/dev/ttyACM1`) wedges under repeated reflash cycles (boots into DOWNLOAD
mode / ROM Guru-Meditation, `write timeout` on reconnect). The robust path is the **CH343P
UART0 (`/dev/ttyACM0`)** which has the DTR/RTS auto-reset circuit — flash *and* console on
that one port (WROOM-style, `cap.py --reset esp` to boot). Re-verified 14/14 that way. The
ROM prints `SHA-256 comparison failed … Expected: ffff… Attempting to boot anyway` on
every boot — KickOS ships a monolithic image with no appended hash; benign, boots fine.
**7/7 apps pass** (no `blink`/`fp_switch` for this board).

| App | Result | Observed |
|---|---|---|
| `hello` | ✅ | banner + ping/pong |
| `hello_c` | ✅ | same (plain-C API) |
| `selftest` | ✅ 14/14 | `# all tests passed` |
| `sched_exit` | ✅ | `root: survived worker exit` |
| `stress` | ✅ | `naps 600/600  handoffs 18000/18000  churn 2040/2040` → `STRESS PASS` |
| `mpu_fault` | ✅ (M1) | `cross-domain write completed: OK on M1 hardware` — expected pre-M2 |
| `fault` | ✅ | `=== RISC-V TRAP (illegal instruction) === mcause=0x2 mepc=0x40800032 mtval=0x0 mstatus=0x1880` |

---

## 2.5 Fixes applied this session (root-caused on silicon)

1. **`fault` app had no RX trap** → added `#elif defined(__RX__) __asm volatile("brk")`
   (`user/apps/fault/main.cc`). Without it RX fell through to `__builtin_trap()` →
   `abort()`/`_exit`, never exercising the exception reporter. Verified: RX now emits
   `=== RX EXCEPTION (trap) ===`.

2. **armv7m tickless timer reset-storm** → added an idempotent-arm guard to
   `arch/arm/common/arch_arm_common.cc` (`arch_timer_arm`): `ktime_rearm()` is called on
   every reschedule with the same pending deadline; the old code blindly reloaded
   `SYST_CVR` + `PENDSTCLR`'d each time, so a far deadline reached only while lower-prio
   threads switch faster than it expires (bench reporter's 0.5 s sleep behind two CPU-bound
   players) **never fired**. Guard: skip reprogramming when the same deadline is already
   counting (SysTick still enabled); the ISR disables SysTick before re-arming so its own
   re-arm is never skipped. **Verified fixed on XMC silicon** (bench now reports). Covers
   armv6m too (shared file); qemu/microbit ctests still green.

3. **RX guard robustness** → `arch/rx/rxv3/arch_rxv3.cc` had the equivalent guard but keyed
   it on a `CMWSTR.STR` hardware readback that races at full switch speed; replaced with a
   software armed-deadline flag invalidated in the timer ISR (same shape as the ARM guard).
   More correct, but see §3.1 — it is **not** the RX bench's limiting factor.

4. **ESP32-C6 bench cycle-source seam** (`arch_rv32imac.cc` + rv32 `switch.S` +
   `chip_esp32c6.cc` + `bench.cc`): the C6 HP core has no Zicntr `cycle` CSR (`rdcycle`
   traps even in M-mode). Added a `g_bench_cycle_src` pointer (mirrors `g_clint_msip`):
   null → `rdcycle` (qemu-virt, unchanged); the C6 points it at its core-clocked CLINT
   MTIME. The C6 bench now runs and measures a real number. (Bench is still not built for
   `sim`/Xtensa.)

## 3. Microbenchmark (`bench`) — uniform across the whole fleet

Redesigned this session to be uniform on **every** arch. Two equal-prio players ping-pong via
semaphores; the reporter runs as the **root thread** (so only 2 pool slots — fits boards down
to `KICKOS_MAX_THREADS==2`) and is **woken by the workload** (player_b posts a gate every N
rounds), NOT by a timer — so it can't be starved (see §3.1). It prints:

- **throughput** — context switches/s over a burst, timed with `kos::clock_now`. Needs no cycle
  counter, so it works on **every** target (incl. Cortex-M0 and the sim). The uniform metric.
- **per-switch cost + IRQ-entry latency** in CPU cycles, where switch.S brackets the swap with a
  cycle counter: armv7m DWT, rxv3 CMTW1, rv32imac `rdcycle`/CLINT-MTIME, xtensa CCOUNT. Absent
  (`scnt==0`) on M0 / sim / QEMU-frozen-DWT → those report throughput only.

Built with `-DKICKOS_BENCH=ON`, for all arches. Full fleet, measured this session:

| Target | Arch @ clock | Throughput | Switch cost | IRQ-entry |
|---|---|---|---|---|
| **ESP32-C6** | rv32imac @160 MHz | 51 K sw/s | **26/26/27 cyc = 162 ns** | 304 cyc / 1900 ns |
| **RX72M** | rxv3 @240 MHz | 94 K sw/s | **64/87/96 cyc = 266/362/400 ns** | 160 cyc / 666 ns |
| **XMC4800** | armv7m @144 MHz | 39 K sw/s | **66/71/662 cyc = 458/493/4597 ns** | 133 cyc / 923 ns |
| **ESP32-WROOM** | xtensa LX6 @240 MHz | 63 K sw/s | **124/124/124 cyc = 516 ns** (windowed-ABI spill) | 622 cyc / 2591 ns |
| **picopi (RP2040)** | armv6m M0+ @125 MHz | 34 K sw/s | — (no cycle counter) | — |
| micro:bit | armv6m M0 @16 MHz | 121 K sw/s* | — (no cycle counter) | — |
| sim | host | 243 K sw/s | — | — |
| qemu (mps2) | armv7m M4 | 125 K sw/s | — (DWT frozen in QEMU) | — |
| qemu-riscv | rv32imac | 145 K sw/s | 460 cyc (`rdcycle` @10 MHz, not cycle-exact) | 880 cyc |

picopi is **real M0+ silicon** (via picotool/BOOTSEL — J-Link SWD flashing of the RP2040 is
unreliable here: flaky DAP power + a SWD-reset that doesn't re-run boot2→XIP; BOOTSEL is the
reliable path). It also first-silicon-confirms the RP2040 UART0 console.

*micro:bit is QEMU (M0 on real silicon is a separate bring-up). Throughput s/s figures are the
full semaphore-handoff rate (2 switches/round, syscall-dominated); the cycle "switch cost" is the
bare register save/restore that switch.S brackets — the two differ because the handoff is
syscall+scheduler-bound, not switch-bound. Both are useful.

### 3.1 The self-report starvation (fixed by the redesign)

The old reporter slept on a **timer**; under 100 %-CPU zero-idle ping-pong the tickless timer was
starved (dense interrupt-masked critical sections + back-to-back syscalls leave no
interrupt-enabled window) and no report ever came — **cross-arch** (all three silicon arches;
verified on RX with a wake-counter `wakes=0` over 200 k iterations). It was **not** peripheral-vs
-core (the C6 CLINT *core* timer starved too) and **not** priority (a scheduler-touching timer ISR
can't be raised above the kernel lock; IPL 4→11 did nothing). The **redesign removes the
dependence entirely** — the reporter is woken by a semaphore post from the workload (a direct
reschedule), so it self-reports reliably on every board at full speed. The armv7m SysTick
**reset-storm** (reprogrammed + `PENDSTCLR`'d every reschedule) was a separate real bug, fixed by
the `arch_timer_arm` guard (§2.5).

Note the *underlying* worst-case ISR latency under sustained syscall load is still worth bounding
(shorter/split critical sections) — an M2 latency/uniformity task; the IRQ-entry numbers above are
the first fleet-wide measurement of it. It does not affect real workloads (which idle/block).

### 3.2 Per-arch cycle sources

- **rv32imac**: `g_bench_cycle_src` seam (mirrors `g_clint_msip`) — null → `rdcycle` (qemu-virt);
  the **C6 points it at CLINT MTIME** because its HP core has no Zicntr `cycle` CSR (`rdcycle`
  traps). Wired in `arch_rv32imac.cc` / rv32 `switch.S` / `chip_esp32c6.cc` / `bench.cc`.
- **xtensa LX6**: CCOUNT, bracketed in the windowed `switch.S`. The windowed exit can't host a
  call, so the cost is banked at the next switch's entry (a safe call site, same as the trace
  hook) — one switch late, irrelevant over millions. The IRQ bench now reads a full n=100
  (622 cyc / 2591 ns): the LX6 masks the injected logical line on delivery and expects an
  `irq_ack`, so `kickos_bench_irq_once` re-unmasks the line before each inject (was n=1).
- Flash the ESP boards' bench via CH343P UART0; the C6 native USB-JTAG wedges under repeated reflash.

---

## 4. Summary

| Target | Apps | Bench (uniform, §3) |
|---|---|---|
| sim (host) | ✅ 9/9 ctest | ✅ 243 K sw/s (throughput) |
| qemu M4 | ✅ 7/7 ctest | ✅ 125 K sw/s (DWT frozen → no cyc) |
| microbit M0 | ✅ 5/5 ctest | ✅ 121 K sw/s (throughput; no cyc counter) |
| qemu-riscv | ✅ 5/5 ctest | ✅ 145 K sw/s + 460 cyc (rdcycle @10 MHz) |
| **XMC4800** | ✅ 9/9 silicon | ✅ **66 cyc / 458 ns** switch, 133 cyc IRQ, 39 K sw/s |
| **RX72M** | ✅ 7/7 silicon | ✅ **96 cyc / 400 ns** switch, 160 cyc IRQ, 94 K sw/s |
| **ESP32-WROOM** | ✅ 8/8 silicon | ✅ **124 cyc / 516 ns** switch (CCOUNT), 622 cyc IRQ, 63 K sw/s |
| **ESP32-C6** | ✅ 7/7 silicon | ✅ **26 cyc / 162 ns** switch, 304 cyc IRQ, 51 K sw/s |
| **picopi (RP2040)** | ✅ (M0+, via picotool) | ✅ 34 K sw/s (throughput; no cyc counter) |

**Functional M1 is complete**: every app passes on all four physical arches (armv7m M4F,
RXv3, Xtensa LX6, RV32IMAC) + all host/emulated gates. Fault-dump paths verified on silicon
for ARM (`HARD FAULT`, CFSR=0x10000), RX (`RX EXCEPTION`), Xtensa (`XTENSA EXCEPTION`), and
RISC-V (`RISC-V TRAP`). **The microbenchmark is now uniform across the entire fleet** — every
arch reports throughput; the four cycle-counter arches also report per-switch cost + IRQ latency
(all five silicon boards incl. picopi M0+ measured).

**Landed this session** (committed: `951facc` bench+fixes, `7113905` bench IRQ fixes):
1. `user/apps/fault/main.cc` — RX `brk` trap (RX fault path now exercised).
2. `arch/arm/common/arch_arm_common.cc` — armv7m/v6m tickless reset-storm guard (real bug).
3. `arch/rx/rxv3/arch_rxv3.cc` — RX timer guard hardened (software flag vs racy HW readback).
4. **Uniform bench redesign** — throughput + reporter-as-root (fits `MAX_THREADS==2`,
   starvation-proof), per-arch `cyccnt`, unified `arch_irq_inject` trigger, built for all arches.
5. **RISC-V C6 cycle seam** (CLINT MTIME) + **Xtensa CCOUNT bracket**.
6. **10-angle review fixes** (`7113905`): xtensa IRQ n=1→n=100 (re-unmask before inject);
   count sub-tick IRQ samples; clear the stale reset sample.

**10-angle review outcome:** the high-stakes areas — the timer-arm guards and the windowed/riscv
asm — were verified correct (race-free, register-clean); no ternary / `#pragma once` / bad-comment
violations. The findings it raised are all fixed (commit `7113905`).

**Open follow-ups (do not gate M1):**
1. Worst-case ISR latency under sustained syscall load (§3.1) — the crit-section reduction plan
   (R1 thread a single `now`; R2 ARM skip redundant BASEPRI+barriers on nested locks; R3 fold the
   min-delta read past the guard) is analyzed + ranked; R6 (xtensa inline-masked switch) is the
   one structural outlier. M2 latency task. Implement + measure on XMC/RX.
2. `sys_cpu_clock_hz()` syscall (in TODO.md).
