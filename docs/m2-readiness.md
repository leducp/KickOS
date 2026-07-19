<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS pre-M2 readiness -- task list

> The remaining work to put a **solid, cross-board-verified foundation** under M2
> (MPU enforcement -- the design lives in `reference/architecture.md` / `reference/invariants.md`). M2
> wires `arch_mpu_apply` into the context-switch hook and adds per-task memory
> protection; it stacks directly on the scheduler / IRQ / timer paths, so those
> must be trustworthy on real silicon first. Ordered by priority.

## 1. Thread-slot reclamation (FIRST PRIORITY) -- [x] DONE

`thread_spawn` reclaims EXITED pool slots at spawn (twin of the sem freelist),
generation-tagged handles, full TCB/context reset on reuse. Validated: sim
(deterministic) + qemu-arm (PendSV) + qemu-riscv (msip) stress churn all PASS; the
full selftest (15/15 on sim) now runs on a **2-slot pool** (`-DKICKOS_MAX_THREADS=2`),
which unblocks the armv6m QEMU (`microbit`) selftest. Passed the 10-angle review gate.
(The qemu-riscv stress *soak* flaky-crashes QEMU under heavy PMP-reprogram load -- guest
is clean, not in CI; keep churn totals modest.)

## 2. Maximum hardware-board coverage -- the final on-silicon pass -- [x] DONE

This was the single batched HW-validation pass (per the HW-test deferral policy: batch
per-board silicon re-validation until after telemetry lands, item 3). The full suite
(`-DKICKOS_ENABLE_SELFTEST=ON`: selftest + stress + `sched_exit`) ran on every board we
can physically flash, captured over each board's console, pass/fail recorded. Result:
**10 boards on silicon across 5 ISAs** -- see section 7 and `M1_state.md`.

| Board | Arch | Status |
|-------|------|--------|
| frdmk64f | armv7m | [x] prior silicon baseline (14/14 + stress soak, 120k handoffs); formal re-confirm folds into M2 SYSMPU (not an M1 gate) |
| rx72m | rxv3 | [x] selftest 14/14 + stress on silicon (2026-07-09); tickless re-arm bug fixed generically; bench recorded (`M1_state.md` section 3). Closed. |
| xmc4800-relax | armv7m | [x] selftest 14/14 + stress + fault dump on silicon (2026-07-09); the `multi_wait` deadlock is resolved (item 5) |
| f411disco / f302nucleo | armv7m/M4 | [x] on silicon (2026-07-14): f411disco 14/14, f302nucleo 13/14 (test 11 = RAM cap) + stress + bench |
| picopi (RP2040) | armv6m | [x] selftest 14/14 + `sched_exit` on silicon (2026-07-09) |
| esp32-wroom / esp32c6-wroom | lx6 / rv32imac | [x] both on silicon (2026-07-09): 14/14 + fault dump; flashing brought up on each |
| microbit (nrf51) | armv6m | **not a physical target** -- a QEMU armv6m vehicle; covered by CI (`-M microbit`) |
| qemu / qemu-riscv | armv7m / rv32imac | [x] automated GitHub CI (item 6); the QEMU functional gate, not silicon |

## 3. Telemetry capture per board

With the observability subsystem (`docs/reference/telemetry.md`, `KICKOS_TELEMETRY=rtt`,
`KICKOS_CONSOLE=rtt|both`), record context-switch / syscall / IRQ traces on each
silicon board and archive the baselines. Confirms the trace clock + RTT wire format
hold on real hardware ahead of M2, and gives a pre-MPU performance reference to
measure the M2 `arch_mpu_apply` switch-hook cost against.

## 4. Context-switch + IRQ-entry bench baselines -- [x] DONE

The per-board bench table (bench app, `KICKOS_BENCH=ON`) is filled -- the full fleet is
recorded in `M1_state.md` section 3 (throughput on every target; per-switch cost +
IRQ-entry latency on every board with a cycle counter). Only K64F re-confirms under M2
SYSMPU (prior baseline 77 cyc / 641 ns on record). These are the M2 regression baseline.

## 5. Open bugs to resolve or track

- ~~XMC `multi_wait` deadlock~~ [x] resolved: XMC4800 runs the full suite on silicon
  (selftest 14/14 + stress + fault dump through the armed ring, 2026-07-09); spawned
  workers run while main is in a tickless timer-sleep. Closed -- no longer gates the XMC
  half of the M2 reference pair.
- ~~RX bench HW re-validation~~ [x] resolved: the tickless re-arm was a real one-shot-reset
  bug, fixed generically; RX runs on silicon and the 96cyc/400ns baseline is recorded.
- ~~microbit full selftest~~ [x] resolved: it exposed a real selftest leak -- raw-handle
  tests never destroyed their semaphores, so the suite overran microbit's 4-slot pool.
  Fixed the suite (every test reclaims its semaphore; peak concurrent = 4), NOT the pool.
  armv6m now runs the full selftest in CI. (The refusal to defer caught the bug -- see the
  no-deferral principle.)

## 6. GitHub Actions CI -- [x] DONE

`.github/workflows/ci.yml` runs the green gates on every push/PR (was hand-run).
Scoped to deterministic, host-runnable gates -- no flaky or HW-only steps:
- **Build matrix**: configure + build every board whose toolchain is apt-installable
  -- the ARM families + RISC-V -- catching build/link/linker-script regressions. Plus
  the `oot_export` in-tree/out-of-tree gate (runs in the sim ctest).
- **sim** (`-DKICKOS_ENABLE_SELFTEST=ON`): full `ctest` -- selftest, stress (incl. the
  thread-reclaim churn), `sched_exit`, `mpu_fault`, telemetry golden-vector. This is
  the authoritative deterministic gate.
- **qemu-arm** (`mps2-an386` + `microbit`): `ctest` -- the **full `selftest` TAP suite**
  (same binary as sim, on real armv7m PendSV) + hello + sched_exit + fp_switch. These
  clock off semihosting `SYS_CLOCK`, which QEMU <= 10 (ubuntu-latest ships 8.2) freezes
  while the core halts in WFI -- a timed sleep with every thread idle never wakes. Fixed
  in the kernel: `arch_idle_wait` spins (not WFI) on the QEMU-only mps2/nrf51 chips.
  (icount does NOT fix it -- verified.) `rr_interleave` scales its RR quantum to the
  target's clock granule, so it runs even on the coarse clock (no skip). **microbit
  (armv6m)** runs the full suite too -- its small pool (`MAX_SEMAPHORES=4`) forced the
  suite to be pool-honest (each test reclaims its semaphores), so armv6m is gated, not
  a silent hole.
- **qemu-riscv** (`virt`): `ctest` -- the **full `selftest` suite** (on rv32imac msip) +
  hello + sched_exit; never the stress soak (QEMU host-crashes flakily under heavy
  PMP-reprogram load; guest is clean -- see item 1). Uses the pinned **RISCStar**
  `riscv32-none-elf` toolchain (newlib, soft-float rv32imac/ilp32 multilib) -- the same
  tarball as local, downloaded + cached. Also build-checks `esp32c6-wroom` +
  `qemu-riscv-bench`. (mtime clock -> no WFI idle issue.)
- Toolchains: the **SAME pinned vendor tarballs as local** -- Arm GNU Toolchain (ARM) +
  RISCStar (RISC-V), both newlib -- downloaded and `actions/cache`-cached; the toolchain
  bin is put on `$PATH` so CMake's try_compile resolves the cross compiler. No apt
  `gcc-arm-none-eabi` / xPack.

RX and Xtensa use vendor toolchains (not in apt) -- kept out of CI for now (one step
at a time); build those boards manually. Silicon likewise stays a manual/self-hosted
step per the HW-test deferral policy.

**CI matches local (pinned vendor toolchains).** Both local and CI build with the same pinned
vendor tarballs -- **Arm GNU Toolchain** (ARM) and **RISCStar** (RISC-V), both newlib, joining
GNURX (RX) -- so the fleet is uniformly newlib on pinned toolchains everywhere. CI downloads +
`actions/cache`-caches them (validated in an `ubuntu:24.04` container). RX/Xtensa silicon stays a
manual/self-hosted step per the HW-test deferral policy.

## 7. Fleet uniformity -- make M1 a coherent standalone release

M1 is the first HW-support milestone and could ship as a public release on its own.
So M1 must be UNIFORM: every board the same shape, so M2's cost is purely MPU wiring
(not "and also finish the console on 8 boards"), and an external reviewer picking any
board gets identical behaviour. Non-M2 work that improves coherence is done HERE.

### Board readiness matrix

| Board(s) | arch / chip | Runs via | Selftest | Console |
|---|---|---|---|---|
| sim | host | native | 15/15 + stress + mpu_fault | ring + sync (in-tree ring CI) |
| qemu | armv7m / mps2 | QEMU | 15/15 | polled (semihosting) |
| qemu-riscv | rv32imac / virt | QEMU | 15/15 | polled (semihosting) |
| microbit | armv6m / nrf51 | QEMU | pass | polled (semihosting) |
| XMC4800 | armv7m / xmc4800 | silicon | 14/14 (HW 2026-07-09) + HARD FAULT dump | ring + sync |
| ESP32-WROOM | lx6 / esp32 | silicon | 14/14 (HW 2026-07-09) + XTENSA dump | ring |
| ESP32-C6 | rv32imac / esp32c6 | silicon | 14/14 (HW) + RISC-V TRAP dump | ring (UART0/CH343P; native USB-JTAG flash-only) |
| RX72M | rxv3 / rx72m | silicon | 14/14 (HW 2026-07-09; sw IRQ controller) | ring; console ttyUSB0/FT232 |
| f411disco | armv7m / stm32f411 | silicon | 14/14 (HW 2026-07-14) + all apps + HARD FAULT + LED + bench | polled (ext UART PA2) |
| blackpill | armv7m / stm32f411 | silicon | 14/14 (HW 2026-07-14) + bench | polled (ext UART PA2) |
| f302nucleo | armv7m / stm32f302 | silicon | 13/14 (HW 2026-07-14; test 11 = 4 K alloc > 16 K) + bench | ST-Link VCP |
| bluepill | armv7m / stm32f103 | silicon | 13/14 (HW 2026-07-14; test 11 = 4 K alloc > 10 K clone) | polled (ext UART PA9) |
| picopi | armv6m / rp2040 | silicon | 14/14 (HW 2026-07-09) | UART0/GP0 |
| bluepill-c8 | armv7m / stm32f103 | build-only (-st) | build-clean | build-only (20 K genuine variant) |
| K64F | armv7m / mk64f | prior silicon; M2 re-confirm | pass (2026-07-09 baseline) | ring + sync |
| due | armv7m / sam3x8e | RETIRED unit (port proven 2026-07-09) | -- (unit peripheral-I/O fault) | -- |

Every board has a `<board>-st` preset (base + `KICKOS_ENABLE_SELFTEST=ON`) so the
full 15-test suite is a first-class, per-board config -- not an ad-hoc `-D`. The
selftest/fault/mpu_fault apps are **diagnostic** (`kickos_add_diagnostic_app`, built
only under that flag: they use the test-only syscall surface / deliberately fault, so
they never enter a production image). Flash an `-st` build with
`FLASH_BUILD=build/<board>-st tools/flash.sh <board> selftest`.

### Console policy (decided)

Buffered IRQ-drained ring on any board with a real byte-FIFO UART + a device-IRQ receive
path. POLLED stays for the semihosting dev vehicles (mps2, virt, microbit) -- synchronous,
no TX-empty IRQ to drain. `arch_console_write` -> the ring; `arch_console_write_sync` ->
the polled writer (panic/fault path).

On the ring today: **K64F, XMC4800, ESP32-WROOM, RX72M, and ESP32-C6** -- all
silicon-validated (the C6 via its own UART0 peripheral-IRQ path added 2026-07-14, a
2048-byte ring). The STM32 fleet (f411disco, blackpill, f302nucleo, bluepill) and
picopi (RP2040) stay polled: real UART, `arch_console_write_sync` bounded per board, but
no IRQ-drained receive path wired onto the template yet (a driver-era, non-M2 task).

**Dependency:** the ring needs a working device-interrupt RECEIVE path per arch. ARM (NVIC)
+ RX (INTB) have it -> mechanical. The **ESP32-C6 now has one** (UART0 TX -> interrupt matrix
-> CPU int 30 -> `switch.S` `.Lextdev`); the RX `kickos_rx_default_irq` demux is still a stub.

### M1-uniformity gaps -- all closed
- Ring console generalised + re-validated on silicon (2026-07-09): RX72M and
  ESP32-WROOM (UART0) onto the K64F/XMC template, 14/14 + fault dump through the armed
  ring; ESP32-C6 also on the ring (its own UART0 peripheral-IRQ path, silicon-validated
  2026-07-14). The STM32/RP2040 fleet stays polled by policy (see Console policy above).
  sim ring done (2026-07-09): a synthetic TX peripheral (SIGUSR1-delivered TX-empty line
  + per-delivery slot budget) arms the buffered path in-tree, so ctest now exercises the
  SPSC ring/drain/wrap/overflow paths the HW-gated MCU backends never run in-tree.
- RX72M selftest 14/14 on silicon (2026-07-09): DONE via the arch/rx software IRQ
  controller (SWINT2 doorbell); console captured on ttyUSB0/FT232. Closed.
- ESP32-C6: full selftest confirmed on silicon -- all 14 PASS (HW), captured over the
  board's CH343P UART0 bridge (ttyACM0) after the console was rerouted off the native
  USB-Serial-JTAG (which re-enumerates on reset and drops boot output). Two real bugs
  fixed en route: the console reroute, and moving the inject-doorbell config out of the
  vestigial INTPRI block into the real PLIC (0x2000_1000). Diag-LED done (WS2812B on
  GPIO8 via RMT, panic heartbeat). The earlier INTPRI-threshold suspicion was disproven.
- Build-only boards: build-clean incl. the -st (selftest) presets; on-silicon deferred
  until units are at disposal.

## Then: M2 proper

Per `reference/architecture.md`: MPU-enforcement fan-out -- reference pair (RISC-V PMP/NAPOT +
XMC v7-M PMSA) -> K64F SYSMPU -> RX -> tail; plus the arch-independent security model
(domains, per-thread private stacks, syscall-arg/user-pointer validation, pow2 region
placement). Capabilities + authenticated grants are M3.

### M2 enforcement fan-out -- status (fleet sync-point, in progress)

Goal: full selftest (17/17) under enforcement on every MPU-capable chip -- an
M2 sync-point mirroring the M1 on-silicon validation pass.

Shared mechanism (done): the arch-independent pieces are `arch_domain_static_regions`
(kernel/domain), `kickos_ranges_init` + the linker copy/zero tables
(arch/common/startup_ranges.cc; a chip's `.ld` emits the tables and a pow2 `.appdata`
block grouping objects under `user/`), and `arch_mpu_probe_addr` (a kernel-side guard
word; arch/common/arch_ram_common.cc). Errno-class per-thread state is NOT in the
shared app-data region: it grants per-thread (the stack now, TLS at M3).

| Chip | MPU flavour | State |
|------|-------------|-------|
| K64F (frdmk64f) | SYSMPU (bus, byte-granular) | **DONE -- silicon 17/17** (SRAM/domain isolation). Two HW-only bugs fixed: RGD0 supervisor SM (`=same-as-user`) and SRAM_L-via-code-bus (M0) master. **Peripheral gating resolved on silicon (k64drv, Stage 2): SYSMPU does NOT gate peripherals; the AIPS bridge does (per privilege+master, per 4 KB slot, NOT per-thread). So per-thread peripheral isolation is not achievable on K64F -- see `reference/architecture.md` Memory domains.** |
| XMC4800 | v7-M PMSA | **DONE -- silicon 17/17.** |
| rp2040 (picopi) | v6-M PMSA (M0+, 8 regions) | Build + enforcement-link validated (reuses the XMC PMSA backend). **HW pending** (flash via BOOTSEL). |
| STM32F411 (f411disco/blackpill) | v7-M PMSA | Build + enforcement-link validated. **HW pending** (ST-Link). |
| RX72M | RX MPU (UM sec.17) | **DONE -- silicon 20/20** (enforcement selftest, 2026-07-17) **+ mpu_fault cross-domain trap** (a user write to another domain raises the access exception, vector 0x54, decoded via MPESTS/MPDEA -> "MPU FAULT: task 'domainA'"). arch_mpu_apply (MPBAC=0 user background + 8 RSPAGEn/REPAGEn regions; RX checks user mode only, so no supervisor-field hazard) reprograms live from the timer ISR on every preemptive switch, glitch-free (UAC R/W/X order, inclusive REPAGE end, supervisor-never-checked, MPEN-latch-on-RTE all confirmed). The test-4 wedge that blocked this was NOT an MPU bug: a hardcoded 8-byte alignment guard on the clock_now out-pointer (`kernel/syscall/syscall.cc`) rejected RX's 4-byte-aligned u64 stack local, so kos_clock_now returned 0 and the rr_interleave granule spin hung -- fixed with `alignof(uint64_t)` (a pre-enforcement regression from 9d7ffa6, untested on RX). |
| STM32F302 (f302nucleo) | v7-M PMSA | **Links again (provisioned), enforcement deferred on RAM.** g_instance sized 12288->6080 B (KICKOS_STACK_POOL_ALIGN=16 + smaller sem/irq pools) so the 16 KiB part builds; arena is 3712 B. Not a viable enforcement target: even the conditional app-data block + a 4 KiB-alloc selftest test exceed that arena. Non-enforcement only for now. |
| ESP32-C6 | RISC-V PMP/NAPOT | **DONE -- silicon 18/18 selftest under enforcement + mpu_fault cross-domain trap (2026-07-17). The earlier boot-loop was an elf2image RAM-only-header flag error, NOT a code bug.** (a),(b) RESOLVED: `esp32c6.ld` now carries the same `#if KICKOS_HAVE_MPU` block as `virt.ld` -- a pow2 NAPOT code region at ORIGIN (0x4080_0000 is 8 MiB-aligned, so 128K code + 32K appdata are naturally NAPOT-aligned) + the `.appdata`/`.appbss` block + the Reset_Handler appbss zeroing; `user/` objects build `-msmall-data-limit=0`, so `-DKICKOS_HAVE_MPU=1` links all apps clean with app globals confirmed landing in `.appdata`/`.appbss` (verified by nm/objdump). The full-C++ default `_appdata_size` is 32K (holds a 16K heap + libstdc++/unwind state, ~18K measured), so cxxtest links under enforcement with NO manual `-DKICKOS_APPDATA_SIZE`. (c) The single-code-region approach is architecturally sound: C6 TRM Table 1.4-1 puts HP SRAM in ONE unified Instruction/Data region (0x4000_0000..0x4FFF_FFFF -- one address for fetch AND load/store, unlike the classic-ESP32 split IRAM/DRAM), so one RX NAPOT region covers U-mode fetch. The U-mode PMP trap FIRES correctly from SRAM -- PROVEN on silicon (18/18 selftest under enforcement + mpu_fault: a U-mode cross-domain store faults mcause=7 and is reported). (d) **PERIPHERAL side (deferred, follow-on -- NOT needed for SRAM enforcement + the selftest): the C6 has a SECOND, bus-side permission unit -- APM/PMS (C6 TRM Ch.16 PMP-APM Management), Access Permission Management -- that defaults DENY-USER on peripheral targets, independent of PMP.** PMP discriminates peripheral access per-thread (CPU-side), but APM must be given a one-time global open before any unprivileged peripheral access succeeds; it is not per-thread. So a C6 userspace driver needs BOTH the PMP grant (per-thread) AND the APM open (global). The APM layer is currently undriven -- scoped in `docs/design-c6-driver.md` (GPIO-blink, an 8 B PMP window + a one-time APM REE0 open). |
| microbit/nRF51, STM32F103, ESP32 LX6 | no per-task MPU | N/A -- `arch_mpu_probe_addr`/`arch_domain_static_regions` return 0; no app-data block. |

Conditional-appdata infra: **DONE.** The chip linker scripts are cpp-preprocessed
(arch/CMakeLists.txt) so the `.appdata` block, its grouping, the app copy/zero-table
entries, and the overflow ASSERT are `#if KICKOS_HAVE_MPU`. A non-enforcement build
reserves nothing and keeps app globals in the kernel `.data/.bss` (zero-overhead);
the startup uniformly calls `kickos_ranges_init` (one range when off, two when on).
Verified fleet-wide: default builds show 0 app-data refs in the generated script,
enforcement builds keep the block; XMC re-flashed 17/17 through the preprocessed .ld.

Per-board `_appdata_size` default: the full-C++ silicon chips (rp2040, esp32c6, rx72m)
and qemu-virt default `_appdata_size` to **32K** under `KICKOS_HAVE_MPU` -- enough for a
16K libc heap (`KICKOS_HEAP_SIZE`, set in each board_config) + libstdc++/unwind writable
state + the RISC-V gp small-data window (~18K measured on all three). So `-DKICKOS_HAVE_MPU=1`
links every app **including cxxtest** with NO manual `-DKICKOS_APPDATA_SIZE`; a tight
freestanding demo can still override the size down. imxrt1062 (teensy, MPU deferred) keeps a
128K default and fail-closes its base alignment (`ALIGN(_appdata_size)`).

**Scope of the cxxtest evidence (honest):** the committed cxxtest spawns an UNPRIVILEGED worker
(`kos::thread::spawn(cxx_worker, ..., privileged=false)`, 8K app-arena stack) that throws/catches/
unwinds under the MPU. On qemu-riscv (rv32imac PMP) this is RUN-PROVEN: `qemu_riscv_cxxtest` is
ALL PASS from the U-mode worker -- a confined unprivileged throw under PMP, not merely
layout-verified. On the five silicon arches `-DKICKOS_HAVE_MPU=1` LINKS the now-U-mode cxxtest
clean, but the standing ALL-PASS silicon logs were the OLD privileged-root cxxtest = the runtime
COEXISTS with enforcement active; a bench re-flash of the now-U-mode cxxtest is the outstanding
silicon U-mode proof. Separately, the U-mode enforcement proven on silicon stands: selftest
domain-isolation + `mpu_fault` (U-mode workers, e.g. C6 18/18) and the per-thread peripheral
drivers (xmcspi/rxdrv/c6blink) -- see the fan-out table above.

### Driver era / peripheral-isolation status (2026-07-16)

The first unprivileged userspace drivers landed on top of M2 enforcement. This is
"anytime coherence" (a userspace driver has no MPU dependency the enforcement backends
did not already provide), but it stresses the peripheral-MMIO side of the isolation model
and surfaced the fleet's per-thread-peripheral ceiling.

**MMIO-grant mechanism (task #9): LANDED + committed.** `kos_thread_params.mmio_base/
mmio_size` (Option A grant-at-spawn), the `arch_mpu_region_encodable(base,size)` arch seam
(exact-cover, no rounding), `thread_spawn` boundary validation (privileged-only, no-wrap,
encodable), and `domain_for` appending the MMIO region as a never-shared capability. Also
closed a Critical: an unprivileged caller's `mem_base` grant is now arena-bounds-checked
(was a peripheral/kernel-SRAM self-grant escalation). See `design-task9-mmio-driver.md`.

**Fleet per-thread peripheral-isolation matrix** (the headline finding -- full table +
CPU-side-vs-bus-slave-side principle in `reference/architecture.md` Memory domains, worked
bring-up in `book/peripheral-isolation-and-the-hardware-ceiling.md`):

| Chip / unit | Per-thread peripheral isolation | Evidence |
|---|---|---|
| XMC4800 -- ARM v7-M PMSA (CPU-side) | **YES** | silicon-proven (xmcspi, 2026-07-17): a granted USIC DEV window works + an ungranted SCU poke faults MemManage. (Some USIC config registers are PV-write-only at the bus -- a kernel-vs-user privilege split under the PMSA, not a per-thread gate.) |
| RISC-V PMP (ESP32-C6) | **YES** by PMP (SRAM enforcement silicon-proven) | PMP discriminates per-thread -- SRAM enforcement DONE on silicon (18/18 + mpu_fault); a SEPARATE APM/PMS bus unit defaults deny-user (one-time global open), still needed for per-thread PERIPHERAL isolation (follow-on: `docs/design-c6-driver.md`) |
| RX72M -- RXv3 MPU (CPU-side) | **YES** | SRAM/domain enforcement DONE on silicon (2026-07-17: selftest 20/20 + mpu_fault cross-domain trap); a real granted peripheral window not yet run on silicon (task #3) |
| K64F -- SYSMPU (bus-slave-side) | **NO** | silicon-proven: SYSMPU does NOT gate the AIPS peripheral bridge; the AIPS PACR does (per privilege+master, per 4 KB slot, all-user once opened) -- no per-thread peripheral boundary |

**Per-board driver status (truthful):**
- **k64drv (K64F, PIT):** DONE on silicon -- the first unprivileged MMIO driver; it is what
  ANSWERED the SYSMPU-vs-AIPS question above (SYSMPU inert for peripherals; AIPS gates,
  coarse). Also added a weak `arch_fault_report_extra` hook (K64F decodes SYSMPU CESR/EARn/
  EDRn + BusFault). `user/apps/k64drv/`.
- **xmcspi (XMC4800, USIC0-CH1 SSC loopback):** DONE on silicon (2026-07-17) -- the CANONICAL
  per-thread PMSA MMIO-isolation proof: a granted 512-byte USIC DEV window does an internal SSC
  loopback (4 words tx==rx) AND an ungranted SCU poke faults MemManage (CFSR=0x82,
  MMFAR=0x50004648), per thread. Internal loopback, no jumper. The USIC CCR/FDR/BRG are
  PV-write-only at the bus (RM Table 18-20) so interrupt-enable+config is privileged bring-up --
  a K64F-AIPS-like bus-privilege layer sitting UNDER the PMSA proof. `design-spi-driver.md`,
  `user/apps/xmcspi/`.
- **rxdrv (RX72M, PORT8/LED6 GPIO):** DONE on silicon (2026-07-17) -- per-thread peripheral
  isolation on the RX MPU: a granted 16-byte PODR window blinks LED6 AND an ungranted PORT8.PDR
  poke faults ("MPU FAULT: task 'rxdrv' attempted read at 0x8c008"). First real granted
  peripheral window on RX. `user/apps/rxdrv/`.
- **f411spi (STM32F411, SPI1 loopback):** BUILT + fable-reviewed, silicon-validation PENDING a
  bench swap to the 32F411E-DISCO. Redundant with xmcspi for the PMSA proof (both ARM v7-M PMSA);
  kept as the STM32-family reference. `design-spi-driver-stm32f411.md`, `user/apps/f411spi/`.
- **k64dspi (K64F, DSPI0 for the KickCAT ESC SPI PDI):** DONE on silicon (2026-07-17): 4-word
  SOUT->SIN loopback tx==rx over the AIPS-opened slot, blocking on the DSPI0 EOQ IRQ with the
  auto-rearm API (no explicit ack). Designed WITHIN the K64F ceiling: the DSPI window grant is
  documentation, not enforcement (AIPS opens the slot to all user code); the microkernel invariant
  is kept (driver in userspace) and the coarse per-slot ceiling is accepted, documented. Brief:
  `design-spi-driver-k64f-dspi.md`. (This is the transport the KickCAT-on-K64F plan builds on --
  `design-kickcat-k64f.md`.)

**Canonical per-thread PMSA silicon proof: DONE (xmcspi on XMC4800, 2026-07-17).** A real granted
peripheral window works AND an ungranted peripheral access faults MemManage, per thread, on PMSA
silicon -- the fleet's former one honest peripheral-isolation gap is closed. Per-thread peripheral
isolation is now silicon-proven on all three CPU-side units: PMSA (xmcspi), RX MPU (rxdrv), and
RISC-V PMP (c6blink). f411spi remains a build-only STM32-family reference (the disco is absent).

## M3 (later)

Capabilities + authenticated grants (the seL4-principled object model). **Also M3:
user-selectable CPU clock / low-power mode** -- once the fleet-wide "max frequency by
default" baseline is confirmed (the M1 clock audit: `M1_state.md` + the clocks section
of `TODO.md`), expose a per-chip clock
select so an app can trade speed for power. Depends on each chip having an explicit
clock bring-up (not just inheriting the ROM/reset default) and a truthful CPU/timer
Hz that the scheduler's ns<->tick math tracks when the clock changes.
