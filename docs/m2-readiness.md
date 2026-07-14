<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS pre-M2 readiness — task list

> The remaining work to put a **solid, cross-board-verified foundation** under M2
> (MPU enforcement — the design lives in `architecture.md` / `invariants.md`). M2
> wires `arch_mpu_apply` into the context-switch hook and adds per-task memory
> protection; it stacks directly on the scheduler / IRQ / timer paths, so those
> must be trustworthy on real silicon first. Ordered by priority.

## 1. Thread-slot reclamation (FIRST PRIORITY) — ✅ DONE

`thread_spawn` reclaims EXITED pool slots at spawn (twin of the sem freelist),
generation-tagged handles, full TCB/context reset on reuse. Validated: sim
(deterministic) + qemu-arm (PendSV) + qemu-riscv (msip) stress churn all PASS; the
full selftest (15/15 on sim) now runs on a **2-slot pool** (`-DKICKOS_MAX_THREADS=2`),
which unblocks the armv6m QEMU (`microbit`) selftest. Passed the 10-angle review gate.
(The qemu-riscv stress *soak* flaky-crashes QEMU under heavy PMP-reprogram load — guest
is clean, not in CI; keep churn totals modest.)

## 2. Maximum hardware-board coverage — the final on-silicon pass

**This is the single batched HW-validation pass** (per the HW-test deferral policy:
batch per-board silicon re-validation until after telemetry lands, item 3). Run the
full suite (`-DKICKOS_ENABLE_SELFTEST=ON`: selftest + stress + `sched_exit`) on every
board we can physically flash, capture over each board's console, record pass/fail.

Run the full suite (`-DKICKOS_ENABLE_SELFTEST=ON`: selftest + stress + `sched_exit`)
on every board we can physically flash, and record pass/fail. Batch per the HW-test
deferral policy; capture over each board's console.

| Board | Arch | Status to reach |
|-------|------|-----------------|
| frdmk64f | armv7m | ✅ selftest 14/14 + stress soak (120k handoffs) on silicon |
| rx72m | rxv3 | ✅ runs on silicon; tickless re-arm bug fixed generically; bench 96cyc/400ns recorded. Re-run selftest+stress to close. |
| xmc4800-relax | armv7m | **blocked by the `multi_wait` deadlock (item 5)**; then selftest + stress |
| f411disco / f302nucleo | armv7m/M4 | selftest + stress |
| picopi (RP2040) | armv6m | `sched_exit`; full selftest now possible (item 1 unblocked small pools) |
| esp32-wroom / esp32c6-wroom | lx6 / rv32imac | bring up flashing (currently build-only), then suite |
| microbit (nrf51) | armv6m | **not a physical target** — a QEMU armv6m vehicle; covered by CI (`-M microbit`) |
| qemu / qemu-riscv | armv7m / rv32imac | ✅ automated GitHub CI (item 6); the QEMU functional gate, not silicon |

## 3. Telemetry capture per board

With the observability subsystem (`docs/telemetry.md`, `KICKOS_TELEMETRY=rtt`,
`KICKOS_CONSOLE=rtt|both`), record context-switch / syscall / IRQ traces on each
silicon board and archive the baselines. Confirms the trace clock + RTT wire format
hold on real hardware ahead of M2, and gives a pre-MPU performance reference to
measure the M2 `arch_mpu_apply` switch-hook cost against.

## 4. Context-switch + IRQ-entry bench baselines

Fill the per-board bench table (bench app, `KICKOS_BENCH=ON`). Have: RX72M 96 cyc /
400 ns, K64F 77 cyc / 641 ns. Record the rest; these are the M2 regression baseline.

## 5. Open bugs to resolve or track

- **XMC `multi_wait` deadlock** — XMC-chip-specific (K64F passes, so not armv7m-general).
  Spawned workers never run while main is in a tickless timer-sleep. Needs a no-reset
  J-Link attach or SWO trace (J-Link resets XMC on connect, defeating post-mortem).
  **Gates the XMC half of the M2 reference pair — highest-leverage open item.**
- ~~RX bench HW re-validation~~ ✅ resolved: the tickless re-arm was a real one-shot-reset
  bug, fixed generically; RX runs on silicon and the 96cyc/400ns baseline is recorded.
- ~~microbit full selftest~~ ✅ resolved: it exposed a real selftest leak — raw-handle
  tests never destroyed their semaphores, so the suite overran microbit's 4-slot pool.
  Fixed the suite (every test reclaims its semaphore; peak concurrent = 4), NOT the pool.
  armv6m now runs the full selftest in CI. (The refusal to defer caught the bug — see the
  no-deferral principle.)

## 6. GitHub Actions CI — ✅ DONE

`.github/workflows/ci.yml` runs the green gates on every push/PR (was hand-run).
Scoped to deterministic, host-runnable gates — no flaky or HW-only steps:
- **Build matrix**: configure + build every board whose toolchain is apt-installable
  — the ARM families + RISC-V — catching build/link/linker-script regressions. Plus
  the `oot_export` in-tree/out-of-tree gate (runs in the sim ctest).
- **sim** (`-DKICKOS_ENABLE_SELFTEST=ON`): full `ctest` — selftest, stress (incl. the
  thread-reclaim churn), `sched_exit`, `mpu_fault`, telemetry golden-vector. This is
  the authoritative deterministic gate.
- **qemu-arm** (`mps2-an386` + `microbit`): `ctest` — the **full `selftest` TAP suite**
  (same binary as sim, on real armv7m PendSV) + hello + sched_exit + fp_switch. These
  clock off semihosting `SYS_CLOCK`, which QEMU ≤ 10 (ubuntu-latest ships 8.2) freezes
  while the core halts in WFI — a timed sleep with every thread idle never wakes. Fixed
  in the kernel: `arch_idle_wait` spins (not WFI) on the QEMU-only mps2/nrf51 chips.
  (icount does NOT fix it — verified.) `rr_interleave` scales its RR quantum to the
  target's clock granule, so it runs even on the coarse clock (no skip). **microbit
  (armv6m)** runs the full suite too — its small pool (`MAX_SEMAPHORES=4`) forced the
  suite to be pool-honest (each test reclaims its semaphores), so armv6m is gated, not
  a silent hole.
- **qemu-riscv** (`virt`): `ctest` — the **full `selftest` suite** (on rv32imac msip) +
  hello + sched_exit; never the stress soak (QEMU host-crashes flakily under heavy
  PMP-reprogram load; guest is clean — see the qemu-riscv-crash note). Uses the **xPack
  `riscv-none-elf` toolchain** (pinned), symlinked to the `riscv64-unknown-elf-` prefix:
  apt's riscv gcc ships no rv32imac newlib/libgloss and Ubuntu has no drop-in lib
  package like ARM's. Also build-checks `esp32c6-wroom` + `qemu-riscv-bench`.
  (mtime clock → no WFI idle issue.)
- Toolchains: ARM = apt `gcc-arm-none-eabi` **+ `libnewlib-arm-none-eabi`** (the C
  library the split apt package omits); RISC-V = pinned xPack tarball.

RX and Xtensa use vendor toolchains (not in apt) — kept out of CI for now (one step
at a time); build those boards manually. Silicon likewise stays a manual/self-hosted
step per the HW-test deferral policy.

## 7. Fleet uniformity — make M1 a coherent standalone release

M1 is the first HW-support milestone and could ship as a public release on its own.
So M1 must be UNIFORM: every board the same shape, so M2's cost is purely MPU wiring
(not "and also finish the console on 8 boards"), and an external reviewer picking any
board gets identical behaviour. Non-M2 work that improves coherence is done HERE.

### Board readiness matrix

| Board(s) | arch / chip | Runs via | Selftest | Console |
|---|---|---|---|---|
| sim | host | native | 15/15 + stress + mpu_fault | ring + sync (in-tree ring CI) |
| qemu | armv7m / mps2 | QEMU | 14/14 | polled (semihosting) |
| qemu-riscv | rv32imac / virt | QEMU | 14/14 | polled (semihosting) |
| microbit | armv6m / nrf51 | QEMU | pass | polled (semihosting) |
| XMC4800 | armv7m / xmc4800 | silicon | 14/14 (HW 2026-07-09) + HARD FAULT dump | ring + sync |
| ESP32-WROOM | lx6 / esp32 | silicon | 14/14 (HW 2026-07-09) + XTENSA dump | ring |
| ESP32-C6 | rv32imac / esp32c6 | silicon | 14/14 (HW) + RISC-V TRAP dump | UART0/CH343P (native USB-JTAG flash-only) |
| RX72M | rxv3 / rx72m | silicon | 14/14 (HW 2026-07-09; sw IRQ controller) | ring; console ttyUSB0/FT232 |
| f411disco | armv7m / stm32f411 | silicon | 14/14 (HW 2026-07-14) + all apps + HARD FAULT + LED + bench | polled (ext UART PA2) |
| blackpill | armv7m / stm32f411 | silicon | 14/14 (HW 2026-07-14) + bench | polled (ext UART PA2) |
| f302nucleo | armv7m / stm32f302 | silicon | 13/14 (HW 2026-07-14; test 11 = 4 K alloc > 16 K) + bench | ST-Link VCP |
| bluepill | armv7m / stm32f103 | silicon | 13/14 (HW 2026-07-14; test 11 = 4 K alloc > 10 K clone) | polled (ext UART PA9) |
| picopi | armv6m / rp2040 | silicon | 14/14 (HW 2026-07-09) | UART0/GP0 |
| bluepill-c8 | armv7m / stm32f103 | build-only (-st) | build-clean | build-only (20 K genuine variant) |
| K64F | armv7m / mk64f | prior silicon; M2 re-confirm | pass (2026-07-09 baseline) | ring + sync |
| due | armv7m / sam3x8e | RETIRED unit (port proven 2026-07-09) | — (unit peripheral-I/O fault) | — |

Every board has a `<board>-st` preset (base + `KICKOS_ENABLE_SELFTEST=ON`) so the
full 14-test suite is a first-class, per-board config -- not an ad-hoc `-D`. The
selftest/fault/mpu_fault apps are **diagnostic** (`kickos_add_diagnostic_app`, built
only under that flag: they use the test-only syscall surface / deliberately fault, so
they never enter a production image). Flash an `-st` build with
`FLASH_BUILD=build/<board>-st tools/flash.sh <board> selftest`.

### Console policy (decided)

Buffered IRQ-drained ring on any board with a real byte-FIFO UART + a device-IRQ receive
path. POLLED stays for the semihosting dev vehicles (mps2, virt, microbit) — synchronous,
no TX-empty IRQ to drain. `arch_console_write` -> the ring; `arch_console_write_sync` ->
the polled writer (panic/fault path).

On the ring today: **K64F, XMC4800, and ESP32-C6** (UART0, via the real peripheral-IRQ path
added 2026-07-14 — a 2048-byte ring, silicon-validated). To generalise onto the same
template: RX72M, ESP32-WROOM (UART0), the STM32/RP2040/SAM3X fleet.

**Dependency:** the ring needs a working device-interrupt RECEIVE path per arch. ARM (NVIC)
+ RX (INTB) have it -> mechanical. The **ESP32-C6 now has one** (UART0 TX -> interrupt matrix
-> CPU int 30 -> `switch.S` `.Lextdev`); the RX `kickos_rx_default_irq` demux is still a stub.

### Remaining M1-uniformity gaps
- Ring console generalised (2026-07-09): RX72M, ESP32-WROOM/UART0, STM32x3, RP2040,
  SAM3X all committed; ESP32-C6 stays polled (USB-Serial-JTAG). HW-revalidate the
  ESP32-WROOM ring + capture once re-plugged. sim ring done (2026-07-09): a
  synthetic TX peripheral (SIGUSR1-delivered TX-empty line + per-delivery slot
  budget) arms the buffered path in-tree, so ctest now exercises the SPSC
  ring/drain/wrap/overflow paths the HW-gated MCU backends never run in-tree.
- RX72M selftest 14/14 on silicon (2026-07-09): DONE via the arch/rx software IRQ
  controller (SWINT2 doorbell); console captured on ttyUSB0/FT232. Closed.
- XMC4800 + ESP32-WROOM re-validated on silicon (2026-07-09): 14/14 + fault dump
  through the armed ring. Closed.
- ESP32-C6: runs on silicon (hello streams), but one-shot boot output (selftest TAP)
  is NOT capturable over its self-hosted USB-JTAG (re-enumerates on reset; the polled
  console drops during the gap). Options: capture over the board's CH343P UART bridge,
  or a pre-crash/pre-test boot delay to land output after re-enum. Also no diag-LED
  blink on C6 (WS2812 RGB on GPIO8 -- needs an addressable driver, not a pin toggle;
  TODO). C6 selftest 10-14 pass/fail on silicon still unconfirmed (capture-gated), but
  the INTPRI-threshold suspicion was disproven -- see [[kickos-panic-console-review]].
- Build-only boards: build-clean incl. the -st (selftest) presets; on-silicon deferred
  until units are at disposal.

## Then: M2 proper

Per `architecture.md`: MPU-enforcement fan-out — reference pair (RISC-V PMP/NAPOT +
XMC v7-M PMSA) → K64F SYSMPU → RX → tail; plus the arch-independent security model
(domains, per-thread private stacks, syscall-arg/user-pointer validation, pow2 region
placement). Capabilities + authenticated grants are M3.

## M3 (later)

Capabilities + authenticated grants (the seL4-principled object model). **Also M3:
user-selectable CPU clock / low-power mode** — once the fleet-wide "max frequency by
default" baseline is confirmed (see [[kickos-clock-audit]]), expose a per-chip clock
select so an app can trade speed for power. Depends on each chip having an explicit
clock bring-up (not just inheriting the ROM/reset default) and a truthful CPU/timer
Hz that the scheduler's ns↔tick math tracks when the clock changes.
