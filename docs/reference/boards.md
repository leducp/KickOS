<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS board support

Status of every board target: what works, what only builds, how to flash it, and
where its console + LED live. Every flash recipe lives in [flashing.md](../flashing.md).

## This file is the status of record -- code must not restate it

**Validation status lives here and nowhere else.** Whether a board has run on silicon, what it
has been proven to do, and what is still only built are tracked in the matrix below -- not in
code comments, not in `mpu.cmake`, not in a preset `displayName`. A status claim written into
code goes stale silently the moment work lands: nothing compiles it, no test reads it, and the
person who fixes the thing never thinks to grep for prose about it. That is not hypothetical
here -- stale `SILICON-PENDING` / `BUILD-ONLY` markers left in `mpu.cmake` and `CMakePresets`
survived the work that invalidated them and misled two separate audits into reporting
`teensy41` as an unvalidated build-only port months after its enforcement selftest passed on
hardware.

So: code that genuinely needs to mention status carries a **pointer** and no claim --
`validation status: see docs/reference/boards.md` -- and a fixed pass count (`14/14`) is never
written into a build file. What *does* belong in code is a **durable technical fact**: the
Teensy exposes no SWD header, the MKL02 holds the K64F bootloader, the nRF51 has no SysTick,
a baud constant is formula-derived rather than measured. Those are properties of the part, not
of our progress, and they do not drift.

Deeper evidence behind any row: [`../m2-readiness.md`](../m2-readiness.md) is the enforcement
ledger (which chip is proven to enforce, and by what evidence), and each `../design-*.md`
record carries its own status header (see [`../design/README.md`](../design/README.md) for the
marker taxonomy). `../../TODO.md` is the live task list. Where those and this file disagree, the
code wins, then this file.

## Status matrix

| Board (preset) | SoC / core | LED (blink) | Console | Flash tool | HW-validated |
|---|---|---|---|---|---|
| `sim` | host process | -- | host stdout | `ctest --preset sim` | [x] CI |
| `qemu` | mps2-an386 / M4 | -- | semihosting | `ctest --preset qemu` | [x] CI, plain and under PMSAv7 enforcement (the ARM runtime enforcement gate -- see *CI coverage* below) |
| `qemu-m33` | mps2-an505 / M33 | -- | semihosting | `ctest --preset qemu-m33` | [x] CI, plain and under **PMSAv8** enforcement incl. full-C++ `cxxtest` + the `mpu_fault` MemManage trap |
| `qemu-m7` | mps2-an500 / M7 | -- | semihosting | `ctest --preset qemu-m7` | [x] CI, plain and under PMSAv7 enforcement (16 MPU regions) |
| `qemu-m3` | mps2-an385 / M3 | -- | semihosting | `ctest --preset qemu-m3` | [x] CI, plain and under PMSAv7 enforcement (soft-float; no FP switch path) |
| `microbit` | nRF51822 / M0 | -- | semihosting | `ctest --preset microbit` | [x] CI (armv6m run gate; the fleet's only measured expected-skip list -- see *microbit* below) |
| `qemu-riscv` | QEMU virt / RV32IMAC | -- | semihosting | `ctest --preset qemu-riscv` | [x] CI (first RISC-V) |
| `esp32c6-wroom` | ESP32-C6-WROOM-1 / RV32IMAC | GP8 (WS2812B, LED2) | UART0, GP16/GP17, 115200 -> CH343P VCOM (`/dev/ttyACM0`) | esptool | [x] full selftest + PMP NAPOT enforcement + `mpu_fault` trap + diag-LED + bench; the `c6blink` granted-GPIO window is the canonical per-thread PMP proof. **Second board with an UNPRIVILEGED root, and the first on RISC-V PMP** (2026-07-28) -- see *Unprivileged root* below. **Multiple physical units exist, and the 2026-07-28 pass was luck-dependent**: `esp32c6.ld` linked `.data` with an LMA outside every loaded segment, so `Reset_Handler` copied uninitialised SRAM over correctly-placed `.data`. Whether that corrupted anything load-bearing varied by die and power-on history. Fixed 2026-07-30 and pinned by an `ASSERT` (`arch/riscv/chip/esp32c6/esp32c6.ld:280`), and the post-fix re-witness closes the owed `c6blink` mux-write arm -- see *M4.5.6* below |
| `esp32-wroom` | ESP32-D0WD / Xtensa LX6 @240 MHz | GP2 (D2, active-high) | UART0, GP1/GP3, 115200 -> CH340 (`/dev/ttyUSB1`) | esptool | [x] 8/8 apps incl fault dump + bench |
| `rx72m` | RX72M / RXv3 @240 MHz | P80 (LED6, active-low) | SCI6 ASC, PB1/PB0, 115200 -> FT232 (`/dev/ttyUSB0`); ring | `rfp-cli` (Renesas Flash Programmer) | [x] full selftest + stress + `RX EXCEPTION` dump (2026-07-09); RX-MPU enforcement selftest + `mpu_fault` cross-domain trap + `rxdrv` granted peripheral window (2026-07-17); DPFPU switch + bench. **Fourth board with an UNPRIVILEGED root, and the only one on the RX MPU** (2026-07-28) -- see *Unprivileged root* below. Re-witnessed 2026-07-30 at a clean `270b6fa`, closing the owed stage-4 `rxdrv` mux-write arm and the M4.5.5 granular-shaping debt in one visit -- see *M4.5.6* below. **No CI gate** -- see *CI coverage* below |
| `xmc4800-relax` | XMC4800 / M4F | P5.9 (LED1) | USIC0 ASC, P1.5/P1.4, 115200 -> VCOM; + RTT | onboard J-Link | [x] full selftest + stress + `HARD FAULT` dump (2026-07-09, 144 MHz); PMSAv7 enforcement selftest + `mpu_fault` cross-domain trap + the `xmcspi` granted-USIC window (2026-07-17) -- the canonical per-thread PMSA proof; console handover to a userspace driver, panic-path reclaim and clock retune all silicon-passed. **First board with an UNPRIVILEGED root** (2026-07-27) -- see *Unprivileged root* below |
| `f411disco` | STM32F411 / M4F | PD12 (LD4 grn) | USART2, PA2/PA3, 115200 (ext adapter) | onboard ST-Link (`st-flash`) | [x] full selftest + all apps + fault dump + bench + LED; **PMSAv7 enforcement silicon-witnessed 2026-07-29** -- enforcement selftest 62/62 + `mpu_fault` cross-domain MemManage denial, closing the `stm32f411` MPU HW debt for the chip. **Fifth board with an UNPRIVILEGED root, and the second on PMSAv7** (2026-07-29) -- see *Unprivileged root* below |
| `blackpill` | STM32F411 / M4F | PC13 (active-low) | USART2, PA2/PA3, 115200 (ext adapter) | USB-DFU / SWD | [x] full selftest + bench (2nd F411; 25 MHz HSE); MPU backend is the shared `stm32f411` one, silicon-witnessed on `f411disco` 2026-07-29 (not re-run on this board) |
| `f302nucleo` | STM32F302R8 / M4 | PB13 (LD2 grn) | USART2, PA2/PA3, 115200 -> ST-Link VCP | onboard ST-Link (`st-flash`) | [x] `hello` + `stress` on silicon 2026-07-29 (`stress` at the tip `9ba4e4b`) -- the fleet's first captures on optimised code; re-run on silicon 2026-07-30 (`selftest` before and after a provisioning right-size, `ringpriv`, `ringppb`, `fault`). The BENCHMARK figures still date to 2026-07-14 and no bench run was taken at either later tip. **The suite needs the `-st` provisioning here:** `63 ok / 0 not ok / 5 skipped` at it after the 2026-07-30 right-size (`62 / 1 / 9` before it), and `17 / 42 / 10` at the application profile, every failure a resource refusal. The older "selftest minus the 4 KiB-alloc test" record dates to 2026-07-14 and predates M4.5.2's static growth. Full captures: *`f302nucleo` on silicon* and *M4.5.6* below. **Not an enforcement target -- the F302R8 (`x8` line) has no MPU** (the F302xB/xC line does). **A bench board** (onboard ST-Link, own VCOM, no external adapter), and the fleet's only physically-present **no-MPU ARM** board -- the sole possible silicon witness for the privilege-ring arm, and it TOOK that witness 2026-07-30 (`ringpriv`, `PASS (5 arms)`); see *Unprivileged root* below. **No AUTOMATED gate of any kind** -- no CTest and no QEMU run gate, though the ring property is machine-checked elsewhere; see *CI coverage* below. **One OPEN silicon bug: the fault reporter produces no dump on this board** -- see *M4.5.6* below |
| `picopi` | RP2040 / M0+ | GP25 | UART0, GP0/GP1, 115200 | `picotool` (BOOTSEL) | [x] LED + UART0 + full selftest with `sched_exit` (2026-07-09, 125 MHz PLL); PMSAv6 cross-domain denial silicon-proven 2026-07-19 (M0+ has no MemManage -- it escalates to HardFault) -- the fleet's only armv6m enforcement proof; U-mode `cxxtest` still awaits a bench re-flash |
| `bluepill-c8` | STM32F103C8 / M3 (64 K/20 K genuine) | PC13 (active-low) | USART1, PA9/PA10, 115200 | external ST-Link (SWD) | (!) build-only, and **no unit exists** -- there is no genuine F103C8 on the bench, so nothing here can be silicon-witnessed at all (64 K/20 K linker; links the full app set incl selftest + stress) |
| `frdmk64f` | MK64FN1M0 / M4F | PTB22 (RGB red, active-low) | UART0, PTB16/PTB17, 115200 -> OpenSDA VCOM | J-Link (OpenSDA) | [x] HW 2026-07-15 (full selftest over the buffered console ring, 120 MHz); SYSMPU enforcement + `mpu_fault` trap silicon-proven at M2. **Sixth board to witness an UNPRIVILEGED root, the only one on SYSMPU, and the only one witnessed on its FULL service list** (2026-07-29; re-taken 2026-07-30) -- see *Unprivileged root* below |
| `teensy41` | i.MX RT1062 / M7 @396 MHz | -- (none wired) | LPUART6 ("Serial1", pins 0/1), 115200 | `teensy_loader_cli` (HalfKay, `.hex`) | [x] full selftest + soak under PMSAv7 enforcement, after the M7 anti-speculation fix (ERR011573; `../design-teensy-mpu-hang.md`) |
| `pizero2350` | RP2350 / M33 @150 MHz (armv7m backend) | -- (none on the Pi-Zero header) | UART1, GP4/GP5, 115200 | `picotool` (BOOTSEL) | [x] full selftest under PMSAv8 enforcement + `mpu_fault` cross-domain MemManage denial + bench/soak. **Third board with an UNPRIVILEGED root, and the first on PMSAv8** (2026-07-28) -- see *Unprivileged root* below. Also the first silicon witness for `kos_reboot` (BOOTSEL handover) and for `KICKOS_SHUTDOWN_TO_BOOTLOADER` on both terminal dead-ends -- see *Terminal dead-ends and BOOTSEL handover* below |

**"Full selftest" rather than N/N.** The TAP suite emits its own plan line (`1..N`) and a closing
`# all tests passed`, and the gates key off *those*, never a number written down here
(`tests/check_qemu_selftest.sh`). N is not a constant: the suite registers ~56 tests on the sim
today and the total moves with `KICKOS_HAVE_MPU` and `KICKOS_ENABLE_SELFTEST`, each of which
compiles in tests that cannot run without it (the IRQ suite, the enforcement bound-checks).
Where a dated silicon record below still names a count -- "14/14", "17/17",
"43/43" -- that is the plan size *on the date of that run*, not a target to reproduce. Run the
board's `-st` preset and read the plan it prints.

**Retired from the available-hardware list:** `due` (AT91SAM3X8E / M3). The SAM3X port
was validated on silicon 2026-07-09 (selftest 14/14, 84 MHz PLL), but *this physical
unit* developed a peripheral-I/O fault (2026-07-14): core + flash-controller + native
USB (SAM-BA) all work -- verified -- but the PIO output (PB27 "L" LED) will not toggle and
the UART console is dead (0 bytes), even under a provably-correct bare-metal blink flashed
via two independent paths. A correct program driving a pin that won't move is hardware,
not software. It was likely marginal all along (the crystal-startup margin is a documented
`GUESS`), landing on the good side once. The **port** stays proven; the **unit** is not a
reliable target. `due` still builds; it is just no longer bench/HW-tested.

Console pins are TX/RX in that order. STM32/XMC flash base is `0x08000000`; K64F is
`0x00000000`; the Due's `bossac` handles the offset itself.

On both ESP32 boards the console is UART0 through the on-board USB-serial bridge
(`esp32c6-wroom` = CH343P on `/dev/ttyACM0`; `esp32-wroom` = CH340 on `/dev/ttyUSB1`).
The ESP32-C6's native USB-Serial-JTAG is flash-only here -- it re-enumerates on reset
and gates on CDC host-drain, so app/boot output is dropped; UART0 does not.

### Per-board caveats (know before you trust it)

- **`bluepill` (32 KiB/10 KiB low-density clone) -- retired.** The clone's 32 KiB flash
  fit only `hello`/`blink` (~2 KiB spare); `selftest` (~55 KiB text) and `stress` never
  fit, so it could not run the self-verifying suite or any driver -- a maintenance burden
  with no test value. Use the genuine 64 KiB/20 KiB `bluepill-c8` for STM32F103 coverage
  (same `stm32f103` chip backend; links the full app set incl `selftest`/`stress`). The
  F103 port was HW-proven on the clone silicon (2026-07-14, selftest 13/14) before retirement.
- **`bluepill-c8`** -- build-only **because no unit exists**: there is no genuine 64 KiB/20 KiB
  F103C8 here, and the F103 port was physically run only on the now-retired 10 K clone. So its
  unwitnessability is hardware absence, not a verdict about the part -- and it costs no coverage,
  since `f302nucleo` is the same class (64 KiB-flash armv7m, no MPU, real privilege ring) and is on
  the bench. Links the full app set.

  **MODEL PREDICTION, not a witness: `bluepill-c8` fails `hello`'s second spawn by 96 bytes.** The
  board can never be flashed, so this is arithmetic and stays arithmetic. Arena 6,560 B, read with
  `arm-none-eabi-nm` on the `hello` ELF at `9ba4e4b`; idle 512 and root 2,048 leave **4,000** against
  the **4,096** two 2,048-byte stacks need (`boards/bluepill-c8/configs/base/defconfig:9`,
  `:10`, `:11`; every figure is a multiple of the 32-byte no-MPU granule, so alignment costs nothing
  here). The cause is the **heap carve**, not the part: 8 KiB `.userheap`
  (`arch/arm/chip/stm32f103/stm32f103.ld`) where `f302nucleo` now takes 2K, and the C8 has 4 KiB *more*
  SRAM. The model is the one in `porting.md`'s `## Minimum hardware requirement` section; it
  predicted all three `f302nucleo` silicon outcomes correctly (see *`f302nucleo` on silicon* below),
  which is the whole basis for quoting a number for a board nobody can run.
- **`due`** -- **retired** (see the table note above): SAM3X port proven 2026-07-09, but
  this unit now has a peripheral-I/O fault.
- **`frdmk64f`** -- **HW-revalidated 2026-07-15** (OpenSDA J-Link): full selftest streamed
  in-order over the buffered console ring; bench re-confirmed on silicon at **77 cyc / 641 ns**
  per switch (=> 120 MHz PLL) and 160 cyc / 1333 ns IRQ-entry; fault-dump path verified
  (UsageFault UNDEFINSTR escalated to HardFault, on PSP). Connect-under-reset was needed only
  once, to displace unknown prior firmware -- a KickOS-flashed board reflashes with a **plain
  connect** (KickOS leaves the SWD pins PTA0/PTA3 alone). Its distinguishing feature -- the
  **SYSMPU** -- is the M2 enforcement backend, and it signed off there. The kernel
  diagnostic LED is the onboard RGB **red, PTB22, active-low**
  (`arch_diag_led_init`/`_set`, `arch/arm/chip/mk64f/chip_mk64f.cc:767-787`), and
  `arch_pinmux_set` refuses PTB22 along with the console pins PTB16/PTB17 so a board
  map cannot steal it. The board pin map additionally muxes PTB21 (blue) as plain GPIO.
- **`teensy41`** -- the fleet's only **Cortex-M7**, and the M7 is the one core here that
  speculates. Under enforcement a dropped (non-pow2) whole-arena grant leaves a privileged
  thread on the PRIVDEFENA background, which types the whole 1 GiB FlexSPI/SEMC aperture as
  Normal -- so the core prefetched past the populated 8 MiB into an AHB slave that never
  responds and stalled forever with **no fault** (NXP ERR011573). Fixed by a chip fixed-MPU
  table that wraps the unbacked apertures as Device + XN + no-access, programmed *before* the
  I-cache is enabled. Runs `SystemCoreClock` at the ROM-default **396 MHz** -- the 600 MHz
  CCM/PLL tree is a follow-up, not a regression. Full story: `../design-teensy-mpu-hang.md`.
- **`f411disco` shares SPI1's PA5/PA6/PA7 with the onboard L3GD20 / I3G4250D gyro**, and the
  gyro's SDO sits on PA6, which is MISO. Its chip-select is **PE3**. Anything driving SPI1 here
  must preset PE3 HIGH (GPIOE output, through the pinmux seam) before any SCK activity, so the
  gyro stays deselected and its SDO tri-stated off MISO. Confirmed against the UM1842 pin table;
  `f411spi` does exactly this in root. A gyro fighting for MISO with PE3 high means the preset
  did not take, not a wiring fault.
- **`pizero2350`** -- Cortex-M33, but it reuses the **armv7m** arch backend verbatim (armv8-M is
  a superset for the switch/NVIC/SVC/PendSV path); only the MPU differs, and PMSAv8 has its own
  backend (`base+limit` RBAR/RLAR + MAIR, compile-gated so the v7-M/v6-M fleet is byte-identical).
  Console is **UART1 on GP4/GP5** -- UART0's pins are not brought out on the Pi-Zero header.
  BOOTSEL-recoverable, so a bad clock or boot-block config cannot brick it.
- **`xmc4800-relax` and `frdmk64f` under enforcement now print TAP through their
  userspace UART driver** (2026-07-27), so the `-DKICKOS_SERVICE_LIST=kickos_services_none`
  workaround is no longer needed to get a verdict. The old symptom was a banner,
  `[xmcuart|k64uart] driver up`, a stray `x`, then silence: the TAP harness wrote to the kernel
  console, which `console_emit` drops once the UART is `USER_OWNED`, and the `x` was `cap_index0`
  asserting stdout was *not* published. Both were fixed at the source (publish-aware harness,
  publish-aware `cap_index0`), and each run now names its own transport:
  `# tap route: stdout endpoint -> console driver (service list published)`. Measured with the
  **default full service list** on both boards: 59 cases, 58 `ok`, 1 skip, 0 fail. The skip is
  `mutex_deadlock # SKIP pool too small` on both, a genuine `KICKOS_MAX_THREADS` constraint.
- **An app's own diagnostics are still dropped if it uses `kos_print` on a published board** --
  same `console_emit` drop, and it is not visible in the TAP stream because the harness has its own
  writer. This is why `apps/mpu_fault`'s `[domain]` lines were absent from every service-list
  silicon capture, leaving the fault marker with nothing to check it against. Diagnostic apps should
  use `kickos::emit` (`user/include/kickos/sys/emit.h`); the `k64dspi` and `xmcssc` client apps use
  `kos::print`, so everything they report is RTT-only.
  **The bench rule that follows**, and it is narrower than "app output is invisible": on a board
  whose console is published, output through a `kos_print` / `kos::print` writer is gone from the
  VCOM, so *that* kind of line going quiet after console-up is no evidence a service stopped. The
  publish-aware writers are unaffected -- `kickos::emit`, the TAP `emit`, and libc `_write`, so
  `printf` and `std::cout` do reach the published driver. For a `kos_print`-based app, build
  `-DKICKOS_CONSOLE=both` and capture RTT, or halt the target and read the peripheral's registers.
  All three `xmc4800-relax` variants (`base`, `st`, `flat`) state `CONFIG_CONSOLE_BOTH=y` in their
  defconfig, plus `sim-telem` and `qemu-telem` which are not boards; every other board preset
  carries neither, so on those it has to be passed on the configure line, and a VCOM-only capture
  is the default a bench run falls into.
- **`kpanic_enter`'s UART reclaim clips bytes the userspace driver had in flight.** Reproducible on
  `xmc4800-relax`: the report always reaches the wire (that is the point of the reclaim), but roughly
  the last 8 bytes queued by the polled TX writer are garbled, eating the tail of the line before the
  dump. Announce-before-poke lines should therefore not be the *only* record of an address.
- **RTT backend** -- generic and wired on the XMC (`KICKOS_CONSOLE=both`); the
  UART VCOM path is the one confirmed on hardware.
- The diagnostic LED is a kernel-owned facility (`kdiag_led_*`, over the chip's
  `arch_diag_led_*`); a chip with no known LED (`qemu`, `microbit`, `teensy41`,
  `pizero2350`) links the no-op fallback (`arch/common/arch_diag_led_set_default.cc`) and the LED
  silently does nothing -- not a failure.
  `blink` is built for every board regardless, so on those it is a legitimate no-op.

### `f302nucleo` on silicon -- the suite passes at the selftest provisioning

Four captures, 2026-07-29, over the ST-Link VCP on `/dev/ttyACM0`. They are the fleet's
first silicon witnesses on **optimised** code (`MinSizeRel`, `cmake/presets/arm.json:10`)
and the first on this board since 2026-07-14. Every banner reads `board f302nucleo /
arch armv7m / mpu off / sched tickless`; the three at the board's application profile add
`heap 2 KiB available`, and the `-st` capture reads `heap none`, since that preset carves no
heap (`KICKOS_USER_HEAP_SIZE=0`).

| App | Commit in the banner | Result |
| --- | --- | --- |
| `hello` | `176109e-dirty` | **PASS** -- both spawned threads run; ran past `ping 504` / `pong 504` before the capture was cut |
| `stress` | `9ba4e4b` | **PASS** |
| `selftest` (board app profile) | `9ba4e4b` | 17 `ok` / 42 `not ok` / 10 skipped, plan `1..59` |
| `selftest` (`-st` provisioning) | `2af3aee`+ | **59 `ok` / 0 `not ok` / 8 skipped -- `# all tests passed`** |

`hello`'s banner stamps `176109e-dirty`, not the tip. That image predates the branch
reorder and is one commit -- `arena: no-MPU region granule on f103/f302, boot-arena link
assert` -- behind the tip; its tree already carried the halved heap carve, which the
banner's `heap 2 KiB` witnesses (`176109e` itself still declared 4K).

**`hello` PASS is the run-floor witness.** Two threads
(`user/apps/common/hello/main.cc:74-75`), both spawned, `printf` alive, at
`KICKOS_USER_HEAP_SIZE` 2048 (its chip default, declared in `Kconfig`) -- the halved carve
neither starves stdio nor costs a thread stack.

**`stress` PASS**, verbatim:

    naps 0/0  handoffs 6000/6000  churn 340/340
    STRESS PASS

`stress` probes the live thread budget before it sizes anything -- it spawns parked threads
until one is refused (`user/apps/common/stress/main.cc:14-20`) -- so those counts are a
measurement of this board, not a fixed workload: budget 2, one ping-pong pair, 6,000
handoffs, no sleepers. `churn 340/340` is 340 spawn/exit cycles through those two slots and
is the **thread-slot reclaim** witness: a broken reclaim exhausts the pool and a spawn
returns -1 (`:22-25`). `naps 0/0` is what the small budget costs -- with zero sleepers the
tickless-timer conservation arm did not run here.

**`selftest` fails 42 of 59, and every failure is a resource refusal, not a logic fault.**
Two independent causes, both measured on the ELF at the tip:

- **Arena.** The suite's static footprint is 7,760 B and its heap carve 2,064 B, leaving an
  arena of 4,512 B (`arch/arm/chip/stm32f302/stm32f302.ld:110-111`). The idle and root boot
  stacks take 512 + 2,048, so **1,952 B remain** -- below the 2,048 one spawned thread's
  stack needs (`boards/f302nucleo/configs/base/defconfig:9`). Every spawning
  case therefore fails on `w >= 0` / `drv >= 0` / `a >= 0 and b >= 0`.
- **Object pools.** A zero-skip run needs `KICKOS_MAX_SEMAPHORES >= 6` (peak is
  `mutex_deadlock`: two permanent plus four live); this chip provisions 4
  (`boards/f302nucleo/configs/base/defconfig:6`). That is the `sem_destroy`
  failure on `h >= 0`.

Two further provisionings were flashed, which is the evidence that **no single knob fixes
it**:

| Configuration | Result |
| --- | --- |
| board defaults | 17 `ok` / 42 `not ok` / 10 skipped |
| `-DKICKOS_MAX_THREADS=4` | 17 `ok` / 42 `not ok` / 10 skipped -- identical; the arena binds, not the pool |
| `-DKICKOS_MAX_THREADS=4 -DKICKOS_USER_STACK_SIZE=1024` | 18 `ok` / 41 `not ok` / 11 skipped -- one case bought, then `sem_destroy` refuses on the semaphore pool |

In that last configuration one failure is **not** a resource refusal: `console_publish_priv`
gets its worker spawned and then fails on `g_pub_rc == -KOS_EPERM`. Do not read that as an
authority-gate defect -- a 1 KiB stack is below what the rest of the fleet gives a worker,
and nothing here separates "the gate returned the wrong code" from "the worker did not get
far enough to call it". The configuration is a sizing probe, not a posture to ship.

So "KickOS runs on this part" and "KickOS is validated on this part" are different claims
about one board. `porting.md`'s `## Minimum hardware requirement` section carries the
model, the four-span SRAM arithmetic and the per-board thread-capacity table these captures
were checked against.

**What these captures do NOT witness.** The **ring arm** of the unprivileged-root boundary
(an unprivileged thread refused a privileged-only register). The prober now exists
(`user/apps/common/ringpriv`) and this board took the witness on 2026-07-30 -- but at a later
tip, in its own captures, not in the ones above; see *Unprivileged root* and *M4.5.6* below. And
**no benchmark**: no bench run was taken
under controlled conditions on this board at these commits, so there is no switch or IRQ
figure to quote at `MinSizeRel`. A caution for whoever takes one: the VCP is not drained on
reset, so a fresh capture opens with **residue from the previous image** -- a stale
`switch:`/`irq:` block or a stale `ping`/`pong` run above the banner is the old image, not
this one. Read only what follows the banner.

## Per-board hardware facts a porter cannot derive

Boot-image field values, recomputed register bases and the register-level gotchas that brick or
silence a board if they are wrong. Each is checked against the chip source; the manual citations
are the clean-room derivation the source carries.

### `pizero2350` (RP2350) -- the boot block and the recomputed bases

**The IMAGE_DEF block is what makes the image bootable at all**, and the RP2350 hazard is a
reincarnation of the RP2040 boot2 class rather than a repeat of it: there IS no boot2 (datasheet
5.9.5). The bootrom does its own best-effort QSPI XIP setup, then scans the **first 4 KiB** of the
image (5.1.5.2) for a block loop containing a valid IMAGE_DEF; if it does not find one, the image
is rejected. With no `ENTRY_POINT` and no `VECTOR_TABLE` item the bootrom takes the Arm vector
table to be **at the image base** and enters via the initial SP at `[base+0]` and reset PC at
`[base+4]`, setting Secure SP and VTOR first (5.9.3.3, 5.9.5.1, 5.2.2).

The 20-byte block emitted verbatim in `arch/arm/chip/rp2350/startup.S` (datasheet 5.9.5.1,
"Minimum Arm IMAGE_DEF"):

| Word | Value | Meaning |
|---|---|---|
| 0 | `0xffffded3` | `PICOBIN_BLOCK_MARKER_START` |
| 1 | `0x10210142` | IMAGE_DEF item: type `0x42`, size 1 word, `image_type_flags = 0x1021` |
| 2 | `0x000001ff` | LAST item, covered size = 1 word |
| 3 | `0x00000000` | LINK = 0, a self-loop (single-block loop) |
| 4 | `0xab123579` | `PICOBIN_BLOCK_MARKER_END` |

`image_type_flags = 0x1021` decodes as EXE(1) | Security **S**(2) | CPU **ARM**(0) | CHIP
**RP2350**(1) (5.9.3.1). Security=S matches the state the M33 boots in -- no TrustZone, never a
drop to Non-secure. "Unsigned" is orthogonal to that field: on a chip that is not secured
(`CRIT1.SECURE_BOOT_ENABLE` clear) no SIGNATURE or HASH_DEF item is required and this block is
accepted as-is. The NS and Security-UNSPECIFIED variants are `0x1011` and `0x1001`; a Hazard3
RISC-V image would be the same block with the CPU field = RISC-V(1), i.e. `0x1121`.

Placement is PINNED rather than merely asserted: `rp2350.ld` puts `.text` at `ORIGIN(FLASH)` with
`KEEP(*(.isr_vector))` first, so the vector table IS the image base, and `KEEP(*(.image_def))`
lands the block immediately after it, well inside the 4 KiB window. Two `ASSERT`s enforce both
(`ADDR(.text) == ORIGIN(FLASH)` and `g_image_def < ORIGIN(FLASH) + 0x1000`), and
`Reset_Handler` writes `SCB->VTOR = 0x1000_0000` explicitly so a warm reboot or a debugger entry
that skipped the bootrom still lands right. Nothing references the block, so it survives only
because it rides into the link inside `startup.o` (already force-pulled by the arm-family
`-Wl,-u,g_isr_vector`) and `KEEP` protects it from `--gc-sections`. No checksum tool and no `-u`
addition -- unlike RP2040.

**Every APB peripheral base moved relative to the RP2040** (datasheet 2.2.4), so no RP2040
address can be reused. Recomputed in `arch/arm/chip/rp2350/include/kickos/chip_mmap.h`:

| Block | Base | Block | Base |
|---|---|---|---|
| CLOCKS | `0x4001_0000` | PADS_BANK0 | `0x4003_8000` |
| RESETS | `0x4002_0000` | XOSC | `0x4004_8000` |
| IO_BANK0 | `0x4002_8000` | PLL_SYS | `0x4005_0000` |
| UART1 (console) | `0x4007_8000` | TIMER0 | `0x400B_0000` |
| TICKS | `0x4010_8000` | | |

Every APB register is also mirrored at `base + 0x1000` (XOR), `+ 0x2000` (SET) and `+ 0x3000`
(CLR) for single-bit changes without a read-modify-write (datasheet 2.1.3), which is why a
reserved-block entry for one of these covers `0x4000` and not `0x1000`.

**Clock recipe** (`clocks_init`, every poll bounded so a dead crystal degrades instead of
hanging): XOSC `CTRL.FREQ_RANGE = 0xaa0` (the 1-15 MHz range, for the 12 MHz crystal), then the
`CTRL.ENABLE` magic `0xfab` in a SEPARATE store so `ENABLE` never latches before `FREQ_RANGE` is
set, poll `STATUS.STABLE`; `clk_ref <- XOSC`, poll the one-hot `CLK_REF_SELECTED`;
start the **TICKS TIMER0** generator with `CYCLES = 12` for a 1 MHz tick (this is the new common
tick block, NOT RP2040's watchdog tick, and TIMER0 does not count until it runs -- kept on
`clk_ref` so the monotonic clock is PLL-independent); PLL_SYS to 150 MHz as
`12 MHz / REFDIV=1 x FBDIV=125 = 1500 MHz VCO / POSTDIV1=5 / POSTDIV2=2`, poll `CS.LOCK`;
`clk_sys <- PLL` with `SystemCoreClock = 150e6` in the same step; `clk_peri <- clk_sys` and
recompute the UART divisors. Fallbacks: no XOSC leaves ROSC (~6.5 MHz) with `CYCLES = 7`; no PLL
lock stays on `clk_ref` at 12 MHz. The board always reaches a console.

**The PADS.ISO gotcha, and it differs from RP2040.** RP2350 pads reset **ISOLATED** --
`PADS.ISO` is **bit 8** and resets SET -- so a pad stays electrically disconnected until it is
cleared. `uart1_init` clears ISO on both console pins (plus OD on TX, sets IE on RX), and
`arch_pinmux_set` clears it unconditionally on every pin it touches. The RP2040 needed none of
this, so a port carried over from it looks correct and drives nothing.

**Interrupts.** 52 NVIC inputs (datasheet 3.2, Table 95): IRQ0..51, of which 46..51 are spare and
never fire. `KICKOS_MAX_IRQ 52` sizes the `startup.S` vector `.rept` and the kernel IRQ table from
one fact. The console line is **`UART1_IRQ = 34`** (only `TXIM` armed; the drain ISR is its sole
source), because the console is UART1 on GP4/GP5 -- UART0's pins are not brought out on the
Pi-Zero header.

### `teensy41` (i.MX RT1062) -- the three ROM-consumed structures and the console

**The boot ROM enters via `IVT.entry`, not the reset vector**, and hardware does NOT load MSP from
the vector table, so `_boot_entry` (`startup.S`) sets `MSP = _estack` before any C runs and
`Reset_Handler` then sets `VTOR`. The three structures sit at fixed flash offsets in the FlexSPI
serial-NOR image based at `0x6000_0000`: **FCB @ +0x0000**, **IVT @ +0x1000** (RM Table 9-36:
the FlexSPI-NOR IVT offset), **Boot Data @ +0x1020**. All three must be **static const image
data**: a field needing a runtime initializer becomes a write to XIP flash and lands as 0 in the
image, which bit this bring-up once (`entry = &_boot_entry | 1` demoted the IVT to a dynamic
initializer, so `entry` was 0; the Thumb LSB is already in the function-symbol relocation, so use
a bare address constant). Layout is self-verified by `offsetof`/`sizeof` `static_assert`s on the
structs, and confirmed in the linked image (`.boot_fcb` @ `0x6000_0000`, `.boot_ivt` @
`0x6000_1000`, `.isr_vector` @ `0x6000_2000`).

Field values (`arch/arm/chip/imxrt1062/chip_imxrt1062.cc`; RM chapter 9):

| Structure | Field | Value |
|---|---|---|
| FCB (RM 9.6.3, Table 9-15/9-18), 512 B | tag | `'FCFB'` = `0x42464346` |
| | version | `0x56010000` (`'V'` 1.0.0) |
| | `deviceType` @ `0x044` | **1** = serial NOR |
| | `sflashPadType` @ `0x045` | **1** = single pad |
| | `serialClkFreq` @ `0x046` | 1 = 30 MHz |
| | read LUT seq 0 (1-1-1 `0x03` + 24-bit address) | `lookupTable[0] = 0x08180403`, `[1] = 0x00002404` |
| IVT (RM 9.7.1, Table 9-37), 32 B | header | `0x412000D1` (tag `0xD1`, len `0x0020`, version `0x41`) |
| | `entry` / `dcd` / `boot_data` / `self` / `csf` | `&_boot_entry` / 0 / `&Boot Data` / `0x6000_1000` / 0 |
| Boot Data (RM 9.7.1.2, Table 9-38) | `start` / `length` / `plugin` | `0x6000_0000` / `__boot_image_length` / 0 |

The LUT instruction encoding is `(opcode << 10) | (pads << 8) | operand` (RM 9.6.3.1).
Single-pad `0x03` at 30 MHz is the universally compatible read -- no quad-enable -- which is what
makes a first image flashable without a bench. **DCD is NULL** deliberately: the ROM register
defaults suffice with no SDRAM/SEMC, and RM Table 9-37 makes it optional.

**Console = LPUART6 = Teensy "Serial1" (pins 0/1)**, and every step of the wiring is a distinct
register block:

| Step | Register | Address | Value |
|---|---|---|---|
| clock gate | `CCM_CCGR3`, `CG3` = bits `[7:6]` | `0x400F_C074` | already enabled at reset; set explicitly anyway |
| pin mux TX (pin 1) | `IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B0_02` | `0x401F_80C4` | ALT2 = `LPUART6_TX` |
| pin mux RX (pin 0) | `IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_B0_03` | `0x401F_80C8` | ALT2 = `LPUART6_RX` |
| RX daisy | `IOMUXC_LPUART6_RX_SELECT_INPUT` | -- | daisy value selecting `AD_B0_03` |
| controller | LPUART6 (RM Table 3-3) | `0x4019_8000` | GLOBAL/BAUD/STAT/CTRL/DATA at `+0x08`/`0x10`/`0x14`/`0x18`/`0x1C` |

LPUART6 is IRQ **25** (RM Table 4-2, combined TX/RX), buffered TX over the shared `console_tx`
ring with a synchronous fallback for the panic path -- the same seam as `mk64f`. Baud assumes the
reset UART clock root (`pll3_80m/1` = 80 MHz, RM chapter 14) with `OSR = 15` and
`SBR = root / (baud * 16)`; it tracks the real root once the CCM bring-up lands.
`arch_pinmux_set` refuses `GPIO1.IO02`/`IO03` so a board pin map cannot steal the console pads.

Why writable state lives in **OCRAM2** (`0x2020_0000`, 512 KiB) and not DTCM: ITCM and DTCM are
carved out of the 512 KiB **FlexRAM**, whose split is set by `IOMUXC_GPR_GPR16`/`GPR17` from a
fuse default at reset, whereas OCRAM2 is a dedicated bank present at reset with no GPR or fuse
dependency (RM 3.2). For a port with no SWD bench that removes an entire class of "what partition
did the ROM leave" risk.

### `xmc4800-relax` -- two USIC-SSC facts and the two addresses every probe uses

**A single-word SSC frame completes into `AIF`, not `RIF`.** The first word of a frame carries
`RBUFSR.SOF = 1`, which sets the ALTERNATIVE receive flag `AIF` (`PSR` bit 15); later words set
`RIF` (bit 14) (RM 18.4.2.7). A single-word transfer is therefore ALL first-word, so a driver
waiting only on `RIF` never wakes. Every USIC-SSC consumer in the tree arms and W1Cs BOTH --
`PSR` `RIF`/`AIF`, cleared through `PSCR` `CRIF`/`CAIF` -- which is correct for any frame length.

**Loopback is INTERNAL and needs no pin and no jumper.** The DX0 input stage selects on-chip
input **"G"** (`DX0CR.DSEL` = G, with `INSW` set), which is the channel's own transmitter, so a
byte shifts out DOUT0 and is received on DIN0 entirely inside the chip (RM 18.2.3.5, Loop Back
Mode). No port pin is muxed, no external MISO-MOSI wire exists, and the port `IOCR` mux therefore
stays outside the driver's window as an escalation surface -- which is what lets `xmcspi` and
`xmcssc` run a real SPI transfer from a thread holding nothing but the 512-byte channel window.

Two addresses the probes on this board all use:

- **`USIC0_SR1` = NVIC line 85** (RM Table 4-3), the channel-1 service-request node the SSC
  driver blocks on. `USIC0_SR0` = **84** is the kernel CONSOLE channel's node, which is why an
  `INPR` re-point onto SR0 is the interference case those probes exist to bound.
- **`SCU_CGATCLR0` = `0x5000_4648`** (SCU peripheral clock-gate clear, at CCU `0x600` +
  `CGATCLR0` `0x048`), the clock-tree escalation surface kept privileged and out of every
  window. It is the canonical ungranted poke: on PMSAv7 an unprivileged access to it MemManages
  before any bus access, and `CFSR=0x82` with `MMFAR=0x5000_4648` is the signature every
  enforcement capture on this board carries.

## CI coverage & cross toolchains

**What CI gates is not uniform across the fleet, and the gaps are structural.** The authoritative
statement lives in the header of `../../.github/workflows/ci.yml`; this is the board-facing view.
Silicon RUN validation is never in CI -- it stays a manual bench step per the HW-test deferral
policy -- so a green CI run means "still builds / still runs under emulation", not "still works on
the board".

| ISA | Boards | CI gate | MPU enforcement in CI |
|---|---|---|---|
| host | `sim` | full `ctest` -- the authoritative deterministic gate | **runtime** (host `mprotect`) |
| rv32imac | `qemu-riscv`, `esp32c6-wroom` | `qemu-riscv` run gate; C6 + bench build-only | **runtime** (PMP, the `qemu-riscv-mpu` job) |
| armv7m | `qemu`, `qemu-m33`, `qemu-m7`, `qemu-m3`, and the board sweep | four MPS2 run gates (an386/an505/an500/an385) + build sweep | **runtime** (PMSAv7 on M4/M7/M3, **PMSAv8** on the M33) |
| armv6m | `microbit`, `picopi` | `microbit` run gate + `picopi` build | **build only** |
| Xtensa LX6 | `esp32-wroom` | build only, plain and `-st` | -- (no per-task unit) |
| RXv3 | `rx72m` | **none** | -- |

- **ARM enforcement is now a run gate too, on both PMSA revisions.** It was build-only for a
  long time, and the reason was real: every enforcing ARM port was a silicon part, and the one
  runnable armv7m target shipped no enforcement block, so `--preset qemu -DKICKOS_HAVE_MPU=1`
  was a hard configure error. Giving `mps2.ld` the enforcement layout removed both halves of
  that at once, because the chip serves four QEMU FPGA images: `qemu`/`qemu-m7`/`qemu-m3`
  exercise the shared armv7m **PMSAv7** commit and `qemu-m33` (mps2-an505) the dedicated
  **PMSAv8** backend, whose RBAR/RLAR encoding is a different register layout entirely. Each
  runs the full TAP suite unprivileged and takes a real `mpu_fault` MemManage denial, and the
  M33 additionally runs `cxxtest` -- the whole libstdc++ runtime, throw and unwind included --
  as an unprivileged thread under PMSAv8.

  `build-boards-mpu` still matters and still runs: it covers the enforcement-only **link
  surface** of the boards QEMU cannot model -- the pow2 `.appdata`/`.appbss` window and its
  placement ASSERTs, the `archive:member` selectors, the app grant symbols, and
  `arch_reserved_blocks`, which has no fallback TU on purpose, so an enforcing port that
  forgets its reserved set fails to link. What is still NOT covered in CI is
  **chip-specific** trapping: SYSMPU (K64F), the M7 anti-speculation wrap (i.MX RT1062) and
  PMSAv6 (M0+) have no QEMU model, and stay silicon-proven (see the matrix above and
  `../m2-readiness.md`).
- **Renesas RX has no CI gate at all.** RX72M needs `-misa=v3` and `-mdfpu`
  (`boards/rx72m/board.cmake`), and both exist only in the registration-gated Renesas GNURX
  build -- upstream `rx-elf` GCC rejects them. That toolchain cannot be fetched anonymously on a
  hosted runner, so RX is bench-validated only. A change that touches the arch seam is *not*
  covered for RX by a green CI run; build it locally.
- **`f302nucleo` has no AUTOMATED gate of any kind.** It is in the `build-boards` sweep and nothing
  else: no CTest, and no QEMU run gate because **no emulator models the part**. So the only thing CI
  says about this board is that it links, and a regression that stops it booting is invisible until
  somebody flashes it. That matters more now than it did, because it is a bench board and the
  fleet's only physically-present no-MPU ARM part (see *Unprivileged root* below).

  **Unwitnessed is not the same as ungated, and the ring arm is the case that separates them.** The
  board now carries a flashable prober (`user/apps/common/ringpriv` -- `ringpriv` plus `ringppb`),
  and the SAME arm runs as a permanent CTest on the emulated targets, because neither test is
  conditioned on enforcement -- a board's `flat` variant IS the ring-only posture, its base one
  enforcing -- so `ringpriv`/`ringppb` are registered on `qemu`, `qemu-m3`, `qemu-m7` and `qemu-m33`
  in both postures, and `microbit` registers `ringpriv` asserting the OPPOSITE outcome. So the ring
  property is machine-checked on every push; what THIS board adds is that the property holds on
  real no-MPU armv7m silicon rather than under emulation. The claim to keep narrow: no gate covers
  this board's own chip code, its clock tree or its USART.

  **The gap's one concrete instance is fixed, and it is why the boot-arena link assert now exists.**
  M4.5.2's static growth took the `f302nucleo-st` arena below what `kmain`'s two boot stacks need.
  `__kickos_ram_end - __kickos_ram_start`, read with `arm-none-eabi-nm` on the built ELF:

  | Ref | Heap carve | Arena | Against the 2,560 B the boot stacks need |
  | --- | --- | --- | --- |
  | `181540e` (master) | 4K | 3,008 B | 448 B of headroom |
  | `176109e` (mid-branch) | 4K | 2,464 B | **96 B short** |
  | `9ba4e4b` (tip) | 2K | **4,512 B** | 1,952 B of headroom |

  `kmain` takes both bootstrap stacks from the arena through `boot_stack_alloc` --
  `KICKOS_IDLE_STACK_SIZE` then `KICKOS_ROOT_STACK_SIZE`, 512 and 2,048 on this chip
  (`boards/f302nucleo/configs/base/defconfig:10`, `:11`) -- so it needs **2,560 B**, and
  an unsatisfied second allocation is `kpanic("kmain: no arena for the root stack")`
  (`kernel/init/kmain.cc:218`) rather than a degraded boot. The fix was the heap carve: `6d49e14`
  halved `KICKOS_USER_HEAP_SIZE` to 2K (then a `stm32f302.ld` default, now the chip's
  declared one in `Kconfig`), which returns
  1:1 to the arena because the heap is carved below `__kickos_ram_start`. **Boot at the tip is now
  witnessed rather than computed** -- see *`f302nucleo` on silicon* above. `176109e` was superseded
  by the branch reorder and resolves against `backup/m4.5.2-pre-reorder`, not the live branch.

  The regression is kept on record because of what it changed. It was found by arithmetic, not by a
  gate: the image linked cleanly, since the linker script's arena `ASSERT` only checked that the
  arena was non-negative. `KICKOS_BOOT_ARENA_ASSERT` (`arch/common/boot_arena.ld.h:33-35`, same
  commit as the carve fix) now
  replays those two allocations, alignment padding included, so an arena that cannot hold them fails
  the **build** on every chip. It still does not check user stacks or pool capacity, which is why
  `f302nucleo-st` at the board's application profile linked clean and then refused every spawn on
  hardware. The `-st` preset now provisions for the suite and passes (see *on silicon* above), but
  the assert's blind spot is unchanged: nothing catches a user-stack or pool shortfall at build time.
- **microbit has no privilege axis at all, so it witnesses nothing about the user/kernel ring.**
  The nRF51822 is a Cortex-M0, and ARMv6-M's Unprivileged/Privileged Extension is optional and
  separate from the MPU extension: the M0 does not implement it (Cortex-M0 TRM DDI0432C), so
  `switch.S`'s `msr control` is discarded and **a thread the kernel marks unprivileged runs
  privileged here.** `picopi` is a Cortex-M0+ and does implement it; one arch backend spans both
  cores and nothing in the tree distinguishes them. See `design-unprivileged-root.md` section 2.
- **microbit is the armv6m run gate, and the fleet's only board that is allowed to skip anything.**
  16 KiB SRAM and a 2-slot pool mean part of the suite genuinely cannot run here, so
  `microbit_selftest` sets `EXPECT_SKIPS` to the eleven test **names** it cannot host; every other
  board keeps the script's default of "nothing may skip". The list is a **measurement, not slack**
  -- each name and why it skips is at the call site
  (`../../user/apps/common/selftest/CMakeLists.txt`), so growing it should mean a board capability
  changed, and a test that merely stopped running shows up as a breach. It is named rather than
  counted for exactly that reason: the earlier `MAX_SKIPS=10` admitted *any* ten skips, so a test
  that quietly stopped running could take a slot another one vacated and leave CI green.

  This gate was **RED from `9ae301f` (M4.4) until M4.5.1** -- two milestones, unnoticed, on a real
  remote. Two independent breakages, neither an armv6m mechanism fault; both were a 16 KiB part
  meeting provisioning sized for larger ones, and the record is kept here because the shape
  recurs:
  1. That commit gave `nrf51.ld` the fleet-uniform 4 KiB `.userheap`, carved from RAM *ahead of*
     the thread arena. On 16 KiB SRAM that left ~1.5 KiB of arena -- not one 2 KiB thread stack --
     so **every** `kos::thread::spawn` failed and the suite cascaded to 39 `not ok`. The heap is
     now empty on this chip: no app built for the board allocates, so the arena gets the RAM.
     (`_sbrk` shares a TU with `_exit` and the link force-links it, so the section has to stay,
     at zero length -- an allocating app here fails at runtime, not at link. See `nrf51.ld`.)
  2. With the arena restored the run then **hung** at `call_infoless_revert`, whose four workers
     wait on each other. Its pool-too-small guard fired *after* the spawns, so on a 2-slot pool
     two started and then blocked forever on `g_done` posts from a call/reply choreography that
     cannot complete with half its cast. A guard for interdependent workers has to ask the pool
     *before* spawning anything, which is what `pool_can_host` in the suite now does; the test
     reports a real TAP skip instead.
- **Xtensa is build-only** because upstream QEMU ships no ESP32 machine model. The gate builds
  both `esp32-wroom` and `esp32-wroom-st` (the `-st` config is what is HW-validated), catching
  link / linker-script / windowed-ABI asm regressions. `esptool` is deliberately absent: only the
  bootable `.app.bin` post-step needs it, and CI cannot boot the image anyway.

### Cross toolchains (the local convention)

Each cross family finds its compiler through a hint variable, **seeded from the environment**,
overridable with `-D`, and falling back to `PATH` when empty:

| Family | Hint variable | Toolchain |
|---|---|---|
| ARM | `KICKOS_ARM_TOOLCHAIN_BIN` | Arm GNU Toolchain, `arm-none-eabi`, newlib |
| RISC-V | `KICKOS_RISCV_TOOLCHAIN_BIN` | RISCstar, `riscv32-none-elf`, newlib, rv32imac soft-float multilib |
| RX | `KICKOS_RX_TOOLCHAIN_BIN` | Renesas GNURX (registration-gated) |
| Xtensa | `KICKOS_XTENSA_BIN` | Espressif crosstool-NG `xtensa-esp-elf` |

No toolchain file carries a default compiler path any more, so keep local pins in
`.session/env.sh` (gitignored) and `source` it before configuring. CI uses the same pinned vendor
tarballs, exporting the variable *and* putting the bin on `PATH` -- either route alone suffices,
and the belt-and-braces matters because CMake's `try_compile` re-reads the toolchain file with a
fresh cache, inheriting the environment and `PATH` but never a `-D` cache entry.

**A resolved compiler is verified, not trusted.** `find_program` HINTS fall through to `PATH` when
the hinted directory is absent, and on Debian the on-`PATH` `arm-none-eabi-g++` is a C-only
picolibc build with no `libstdc++`/`libsupc++` for any multilib. The ARM and RISC-V toolchain files
therefore probe the compiler they actually resolved -- carrying the board's own `-mcpu`/`-march`,
since a toolchain can ship `libstdc++` for one multilib and not another -- and **refuse it at
configure time**, naming the compiler, the multilib, what was missing, the override variable and
the official tarball URL. Previously this surfaced dozens of build steps later as
`fatal error: exception: No such file or directory`. RX and Xtensa skip the check: neither has a
same-name C-only twin on `PATH` to fall through to.

## Flashing

Every recipe -- ST-Link, external SWD, USB-DFU, picotool/BOOTSEL, esptool, bossac, HalfKay,
`rfp-cli`, and the J-Link / RTT deep-dive -- lives in [flashing.md](../flashing.md). Nothing
operational belongs in this file.

### The 64 KiB parts run the selftest as TWO images

`bluepill-c8` and `f302nucleo` only. Every other board still produces one `selftest`, unchanged.
The suite outgrew a 64 KiB part (`f302nucleo-st` was at 4 free bytes of 65536), so it is built as
two self-contained images that partition the arms between them.

- `selftest` is **part 1**, `selftest_p2` is **part 2**; both land in
  `<build>/user/apps/common/selftest/` with the usual `.elf` / `.bin` / `.hex`.
- **Flash and run them one after the other. Order does not matter.** Each initialises the board,
  runs its own arms and self-terminates; they share no state and there is no handover between them.
- **TAP numbering RESTARTS at 1 in each part.** `tap.cc` plans `1..N` from its own runtime registry,
  so part 1 emits `1..43` and part 2 `1..30` under `KICKOS_ENABLE_SELFTEST`. **A pass is BOTH parts
  green** -- a single `1..43` stream is half a run, not a short one, and reading it as a pass is the
  obvious way to be fooled here.
- Configure prints what to expect:
  `-- selftest: split into two images -- selftest plans 43 arms, selftest_p2 plans 30 (73 together)`
- These two boards run the FULL arm set: `cap_dest` and `irq_discard` are not excluded on them.
- Which arm sits in which part is decided by POSITION in the registration list at the bottom of
  `user/apps/common/selftest/main.cc`, not by an annotation; the boundary is the `#undef TAP_ADD`.
  Adding or moving an arm means updating the whole-suite floor AND the matching per-part clause in
  `user/apps/common/selftest/CMakeLists.txt` -- getting it wrong is a configure error on every
  board in the fleet, not a quietly smaller suite.

**Witnessed on `f302nucleo`, not on `bluepill-c8`.** Both `f302nucleo` images boot at `9a00e73`
(`1..44` and `1..30`, zero `not ok`) and all three restored arms ran there: `cap_dest` pass,
`cap_capacity` PARTIAL, `irq_discard` pass. `bluepill-c8` has no unit on the bench, so its half of
the split is build-only. They pass today on `sim`, `qemu`, `qemu-riscv` and `microbit`,
and `microbit` is the harsher board (`KICKOS_MAX_THREADS` 2 against 3), which is the basis for
expecting them to pass -- not evidence that they do.

## Terminal dead-ends and BOOTSEL handover -- `pizero2350`

**`kos_reboot` is silicon-witnessed here** (2026-07-28, `6857df3`, the first execution of syscall 38
and of its RP2350 backend on any chip). `rebootdemo` announced, waited out its 3 s arming timeout,
and never printed the `reboot declined: rc=` line -- the call did not return, which is the contract
for the BOOTSEL-type `reboot(0x0102, 10, 0, 0)`:

```
[rebootdemo] KickOS reboot-to-bootloader demo
[rebootdemo] handing the chip to its bootloader in 3s
```

The board re-enumerated as
`2e8a:000f Raspberry Pi RP2350 Boot` on a fresh USB device number and `picotool info` answered again
(`target chip: RP2350`, `image type: ARM Secure`), so the bootrom magic check at `0x10`, the
lookup-helper halfword at `0x16` and the `'R','B'` lookup under `RT_FLAG_FUNC_ARM_SEC` all resolved
on real silicon.

**`-DKICKOS_SHUTDOWN_TO_BOOTLOADER=ON` extends that to any image, including one that ends in a
fault**, by putting the handover in the kernel's two terminal dead-ends (`kickos_terminate` and the
fallback `kfault_terminate`, `arch/common/kfault_terminate_default.cc`). **Both are
silicon-witnessed here (2026-07-28, `3204121`), and the evidence
is the USB device number**: the bootrom re-enumerates on every handback, so a number that rises at
each step proves a fresh enumeration rather than a stale node. Seven images ran off the ONE human
press that produced device 022:

| step | image | dead-end taken | board after |
| --- | --- | --- | --- |
| press | -- | -- | BOOTSEL dev 022 |
| 1 | `rootfault` (flipped) | fault | dev 023 |
| 2 | `selftest` (flipped) | ordered | dev 024 |
| 3 | `mpu_fault` (flipped) | fault | dev 025 |
| 4 | `selftest` (control) | ordered | dev 026 |
| 5 | `selftest` (flipped, re-capture) | ordered | dev 027 |
| 6 | `selftest` (control, re-capture) | ordered | dev 028 |
| 7 | `mpu_fault` (flipped, re-capture) | fault | dev 029 |

`kickos_terminate` is witnessed four times and `kfault_terminate` three, each fault image having
printed its complete `=== MPU FAULT ===` dump through `MMFAR` before handing over -- the drain does
run ahead of the reboot. Steps 5-7 are re-captures of 2, 4 and 3 (the first pass's logs were lost to
a capture-tooling fault, not a board fault), and cost no press.

**The handler-mode call is witnessed, which was this knob's one unverified risk.**
`kfault_terminate` runs in MemManage **handler** mode with the faulting context still live, and
`kos_reboot`'s witness above covers only thread mode on an ordered exit -- a bootrom call from a
fault handler had never been shown on this part. Steps 1, 3 and 7 are exactly that call returning
the board to BOOTSEL.

## Unprivileged root -- the silicon record

**Condensation happens at each milestone boundary; it is a standing ritual, not a one-off.** This
section grows monotonically as captures land, so at every milestone boundary the closed stages
collapse into tables and only the live milestone keeps its full transcripts. The pass that
established the ritual ran at the M4.5.6 boundary (2026-07-30) and collapsed stages 2, 3, 4 and
M4.5.5. What a collapsed stage keeps is the `CFSR` / `MMFAR` / `EDR` / `mcause`-class value, the
app's own verdict line, and any capture that is the sole evidence for a claim still live in the
tree. What it drops is the `PC`/`LR`/`xPSR` and `R0..R12` lines of a fault dump, which carry nothing
checkable -- except in the two exemplars kept verbatim below to show the shape.

**Root is unprivileged on every board by construction, as of 2026-07-30.** It holds an authority
word rather than the whole arena, and there is no second posture: `KICKOS_ROOT_PRIVILEGED`
is **deleted from the tree** -- no knob, no default, nothing to flip. The name has **zero hits in
every code and build file**, including `cmake/KickOSConfig.cmake.in`; only docs still discuss it,
historically. So a build passing the `-D` gets nothing louder than CMake's unused-variable warning,
in tree or out of it. The banner suffix went with it: every board now prints
`mpu enforce` and says nothing about the posture, because under a single posture the suffix carried
no information.

**The posture witnesses are the two `rootfault` captures, not the banner.** Banner-suffix absence
proves only that the image carries the new banner code -- the argument for deleting the suffix is
precisely that it carried no information, so its absence carries none either. What discriminates is
`rootfault` on `frdmk64f`, where `chip_mk64f.cc` keeps SYSMPU `RGD0` supervisor-`rwx` so the
isolation fault is reachable ONLY from a user-mode root, and `rootfault` on `pizero2350`. Both are
captured at `c5d9b0d` -- see *M4.5.6* below.

The witness a board can carry depends on its hardware, in three arms: **authority** (a capability
gate refusing a thread that lacks the bit -- pure kernel logic, so every target), **confinement** (a
cross-domain fault -- needs an MPU), and **ring** (an unprivileged thread refused a privileged-only
register -- needs a privilege ring and no MPU). Every subsection below but one is a confinement
witness. **The ring arm is witnessed as of 2026-07-30**, on `f302nucleo`, the only board that could
take it: `ringpriv` `PASS (5 arms)` on real no-MPU armv7m silicon (see *M4.5.6* below). It is also
the only arm that is PERMANENT CI rather than a bench capture -- `user/apps/common/ringpriv`
registers `ringpriv`/`ringppb` on the MPS2 presets and `ringpriv` on `microbit`, which asserts the
opposite outcome instead of skipping.
The honest asymmetry, because a green run is easy to over-read: on a board with no ring the authority
arm still passes, and it never implies confinement -- nothing there stops a thread walking past the
syscall and touching the peripheral directly.

### Stage 2 and 3 -- the six confinement witnesses (2026-07-27 .. 2026-07-29)

Six boards, every enforcement backend in the fleet. `frdmk64f` is the only one witnessed on its
**full** service list; the other five were captured console-only or on `kickos_services_none`. That
is a property of the captures, not of the boards: no board is marked as unable to run an
unprivileged root (`design-unprivileged-root.md` sections 4 and 10).

**Read the table as six dated historical records, not as a reproducible A/B.** On the date each row
was taken its two `selftest` columns really were two buildable images off one tree, and that
side-by-side is what attributed the denial to the posture rather than to something else on the board.
**The privileged control no longer describes any buildable configuration**; it cannot be re-run, and
the `mpu enforce` banner it printed no longer distinguishes it from anything. Two more things these
rows depend on are also gone: `mpu_privileged_guard` -- the single case supplying the whole skip
delta in every row -- was **deleted** along with the knob, so no current build registers it; and the
words "the flip", "flipped board", "default posture" and "control arm" below date the runs, they do
not describe a fleet split today.

| Board | Backend | Date, tip | Service list | `selftest` unpriv / privileged control | Confinement fault, as reported on the wire |
|---|---|---|---|---|---|
| `xmc4800-relax` | PMSAv7, 144 MHz | 2026-07-27 `22e1c5a`; re-witnessed 2026-07-28 `75227d4` | console-only | 61 / 60 `ok` / 1 skip -- vs 61 / 61 `ok` / 0 skip | `=== MPU FAULT ===` `CFSR=0x82` `MMFAR=0x20013000` |
| `esp32c6-wroom` | RISC-V PMP NAPOT, ~160 MHz | 2026-07-28 `e5c651b` | `kickos_services_none`, kernel console | 62 / 61 `ok` / 1 skip -- vs 62 / 62 `ok` / 0 skip | `MPU FAULT: task 'root' attempted write at 0x40834000 -- reported` |
| `pizero2350` | PMSAv8, 150 MHz | 2026-07-28 `6857df3`; `selftest` + `mpu_fault` at `3204121` | `kickos_services_none`, kernel console | 62 / 61 `ok` / 1 skip -- vs 62 / 62 `ok` / 0 skip | `=== MPU FAULT ===` `CFSR=0x82` `MMFAR=0x20026000`; plus `mpu_fault` child-to-child `CFSR=0x82` `MMFAR=0x20027000` |
| `rx72m` | RXv3 RX-MPU, 240 MHz | 2026-07-28 `d71b313` | `kickos_services_none`, kernel console | 62 / 61 `ok` / 1 skip -- vs 62 / 62 `ok` / 0 skip | `MPU FAULT: task 'root' attempted write at 0x14000 -- reported` |
| `f411disco` | PMSAv7, 84 MHz | 2026-07-29 `6646c8e` | `kickos_services_none`, kernel console | 62 / 61 `ok` / 1 skip -- vs 62 / 62 `ok` / 0 skip | `=== MPU FAULT ===` `CFSR=0x82` `MMFAR=0x2000a000`; the control-side `mpu_fault` is `CFSR=0x82` `MMFAR=0x2000b000` |
| `frdmk64f` | SYSMPU, 120 MHz | 2026-07-29 `127efb5` | **full** (`k64uart` + `k64dspi`) | 65 / 63 `ok` / 2 skips -- **no control at this tip** | `=== HARD FAULT ===` `CFSR=0x400` `HFSR=0x40000000` + `SYSMPU ISOLATION FAULT: port=3 addr=0x2001a000 master=0 W EDR=0x80000003` |

Five things read across the whole table.

- **The one skip is always `mpu_privileged_guard`**, named on the wire, and it is the entire skip
  delta between the columns -- posture-driven rather than provisioning. Its wire line, identical on
  every board that printed it, against the control where it simply ran:

  ```
  ok 54 - mpu_privileged_guard # SKIP root unprivileged: no privileged caller exists; the inverse claim is apps/rootfault
  # skipped: 1
  # all tests passed (1 skipped)
  ```
  ```
  ok 54 - mpu_privileged_guard
  # skipped: 0
  # all tests passed
  ```

  Same plan (`1..62`), same 61 other verdicts in the same order: that one line was the whole A/B
  inside the TAP stream. Neither arm can be re-run -- the case is gone and so is the knob.
  `frdmk64f`'s two-skip variant is kept as its own transcript below, because *M4.5.6* reads a
  measured 2 -> 1 delta against it.
- **The report carries a task name on some families and cannot on others.** ARM MemManage goes
  straight to the armv7m reporter, which prints the register dump and labels it `=== MPU FAULT ===`
  only when the CFSR MMFSR byte is set; the `MPU FAULT: task 'root'` form comes from
  `kickos_isr_fault`, the RISC-V / chip-hook route. `tests/check_rootfault.sh` encodes exactly
  that two-family split. On the ARM boards, attribution to root therefore rests on the
  announce-before-poke ordering plus the `MMFAR` match.
- **Only the XMC's announce line is clipped.** It runs a userspace console driver, so
  `kpanic_enter`'s reclaim eats the tail (`(expect fault` with no closing paren), reproducibly across
  resets. The other five run the kernel console, so no userspace-driver reclaim sits in the panic
  path and the announce line prints whole.
- **In every row the child had already written the same region and was still parked**, so the page
  belonged to a live foreign domain rather than being unmapped.
- **Nothing outside the XMC witnesses a console handover under the flip** -- no other board of the
  six has a userspace console driver.

Captures are machine-local: `.session/n33-rewitness/` (XMC re-witness), `.session/logs/` (Pi, RX,
F411), `.session/logs/m453-witness/` (K64F), `.session/` (C6). The XMC hashes are what the silicon
banners stamped; after the M4.5.1 squash they resolve against `backup/m4.5.1-pre-squash`, not the
live branch.

#### `xmc4800-relax` -- PMSAv7

The first board flipped, and the first silicon witness for the boundary. Both arms were re-captured
on the post-rebase tree; the earlier pre-rebase capture (`a463ab9`) is superseded and its hash no
longer exists on this branch. The `75227d4` re-run reproduced `PC`, `LR`, `CFSR` and `MMFAR`
identically, including the reclaim clip.

**The control column previously read "58 `ok`, 1 skip", and the way it was wrong is worth keeping**:
the 1 was `mutex_deadlock # SKIP pool too small` from the **FRDM-K64F full-service-list** run,
transcribed into the XMC row. Under the console-only list this board skips nothing in the privileged
arm. **The 2026-07-27 run skipped two cases, not one**, because it predates `kos_mem_self_grant`:
`irq_as_event` (root plays the device, writing a page it allocated but was never granted) was a
missing capability rather than a posture cost and it ran in both postures. Re-measured at `75227d4`,
`irq_as_event`, `mem_self_grant` and `mem_self_grant_nonpow2` all ran `ok` under enforcement in both
postures.

**The `xmcssc` SPI service passes on silicon** (2026-07-29, `aa084a9`, default privileged-root
posture -- banner `mpu enforce`; captures under `.session/logs/m453-spihalt/`). Not an arm of the A/B
and it could not be one at that tip: the flipped image dropped `xmcssc` from the service list
entirely, pending the USIC `FDR`/`BRG`/`CCR` write seam. **That is closed** -- the seam landed and
`xmcssc` came up as a SERVICE on the full default list under an unprivileged root on 2026-07-30
(*M4.5.6* below). All five arms pass here, on RTT only, since the console belongs to `xmcuart`:

```
[xmcssc] config rc=0 achieved=57599 Hz
[xmcssc] config: PASS
[xmcssc] single-byte loopback: PASS
[xmcssc] multi-byte loopback: PASS
[xmcssc] null-tx (dummy 0x00) loopback: PASS
[xmcssc] transact (cmd+read, one CS bracket): PASS
[xmcssc] loopback PASS (call/reply SSC service echoes tx==rx)
```

The loopback is internal -- no device is fitted on that bus -- so it witnesses the channel clocking
and echoing its own words, not a link to a peer. Register state on the halted target:
`U0C1_KSCFG=0x00000005` (`MODEN` set, so the write landed), `CCR=0x0000C001` (`MODE`=SSC),
`FDR=0x0288816F`, `BRG=0x000D3C00`, `SCTR=0x073F0101`, SCU `CGATSTAT0=0x00000000` (nothing gated)
and CCU4 slice-0 `TIMER` advancing, with the core in `arch_idle_wait`. Bench note: those USIC
registers all read `0xFFFFFFFF` on a target halted at reset, before the block is ungated -- a dump
taken too early says nothing about what the driver programmed.

#### `esp32c6-wroom` -- RISC-V PMP

The second board flipped, and the first silicon witness for the boundary on **PMP** rather than an
ARM MPU.

**What had to close first: the GPIO matrix.** A pad on this family passes two mux stages, and
`arch_pinmux_set` mediated only the IO_MUX pad word -- so the matrix out-sel was reachable only as
raw MMIO, and `c6blink` did that out-sel write plus its `GPIO_ENABLE_W1TS` direction write from
`main`, i.e. from root. Both stages now ride one `kos_pinmux_set` call (`4947e6e`) and `c6blink` does
no MMIO from root at all (`e5c651b`); its unprivileged driver holds the 64 B pin bank and does
direction, drive and readback itself. The silicon proof that both stages really landed is the pad
readback -- `GPIO_IN` tracked the drive on all ten cycles in both postures, which it cannot do unless
the pad was muxed to the matrix *and* the matrix out-sel set to 128. `c6blink`'s isolation oracle
moved with the widened window and now pokes ungranted `GPIO_FUNC10_OUT_SEL_CFG`, the matrix
escalation surface itself:

```
[c6blink] PASS (pad tracked the drive on every cycle)
[c6blink] poking UNGRANTED out-sel @ 0x6009157c (expect MPU FAULT)

MPU FAULT: task 'c6blink' attempted write at 0x6009157c -- reported
```

Identical in both postures, which is the point: the app is posture-independent, so its enforcement
signal is attributable to PMP and not to the root posture.

#### `pizero2350` -- PMSAv8

The third board flipped, and the first silicon witness for the boundary on **PMSAv8** -- a
Cortex-M33 running the `armv7m` arch backend with `KICKOS_ARM_PMSAV8_SOURCE` linked in. The
`rootfault` row is the `6857df3` capture; the `selftest` pair and `mpu_fault` were measured at
`3204121` with `KICKOS_SHUTDOWN_TO_BOOTLOADER=ON`, and all three banners stamp that commit, so the
two `selftest` columns are the same tree and differ only in the knob (`clean-selftest-{flip,ctl}.log`
and `clean-mpufault-flip.log`).

**This transcript is one of the two kept verbatim, register lines included, as the exemplar of the
fault's shape. Do not condense it.**

```
[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x20026000 (expect fault)

=== MPU FAULT ===
  PC=0x10000530 LR=0x10000577 xPSR=0x1000000 (PSP)
  R0=0x52 R1=0x3 R2=0x2222 R3=0x20026000 R12=0x0
  CFSR=0x82 HFSR=0x0
  MMFAR=0x20026000
```

`MMFAR` matches the address the app announced, `R3` holds it and `R2` the stored `0x2222`,
`CFSR=0x82` is DACCVIOL + MMARVALID, and `(PSP)` puts the fault in thread mode. `PC` sits in XIP
flash at `0x10000530`, which is where root's code executes from on this part.

**The privileged control was measured, so the trap is attributable to the posture.** The same binary
on the same tip and board, built with `KICKOS_ROOT_PRIVILEGED` at what was then its default ON,
printed banner `mpu enforce` and then
`[rootfault] cross-domain write completed: root is NOT confined (expected with KICKOS_ROOT_PRIVILEGED=1 or no enforcement)`
-- no fault, no dump, at the same announced `0x20026000`. So the two runs put root's write at the
same place and differed only in the knob.

**`mpu_fault` adds a second, independent PMSAv8 denial under the flip** -- child-to-child rather than
root-to-child, so it checks the descriptor programming without root's posture in the picture at all.
Its subject is an unprivileged domain-A thread in either posture (the region base arrives through the
thread ARG by value precisely so root never touches domain A), which is why that arm has no control:

```
[domain] A: writing my own region
[domain] A: my region ok; writing domain B (expect fault)
=== MPU FAULT ===  CFSR=0x82  MMFAR=0x20027000
```

Same signature shape as the `rootfault` arm at a different address and from a different faulting PC.
This board had a pre-flip `mpu_fault` witness (see the status matrix); this is the run under an
unprivileged root.

#### `rx72m` -- RXv3 RX-MPU

The fourth board flipped, and the only witness for the boundary on the **RX MPU** -- the fleet's one
CPU-side unit that checks the peripheral/SFR aperture as well as RAM. Both arms' banners stamp
`commit d71b313`, so the two columns are the same tree. **There is NO emulator model for RXv3
anywhere** -- no QEMU machine, no CI job (see *CI coverage* above, RXv3 row: `none`). This flip is
silicon-only and unfalsifiable off the bench: nothing in the repo can re-derive it.

The fault is the **named** reporter, not the nameless `=== RX EXCEPTION (access exception) ===` dump:
`kickos_rx_fault_report` routes vector `0x54` to `kickos_isr_fault` only when the faulting `PSW.PM`
is set, and a flipped root runs in user mode, so it qualifies. That branch had never been reachable
from root before the flip.

**What had to close first: the mux had no mediation at all.** `rx72m` had no `arch_pinmux_set`
backend, so the declining fallback (`arch/common/arch_pinmux_set_default.cc`) answered `-KOS_ENOSYS`
and any privileged caller could re-point the SCI6 console
pins; and `rxdrv` wrote `PORT8.PMR`, `PODR` and `PDR` raw from `main`, i.e. from root. `PMR` selects
peripheral-vs-GPIO per pin and is therefore an escalation surface, so it is now kernel-mediated
(`f891ea0`); `PDR` and `PODR` are not, once the pin is owned, so they moved in-window (`a5a70b6`).
The backend covers **both** RX mux stages in one call -- the MPC `PmnPFS` function select and
`PORTm.PMR` -- because `PSEL` can re-point a pin already at `PMR=1`, which the console pins are,
without `PMR` ever being written; mediating `PMR` alone would have left the refusal bypassable.
`PB1/TXD6` and `PB0/RXD6` are refused `-KOS_EBUSY`, and `rxdrv` asks for exactly that refusal on the
wire:

```
[rxdrv] pinmux P80 -> general I/O rc 0
[rxdrv] pinmux PB1/TXD6 refused (-KOS_EBUSY): console pin is kernel-owned
```

That second call requests `PSEL=000000b` with `PMR=0` on the console's TX pin -- the write that would
dark the console if the refusal ever regressed -- so a run that stops at that line is itself the
failure signal.

A `PmnPFS` write needs the MPC `PWPR` unlock (`B0WI=0`, then `PFSWE=1`) and is legal only while the
pin's `PMR` bit is 0 (UM r01uh0804ej0120 sec.23.4.1 steps 1-6, sec.23.4.2 (1)); miss either and the
write is silently dropped. `PORTm.PMR` itself needs no unlock -- `PRCR` gates only the clock /
operating-mode / low-power / LVD registers (sec.13.1.1) and does not cover the PORT or MPC blocks.
**The unlock is not independently witnessed by `rxdrv`**: `P80PFS`'s reset value is already the
`0x00` the app writes, so a dropped write would look identical. It is witnessed instead by the
console itself -- `sci6_console_init` goes through the same unlock helper, and its `PSEL=001011b`
writes to `PB1PFS`/`PB0PFS` are what put the banner on the wire at all.

`rxdrv`'s isolation oracle moved with the widened window and now pokes ungranted `PORT8.PMR`, the mux
escalation surface the backend just took over:

```
[rxdrv] blink 10 pad=0/0 pad=1/1
[rxdrv] PASS (pad tracked the drive on every cycle)
[rxdrv] poking UNGRANTED PORT8.PMR @ 0x0008C068 (expect MPU FAULT)

MPU FAULT: task 'rxdrv' attempted write at 0x8c068 -- reported
```

Identical in both postures, which is the point: the app is posture-independent, so its enforcement
signal is attributable to the RX MPU and not to the root posture. The poke is a plain store, not a
read-modify-write -- an RMW faults on its read half and the report named a read, which understates
the escalation. The `pad=0/0 pad=1/1` columns are `PORT8.PIDR`, the pin state, which reads regardless
of `PDR`/`PMR` (UM sec.22.3.3); it tracked the drive on all ten cycles in both arms, and it cannot
unless `PMR` really cleared and `PDR` really set output. The window widened from the 16 B `PODR`
block to 80 B at the port base (`PDR` + `PODR` + `PIDR` up to `PORTF`), ending at `0x0008C04F` -- 16
B, one full register row, short of the `PMR` block at `0x0008C060` -- still one RX-MPU descriptor,
since the unit wants a 16-aligned base and a 16-multiple size and no power of two. Covering
`PDR`/`PODR` for every port is an unavoidable over-grant (the RX interleaves ports inside each
register block rather than blocking per port) but not an escalation: a pin at `PMR=1` ignores
`PDR`/`PODR` entirely (UM Table 23.47), so the console pins stay unreachable through the window.

**`stress` was NOT run under the flip.** RX sign-off historically paired "selftest + stress", but
`apps/common/stress` spawns privileged children at three sites and structurally cannot work with an
unprivileged root -- that is stage-4 work, not a regression of this flip.

#### `f411disco` -- PMSAv7

The fifth and last board of the declared stage-2 set, and the **second** PMSAv7 witness after the
XMC. Flashed over the onboard ST-Link and read on USART2/PA2 through an FT232 (`/dev/ttyUSB1`); both
banners stamp `commit 6646c8e`, so the two columns are the same tree and differ only in the knob.

**This board's enforcement had never been witnessed at all before this run** -- the status matrix
carried `stm32f411` as build-and-link validated with the MPU HW pending. So the debt was closed
first, in the then-default posture, and only then was the knob flipped.

**Phase 1, the enforcement witness in the then-default posture.** `selftest` under
`-DKICKOS_HAVE_MPU=1` ran **62 of 62 with nothing skipped**, including `mmio_grant`, `domain_share`,
`grant_reserved`, `confused_deputy`, `mem_self_grant` and `mem_self_grant_nonpow2` -- the cases that
only mean anything with a live MPU. Then the cross-domain denial:

```
[domain] A: writing my own region
[domain] A: my region ok; writing domain B (expect fault)
=== MPU FAULT ===  CFSR=0x82  MMFAR=0x2000b000
```

`kos_ram_alloc` handed domain A `0x2000a000` and granted it the low 4 KiB, so `0x2000b000` is one
region past the grant -- the app does not print the number, so that arithmetic is what ties the trap
to the announced write. That closes the `stm32f411` MPU HW debt: PMSAv7 on this chip enforces and is
no longer inferred from the XMC. Since the MPU backend is the shared `stm32f411` one, the closure
covers `blackpill` too.

**Phase 2, the flip. This transcript is the second of the two kept verbatim, register lines included.
Do not condense it.**

```
[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x2000a000 (expect fault)

=== MPU FAULT ===
  PC=0x80005a4 LR=0x80005eb xPSR=0x1000000 (PSP)
  R0=0x52 R1=0x3 R2=0x2222 R3=0x2000a000 R12=0x0
  CFSR=0x82 HFSR=0x0
  MMFAR=0x2000a000
```

`MMFAR` matches the address the app announced, `R3` holds it and `R2` the stored `0x2222`,
`CFSR=0x82` is DACCVIOL + MMARVALID, and `(PSP)` puts the fault in thread mode. The privileged
control ran on the same tip and board, completed the write and printed
`cross-domain write completed: root is NOT confined` at the same `0x2000a000`.

**`f411spi` stays unwitnessed on this board.** It writes no MMIO from root -- `main` muxes `PE3` and
`PA5`/`PA6`/`PA7` through `kos_pinmux_set`, and the unprivileged driver thread holding the 32 B SPI1
grant calls `kos_periph_enable(win)` as its first act, then configures SPI1 inside that window
(`design-spi-driver-stm32f411.md`; the seam's contract is `design-unprivileged-root.md` stage 3).
What is open is bench time, not code: the app has not run on silicon in either posture since that
rework, and its loopback arm additionally needs the PA7->PA6 jumper fitted. So the chip's
peripheral-window proof remains open and the canonical PMSA peripheral proof stays `xmcspi`.

**The pre-stage-3 fault, measured.** With the bring-up still in `main`, i.e. in root, the app faulted
under the flip on its very first bring-up store -- `=== MPU FAULT === CFSR=0x82 MMFAR=0x40023830`,
and `0x40023830` is `RCC_AHB1ENR` (RCC base `0x40023800` + `0x30`). The same shape `c6blink` and
`rxdrv` showed before their windows were reworked, and the reason the STM32 apps got the same
treatment.

#### `frdmk64f` -- SYSMPU

The sixth board, the last enforcement backend to be flipped, and the **only board flipped on its full
service list** (`KICKOS_SERVICE_LIST=kickos_services_frdmk64f` = console `k64uart` + SPI `k64dspi`,
board pin map `kickos_pinmap_frdmk64f`). Captured over the OpenSDA J-Link VCOM; both banners stamp
`commit 127efb5`.

What makes this a stage-3 witness rather than a sixth repeat of stage 2: **root writes no MMIO on
this board at all.** The other five got flipped by taking the peripheral work out of the image
(console-only, or `kickos_services_none`); this one keeps both drivers and moves the work across a
seam instead. `k64uart` and `k64dspi` each call `kos_periph_enable(win)` as their own first act, so
the `SIM_SCGC*` clock ungate and the `AIPS0` PACR unprotect happen inside the unprivileged window
holder rather than in root, and the four DSPI pins moved into the board pin map, which root applies
through `kos_pinmux_set` before any service starts. `kickos_services_frdmk64f` is therefore no longer
listed in `KICKOS_SERVICE_LIST_ROOT_MMIO`, so the configure-time refusal that used to fire for this
board no longer applies. The run also witnessed the console handover to `k64uart` (TAP via the
driver), `k64dspi` up in the same image, and `ok 47 - periph_enable_unheld`.

**There is no control column at this tip**, so read this board's row as one arm, not an A/B. Its
privileged-posture reference is the earlier SYSMPU regression at `75227d4` (61 cases / 60 `ok` / 1
skip / 0 fail, `mpu_fault` trapping at `0x2001b000`) -- a different tree and a different case count.
What this run attributes is the seam, not a posture delta measured side by side on one tree.

#### When an MMIO grant is INERT, and the one test that decides it

**Canonical statement. Seven source files used to write this out longhand, which is how one of the
copies came to be wrong.** Cite this section instead of restating it.

The chip fact: on `mk64f`, AIPS peripheral bridges are **not** SYSMPU slave ports (K64 RM 3.3.6.2 /
3.3.7.1 -- the MPU's slave ports cover flash, SRAM and FlexBus only, and protection for the bridges
is "built into the bridge"). User access is enabled per 4 KiB slot by clearing that slot's `PACR` SP
bit, and AIPS granularity is the whole slot, so **once a slot is open EVERY unprivileged thread
reaches it**. On this chip an MMIO window is therefore not a per-thread *peripheral* capability. What
SYSMPU still enforces is MEMORY isolation, which is what the scramble tests fault against.

**But "the MPU does not gate this chip's peripherals" is NOT the test for whether a grant may be
deleted.** The test is: **does this spawn's grantee call a `kos_periph_*` syscall?** MMIO possession
is the *sole* authorisation for `arch_periph_enable` (`kernel/syscall/syscall_mem.cc`), so a grant
feeding such a call is load-bearing no matter what the MPU does with peripherals. Measured across the
tree:

| grant | verdict |
|---|---|
| `system/driver/mk64f/k64uart`, `k64uartirq`, `k64dspi`; `user/apps/rx72m/rxdrv`; `user/apps/f411disco/f411spi`; `user/apps/xmc4800-relax/xmcspi` | **LOAD-BEARING** -- each calls `kos_periph_enable`. Deleting the grant breaks the device |
| `user/apps/common/gpioblink`, `user/apps/frdmk64f/k64console`, `user/apps/frdmk64f/k64drv` | genuinely inert; kept for spawn-signature parity and portability to an enforcing chip |

`k64uart.cc` had already drifted into calling its own live grant inert. Deleting it on that comment's
word would have silenced the K64F console, which is why the test above is stated in terms of the
syscall and not the silicon.

**Both skips are named on the wire, and this `# skipped: 2` is the before-side of the
`mpu_privileged_guard` deletion** -- *M4.5.6* below reads a measured 2 -> 1 delta against exactly
this transcript, so it is kept whole:

```
ok 18 - mutex_deadlock # SKIP pool too small
ok 56 - mpu_privileged_guard # SKIP root unprivileged: no privileged caller exists; the inverse claim is apps/rootfault
# skipped: 2
# all tests passed (2 skipped)
```

`mutex_deadlock` is the pre-existing `KICKOS_MAX_THREADS` constraint of the full service list
(recorded under *Per-board caveats* above, and shared with `xmc4800-relax`'s full-list runs), not a
posture cost -- which is why this board reported two skips where the console-only boards reported
one.

The core label on the confinement fault reads `HARD FAULT`, not `MPU FAULT`, because SYSMPU is
bus-slave-side: the denial arrives as an imprecise bus fault (`CFSR=0x400` IMPRECISERR,
`HFSR=0x40000000` FORCED), so the architectural registers are post-fault and `PC`/`LR` do not name
the culprit. The attributable evidence is the chip hook's own line -- `addr` matches the announced
address, `master=0` is the core, `W` is the direction, `port=3` is the SRAM slave port. Unlike the
PMSAv7 boards there is no `MMFAR` to cross-check.

**What had to close first: the clock gate and the bus protect were root's.** Both drivers' bring-up
wrote `SIM_SCGC4`/`SIM_SCGC6` and the `AIPS0` PACR directly, and no window grant can ever cover those
-- they are chip-global registers, and SYSMPU cannot gate peripherals at all. That seam is
`arch_periph_enable`, keyed on the exact register-block base and gated on possession of a live DEV
window at that base rather than on an authority bit; the contract and the per-chip table are in
`porting.md`. The **PIT is deliberately absent from that table**: `AIPS0` classifies per 4 KiB slot,
so a granted channel-2 window would also expose the chained ch0+ch1 pair `arch_clock_now` runs on, so
the PIT stays kernel-gated at boot instead.

The board pin map is 5 rows (`system/init/frdmk64f/pinmap.cc`) -- the LED plus the four DSPI pins
root muxes before any service starts: PTB21 `func=0x100` (ALT1) blue LED as GPIO; PTD1 / PTD2 / PTD3
`func=0x200` (ALT2) as DSPI0 SCK / SOUT / SIN; PTC4 `func=0x100` (ALT1) as the DSPI0 software CS.
All four DSPI rows read back their programmed mux on a halted target in the runs below --
`PORTD_PCR1`/`PCR2`/`PCR3` = `0x200`, `PORTC_PCR4` = `0x100`.

**`k64dspi` drives a real transfer, and this is the first silicon witness of one** (2026-07-29,
`aa084a9`, an EasyCAT LAN9252 shield fitted on the CS above, default privileged-root posture --
banner `mpu enforce`; captures under `.session/logs/m453-spihalt/`). The service reads the ESC's
`BYTE_TEST` signature and passes on the first attempt:

```
[k64dspi] SPI service up (DSPI0, polled FIFO, GPIO CS on PTC4)
[k64dspi] config rc=0 achieved=10000000 Hz
[k64dspi] BYTE_TEST attempt 1: 0x87654321 (xfer OK)
[k64dspi] LAN9252 BYTE_TEST PASS: ESC SPI link OK (read 0x87654321 through the call/reply SPI service)
```

Those four lines are **RTT-only**: the console belongs to `k64uart`, so no app line reaches the VCOM
(see *Per-board caveats* above). The plain `-st` image carries no RTT, and there the same outcome is
read off the halted target instead -- `MCR=0x80000000` (master, out of reset), `CTAR0=0x38010000`
(8-bit frame), `SR=0xC2020303` with TCF set and `TXNXTPTR`/`POPNXTPTR` both 3, which is the seven
words the transfer moved taken modulo the 4-entry FIFO; `AIPS0 PACRF=0x44440444`, i.e. slot 44's `SP`
cleared, which is `arch_periph_enable`'s unprotect standing; and `GPIOC_PDDR`/`PDOR` both `0x10`,
PTC4 an output with CS idle high. The core sits in `arch_idle_wait` with `CycleCnt` advancing between
halts, and does not fault.

What this adds is the bus round trip, not a second stage-3 arm: it is the privileged posture, so the
same transfer **under the flip is still owed**, and the canonical PMSA peripheral proof stays
`xmcspi` on the XMC.

### Stage 4 -- root narrows its own authority (2026-07-30)

Stage 4 makes root hand the app only the authority the app declared: the default init calls
`kos_cap_narrow` after the pin map and the service list, with a mask from the per-app
`KICKOS_APP_AUTHORITY`. Witnessed on **two of the six** boards carrying a confinement witness, the
two J-Link ones, each captured over its own VCOM from a clean worktree. Both banners read
`enforce, root unprivileged`.

| Board | Backend | Service list | `selftest` | `authority_cap` | Console driver + TAP route |
|---|---|---|---|---|---|
| `xmc4800-relax` | PMSAv7 | console-only | 65 / 64 `ok` / 1 skip | `ok 46` | `[xmcuart] driver up (polled TX)`; `stdout endpoint -> console driver (service list published)` |
| `frdmk64f` | SYSMPU | **full** (`k64uart` + `k64dspi`) | 65 / 63 `ok` / 2 skips | `ok 46` | `[k64uart] driver up (polled TX)`; same TAP route |

The skips were named on the wire and neither was new: `mpu_privileged_guard # SKIP root unprivileged:
no privileged caller exists` on both, plus `mutex_deadlock # SKIP pool too small` on the K64F only
(the XMC's pool is large enough, which is why its `ok` count is one higher). Of those two only
`mutex_deadlock` survives in a current run.

**Read the selftest rows for what they actually prove, which is not the narrow.** `selftest` declares
five of the six bits (`KICKOS_APP_AUTHORITY`, everything but `AUTH_PSTATE`), because the suite drives
the authority gates from root. So on these two runs root gave up **`AUTH_PSTATE` and nothing else**:
`AUTH_PINMUX` and `AUTH_CONSOLE` were kept, and the TAP route line shows the console path surviving
bring-up, not surviving a narrow. What the rows do witness is that the re-cut, the delegation refusal
and `kos_cap_narrow` are all correct on two MPU families -- `ok 46 - authority_cap` carries the
narrow arms, where a worker drops its only authority and the gate that had just answered for it
returns `-KOS_EPERM` -- plus the fact that a real per-app mask takes effect at all, since root
demonstrably still held `AUTH_PINMUX` (it hands that bit to `auth_worker`, which `thread_spawn`
refuses for a caller that lacks it) and `AUTH_CONSOLE` (`console_publish_priv` asserts `-KOS_EBADF`,
which becomes `-KOS_EPERM` without the bit).

**`consoledemo` on the XMC is the run where root does give the bits up**, and the only capture here
that witnesses the narrow end to end: it declares no mask, so it takes the default
`AUTH_MEMORY | AUTH_SYSTEM`, and root loses `AUTH_PINMUX` and `AUTH_CONSOLE` after using both.
`[init] pre-publish ctor line` on the kernel console, then `[xmcuart] driver up`, then
`[root] post-publish line via the userspace driver` and five worker lines -- root printing through
the driver it just handed off, with the console and pinmux authorities already gone.

**What this does NOT cover.** Three of the six boards were not re-witnessed at THIS tip
(`esp32c6-wroom`, `rx72m`, `f411disco`), so for them stage 4 rested on emulation here.
(`pizero2350` was not re-witnessed at this tip either, but it WAS re-witnessed later, at `c5d9b0d`
-- its `rootfault` and `rootauth` are in *M4.5.6* below.) The three apps that declare a wider mask
and mux their own pins from root -- `c6blink`, `rxdrv`, `f411spi` -- are the direct hardware witness
that the per-app declaration works, and at THIS tip none of them had been run: the C6 attempt
reached `entry 0x40800000` and no further (the cause is now known and fixed -- the `.data` LMA bug
recorded in *M4.5.6* below), the RX72M was not reached, and the `f411disco` was not plugged. **Two of the three are now closed**: `rxdrv` on `rx72m` and `c6blink` on `esp32c6-wroom`,
both witnessed 2026-07-30 with their two-arm `kos_periph_enable` possession probes (see *M4.5.6*
below). **`f411spi` on `f411disco` is the only one still owed** -- that board was not on the
2026-07-30 bench. Stage 4's own confinement claim is also narrower than it
looks: a missed declaration is a runtime `-KOS_EPERM`, and no capture here exercises that path on
hardware -- it was witnessed on `qemu` only, by removing `AUTH_CONSOLE` from `initdemo`.

### M4.5.5 -- the third region-encoding mode (2026-07-30)

`arch_mpu_region_pow2()` splits the enforcing backends into two shaping modes, so the boards whose
descriptors actually change are the base+limit ones. Two of the three were witnessed here; `rx72m`
(RX MPU, 16-byte granule) was not available on this pass and was the one owed. **It was witnessed
later the same day** and reports `GRANULE-MULTIPLE (granule 16, 3-granule request reserved 48)` -- see
*M4.5.6* below -- so nothing in this table is owed any more.

| Board | Backend | `pow2` | `selftest` | `region_mode` reports | Granular `mpu_fault` signature |
|---|---|---|---|---|---|
| `xmc4800-relax` | PMSAv7 | 1 | 66 / 65 `ok` / 1 skip | `POWER-OF-TWO (granule 32, 3-granule request reserved 128)` | -- (the CONTROL; it must not move) |
| `frdmk64f` | SYSMPU | 0 | 66 / 65 `ok` / 1 skip | `GRANULE-MULTIPLE (granule 32, 3-granule request reserved 96)` | `SYSMPU ISOLATION FAULT: port=3 addr=0x2001a140 master=0 W EDR=0x80000003` |
| `pizero2350` | PMSAv8 | 0 | 66 / 66 `ok` / 0 skip | `GRANULE-MULTIPLE (granule 32, 3-granule request reserved 96)` | `CFSR=0x82` (DACCVIOL + MMARVALID, a PRECISE fault) with `MMFAR=0x20026020` |

`xmc4800-relax` is the CONTROL: PMSAv7 genuinely requires a power of two. Its one skip is
`mutex_deadlock # SKIP pool too small`, the same skip the stage-4 capture carried. Both moved boards
run the full service list, so TAP routes through the userspace console driver.

**The fault captures are the part emulation cannot give.** Under granular shaping a granted base is
only granule-aligned, so the enforced boundary lands on an address that is NOT a round power of two
-- exactly what makes a mis-programmed base+limit descriptor visible. Both faults are at
`base + 4096` off a 32-aligned base (`frdmk64f` `base = 0x20019140`), the worker having written its
own granted region first. The K64F's is an imprecise bus fault, so the SYSMPU `EDR` line rather than
`MMFAR` is the authoritative address; the Pi's is the only capture in the fleet that pins PMSAv8
`RBAR`/`RLAR` bounds for a non-power-of-two-aligned region, and QEMU is a weak witness for it.

**`rootauth` on `frdmk64f`, root unprivileged** -- the first silicon run of the ROOT-narrow gate, in
the posture where it bites: `PASS`, five arms, banner `mpu enforce, root unprivileged`. (That tip
still had the `KICKOS_ROOT_PRIVILEGED` knob and this image set it `OFF`; the knob and the banner
suffix are both gone as of 2026-07-30, and the same gate was re-run at the new tip -- see *M4.5.6*
below.) Its declared-bit arm answered `rc=-22` (`-KOS_EINVAL`) from the REAL mk64f
`arch_pinmux_set`, where `qemu`/`sim` see `-38` (`-KOS_ENOSYS`) from the declining fallback
(`arch/common/arch_pinmux_set_default.cc`) -- so that arm is
not an artefact of the stub. It does NOT witness a real mux WRITE: the probe uses an out-of-range
port that the backend rejects before touching a register, which is why `c6blink` / `rxdrv` /
`f411spi` were owed at this tip. `rxdrv` and `c6blink` have since landed real mux writes
(*M4.5.6* below); `f411spi` is the only one still owed.

**What this does NOT cover.** No `rx72m` at THIS tip (the third moved board; it was witnessed later
the same day, see *M4.5.6* below), and the boot-arena recovery is not
directly observable in a capture -- it is the configure-time `boot stacks ... pow2=N` line, and it
moves alignment only, never a size. A wrong `arch_mpu_region_pow2()` literal in a backend cannot be
caught in-tree at all (`cmake/boot_arena.cmake` scrapes the same file the link resolves), so these
captures are the only check on that class.

### M4.5.6 -- the privileged-register write seam, the knob's deletion, the ring arm (2026-07-30)

**Read the provenance first: this subsection holds 31 wire captures under FIVE distinct commit
stamps, and the milestone BROKE ITS OWN PROCESS RULE.** Five stamps is a floor on the number of
trees, not a count of them: a `-dirty` stamp names no content, and the two `2fc7799-dirty` sets below
are demonstrably different trees. Captures are machine-local, under `.session/m456-silicon/`,
alongside the f302 fault-investigation debugger transcripts; the two provisioning watermark logs are
`.session/pv-wm1.log` and `.session/pv-cfgY1.log`. What each stamp covers:

- **`2fc7799-dirty` -- 7 files, 6 rows.** Master plus uncommitted work. Five from the first half
  (`xmcspi`, `xmccshold`, the `frdmk64f` `selftest`, the `frdmk64f` `rootauth`, the `pizero2350`
  `selftest`) and two from the second (both `f302nucleo` `selftest` AFTER captures, which are
  MASTER-based with the provisioning change applied -- not the branch tip at all).
- **`c5d9b0d` -- 7 files, 6 rows.** Committed. `pvprobe`, `conreclaim`, `inprstorm` at two rate
  profiles, `frdmk64f` `rootfault`, `pizero2350` `rootfault`, `pizero2350` `rootauth`.
- **`270b6fa` -- 6 files, 6 rows.** Committed and CLEAN: the `b2-*` set (`rx72m` `selftest` /
  `rootauth` / `rxdrv`, `xmc4800-relax` `pvprobe` on the second unit and `xmcssc` as a service, the
  `f302nucleo` `selftest` BEFORE capture).
- **`270b6fa-dirty` -- 9 files, 7 rows.** The committed tip plus uncommitted work: the `b3-*`,
  `b5-c6*` and `c3-*` sets (`f302nucleo` `ringpriv`, every `esp32c6-wroom` capture, `inprstorm` at
  three rate profiles).
- **`124b68c` -- 2 files, 2 rows.** The milestone's own committed tip, and the ONLY captures taken
  from it: `f302nucleo` `ringppb` and `fault`.

**The process rule this milestone wrote is the rule this milestone then broke.** The first half's
five dirty captures produced the rule -- **commit before a witness pass** -- and the second half
went on to stamp `270b6fa-dirty` for nine files and a MASTER-based `2fc7799-dirty` for two more.
Only `b4-fault.log` and `b4-ringppb.log` came off a clean tree at `124b68c`. That is a finding about
the process, not about the boards: every dirty-tree capture remains valid for what it shows, but
none of them may be re-attributed to a committed tip, because the dirty content is pinned nowhere.
Where a claim depends on the difference -- the `f302nucleo` provisioning delta most of all -- the
transcript says so on its own line.

`MinSizeRel` throughout, on six boards: `xmc4800-relax` (PMSAv7), `frdmk64f` (SYSMPU, **full**
service list), `pizero2350` (PMSAv8, kernel console), `rx72m` (RX MPU), `esp32c6-wroom` (PMP NAPOT)
and `f302nucleo` (**no MPU at all** -- banner `mpu off`, and the only board here that can carry the
ring arm). The optimisation level is the preset default rather than a per-capture observation: every
base preset in `cmake/presets/*.json` sets `CMAKE_BUILD_TYPE=MinSizeRel`.

**The FIVE `xmc4800-relax` diagnostic apps ran `kickos_services_none`, NOT the console-only service
list** -- `pvprobe`, `conreclaim`, `inprstorm`, `xmcspi` and `xmccshold`, at every tip they were taken
at. An earlier revision of this line said console-only, and said four, and was wrong on both. Two
independent proofs: they were flashed from a `kickos_services_none` build dir, and every one of them
prints via `kos::print`, which a PUBLISHED console DROPS -- so a console-only list would have
produced silent captures. No `[xmcuart] driver up` line appears in any of them either. The
consequence matters most for `inprstorm`: what survived that storm was the **KERNEL console path**,
not the userspace `xmcuart` driver. The sixth XMC capture, `xmcssc` as a SERVICE, is the exception
and reads the other way: `[xmcuart] driver up` IS on its wire, because it runs the full list.

**Eight things are under test here, three from the first half and five from the second.** The first
three: the privileged-write seam `KOS_SYS_PERIPH_REG_WRITE` /
`arch_periph_reg_write`, possession-gated on a live DEV window based exactly at the block base, now
also bounded so the region CONTAINS the target word, and narrowed further by an exact
`(base, offset)` allowlist per chip; on the XMC4800 that allowlist is U0C1 `FDR` `0x010`,
`BRG` `0x014` and `CCR` `0x040`, the registers the XMC4800 reference manual marks `Write = PV`
(RM V1.3 Table 18-20). Then the deletion of `KICKOS_ROOT_PRIVILEGED`, of its banner suffix, and of
the `mpu_privileged_guard` test case. Then the widening of the panic-path console reclaim.

The five from the second half: the **ring arm**, both as a new permanent gate and as its first
silicon witness; the **`.data` LMA bug** on the esptool-loaded chips, which made the 2026-07-28 C6
witness a pass by luck; **honest thread-pool provisioning** on the fleet's smallest runnable board;
a **host gate for the seam** in `arch/sim/sim.cc`; and the three owed **mux-write witnesses**, two of
which close here. One thing came out OPEN: the `f302nucleo` fault reporter emits no dump.

| Board | Capture | Tip | Verdict |
|---|---|---|---|
| `xmc4800-relax` | `pvprobe` | `c5d9b0d` | seam writes land `exact`; the same thread's direct stores to the same three registers are `DROPPED`; both controls answer |
| `xmc4800-relax` | `conreclaim` | `c5d9b0d` | panic banner reaches the wire off a clock-gated channel; the widened reclaim is witnessed |
| `xmc4800-relax` | `inprstorm` | `c5d9b0d` | console survives an INPR reroute plus an unbounded TBUF storm, at BOTH ends of the rate knob |
| `xmc4800-relax` | `xmcspi` | `2fc7799-dirty` | `loopback PASS`, four words echoed on a real SSC channel |
| `xmc4800-relax` | `xmccshold` | `2fc7799-dirty` | `VERDICT: hardware CS-hold USABLE` |
| `frdmk64f` | `selftest` (full list) | `2fc7799-dirty` | 66 cases, 65 `ok`, `# skipped: 1`, 0 fail |
| `frdmk64f` | `rootfault` | `c5d9b0d` | `SYSMPU ISOLATION FAULT` |
| `frdmk64f` | `rootauth` | `2fc7799-dirty` | `PASS`, all five arms |
| `pizero2350` | `selftest` | `2fc7799-dirty` | 66 cases, 66 `ok`, `# skipped: 0`, 0 fail |
| `pizero2350` | `rootfault` | `c5d9b0d` | precise MemManage, `MMFAR=0x20025020 CFSR=0x82` |
| `pizero2350` | `rootauth` | `c5d9b0d` | `PASS`, five arms |
| `xmc4800-relax` | `pvprobe` (2nd physical unit) | `270b6fa` | adds the MASK-refusal arm: `CCR\|TBIEN rc=-22`, `pre == post` -- refused WHOLE, not trimmed |
| `xmc4800-relax` | `xmcssc` as a SERVICE | `270b6fa` | `[xmcssc] SPI service up` behind `[xmcuart] driver up` on the FULL default list; the board did not go dark |
| `xmc4800-relax` | `inprstorm`, THREE rate profiles | `270b6fa-dirty` | the TX-FIFO vector CLOSED: FIFO armed `SIZE=64`, 143 distinct beats, cadence unmoved |
| `rx72m` | `selftest` | `270b6fa` | 67 cases, every one `ok`, `# skipped: 0`; `GRANULE-MULTIPLE (granule 16, 3-granule request reserved 48)` |
| `rx72m` | `rootauth` | `270b6fa` | `PASS`, five arms, declared-bit arm `rc=-22` from the real RX `arch_pinmux_set` |
| `rx72m` | `rxdrv` | `270b6fa` | a real mux WRITE plus both possession arms; ungranted `PORT8.PMR` trapped -- **closes the owed `rxdrv` witness** |
| `esp32c6-wroom` | `hello` LMA diagnostic | `270b6fa-dirty` | `sidata=40806810 sdata=40820000` -- `.data`'s LMA outside every loaded segment |
| `esp32c6-wroom` | first-fault trap | `270b6fa-dirty` | `mcause=00000002 mepc=00000000` -- a NULL call, then infinite trap recursion and ZERO bytes out |
| `esp32c6-wroom` | `hello` raw-UART marker, post-fix | `270b6fa-dirty` | `ABCDEFGH` -- boot reaches the marker |
| `esp32c6-wroom` | `c6blink`, post-fix | `270b6fa-dirty` | `PASS`, both possession arms, ungranted out-sel trapped -- **closes the owed `c6blink` witness** |
| `esp32c6-wroom` | `selftest`, post-fix | `270b6fa-dirty` | 67 cases, every one `ok`, `# skipped: 0`; `POWER-OF-TWO (granule 8, 3-granule request reserved 32)` |
| `f302nucleo` | `selftest` BEFORE | `270b6fa` | 63 cases, `not ok 46 - periph_enable_unheld`, `# skipped: 9`, `# 1 test(s) failed` |
| `f302nucleo` | `selftest` AFTER (two identical runs) | `2fc7799-dirty` | 63 cases, `ok 46`, `# skipped: 5`, `# all tests passed (5 skipped)` -- FOUR real arms un-skipped |
| `f302nucleo` | `ringpriv` | `270b6fa-dirty` | `PASS (5 arms)` -- the project's FIRST ring-arm silicon witness |
| `f302nucleo` | `ringppb` | `124b68c` | BusFault CONFIRMED by live debugger (`CFSR=0x00008200`, `BFAR=0xe000ed00`), but **NO fault dump reaches the wire** -- OPEN |
| `f302nucleo` | `fault` | `124b68c` | truncates at `[f`, 338 bytes -- the same reporter hole, so it is not a `ringppb` bug |

Six boards, 27 rows, 31 files. Both `pizero2350` arms were owed by an earlier revision of this
subsection and are now TAKEN. The
`selftest` rows record **66** cases because that is what those images planned; the tree now plans
**67** (`privileged_spawn_refused` was added after they were captured), so do not read those totals
against a current in-env run.

**`pvprobe` is the capture that proves the seam is not a no-op**, and no other capture in the fleet
can stand in for it. One run, one unprivileged thread, one held U0C1 window, four measurements
inside that single window: the seam writes land, the same thread's direct stores to the *same three
registers* are dropped by the silicon, a positive control shows the thread's stores reach the window
at all, and a negative control shows the MPU enforcing. Byte-for-byte from the wire:

```
   commit  c5d9b0d

[pvprobe] XMC4800 U0C1 PV-write probe (RM V1.3 Table 18-20)
[pvprobe] unprivileged probe up (granted U0C1 window 0x200)
[pvprobe] unpriv KSCFG=0x5
[pvprobe] baseline through the seam: pattern B
[pvprobe] seam FDR: rc=0 wrote=0x2aa read=0x2aa exact
[pvprobe] seam BRG: rc=0 wrote=0x2aa0000 read=0x2aa0000 exact
[pvprobe] seam CCR: rc=0 wrote=0x2000 read=0x2000 exact
[pvprobe] unpriv SCTR[U,PV control]: pre=0x3030100 writing 0x7070101 ...
[pvprobe] unpriv SCTR[U,PV control]: post=0x7070101 LANDED (post == written)
[pvprobe] unpriv FDR[PV]: pre=0x2aa writing 0x155 ...
[pvprobe] unpriv FDR[PV]: post=0x2aa DROPPED (post == pre)
[pvprobe] unpriv BRG[PV]: pre=0x2aa0000 writing 0x1550000 ...
[pvprobe] unpriv BRG[PV]: post=0x2aa0000 DROPPED (post == pre)
[pvprobe] unpriv CCR[PV]: pre=0x2000 writing 0xc001 ...
[pvprobe] unpriv CCR[PV]: post=0x2000 DROPPED (post == pre)
[pvprobe] pattern A through the seam (expect exact)
[pvprobe] seam FDR: rc=0 wrote=0x155 read=0x155 exact
[pvprobe] seam BRG: rc=0 wrote=0x1550000 read=0x1550000 exact
[pvprobe] seam CCR: rc=0 wrote=0xc001 read=0xc001 exact
[pvprobe] refusals: off-allowlist rc=-22 (want -22), unheld-window rc=-1 (want -1)
[pvprobe] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)

=== MPU FAULT ===
  PC=0x80003b0 LR=0x800055d xPSR=0x61000000 (PSP)
  R0=0x3f R1=0x3 R2=0x8005597 R3=0x50004000 R12=0x4
  CFSR=0x82 HFSR=0x0
  MMFAR=0x50004648
```

**This is the RETAKE, and its point is partly a negative one.** It was re-run at `c5d9b0d`
specifically to check that the new offset-containment bound did not break the legitimate consumer
path, because that bound is the one change in this milestone that could have refused a call the
driver depends on. It did not: all six seam writes across both patterns still land `exact`, and both
refusals still answer with the codes they answered before (`rc=-22` off-allowlist, `rc=-1`
unheld-window). A containment check that is invisible to a correct caller and fatal to an
out-of-window one is exactly the shape wanted.

Read it as four claims that only mean something together:

- **The seam works.** `FDR`, `BRG` and `CCR` read back `exact` through `arch_periph_reg_write`, on
  two different patterns, so the second trio cannot be reading a value the first trio happened to
  leave behind.
- **The silicon really withholds those registers from an unprivileged store.** The identical thread,
  in the identical window, writing the identical registers directly, gets
  `DROPPED (post == pre)` on all three. That is the whole reason the seam exists.
- **Positive control: `SCTR[U,PV control]` `LANDED`.** An unprivileged store from this thread does
  reach the window -- `pre=0x3030100`, wrote `0x7070101`, `post=0x7070101`. So the three `DROPPED`
  lines are the PV restriction and not a dead window, a wrong base or a thread that never ran.
- **Negative control: `MMFAR=0x50004648 CFSR=0x82`.** The same thread poking ungranted SCU traps
  (DACCVIOL + MMARVALID). So the MPU was live and enforcing for the whole run, and none of the four
  lines above can be explained by enforcement having been off.

**Both refusals were answered on hardware, not only in the in-env gate**: off-allowlist
`rc=-22` (`-KOS_EINVAL`) -- an offset inside the granted block that the chip table does not list --
and unheld-window `rc=-1`, the possession refusal, from a caller with no live DEV window at the
base. Those are the two ways the seam could have been a general privileged-write primitive, and
both were closed on silicon.

**The MASK-refusal arm arrives in a later `pvprobe`, on the OTHER physical XMC unit (`270b6fa`,
CLEAN).** The transcript above predates the per-entry mask, so it carries no mask arm; this capture
adds one and confirms the rest independently, on a second J-Link (`591165808`). It supersedes
nothing -- the older transcript stays, because it is the one that pinned the containment retake:

```
[pvprobe] mask refusal: CCR|TBIEN rc=-22 (want -22), pre=0xc001 post=0xc001 unchanged
[pvprobe] refusals: off-allowlist rc=-22 (want -22), unheld-window rc=-1 (want -1)
```

`pre == post` is the load-bearing part. A request whose value sets a bit outside the entry's mask
column is refused WHOLE and the register does not move -- not trimmed to the permitted bits and
written anyway, which is the failure mode a mask column invites. Two boards' worth of confirmation
for the same seam, from two units, is also the reason this arm is not read as a single-unit quirk.
Note the CCR baselines differ between the two units' captures (`0x2000` there, `0x4000` here); the
verdict lines do not depend on the baseline, only on `post == pre`.

**`xmcspi` -- a real SSC transfer, and the run that validated `FDR_RESULT_MASK`.** The seam
configures the channel and then the loopback moves four words:

```
[xmcspi] seam FDR: rc=0 wrote=0x816f read=0x39b816f LANDED
[xmcspi] seam BRG: rc=0 wrote=0xd3c00 read=0xd3c00 LANDED
[xmcspi] seam CCR: rc=0 wrote=0xc001 read=0xc001 LANDED
[xmcspi] starting SSC loopback (blocking on USIC0 SR1 IRQ 85)
[xmcspi] word 0: tx=0xa5 rx=0xa5 PASS
[xmcspi] word 1: tx=0x3c rx=0x3c PASS
[xmcspi] word 2: tx=0x0 rx=0x0 PASS
[xmcspi] word 3: tx=0xff rx=0xff PASS
[xmcspi] loopback PASS (all words echoed equal)
```

The `FDR` line is the point. `wrote=0x816f` but `read=0x39b816f`: `FDR` carries a live
`RESULT[25:16]` field that the fractional divider keeps moving, so a naive read-back-and-compare
would have reported a **false** `DISCARDED` on a write that landed perfectly. `FDR_RESULT_MASK`
excludes that field from the comparison and the verdict came out `LANDED`, correctly. That mask was
the implementer's only self-flagged unconfirmed literal.

**Read that as exercised, not validated.** The drift is real and directly visible across captures --
the same `wrote=0x816f` reads back `0x39b816f` here and `0x2c6816f` in `inprstorm` below, differing
only above bit 16, and `inprstorm`'s MAX profile adds a second operating point (`wrote=0x83ff
read=0x3d183ff LANDED`). So the mask is confirmed WIDE ENOUGH to cover the field that moves, at two
operating points. It is NOT confirmed to be no wider than that: a mask that also excluded genuine
config bits would produce an identical `LANDED` verdict on every one of these runs, because nothing
here writes a value that would differ only in an over-masked bit. Narrowing that gap needs a probe
that writes a config bit inside the masked span and requires a `DISCARDED`. The literal is also
duplicated in **four** places (`xmcspi`, `xmccshold`, `inprstorm`, `xmcssc.cc`), so a correction has
four sites to reach; one home would be better.

As on `pvprobe`, the ungranted
SCU poke at `0x50004648` traps with `CFSR=0x82` and the matching `MMFAR`, so this transfer happened
under live enforcement. This is the canonical PMSA peripheral proof, now taken with root writing no
USIC register at all -- `xmc_spi0_start` contains no register access.

**`xmccshold` -- hardware CS-hold, through the same seam:**

```
[xmccshold] seam FDR: rc=0 wrote=0x816f read=0x39b816f LANDED
[xmccshold] seam BRG: rc=0 wrote=0xd3c00 read=0xd3c00 LANDED
[xmccshold] seam CCR: rc=0 wrote=0x1 read=0x1 LANDED
[xmccshold] run 1 FEM=1: driving 4-word software-paced frame
[xmccshold] FEM=1: MSLS edges = 2 : PASS
[xmccshold] run 2 FEM=0: driving 4-word software-paced frame
[xmccshold] FEM=0: MSLS edges = 8 : PASS
[xmccshold] VERDICT: hardware CS-hold USABLE (FEM=1 holds, FEM=0 pulses)
```

Two edges with `FEM=1` -- one assert, one deassert around the whole four-word frame -- against
eight with `FEM=0`, one pair per word. That is the hold behaving as a hold, measured rather than
assumed, and it is the answer a software-paced multi-word transaction needs.

**`inprstorm` -- the console DoS probe, run at both ends of the rate knob at `c5d9b0d` and then at
THREE profiles at `270b6fa-dirty`.** The holder reroutes U0C1's interrupt nodes onto `SR0`, the
console's node, and then storms the channel without ever clearing `RIF`/`AIF`. The comparison
profile, from the earlier pair:

```
   commit  c5d9b0d

[inprstorm] XMC4800 console DoS probe via U0C1 INPR reroute onto SR0
[inprstorm] MARKER: root up, spawning the U0C1 holder
[inprstorm] rate profile: 115.2 kHz comparison
[inprstorm] heartbeat 0 t=0ms dt=0ms
[inprstorm] unpriv up (granted U0C1 window 0x200)
[inprstorm] profile 115.2 kHz comparison: STEP=367 PDIV+1=14 PCTQ+1=1 DCTQ+1=16 fPERIPH=72000000 Hz
[inprstorm] seam FDR: rc=0 wrote=0x816f read=0x39b816f LANDED
[inprstorm] seam BRG: rc=0 wrote=0xd3c00 read=0xd3c00 LANDED
[inprstorm] seam CCR: rc=0 wrote=0xc001 read=0xc001 LANDED
...
[inprstorm] rerouting INPR RINP/AINP -> SR0 (console node)
[inprstorm] INPR before=0x1100
[inprstorm] INPR after =0x0
[inprstorm] CCR        =0xc001
[inprstorm] storming TBUF0 forever (RIF/AIF never cleared) ...
[inprstorm] heartbeat 7 t=2100ms dt=300ms
...
[inprstorm] heartbeat 133 t=39908ms dt=300ms
```

And the MAX-RATE profile, same image, same board, only the divider chain changed:

```
[inprstorm] rate profile: MAX RATE
[inprstorm] profile MAX RATE: STEP=1023 PDIV+1=1 PCTQ+1=1 DCTQ+1=1 fPERIPH=72000000 Hz
[inprstorm] seam FDR: rc=0 wrote=0x83ff read=0x3d183ff LANDED
[inprstorm] seam BRG: rc=0 wrote=0x0 read=0x0 LANDED
[inprstorm] seam CCR: rc=0 wrote=0xc001 read=0xc001 LANDED
...
[inprstorm] heartbeat 133 t=39908ms dt=300ms
```

**The two `c5d9b0d` profiles are IDENTICAL where it counts**: 134 heartbeats, a steady `dt=300ms`,
the last one at `t=39908ms`, in both. The second profile shifts roughly **625x faster** than the
first.

**The pair was then re-run as a TRIPLE at `270b6fa-dirty`, and the third profile is the one that
closes the residual.** All three runs are ~45 s. Same board, same J-Link (`591165808`):

| Profile | `STEP` | `PDIV+1` | `PCTQ+1` | `DCTQ+1` | Distinct beats | Steady `dt` | Last |
|---|---|---|---|---|---|---|---|
| `115.2 kHz comparison` | 367 | 14 | 1 | 16 | 143 | 300 ms | `t=42608ms` |
| `MAX RATE` | 1023 | 1 | 1 | 1 | 143 | 300 ms | `t=42608ms` |
| `FIFO autonomous drain + MAX RATE` | 1023 | 1 | 1 | 1 | 143 | 300 ms | `t=42608ms` |

```
[inprstorm] heartbeat 142 t=42608ms dt=300ms
```

**COUNTING CONSTRAINT.** Each log holds 150 heartbeat LINES but only **143** distinct beats,
numbered 0 through 142: the serial reader captured seven duplicated lines ahead of the banner. Read
143, and read the beat NUMBER rather than counting lines. Every `dt` is `300ms` except beat 0's
`dt=0ms`; "no outliers" is a DERIVED reading of that, not a word any capture prints. The 134-beat
figure above and the 143-beat figure here differ only by capture window length -- roughly 40 s
against 43 s of wire -- not by cadence.

**It remains a bounded CPU tax, not a denial of service -- but the REASON is the scheduling model,
not the baud.** That distinction is the whole substance of these captures, and it replaces the
earlier record. The earlier verdict rested on a single measurement at the 115.2 kHz profile plus an
instrumented `~37,700 console_tx_isr invocations/second` figure, and a single operating point could
not establish a worst case here, because **the seam hands the attacker the rate knob**: `FDR` and
`BRG` are two of the three allowlisted registers, so the attacker reconfigures the very parameter the
measurement was taken at.

Silicon settles it. The storm thread runs at priority 1, below root's 2, and its loop writes `TBUF0`
**one word at a time from software**, so its rate is capped by its CPU share however fast the channel
shifts. Raising the shift clock ~625x changes the console cadence not at all, because the loop was
never shift-clock-bound. The bound is therefore a property of the scheduler, which the attacker
cannot reach, rather than of a divider setting, which it can. That is a strictly stronger claim than
the one it replaces.

Two further notes on reading this. The heartbeats now carry `t=<ms> dt=<ms>` precisely so that
DEGRADATION is measurable off the wire instead of only survival -- the earlier capture had no
timestamps at all, so a large cadence slowdown would have been invisible in it. And no frequency is
asserted anywhere: the RM `fSCLK` factor could not be confirmed, so the app prints programmed value,
read-back and branch clock, and the operator reads the actual rate off the wire.

The attack has no privileged accomplice: the three `FDR`/`BRG`/`CCR` writes that set the channel up go
through the seam from the holder itself, and root arms nothing. **What survived is the KERNEL console
path**, not a userspace driver -- these apps ran `kickos_services_none` (see the provenance note at
the top of this subsection), so no `xmcuart` was ever up. What an in-window holder can do to a shared
interrupt node is take cycles; it cannot silence the console.

**The TX-FIFO vector is CLOSED, and the verdict came out STRONGER than the one it replaces.** The
residual named here before was the TX FIFO clocking words back-to-back with no per-word CPU work --
the one path that escapes the scheduling bound. The third profile exercises it and the cadence does
not move.

**It needed NO allowlist widening, which is itself the finding.** Per XMC4700/XMC4800 RM V1.3
Table 18-20 only `FDR`, `BRG` and `CCR` carry `Write = PV`; `TBCTR` (108H), the `INx` push aperture
(180H + x*4), `TRBSR` (114H) and `CCFG` (004H) are all `U,PV`. So an in-window holder reaches every
one of them with a plain store, and this profile measures what the CURRENT grant already permits
rather than what a widened one would.

**RM subtlety worth recording, because it invalidates the ORIGINAL probe.** Writing `TBUF0` (080H) is
the STANDARD buffer and silently BYPASSES the FIFO; the push aperture is `INx` (RM p.18-223), and the
FIFO auto-loads `TBUF` whenever `TCSR.TDV=0` (RM 18.2.8.4). The original `inprstorm` fed `TBUF0`, so
**it was never exercising the FIFO at all** -- the residual was real and the probe that was supposed
to cover it could not have.

Autonomy is proven BEFORE the null result, which is the order that makes the null result mean
something:

```
[inprstorm] CCFG (TB=bit7)=0x80cf
[inprstorm] TBCTR=0x6000000 SIZE=64 LANDED (FIFO armed, no seam)
[inprstorm] slow-divider preload TBFLVL=64, after 10ms no-fill=0 (backlog drains autonomously if falling)
```

`CCFG` bit 7 set says the transmit-buffer FIFO is present on this channel; `TBCTR` landing with
`SIZE=64` says it is armed, and `no seam` says the store went straight through the granted window.
Then `TBFLVL=64` preloaded against a slow divider, with the level FALLING over 10 ms and no refill,
is the channel shifting words out on its own -- the autonomy the vector depends on. A capture that
showed a steady console WITHOUT this arm would be indistinguishable from a FIFO that never armed.

**The structural reason, which is what generalises past this operating point.** A backlog only builds
while drain < fill, so the SUSTAINED rate on `SR0` is `min(fill, drain)`. The drain side is the
channel; the fill side is the attacker's sub-root CPU share refilling a **finite 64-deep** FIFO. A
deeper FIFO buys a longer burst at the same sustained rate, and a faster clock drains the burst
sooner: **FIFO depth and clock rate trade off against each other, they do not multiply.** That is why
no third knob setting needs trying. Still a bounded parasitic CPU tax, not a DoS.

Deliberate note on reading the three logs: the two non-FIFO profiles still print
`storming TBUF0 forever (RIF/AIF never cleared) ...` and that is CORRECT -- they are the
software-paced comparison points, kept unchanged so the FIFO profile has something to be compared
against. Only the third feeds `IN0`, and it says so on its own line
(`FIFO drain storm: refilling TX FIFO, channel clocks SR0 autonomously`).

**`conreclaim` -- the panic banner reaches the wire off a channel that was already dead
(`c5d9b0d`).** This app had NEVER been run before this capture. An unprivileged holder of the U0C0
window gates the console channel's kernel clock (`KSCFG.BPMODEN=1, MODEN=0`) and then forces an MPU
fault; PASS is the fault dump arriving at all. Full capture:

```
   commit  c5d9b0d

[onreclaim] scramble-then-panic console-reclaim test
[conreclaim] U0C0 garbled (clock gated); forcing MPU fault

=== MPU FAULT ===
  PC=0x8000218 LR=0x800035d xPSR=0x61000000 (PSP)
  R0=0x8005130 R1=0x3 R2=0x1 R3=0xa R12=0x4
  CFSR=0x82 HFSR=0x0
  MMFAR=0x40030200
```

**Read the ORDER of those lines, because it is the evidence.** The
`U0C0 garbled (clock gated); forcing MPU fault` line was printed *after* the channel clock was
gated, so it could not possibly have reached the wire when it was written. It appears here because
the post-reclaim flush drained it out of the console ring. Then the `=== MPU FAULT ===` dump
transmits on a channel that was dead moments earlier. **Pre-fix behaviour was nothing at all after
the pre-scramble line** -- the banner was written into a gated channel and lost, which is the exact
silent-panic-loss the reclaim exists to prevent.

`MMFAR=0x40030200` is one word past the granted window, the deliberate fault address. `CFSR=0x82`
is `DACCVIOL | MMARVALID`, a precise MemManage. The first line reads `[onreclaim]` rather than
`[conreclaim]`: that is the documented leading-byte artifact of this board's reclaim (the XMC ASC
line-recovery transient described under *Known artifact* in `console.md`), not a corrupted capture.

What this witnesses is the **widened reclaim gate**. `kpanic_enter` previously reclaimed only while
the console was `USER_OWNED`; this app runs under `kickos_services_none`, so nothing ever published
and the console was `KERNEL_OWNED` when the channel died. The gate is now `state != RECLAIMED`, and
that widening is only safe because every chip reclaim body is idempotent absolute stores -- so this
capture is simultaneously the witness for the fix and the reason that idempotence requirement is
load-bearing rather than incidental. `design-unprivileged-root.md` section 8 carries the argument.
No CTest gate exists or can: the XMC has no QEMU model, and PASS is an operator reading a banner.

**`frdmk64f` `selftest` on its FULL service list (`k64uart` + `k64dspi`)** -- 66 cases, 65 `ok`,
one skip, no failures:

```
[k64uart] driver up (polled TX)
1..66
# tap route: stdout endpoint -> console driver (service list published)
...
ok 18 - mutex_deadlock # SKIP pool too small
...
ok 47 - periph_enable_unheld
ok 48 - periph_reg_write_unheld
...
# region shaping: GRANULE-MULTIPLE (granule 32, 3-granule request reserved 96)
ok 59 - region_mode
...
ok 66 - mem_self_grant
# skipped: 1
# all tests passed (1 skipped)
```

**The skip count is the independent silicon confirmation that `mpu_privileged_guard` is gone.**
The previous full-service-list run on this board reported `# skipped: 2`, naming both
`mutex_deadlock` and `mpu_privileged_guard` on the wire (the transcript is in the *`frdmk64f` --
SYSMPU* subsection above). This run reports `# skipped: 1`, `mutex_deadlock` alone. That 2 -> 1 is
exactly the delta the deletion predicts, measured on hardware rather than inferred from the source
tree. The before-side to read it against is that full-service-list transcript and not the *M4.5.5*
row above: that row also reports one skip for this board, but it never broke the skip out on the
wire, so the transcript is the only prior record that names which cases skipped.

The plan total also happened to stay at **66** across the deletion, because the same milestone added
`ok 48 - periph_reg_write_unheld` next to the older `periph_enable_unheld` -- one case out, one case
in. **Do not lean on that.** The inference above is carried entirely by the fact that BOTH runs NAME
their skips on the wire, not by the totals agreeing: a plan count can hold steady for unrelated
reasons, and this one has since MOVED to 67 (`privileged_spawn_refused` was added after these
captures). Compare the named transcripts, never the numbers.

That also settles a question M4.5.5 left open, where three skip counts across boards looked mutually
inconsistent. `master` defaults `KICKOS_ROOT_PRIVILEGED=ON`, so the M4.5.5 selftest rows ran
**privileged** and `mpu_privileged_guard` therefore RAN instead of skipping. All three counts
reconcile on that, and the open question is closed.

`region_mode` reports `GRANULE-MULTIPLE (granule 32, 3-granule request reserved 96)`, re-confirming
the M4.5.5 SYSMPU shaping at the new tip, and the wire names its own transport --
`# tap route: stdout endpoint -> console driver (service list published)` -- so this verdict came
out through the userspace `k64uart` driver, not the kernel console.

**`frdmk64f` `rootfault` -- SYSMPU denies root's cross-domain write (`c5d9b0d`, retaken):**

```
[rootfault] root: writing the child's granted region at 0x20015140 (expect fault)

=== HARD FAULT ===
  PC=0x4a0 LR=0x5a9 xPSR=0x61000000 (PSP)
  R0=0x523c R1=0x3 R2=0x1 R3=0x2222 R12=0x3
  CFSR=0x400 HFSR=0x40000000
  (imprecise bus fault: PC/regs are post-fault, not the culprit)
  SYSMPU ISOLATION FAULT: port=3 addr=0x20015140 master=0 W EDR=0x80000003
```

Retaken at the committed tip because the reclaim widening changed the panic path on EVERY board, so
every fault capture in the fleet needed re-witnessing rather than inheriting. The attributable line
is byte-identical to the earlier run: `addr=0x20015140`, `master=0`, `W`, `port=3`.

**This is the discriminating posture witness for the knob deletion.** `chip_mk64f.cc` keeps SYSMPU
`RGD0` at supervisor-`rwx`, so a privileged root writing this address would NOT fault -- the
isolation fault is reachable only from a user-mode root. Unlike the banner suffix, whose absence
proves nothing about posture, this capture cannot be produced by a privileged root at all.

The core label reads `HARD FAULT` because SYSMPU is bus-slave-side: the denial arrives as an
**imprecise** bus fault (`CFSR=0x400` IMPRECISERR, `HFSR=0x40000000` FORCED), which is how SYSMPU
reports, and the capture carries that caveat on its own wire -- `PC`/`LR`/registers are post-fault
and do not name the culprit. The attributable line is the chip hook's: `addr=0x20015140` matches the
address the app announced, `master=0` is the core, `W` the direction, `port=3` the SRAM slave port.
The granted base is 32-aligned rather than 4096-aligned, the same granular shaping the M4.5.5 fault
captures pinned.

**`frdmk64f` `rootauth` -- `PASS`, all five arms:**

```
[rootauth]   pinmux_set (declared bit) rc=-22
[rootauth] ok - declared KOS_AUTH_PINMUX survived the narrow
[rootauth]   console_publish (undeclared bit) rc=-1
[rootauth] ok - undeclared KOS_AUTH_CONSOLE was dropped by the narrow
[rootauth] ok - declared KOS_AUTH_MEMORY reached  cap_narrow to KOS_AUTH_SYSTEM rc=0
[rootauth] ok - root narrowed its own cap further
[rootauth]   pinmux_set after dropping it rc=-1
[rootauth] ok - the just-dropped KOS_AUTH_PINMUX is now refused
[rootauth] PASS
```

Same five arms as the M4.5.5 run above, re-taken at this tip with the knob deleted, and the
declared-bit arm again answers `rc=-22` from the real mk64f `arch_pinmux_set` rather than a stub.
It still does not witness a real mux WRITE, for the same reason recorded there.

**`pizero2350` `selftest` (PMSAv8)** -- a clean sweep, no skips at all:

```
1..66
# tap route: kernel debug console (stdout not published)
...
ok 18 - mutex_deadlock
...
ok 48 - periph_reg_write_unheld
...
# region shaping: GRANULE-MULTIPLE (granule 32, 3-granule request reserved 96)
ok 59 - region_mode
...
ok 66 - mem_self_grant
# skipped: 0
# all tests passed
```

66 of 66, `# skipped: 0`, `# all tests passed`. `mutex_deadlock` runs here -- this board's thread
pool is large enough, so the K64F's one remaining skip is provisioning and nothing else.
`region_mode` again reports `GRANULE-MULTIPLE (granule 32, 3-granule request reserved 96)`,
re-confirming PMSAv8 granular shaping at the new tip. The wire names its transport as
`# tap route: kernel debug console (stdout not published)`, the counterpart of the K64F line: this
board publishes no console, so the verdict comes out of the kernel console directly.

**The banner collapse, observed on three boards -- and it witnesses LESS than it looks.** Every
banner in every capture above prints its `mpu` line as the bare word, the XMC's being:

```
   board   xmc4800-relax
   arch    armv7m
   mpu     enforce
```

`mpu enforce` with **no `, root unprivileged` suffix** -- the same on `xmc4800-relax`, `frdmk64f` and
`pizero2350`. Under a single posture that suffix was a constant, so it carried no information; it is
gone from the tree along with the knob.

**That absence is evidence about the IMAGE, not about the POSTURE.** It proves these three boards ran
an image carrying the new banner code, and nothing more. The argument for deleting the suffix is
precisely that it conveyed nothing about posture, so its absence cannot convey anything either --
treating three suffix-free banners as "the knob deletion witnessed on three boards" would be
circular. The posture is witnessed by the two `rootfault` captures above (`frdmk64f`, where SYSMPU
`RGD0` keeps supervisor `rwx` so the fault is unreachable from a privileged root; and `pizero2350`),
and by the `# skipped: 2` -> `# skipped: 1` named-transcript delta. Those are discriminating; the
banner is not.

The practical consequence for reading this file: the banner `mpu` line no longer distinguishes
postures, which is what several of the dated A/B tables above used it for.

**`pizero2350` `rootfault` -- PMSAv8 denies root's cross-domain write (`c5d9b0d`).** Owed by the
previous revision of this subsection, and now TAKEN:

```
   board   pizero2350
   commit  c5d9b0d

[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x20025020 (expect fault)

=== MPU FAULT ===
  PC=0x100002ba LR=0x100002ed xPSR=0x61000000 (PSP)
  R0=0x10004d30 R1=0x3 R2=0x1 R3=0x2222 R12=0x3
  CFSR=0x82 HFSR=0x0
  MMFAR=0x20025020
```

A **precise** MemManage (`CFSR=0x82` = `DACCVIOL | MMARVALID`) with `MMFAR` naming the exact address
the app announced -- the cleanest shape a confinement fault can have, and the counterpart to the
K64F's imprecise bus fault above. PMSAv8 reports core-side, so `MMFAR` is authoritative and the
dumped `PC`/`LR` do name the culprit. This is the second discriminating posture witness for the knob
deletion, alongside the `frdmk64f` fault.

**`pizero2350` `rootauth` -- `PASS`, five arms (`c5d9b0d`).** Also owed by the previous revision:

```
[rootauth]   pinmux_set (declared bit) rc=-22
[rootauth] ok - declared KOS_AUTH_PINMUX survived the narrow
[rootauth]   console_publish (undeclared bit) rc=-1
[rootauth] ok - undeclared KOS_AUTH_CONSOLE was dropped by the narrow
[rootauth] ok - declared KOS_AUTH_MEMORY reached kos_ram_alloc
[rootauth]   cap_narrow to KOS_AUTH_SYSTEM rc=0
[rootauth] ok - root narrowed its own cap further
[rootauth]   pinmux_set after dropping it rc=-1
[rootauth] ok - the just-dropped KOS_AUTH_PINMUX is now refused
[rootauth] PASS
```

All five arms print and the tail reads `PASS (5 arms)`. The count is not a floor: `tests/check_app_arms.sh`
requires EXACTLY the number its caller declares, on the sim as well as under QEMU, so the post-narrow
refusal arm -- the only one proving `kos_cap_narrow` takes EFFECT rather than returning 0 and changing
nothing -- cannot be deleted with the gates still green. The capture above predates the `(5 arms)`
suffix the verdict now carries.

The board getting back on the USB bus is what unblocked both: it left mid-session in the previous
pass (KickOS has no USB device stack, so nothing in the image can hold the connection up) and needed
a physical BOOTSEL press. `KICKOS_SHUTDOWN_TO_BOOTLOADER` returning to BOOTSEL after a FAULT, not
only after a clean shutdown, is what made the pair capturable off one press.

**`xmcssc` AS A SERVICE is now witnessed (`270b6fa`, CLEAN).** Every earlier XMC capture in this
subsection ran `kickos_services_none`; this one runs the board's FULL default service list under
enforcement:

```
[xmcuart] driver up (polled TX)
[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)
```

Two lines, and both of them matter. `KICKOS_SERVICE_LIST_ROOT_MMIO` is now **EMPTY**
(`CMakeLists.txt:380`) and `xmc4800-relax` defaults to its full list at `KICKOS_HAVE_MPU=1`, so this
image is the configuration the refusal used to forbid. **The board did not go dark**, which is
exactly the outcome that refusal guarded: bring-up MMIO from an unprivileged root after the console
handover fails silently and totally, with no diagnostic to read. Anything elsewhere in this file
claiming that list is still refused, or that `xmcssc` as a SERVICE is unwitnessed, is stale.
This capture does NOT witness a transfer through the service -- it is the bring-up line only; the
transfer evidence is `xmcspi`/`xmccshold` above, which are apps rather than services.

**The seam gains a HOST gate (`arch/sim/sim.cc`), so the mechanism is no longer silicon-only.**
`arch_periph_reg_write` now has a sim backend over real pages: a 64 KiB window taken from the FIRST
of five candidate bases -- `0x40000000`, `0x100000000`, `0x400000000`, `0x10000000000`,
`0x100000000000`, 1 GiB to 16 TiB -- that `MAP_FIXED_NOREPLACE` accepts, PUBLISHED at init so the
seam, the encoder and `selftest` all answer for the same one. A fixed address would have made the
test's premise a bet on the host's address-space layout. The allowlist carries NO base column, and
one entry sits deliberately BEYOND the grantable window, so the containment hazard is reproducible
on the host rather than only arguable. Six mutations were each caught by a DISTINCT check: mask
widened, silent trim, refuse-and-act, containment dropped, alignment dropped, wrap dropped. So the
mechanism -- allowlist match, mask compare, refuse-not-trim, containment, alignment, wrap -- is
gated in CI, and the XMC captures are no longer its only evidence.

**HONEST RETRACTION carried with it.** With the window at 64 KiB, `grant_reserved`'s
`DEV_UNPRIVILEGED(0x40000000, 0x1000)` case is refused by ENCODABILITY again and is **VACUOUS on the
sim**, exactly as it was at baseline. An earlier claim that it became a real `AUTH_MEMORY` test does
NOT hold and is withdrawn. Two arms did genuinely change: the misaligned and the wrap arms were
VACUOUS EVERYWHERE before -- called from a HOLDER, both answer `-KOS_EINVAL` whatever the seam does
-- and they discriminate now only because they moved to the UNHELD worker, where the possession
refusal and the argument refusal are different codes.

What the host still CANNOT do, and these stay on the not-covered list: model the bus's PV
classification (a silent discard remains a `pvprobe`-only fact -- the host has no bus that withholds
a register's write side from user mode), prove a tabled block is CLOCKED, or check any real chip's
actual mask column. A green host gate says the seam's LOGIC is right, never that a chip's table is.

**`panicgate` is FIVE cases, and it is a real gate.** One source, five images, because a run observes
exactly one panic; each image carries `KICKOS_PANICGATE_CASE=<n>` and is registered as a CTest on
every one of the FOURTEEN ctest presets (`sim`, `sim-telem`, `qemu`, `qemu-telem`, `qemu-m3`, `qemu-m7`,
`qemu-m33`, `microbit`, `qemu-riscv`, and the five `-flat` variant presets `qemu-flat`,
`qemu-m33-flat`, `qemu-m7-flat`, `qemu-m3-flat`, `qemu-riscv-flat` -- registration keys off
`KICKOS_BOARD`, not the configuration variant), with two spellings of each verdict -- a CTest regex
on the sim and an `-F` literal for the QEMU script. Case 4 additionally asserts an ABSENT literal (`CUTME`), so
a truncation that dropped nothing would fail rather than pass. Keep it distinct from the
`conreclaim` note above, which correctly says no CTest gate exists or can for THAT capture: what
cannot be gated is a panic reaching the wire off a specific board's clock-gated channel. The panic
message CONTRACT -- truncation marker, control-byte stripping, unreadable-message fallback -- is
gated everywhere.

**`rx72m` -- all three owed items closed in one visit, and this one is CLEAN.** All three captures
stamp `commit 270b6fa`, a committed tip, unlike most of this half. E2 Lite `OBE110014` for flashing,
FT232 `BH001J9H` for the console.

`selftest`: `1..67`, every case `ok`, `# skipped: 0`, `# all tests passed`, and

```
# region shaping: GRANULE-MULTIPLE (granule 16, 3-granule request reserved 48)
```

**48, not 64, is the discriminating value.** A power-of-two backend would have rounded a 3-granule
request up to 64; 48 is `3 x 16` exactly, so the descriptor really is granule-multiple. That closes
the M4.5.5 debt for the third moved board, and it is the class `cmake/boot_arena.cmake` cannot catch,
because it scrapes the same file the link resolves.

**Do not over-read the number, though.** `f302nucleo`, which has **no MPU at all**, prints the
identical line -- `# region shaping: GRANULE-MULTIPLE (granule 16, 3-granule request reserved 48)`
-- because the no-MPU software model reports the same granule and the same arithmetic. So the value
alone does not prove the RX MPU. What the `rx72m` run adds is that the **RX backend's own literals**
produce it with the banner reading `mpu enforce`, which the no-MPU board cannot claim.

`rootauth` -- the stage-4 authority witness, in the posture where it bites:

```
[rootauth]   pinmux_set (declared bit) rc=-22
[rootauth] ok - declared KOS_AUTH_PINMUX survived the narrow
[rootauth]   console_publish (undeclared bit) rc=-1
[rootauth] ok - undeclared KOS_AUTH_CONSOLE was dropped by the narrow
[rootauth] ok - declared KOS_AUTH_MEMORY reached kos_ram_alloc
[rootauth]   cap_narrow to KOS_AUTH_SYSTEM rc=0
[rootauth] ok - root narrowed its own cap further
[rootauth]   pinmux_set after dropping it rc=-1
[rootauth] ok - the just-dropped KOS_AUTH_PINMUX is now refused
[rootauth] PASS
```

`rc=-22` on the declared-bit arm comes from the REAL RX `arch_pinmux_set` rejecting the probe's
argument, not from the declining fallback, which answers `-38`. Same reading as the `frdmk64f` row
above.

`rxdrv` -- **the mux WRITE, which is what was actually owed**, with two controls around it:

```
[rxdrv] PASS periph_enable root rc -1 (want -1)
[rxdrv] pinmux P80 -> general I/O rc 0
[rxdrv] pinmux PB1/TXD6 refused (-KOS_EBUSY): console pin is kernel-owned
[rxdrv] PASS periph_enable holder rc -38 (want -38)
[rxdrv] PORT8 PODR readback 0x1
[rxdrv] blinking LED6 (P80) via the 80 B port window
[rxdrv] blink 1 pad=0/0 pad=1/1
...
[rxdrv] blink 10 pad=0/0 pad=1/1
[rxdrv] PASS (pad tracked the drive on every cycle)
[rxdrv] poking UNGRANTED PORT8.PMR @ 0x0008C068 (expect MPU FAULT)

MPU FAULT: task 'rxdrv' attempted write at 0x8c068 -- reported
```

`pinmux P80 ... rc 0` is a mux write that LANDED, which no earlier capture in the fleet had -- the
`frdmk64f` `rootauth` arm reaches the backend and is rejected before any register is touched. The
next line is its counterpart: `PB1/TXD6` refused `-KOS_EBUSY` because the console pin is
kernel-owned, so the declaration widens the mask without handing over the console. `pad=0/0
pad=1/1` reads the pad register back against the drive on all ten cycles, and the ungranted
`PORT8.PMR` poke traps, so the MPU was live throughout.

**The `periph_enable` split is the two-arm possession probe, and the two codes mean different
things.** Root answers `-1` (`-KOS_EPERM`): it holds no window, so possession refuses it. The holder
answers `-38` (`-KOS_ENOSYS`): possession PASSED and the call reached a seam that has no RX backend.
A single arm could not separate those, which is the point of running both.

**`esp32c6-wroom` -- a real linker bug, and a recorded witness that had passed by LUCK.**
`esp32c6.ld` linked `.data` with an `AT > RAM` clause alongside an explicit VMA at `ORIGIN + 128K`,
while the load counter kept counting from the end of `.text`. `_sidata` therefore landed at
`0x408064c8`, OUTSIDE every loaded segment, and `Reset_Handler` dutifully copied uninitialised SRAM
over the `.data` the ROM had already placed correctly. `KICKOS_HAVE_MPU=1` only: the non-MPU `.data`
carries no explicit VMA, so LMA == VMA there and the copy is a no-op. Fixed by dropping the `AT` and
pinning it with `ASSERT(_sidata == _sdata)`, now `arch/riscv/chip/esp32c6/esp32c6.ld:280`. The same
latent construct was removed from `esp32.ld` -- image-neutral there, and the same assert now sits at
`arch/xtensa/chip/esp32/esp32.ld:152` so the construct cannot come back on either Espressif port.

The diagnostic capture, `270b6fa-dirty`:

```
sidata=40806810 sdata=40820000 edata=40820030 src+00=4cfe11d6 src+1c=ee95242c dst+00=00000000 dst+1c=00000000 alma=40828000 avma=40828000
```

`sidata` and `sdata` are about 102 KiB apart, `src+00`/`src+1c` hold garbage where `dst+00`/`dst+1c`
hold the zeroes the ROM left, and the `alma`/`avma` pair is a second span the same diagnostic prints,
where LMA and VMA DO agree -- so the divergence is specific to the section carrying the `AT`. The
`_sidata` value differs between the two captures below (`0x40806810` here, `0x408064c8` in the
first-fault image) because it tracks the end of `.text`, which moves with the image; the defect is
that it is outside every loaded segment in both. `src+1c` landed on `g_tx.armed`, statically false,
so `console_tx_write` called
`g_tx.backend->irq_enable` through NULL. The first fault:

```
FIRSTFAULT mcause=00000002 mepc=00000000 mtval=00000000 mstatus=00001801 ra=40800f82 sp=4087fdf0
```

`mcause=2` is an illegal instruction with `mepc=0` -- execution at NULL, the signature of a call
through a null function pointer. Then the fault reporter's own `kprintf` re-entered the same broken
path, giving **infinite trap recursion and ZERO bytes out**. That is why the symptom was a silent
board that printed `entry 0x40800000` and stopped: not a hang, a reporter eating its own tail. The
post-fix raw-UART marker run prints `ABCDEFGH`, so boot reaches the marker with the copy removed.

**This makes the 2026-07-28 `esp32c6-wroom` witness a pass BY LUCK, and the record has to say so.**
The bytes doing the corrupting are uninitialised SRAM, so whether anything load-bearing got clobbered
depended on the die and on power-on history. A passing run and a silent run are the SAME image. That
is also why bisecting found nothing -- there was no bad commit to find -- and why having multiple
physical C6 units mattered: this bench's unit is CH343P `5B61094913`, and `CONTEXT.local.md`'s
warning that the XMC and K64F exist in several copies applies to the C6 too. Two units of the same
part disagreeing is evidence about the image, not about either unit.

`c6blink` post-fix (`270b6fa-dirty`) -- **this CLOSES the owed `c6blink` mux-write witness**:

```
[c6blink] PASS periph_enable root rc -1 (want -1)
[c6blink] PASS periph_enable holder rc -38 (want -38)
[c6blink] GPIO_OUT readback 0x0
[c6blink] blinking GPIO10 via the 64 B PMP window
[c6blink] blink 1 pad=1/1 pad=0/0
...
[c6blink] blink 10 pad=1/1 pad=0/0
[c6blink] PASS (pad tracked the drive on every cycle)
[c6blink] poking UNGRANTED out-sel @ 0x6009157c (expect MPU FAULT)

MPU FAULT: task 'c6blink' attempted write at 0x6009157c -- reported
```

Same two-arm possession split as `rxdrv` (`-1` from root, `-38` from the holder), the pad tracked on
all ten cycles through a 64-byte PMP window, and the ungranted out-select trapped. `selftest`
post-fix: `1..67`, every case `ok`, `# skipped: 0`, `# all tests passed`, and

```
# region shaping: POWER-OF-TWO (granule 8, 3-granule request reserved 32)
```

**That granule-8 line is NOT a value only this board can produce.** `qemu-riscv +MPU` reports the
identical string, both being RISC-V PMP NAPOT, which forces a power of two by construction. So this
re-witnesses the ARCH CLASS on real silicon and confirms the emulator is faithful for it; it is not a
discriminating measurement the way `rx72m`'s `48` is against a pow2 backend.

**`virt.ld` is the COUNTER-CASE, and it belongs here as a trap for the next reader.**
`arch/riscv/chip/virt/virt.ld` carries the SAME `> RAM AT > RAM` construct and shows the SAME
divergence -- `_sidata=0x80010ce8` against `_sdata=0x80020000`, the segment reading
`VirtAddr=0x80020000 PhysAddr=0x80010ce8` -- and **there it is CORRECT**. QEMU's ELF loader honours
PhysAddr, so `.data`'s bytes really are at the LMA and the `Reset_Handler` copy does real work; the
green `qemu-riscv-mpu` run gate is the proof that it does -- a `.data` copy reading unloaded memory
there would take the suite down, not pass it. esptool `elf2image` builds from VMAs, which
is why the identical construct broke the ESP boards and not this one. The rule is **"an `AT` clause
is only valid if the loader honours LMA"** -- LOADER-dependent, not arch-dependent, and two RISC-V
chip scripts on opposite sides of it are the demonstration. A comment in `virt.ld` around line 158
already says do NOT add the esptool assert there; the porting-guide home for the rule is
`porting.md`, so read the guidance there rather than expecting it duplicated here.

**`f302nucleo` -- the board advertised capacity it could not back.** Arena consumed TO THE BYTE:
`5632 = 512 (idle) + 2048 (root) + 3 x 1024 (pool)`. The `-st` preset declared
`KICKOS_MAX_THREADS=4`, but a post-boot arena of 3072 B backs only THREE 1024 B stacks, so
`kos_ram_alloc(1)` returned NULL and `periph_enable_unheld` failed. **NOT a regression** -- master
(`2fc7799`) fails identically, so nothing in this milestone caused it.

BEFORE, at the clean `270b6fa`: `1..63`, `# skipped: 9`, `# 1 test(s) failed`, and

```
not ok 46 - periph_enable_unheld # .../user/apps/common/selftest/main.cc:3278: g_pe_ram_ran == 1
```

AFTER (and a second, identical run): `1..63`, `ok 46 - periph_enable_unheld`, `# skipped: 5`,
`# all tests passed (5 skipped)`. **Skips 9 -> 5, un-skipping FOUR real arms**:
`endpoint_crossdomain`, `mem_self_grant_nonpow2`, `region_mode` and `domain_share`.

**PROVENANCE, and it limits the claim.** Both AFTER captures stamp `commit 2fc7799-dirty` -- a
MASTER-based tree with the provisioning change applied, NOT the branch tip. The BEFORE capture is at
the clean `270b6fa`. So the 9 -> 5 delta is attributable to the provisioning change and to nothing
else on either tree, but it was **not re-taken at `124b68c`**, where the change actually landed. That
is the sharpest instance of this milestone breaking its own commit-before-witness rule.

The fix is `commit 124b68c`: `boards/f302nucleo/configs/base/defconfig` takes
`KICKOS_USER_STACK_SIZE` 2048 -> 1024 and `KICKOS_ROOT_STACK_SIZE` 2048 -> 1536, and the
`f302nucleo-st` preset takes `KICKOS_MAX_THREADS` 4 -> 3 and drops its own
`KICKOS_USER_STACK_SIZE` override. Every number was chosen against MEASURED paint-and-scan
watermarks over the whole selftest suite, which survive in `porting.md` rather than in any
generated header: deepest pool worker **592 B**, root **1048 B**, idle **76 B**. The wire evidence
is two arena lines,
baseline then new provisioning:

```
# PV arena total=5632 idle=512 free=436 root=2048 free=1000 pool0free=0 pool1free=0 pool2free=0 tail=0
# PV arena total=6048 idle=512 free=436 root=1536 free=488 user=1024
```

Same 1048 B root watermark either way (`2048 - 1000` and `1536 - 488`), so the smaller stack did not
push root deeper; it left **488 B**, 31.8% margin, at 1536 B. `pool0free=0 pool1free=0 pool2free=0
tail=0` on the baseline line is the starvation itself, printed: three pool stacks and not one byte
left over.

**TWO FINDINGS worth more than the numbers.**

- **8 of the board's 9 former skips were ARENA starvation, not the pool**, and every one of them was
  MISLABELLED `SKIP pool too small`. The reason is a real ambiguity, not sloppy text:
  `kos_thread_spawn` returns `-KOS_ENOMEM` for BOTH "slot table full" and "no stack block", and a
  test cannot tell those apart at runtime. So a message naming the pool was the honest guess and it
  named the wrong resource eight times out of nine. The right-size recovered FOUR of the eight; the
  remaining four want more arena than right-sizing can free on a 16 KiB part, so they stay skipped --
  correctly, and still under the wrong label.
- **Test 18 (`mutex_deadlock`) is mislabelled DIFFERENTLY, and no arena work will ever un-skip it.**
  Its guard is the configured `KICKOS_MAX_HANDLES` (7 on the supply-7 boards, of which this is one)
  or semaphore exhaustion, so it will keep skipping on this board however the stacks are sized.
  Anyone reading a residual `pool too small` there and reaching for more arena is chasing the wrong
  resource.

The 16 KiB limits that remain are genuine: `irq_as_event` needs one 4096 B block, and
`caller_stack`'s accept half needs 2064 B -- it still prints
`# caller_stack: PARTIAL -- accept half not run (arena cannot spare a stack)`, which is the honest
shape for a half-run case.

**`f302nucleo` `ringpriv` -- the ring arm, a FIRST for the project (`270b6fa-dirty`).** Board
`f302nucleo`, banner `mpu off`, real no-MPU armv7m silicon:

```
[ringpriv]   CONTROL=0x3
[ringpriv] ok - CONTROL.nPRIV=1: unprivileged, and off its reset value (a priv msr landed)
[ringpriv] ok - CONTROL.SPSEL=1: on SP_process, a second bit off reset
[ringpriv]   APSR after writing 0xF8000000=0xf8000000
[ringpriv]   APSR after writing 0x00000000=0x0
[ringpriv] ok - a permitted unprivileged msr DOES move the mrs read-back
[ringpriv]   CONTROL before the attempt=0x3
[ringpriv]   CONTROL after attempting to clear nPRIV=0x3
[ringpriv] ok - the unprivileged write to CONTROL.nPRIV was IGNORED: no self-promotion
[ringpriv] ok - the ignored write changed nothing in CONTROL (SPSEL/FPCA intact)
[ringpriv] PASS (5 arms)
```

`CONTROL=0x3` has `nPRIV` and `SPSEL` both OFF their reset values, so a privileged `msr` did land and
the thread really is on `SP_process` unprivileged -- two bits, not one, so a stuck read is ruled out.
The APSR arm then shows that a PERMITTED unprivileged `msr` DOES move the `mrs` read-back
(`0xf8000000` then `0x0`), which is what makes the negative arm meaningful: without it, an ignored
write and a broken read-back look the same. `CONTROL` unchanged at `0x3` after the clear attempt is
the result -- **no self-promotion** -- and the last arm adds that nothing else in `CONTROL` moved
either.

**`ringpriv` and `ringppb` are PERMANENT CI, not a bench capture, and that is the more durable half
of this.** Neither test is conditioned on enforcement -- a board's `flat` variant IS the ring-only
posture, its base one enforcing -- so both run on `qemu`, `qemu-m3`, `qemu-m7` and `qemu-m33` in
both postures. `microbit` asserts the OPPOSITE outcome with one arm
(`CONTROL.nPRIV == 0`: the nRF51822's Cortex-M0 has no privilege axis) rather than skipping, which
machine-checks the armv6m classification instead of quietly excusing it -- and it does NOT build
`ringppb`, because on a no-ring core the PPB read legitimately SUCCEEDS and a confinement gate there
would assert the wrong thing. The deeper design facts -- why a fault is the wrong expectation, why
only `CONTROL` can carry the read-back, and the `resting_npriv` hazard -- live in `porting.md`; read
them there rather than expecting them restated here.

**`f302nucleo` -- the fault reporter produces NO dump. RECORD THIS AS OPEN.** `ringppb` exposed it
and it is not `ringppb`'s bug. At `124b68c` that capture stops after two lines:

```
[ringppb] ok - control: a 32-bit volatile load of held memory succeeded
[ringppb] root: reading privileged-only SCB->CPUID at 0xe000ed00 (expect BusFault)
```

and the pre-existing `fault` app, which touches no privileged register at all, truncates identically
at `[f` -- 338 bytes, same tip. So the hole is the reporter, on this board, for any fault.

A live debugger attach at `HardFault_Handler` entry CONFIRMS the hardware:

```
***** HardFault_Handler ENTERED *****
PC=0x80008d0  LR(EXC_RETURN)=0xfffffffd  MSP=0x20003f60  PSP=0x200017e0
HFSR=0xe000ed2c:	0x40000000
CFSR=0xe000ed28:	0x00008200
BFAR=0xe000ed38:	0xe000ed00
USART2_CR1=0x40004400:	0x0000000d
USART2_ISR=0x4000441c:	0x006000d0
DHCSR(bit19=S_LOCKUP)=0xe000edf0:	0x01030003
```

Reading it: `CFSR=0x00008200` is BFSR byte `0x82` = `PRECISERR | BFARVALID`, and `BFAR=0xe000ed00` is
the exact address `ringppb` announced. `HFSR=0x40000000` is FORCED because `BUSFAULTENA` is never set
in-tree, so the BusFault escalates. **ARM ARM B3.1.1 is therefore CONFIRMED on hardware -- an
unprivileged PPB read does fault -- and `ringppb` stays REGISTERED, not skipped.** The reporter IS
entered and executes at least the 120 traced instructions; `MSP` moves `0x20003f60` -> `0x20003de8`,
at least 376 B of the 2 KiB kernel stack with no overflow. `DHCSR` bit 19 is clear, so the core is
not locked up. And the UART is READY at fault time: `CR1=0x0d` (UE/TE/RE), `ISR=0x006000d0` with TXE
and TC both set.

Hypotheses KILLED, listed so nobody re-runs them: hardware-does-not-fault (the `CFSR`/`BFAR` above);
vector unwired (the breakpoint hit); null backend pointer; output stuck in the ring
(`kpanic_enter` stores `RECLAIMED` first, and f302 DOES define its own `arch_console_write_sync`);
`KICKOS_POLL_SPIN_MAX` (it is 1,000,000); and "buffered ring plus a missing
`arch_console_reclaim`" -- because `stm32f411` is ALSO buffered and ALSO lacks one, yet dumps. The
only per-board number still differing is `_kernel_stack_size`, 2 KiB here against 8 KiB elsewhere,
and the 376 B measurement argues against it, so it is NOT the answer.

**UNRESOLVED SPAN**, stated narrowly: between "the reporter is running inside `kpanic_enter`" and "a
byte reaches `USART2_TDR` (`0x40004428`)". Nothing in between is measured. Blocked on a physical
ST-Link replug -- that unit currently fails `Failed to enter SWD mode`.

**And the STRUCTURAL coverage hole that let this survive to silicon.** Every fault-dump gate in the
fleet runs on an UNBUFFERED-console board: `mps2` semihosting, `microbit`, `virt`. So no emulated
gate can exercise a **buffered-ring panic flush** at all -- the code path that only exists when the
console has a ring behind it is gated nowhere. That is not a missing test on one board; it is a class
of path with no runnable target, and it belongs in *Coverage boundary* below as well as here.

### M4.6.1 -- the IRQ-driven userspace UART consoles (2026-08-01/02)

Five per-chip drivers, selected with `-DKICKOS_SERVICE_LIST=kickos_services_<board>_uartirq`. **No
board default points at any of them**, deliberately. Every capture named here carries
`# tap route: stdout endpoint -> console driver (service list published)`, which is what proves the
stream crossed the userspace driver rather than falling back to the kernel's polled route.

**The capability-table pass, 2026-08-04, two boards at ONE TREE.** The banners stamp `da716a8` and
`15fdd82`, two commits with the IDENTICAL tree `e74933d` (the difference is a message rewrite), so the
images are byte-identical. **Tree identity is the test, not hash identity** -- which is what the
M4.7.1 pass below fails, its tree being 26 code files from what merged. It
covers M4.7.1 and M4.7.2 together, and it is the FIRST silicon witness of either: M4.7.1's own
two-board run was taken at `c82af2c`, which is **not an ancestor of `master`** (it survives only on
`backup/m47-presquash`, and 26 code files changed between it and the merged `4ad39a8` -- `cap.h`,
`cap.cc`, `thread.h`, `syscall_thread.cc`, `sync.cc`, `slotpool.h`, `cmake/cap_table.cmake` and the
selftest among them). No record may quote `c82af2c` as a witness of the merged rework.

| board | enforcement | plan | result | capture (`.session/logs/`) |
|---|---|---|---|---|
| `xmc4800-relax` | PMSAv7, `-st` + `MPU=1` | `1..79` | 79 ok, 0 not-ok, 0 skip, 0 partial | `m472-xmc-st.log` |
| `frdmk64f` | SYSMPU, `-st` + `MPU=1` | `1..79` | 79 ok, 0 not-ok, 0 skip, 0 partial | `m472-k64-st.log` |

Both ran the RETAINING service list, which is the enforcing default on these two boards
(`CMakeLists.txt` resolves `KICKOS_SERVICE_LIST` after `KICKOS_HAVE_MPU` is known), so both
configured an **11-slot** table -- the widest in the fleet, and the only width that exercises the
retained term. Both carry `# tap route: stdout endpoint -> console driver (service list published)`,
so the whole suite crossed the userspace driver. The two arms this milestone added are on the wire as
`ok 49 - cap_chunk_span` and `ok 50 - cap_gen_reuse`; the first asserts an installed index at or
above the chunk granule and round-trips it through the segmented decode, which no capture before
this one did.

**What these two captures do NOT witness.** Only the 2-chunk geometry: the flat run is a supply-7
board's shape and `cap_chunk_span` reports PARTIAL there, which no bench board reproduces. Nothing
on any other ISA -- the rework is arch-neutral but only armv7m ran it. The fourth provisioning term
is 0 in every configure in the tree, so its summing path is unexercised on silicon and everywhere
else. And `t_cap_chunk_span` cannot be mutation-proved even in principle (a consistent bijective
mis-decode relabels slots and every install/lookup pair still agrees), so its evidence is coverage,
not detection; `t_cap_gen_reuse` does have a clean kill.

**Both logs were checked for the two silent capture failures this bench has produced.** Exactly one
reader per log with `fuser` confirmed free before arming, no zero-byte log, zero interleaved
half-lines, one banner and one plan line and one completion marker each, and both stamp their tree
with no `-dirty`. A first `-st` attempt produced a log with 158 `ok` lines: that was the headless
TAIL of the flash script's own `r;g` run, captured because the reader attached mid-stream, and the
remedy is to let that run finish before arming. It is not a clobber, and it is not in the record.

**The earlier pass of record, 2026-08-03, all six boards at ONE CLEAN COMMITTED TIP `9a00e73`** -- the
finished tree: Stage 3's capability slab, Stage 4's per-grant destinations, the CRLF cook, the
first-light markers, the `rxsci` TIE ordering fix, `KOS_SYS_IRQ_DISCARD`, the device-window console
reclaim with `KOS_SYS_THREAD_KILL`, the stable `ktime_rearm` deadline, and `rx72m` under MPU
enforcement. `257def0` was the five-board pass that preceded it; see the boundary table.

| board | enforcement | plan | result | capture (`.session/logs/`) |
|---|---|---|---|---|
| `xmc4800-relax` | PMSAv7, `MPU=1` | `1..78` | all pass, 0 skip, 1 partial, 2 boots | `m461h-xmc-uartirq.log` |
| `frdmk64f` | SYSMPU, `MPU=1` | `1..78` | all pass, 0 skip, 1 partial, 2 boots | `m461h-k64-uartirq.log` |
| `esp32c6-wroom` | PMP NAPOT, `MPU=1` | `1..78` | all pass, 0 skip, 1 partial | `m461h-c6-uartirq.log` |
| `esp32-wroom` | none (no MPU) | `1..74` | all pass, 0 skip, 1 partial | `m461h-lx6-uartirq.log` |
| `rx72m` | none (no MPU) | `1..74` | all pass, 0 skip, 1 partial | `m461h-rx-uartirq.log` |

The one partial on every row is `cap_capacity`, reporting that the board had a single capability
class -- true of every hardware board then, since they all shipped the behaviour-identical default,
with `sim` and `qemu` carrying the multi-class witness. **That arm no longer exists**: it died in
`4ad39a8` and `cap_chunk_span` took its place in the one-partial slot, so do not grep for it in
today's suite. The captures above are unchanged, as a measurement is never renamed.

**What this pass witnessed that the earlier one could not.** All five first-light markers reached
the wire (`[xmcuartirq] device up (IRQ TX)`, `[k64uartirq]`, `[c6uart]`, `[rxsci]`, `[lx6uart]`), so
a silent bring-up can now self-diagnose on every board rather than on one. And the CRLF cook is
visible in the bytes: `m461d-xmc-uartirq.log` ends its TAP lines `\r\n` where `m461c-xmc-uartirq.log`
from the same board ends them with a bare `\n`. The published console and the kernel console agree
byte for byte for the first time.

**The immediately preceding pass, `cb5f2a4` (`m461c-*`)**, is what closed the driver work: `1..74` /
`1..70`, zero partials, and the `rx72m` A/B below. Superseded as a status record, kept as the
provenance of that fix.

**`rx72m`'s stop at `ok 51` is closed, and the A/B that closed it is the artifact worth keeping.**
Reverting ONLY the zero-length-plain-send FLUSH arm from `rxsci`'s service loop, at the same tip,
reproduces the stop three times, byte-identical at 1716 B, each ending mid-string right after
`ok 53 - privileged_spawn_refused` -- the same test the original `1..67` capture stopped after, in
different numbering (`m461c-rx-noflush{,-2,-3}.log`). Deterministic in both directions.
**The doorbell is not involved**, bench-measured both ways on this chip: an image with zero posts
truncates identically, and one with 200 posts at bring-up plus three per write completes cleanly.
The truncation is a byte-exact prefix short by one TX ring, lost at SHUTDOWN because `rxsci` alone
did not implement root's
zero-length flush request. The separate real defect was `service_irq` observing `TDRE` before arming
`TIE`, a RULE T1 violation that intermittently left the drain a ring behind. `TODO.md` carries both.
The board also carries the probe rule this bench keeps re-learning: a TDR marker inside a TDR bug
generates false negatives, so the discriminating probe here is the LED path
(`m461-rx-led.log`) or the `KOS_UART_STATS` counters, never a UART marker.

**The K64F needed a human**, as its own entry above predicts: the first attempt hung right after
`InitTarget()` and cleared only after physical intervention on the board. Nothing in the image.

**The superseded first pass, 2026-08-01**, kept because it cost bench time and because the two
`0caf982`-dirty boards were the run that found the RR scheduler bugs: `m461-xmc-schedfix.log` and
`m461-k64-schedfix.log` (`1..71`), `m461-c6-ringfix.log` (`1..71`, `c42f054`),
`m461-lx6-writeall.log` (`1..67`), `m461-rx-fixed-selftest.log` (`1..67`, stops at `ok 51`,
untracked tree). Four of those five images are not identified by a commit and one carries no git
identity at all.

**The first per-board runs, kept because each found something the later full-suite run cannot show**:
`m461-xmc-uartirq.log` (the first IRQ console ever to reach a wire, at `a946a12`-dirty),
`m461-k64-uartirq.log` (`372e7b4`, the first-light marker plus the four review fixes),
`m461-c6-uartirq.log`, `m461-lx6-uartirq.log`. The `m461-*-stall-*` and `m461-rx-trace*` captures are
the storm and short-accept hunts and are only meaningful against the commits that closed them.

### Coverage boundary -- what this silicon witness covers

The gap this section exists to track is commits green under emulation but never run on hardware. It
is closed for everything at or before `270b6fa`, and PARTIALLY at `124b68c` -- only `f302nucleo`
`ringppb` and `fault` were taken from that tip. Extended one bench session at a time:

| Date | Tip | What it extended |
|---|---|---|
| 2026-07-28 | `75227d4` | The XMC A/B re-run plus the `frdmk64f` SYSMPU regression. Six flash-and-capture runs, every signature matched, zero reflashes -- the first tip at which the gap was closed rather than narrowed. |
| 2026-07-28 | `e5c651b` | The C6 A/B plus the `esp32-wroom` LX6 regression. Seven more runs, zero reflashes. |
| 2026-07-28 | `6857df3` | The `pizero2350` A/B (PMSAv8) and the `kos_reboot` witness. Four runs on three BOOTSEL states, both `rootfault` arms measured. |
| 2026-07-28 | `3204121` | Carried on that board under `KICKOS_SHUTDOWN_TO_BOOTLOADER=ON`: seven images off one human press, closing its control `selftest` (62/62, 0 skips) and its `mpu_fault` arm under the flip, and witnessing both terminal dead-ends. Its console-handover arm stays unwitnessed -- no userspace console driver on this board. |
| 2026-07-28 | `d71b313` | The `rx72m` A/B: thirteen flash-and-capture runs in one session (`selftest`, `rootfault`, `rxdrv` per arm, an `rxdrv` re-run after the oracle poke became a plain store, then all six re-run at `d71b313` so both arms stamp the same commit), zero reflashes, every signature matched. No RXv3 emulator exists anywhere, so for `arch/rx/**` the boundary IS this bench run. |
| 2026-07-29 | `6646c8e` | The `f411disco` phase-1 enforcement witness plus its A/B: five runs (control `selftest` + `mpu_fault`, flipped `selftest` + `rootfault`, control `rootfault`), zero reflashes. Closes the last never-witnessed enforcement backend in the fleet -- `stm32f411` PMSAv7, shared with `blackpill`. |
| 2026-07-29 | `127efb5` | The `frdmk64f` stage-3 witness: `selftest` and `rootfault` under the FULL service list, the first silicon run of `arch_periph_enable` and the first of a board whose root writes no MMIO. Two captures, one posture only -- no privileged arm at that tip -- and the first K64F captures taken on `-Os` code. |
| 2026-07-30 | `2fc7799-dirty` | The first M4.5.6 pass: `xmc4800-relax` `xmcspi` / `xmccshold`, `frdmk64f` `selftest` / `rootauth`, `pizero2350` `selftest`. Covers the new `KOS_SYS_PERIPH_REG_WRITE` seam on real PV-restricted registers and the `mpu_privileged_guard` deletion via a measured 2 -> 1 named-skip delta. Those captures stamp a **dirty** tree and three files were edited after they were taken, which is why the process rule is now **commit before a witness pass**. |
| 2026-07-30 | `c5d9b0d` | The committed tip, by seven further captures: `xmc4800-relax` `pvprobe` (the seam A/B retaken to prove the new offset-containment bound did not break the consumer path), `conreclaim` (first run ever; the widened panic reclaim witnessed), `inprstorm` at two rate profiles ~625x apart (the rate-knob hypothesis REFUTED -- the bound is the scheduling model), `frdmk64f` `rootfault`, and `pizero2350` `rootfault` + `rootauth` (both previously owed, both now taken). The reclaim widening changed the panic path on EVERY board, which is why the fault captures were retaken rather than inherited. |
| 2026-07-30 | `270b6fa` | Three boards not on the earlier passes, all off a CLEAN committed tip: `rx72m` `selftest` / `rootauth` / `rxdrv` (all three owed items closed in one visit, including the fleet's first mux WRITE that lands), `xmc4800-relax` `pvprobe` on a SECOND physical unit (adding the mask-refusal arm) and `xmcssc` as a SERVICE on the full default list, and the `f302nucleo` `selftest` BEFORE capture. Six captures. |
| 2026-07-30 | `270b6fa-dirty` | The committed tip plus uncommitted work, nine captures: the `esp32c6-wroom` `.data` LMA diagnosis and its post-fix `c6blink` / `selftest` (closing the C6 mux-write debt and making the 2026-07-28 C6 witness retrospectively a pass by LUCK), `f302nucleo` `ringpriv` (the project's FIRST ring-arm silicon witness), and `inprstorm` at THREE rate profiles (closing the TX-FIFO residual). Dirty again, after the rule; see the M4.5.6 provenance note. |
| 2026-07-30 | `124b68c` | Two captures only, and they are the milestone's sole clean-tip witnesses: `f302nucleo` `ringppb` and `fault`. Both expose one OPEN defect -- the fault reporter emits no dump on this board -- with the BusFault itself confirmed by a live debugger attach. Everything else in M4.5.6 predates this tip. |
| 2026-08-01 | `97a85e4` | The M4.6.1 IRQ-capability pass: `irq_claim_gate` and `irq_reclaim` on all five bench boards -- four ISAs, four enforcement backends (PMSAv7, SYSMPU, RX-MPU, PMP) plus one no-MPU part -- and the first silicon confirmation of both plan counts, `1..71` under enforcement and `1..67` without. Also the `f302nucleo` fault-reporter LED pass, which is what reframed that defect. |
| 2026-08-01 | `2511e20`-dirty .. `182e0dd`-dirty | The first M4.6.1 UART-console pass, four boards green and one short. Four of five images uncommitted, one with no git identity, and all five three arms behind the tree. SUPERSEDED by the row below. |
| 2026-08-02 | `cb5f2a4` | The M4.6.1 UART-console pass: all five boards, one CLEAN committed tip, `1..74` enforcing / `1..70` not, zero failures, skips and partials. Closes the three-arm gap the previous row opened and the `rx72m` stop, with a reverting A/B that reproduces the stop three times. |
| 2026-08-02 | `0f5a5bd-dirty` | Stage 3's capability slab, the CRLF cook and the five first-light markers, at `1..76` / `1..72`. The `m461d-*` banners stamp `0f5a5bd-dirty`, an ANCESTOR of `c82cc63` -- credit these captures to the banner, not to the branch tip they were taken from. Not a committed-tip pass. |
| 2026-08-02 | `257def0` | The finished tree at `1..78` / `1..74`, five boards, zero failures. Also the pass that CAUGHT the `esp32-wroom` hang at `sleep_order` -- the Xtensa `CCOMPARE0` equality-match defect, fixed on its own merits at `b4e888d`. The boundary is closed as far as `257def0`. |
| 2026-08-04 | `da716a8` | **The capability-table pass of RECORD, and the FIRST witness of the merged rework.** Two captures, `m472-{xmc,k64}-st`, both `1..79` with 79 ok and zero not-ok, skips or partials, at an 11-slot table under the retaining service list. Covers M4.7.1 and M4.7.2 together, because M4.7.1's own `c82af2c` run is not an ancestor of `master` and stamps a tree 26 code files away from what merged. First silicon for the segmented chunk decode being ASSERTED (`cap_chunk_span`) and for the capability generation-mismatch branch of `cap_lookup` being reached at all (`cap_gen_reuse`). Two boards, one ISA, one geometry: see the section above for what it does not cover. |
| 2026-08-02 | `9a00e73` | Superseded as the pass of record by `da716a8` above for the capability subsystem; still the pass of record for the M4.6.1 IRQ and UART work on six boards. **The boundary is closed.** Seven captures on six boards, `m461n-*`, all at this clean tip, zero `not ok`: `xmc4800-relax` `1..78`, `frdmk64f` `1..78`, `esp32c6-wroom` `1..78`, **`rx72m` `1..78` under MPU ENFORCEMENT for the first time**, `esp32-wroom` `1..74`, `f302nucleo` `1..44` + `1..30`. First silicon for the `ktime_rearm` fix, including the two boards it most exposes: `esp32-wroom` (Xtensa, no dedup guard at all) and `rx72m` (software dedup on the deadline value). |
| 2026-08-02 | `20f6d43` | Superseded as the pass of record by `9a00e73` above. Seven captures on six boards, `m461m-*`, all stamping this clean tip: `xmc4800-relax` `1..78`, `frdmk64f` `1..78`, `esp32c6-wroom` `1..78`, `esp32-wroom` `1..74`, `rx72m` `1..74`, all via their `*_uartirq` service lists with first-light markers on the wire; plus `f302nucleo` `1..44` + `1..30` as two images. Zero `not ok`. First silicon for: the device-window console reclaim, `KOS_SYS_THREAD_KILL`, the Xtensa CCOMPARE fix (`b4e888d`), the LX6 unroutable-source silencing, the selftest suite split, and the three arms it restored (`cap_dest` PASS, `cap_capacity` PARTIAL, `irq_discard` PASS). |
| 2026-08-02 | `b4e888d` | Witnessed at a clean tip by the `20f6d43` row above. `b4e888d` changed `arch_xtensa.cc` (`arch_timer_arm`, the CCOMPARE equality-match fix) and `selftest/main.cc`. Its OWN captures (`m461i-lx6-{1,2}`, `m461i-rx-{1,2,3}`, `m461j-rx`, `m461k-lx6`, `m461k-rx`) all stamp `cab37e6-dirty` and are named in no other tracked doc, so they identify no tree and cannot extend the boundary by themselves. |

From `2fc7799` on there is only one posture to witness, so "both arms" stops being a thing a capture
can have.

**What the boundary still does NOT cover at this tip.** The seam's positive path is still
silicon-only in the sense that no emulator models a PV-restricted register; what IS gated now is the
MECHANISM -- allowlist match, mask compare, refuse-not-trim, containment, alignment, wrap -- on the
host, via `kickos_arch_sim`'s `arch_periph_reg_write` backend over a published 64 KiB window, six mutations
each caught by a distinct check. What stays silicon-only is the bus's PV CLASSIFICATION (the silent
discard remains a `pvprobe`-only fact), that a tabled block is CLOCKED, and any real chip's mask
column. **No emulated gate can exercise a buffered-ring panic flush**, because every fault-dump gate
in the fleet runs on an unbuffered console (`mps2` semihosting, `microbit`, `virt`) -- a class of path
with no runnable target, and the reason the `f302nucleo` reporter hole survived to silicon.
`esp32-wroom` can carry no run gate at all (upstream QEMU has no ESP32 machine).

**The owed list, in full, so it can be read in one place.** Three items and no more:

- **`f411spi` on `f411disco`** -- the ONE remaining mux-write debt. That board was not on the
  2026-07-30 bench; `rxdrv` and `c6blink` both closed there.
- **The `f302nucleo` fault-reporter root cause** -- the reporter runs and the UART is ready, yet no
  dump reaches the wire. Blocked on an ST-Link replug (*M4.5.6* above).
- **Right-sizing `frdmk64f` and `bluepill-c8` against MEASURED watermarks**, the way `f302nucleo` was
  at `124b68c`. The K64F's one remaining full-list skip is provisioning and nothing else, and
  `bluepill-c8`'s 96-byte shortfall is still arithmetic rather than a measurement (*Per-board
  caveats* above) -- and that board can never be flashed, so for it the paint-and-scan route does not
  exist at all.

Everything else this subsection previously listed as owed is closed: `rxdrv`, `c6blink`, the RING
arm, `xmcssc` as a service, the TX-FIFO storm vector, both `pizero2350` arms, and the `rx72m`
region-shaping mode.

| Commit | Touches the enforcement path? | Silicon |
| --- | --- | --- |
| `af696e6` self-grant syscall | yes -- new `KOS_SYS_MEM_SELF_GRANT`, writes the caller's region set | witnessed at `75227d4`: `mem_self_grant` `ok` under PMSAv7 (both postures) and SYSMPU |
| `e4e9653` volatile increment | no -- selftest source only | n/a |
| `3c772b9` commit a self-grant before return | **yes -- MPU programming path** | witnessed at `75227d4`: same probes; the fault it fixed was the enforcing-backend class |
| `30465a0` skips gated by name | no -- CTest harness only | n/a |
| `f85b51e` alignment refusal gated on a descriptor | yes -- self-grant admission | witnessed at `75227d4`: `mem_self_grant_nonpow2` `ok` on all three runs (its checked-to-fail arm is no-MPU microbit, under emulation) |

`3c772b9` was the one to re-witness first, and the reasoning is kept because it names the class:
it fixed a fault that **only the enforcing backends showed** -- `arch_mpu_apply` merely stashes on
every deferred-switch arch, so the grant was not programmed until some later switch, and the
caller faulted on memory the kernel had just granted it. The host sim was green throughout,
because its apply mprotects as it records. XMC4800 is PMSAv7, exactly the class that faulted, so
this silicon run is the strongest witness the fix can have.

**`frdmk64f`'s witness is on its full service list** -- its own subsection above carries it, and
*M4.5.6* re-takes it, at `2fc7799-dirty` for `selftest`/`rootauth` and `c5d9b0d` for `rootfault` --
not at the milestone's own tip, which only `f302nucleo` reached. Its privileged-root SYSMPU
regressions stay the historical
control reference, from a tree where that posture was still buildable, and a different one: at
`22e1c5a` and again at `75227d4` (2026-07-28) selftest ran 61 cases / 60 `ok` / 1 skip
(`mutex_deadlock`, pool too small) / 0 fail, and `mpu_fault` trapped a child's cross-domain write
with `SYSMPU ISOLATION FAULT: port=3 addr=0x2001b000 master=0 W EDR=0x80000003` -- byte-identical to
the prior witness (SYSMPU surfaces as an imprecise bus fault, so the core label reads `HARD FAULT`
and the chip hook adds the real line). Nothing at `127efb5` was captured in the privileged posture,
so this board has no single-tree A/B.

**`esp32-wroom` can carry no confinement witness at all** -- its root is unprivileged like every
other board's, but the LX6 has no MPU, so there is no boundary to enforce and the banner reads
`mpu off`. It is regressed instead: at `e5c651b`
(2026-07-28) its selftest ran 58 cases / 58 `ok` / 0 skips / 0 fail on silicon, unchanged by the
C6 work, which is what the run is for (the two Espressif ports share nothing but the flasher).

**`microbit` witnesses no arm on silicon, for two independent reasons.** There is **no physical
unit** -- it is a QEMU armv6m vehicle, not a bench board (`../m2-readiness.md`) -- and the
nRF51822's Cortex-M0 implements no privilege axis anyway, so the kernel's `unprivileged` marking is
discarded by the hardware and such a thread runs privileged (see the caveat under *Per-board
caveats*). Even a unit would therefore witness the authority arm and nothing about the ring. The
armv6m privilege boundary is `picopi`'s alone -- an M0+, which does implement the extension.

**`bluepill-c8` carries no witness because no unit exists.** That is the binding reason, not RAM
(heap policy, `../archive/M4.5_footprint_meas.md` section 7), not the 7-handle provisioning (the authority
word is TCB state and costs no slot at all), and not the missing MPU. It costs nothing either: `f302nucleo` is the
same class -- a 64 KiB-flash armv7m part with no MPU and a real privilege ring -- and is on the
bench, so it carries the hardware coverage for both.

**`f302nucleo` is the fleet's only physically-present no-MPU ARM board, so it is the sole possible
silicon witness for the ring arm -- and it TOOK it on 2026-07-30.** Having no MPU rules out the
confinement arm and nothing else; the ring arm wants exactly a ring with no MPU, which the F302R8's
M4 has, and that is still why no other board can stand in. The prober now exists
(`user/apps/common/ringpriv`: `ringpriv` plus `ringppb`) and `ringpriv` returned `PASS (5 arms)` on
this silicon -- see *M4.5.6* above. It is also the one arm that is **permanent CI** rather than a
bench-only capture: neither test is conditioned on enforcement -- a board's `flat` variant IS the
ring-only posture, its base one enforcing -- so the same arms run on the four MPS2 presets in both
postures, and `microbit` asserts the opposite outcome instead of skipping.

What is left on this board is narrower and different from what it was. Not the arena (boot and the
suite are witnessed, and the pool was right-sized at `124b68c`), not the prober, and not the ring
property. It is: **the fault reporter emits no dump here**, root cause unresolved and blocked on an
ST-Link replug; and the board still has no AUTOMATED gate of its own, so its chip code, clock tree
and USART are covered by nothing but the `build-boards` link (see *CI coverage* above).
`design-unprivileged-root.md` sections 9 and 10 carry the arms and the arithmetic.
