<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS board support

Status of every board target: what works, what only builds, how to flash it, and
where its console + LED live. For J-Link / RTT details see [flashing.md](../flashing.md).

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
marker taxonomy). `../TODO.md` is the live task list. Where those and this file disagree, the
code wins, then this file.

## Status matrix

| Board (preset) | SoC / core | LED (blink) | Console | Flash tool | HW-validated |
|---|---|---|---|---|---|
| `sim` | host process | -- | host stdout | `ctest --preset sim` | [x] CI |
| `qemu` | mps2-an386 / M4 | -- | semihosting | `ctest --preset qemu` | [x] CI, plain and under PMSAv7 enforcement (the ARM runtime enforcement gate -- see *CI coverage* below) |
| `qemu-m33` | mps2-an505 / M33 | -- | semihosting | `ctest --preset qemu-m33` | [x] CI, plain and under **PMSAv8** enforcement incl. full-C++ `cxxtest` + the `mpu_fault` MemManage trap |
| `qemu-m7` | mps2-an500 / M7 | -- | semihosting | `ctest --preset qemu-m7` | [x] CI, plain and under PMSAv7 enforcement (16 MPU regions) |
| `qemu-m3` | mps2-an385 / M3 | -- | semihosting | `ctest --preset qemu-m3` | [x] CI, plain and under PMSAv7 enforcement (soft-float; no FP switch path) |
| `microbit` | nRF51822 / M0 | -- | semihosting | `ctest --preset microbit` | [x] CI (armv6m run gate; the fleet's only measured skip budget -- see *microbit* below) |
| `qemu-riscv` | QEMU virt / RV32IMAC | -- | semihosting | `ctest --preset qemu-riscv` | [x] CI (first RISC-V) |
| `esp32c6-wroom` | ESP32-C6-WROOM-1 / RV32IMAC | GP8 (WS2812B, LED2) | UART0, GP16/GP17, 115200 -> CH343P VCOM (`/dev/ttyACM0`) | esptool | [x] full selftest + PMP NAPOT enforcement + `mpu_fault` trap + diag-LED + bench |
| `esp32-wroom` | ESP32-D0WD / Xtensa LX6 @240 MHz | GP2 (D2, active-high) | UART0, GP1/GP3, 115200 -> CH340 (`/dev/ttyUSB1`) | esptool | [x] 8/8 apps incl fault dump + bench |
| `rx72m` | RX72M / RXv3 @240 MHz | P80 (LED6, active-low) | SCI6 ASC, PB1/PB0, 115200 -> FT232 (`/dev/ttyUSB0`); ring | `rfp-cli` (Renesas Flash Programmer) | [x] full selftest + stress + `RX EXCEPTION` dump (2026-07-09); RX-MPU enforcement selftest + `mpu_fault` cross-domain trap + `rxdrv` granted peripheral window (2026-07-17); DPFPU switch + bench. **No CI gate** -- see *CI coverage* below |
| `xmc4800-relax` | XMC4800 / M4F | P5.9 (LED1) | USIC0 ASC, P1.5/P1.4, 115200 -> VCOM; + RTT | onboard J-Link | [x] full selftest + stress + `HARD FAULT` dump (2026-07-09, 144 MHz); PMSAv7 enforcement selftest + `mpu_fault` cross-domain trap + the `xmcspi` granted-USIC window (2026-07-17) -- the canonical per-thread PMSA proof; console handover to a userspace driver, panic-path reclaim and clock retune all silicon-passed |
| `f411disco` | STM32F411 / M4F | PD12 (LD4 grn) | USART2, PA2/PA3, 115200 (ext adapter) | onboard ST-Link (`st-flash`) | [x] full selftest + all apps + fault dump + bench + LED; enforcement link-validated, MPU **HW pending** |
| `blackpill` | STM32F411 / M4F | PC13 (active-low) | USART2, PA2/PA3, 115200 (ext adapter) | USB-DFU / SWD | [x] full selftest + bench (2nd F411; 25 MHz HSE); enforcement link-validated, MPU **HW pending** |
| `f302nucleo` | STM32F302R8 / M4 | PB13 (LD2 grn) | USART2, PA2/PA3, 115200 -> ST-Link VCP | onboard ST-Link (`st-flash`) | [x] selftest minus the 4 KiB-alloc test (16 K RAM) + bench; not an enforcement target (3712 B arena) |
| `picopi` | RP2040 / M0+ | GP25 | UART0, GP0/GP1, 115200 | `picotool` (BOOTSEL) | [x] LED + UART0 + full selftest with `sched_exit` (2026-07-09, 125 MHz PLL); PMSAv6 cross-domain denial silicon-proven 2026-07-19 (M0+ has no MemManage -- it escalates to HardFault) -- the fleet's only armv6m enforcement proof; U-mode `cxxtest` still awaits a bench re-flash |
| `bluepill-c8` | STM32F103C8 / M3 (64 K/20 K genuine) | PC13 (active-low) | USART1, PA9/PA10, 115200 | external ST-Link (SWD) | (!) build-only (64 K/20 K linker; links the full app set incl selftest + stress) |
| `frdmk64f` | MK64FN1M0 / M4F | -- (none) | UART0, PTB16/PTB17, 115200 -> OpenSDA VCOM | J-Link (OpenSDA) | [x] HW 2026-07-15 (full selftest over the buffered console ring, 120 MHz); SYSMPU enforcement + `mpu_fault` trap silicon-proven at M2 |
| `teensy41` | i.MX RT1062 / M7 @396 MHz | -- (none wired) | LPUART6 ("Serial1", pins 0/1), 115200 | `teensy_loader_cli` (HalfKay, `.hex`) | [x] full selftest + soak under PMSAv7 enforcement, after the M7 anti-speculation fix (ERR011573; `../design-teensy-mpu-hang.md`) |
| `pizero2350` | RP2350 / M33 @150 MHz (armv7m backend) | -- (none on the Pi-Zero header) | UART1, GP4/GP5, 115200 | `picotool` (BOOTSEL) | [x] full selftest under PMSAv8 enforcement + `mpu_fault` cross-domain MemManage denial + bench/soak |

**"Full selftest" rather than N/N.** The TAP suite emits its own plan line (`1..N`) and a closing
`# all tests passed`, and the gates key off *those*, never a number written down here
(`tests/check_qemu_selftest.sh`). N is not a constant: the suite registers ~56 tests on the sim
today and the total moves with `KICKOS_HAVE_MPU` and `KICKOS_ENABLE_SELFTEST`, each of which
compiles in tests that cannot run without it (the IRQ suite, the enforcement bound-checks, the
privileged guard). Where a dated silicon record below still names a count -- "14/14", "17/17",
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
- **`bluepill-c8`** -- build-only (genuine 64 KiB/20 KiB F103C8; the F103 port was
  physically run only on the now-retired 10 K clone). Links the full app set.
- **`due`** -- **retired** (see the table note above): SAM3X port proven 2026-07-09, but
  this unit now has a peripheral-I/O fault.
- **`frdmk64f`** -- **HW-revalidated 2026-07-15** (OpenSDA J-Link): full selftest streamed
  in-order over the buffered console ring; bench re-confirmed on silicon at **77 cyc / 641 ns**
  per switch (=> 120 MHz PLL) and 160 cyc / 1333 ns IRQ-entry; fault-dump path verified
  (UsageFault UNDEFINSTR escalated to HardFault, on PSP). Connect-under-reset was needed only
  once, to displace unknown prior firmware -- a KickOS-flashed board reflashes with a **plain
  connect** (KickOS leaves the SWD pins PTA0/PTA3 alone). Its distinguishing feature -- the
  **SYSMPU** -- is the M2 enforcement backend, and it signed off there. No diagnostic
  LED wired (FRDM RGB not mapped; `blink` isn't built for it).
- **`teensy41`** -- the fleet's only **Cortex-M7**, and the M7 is the one core here that
  speculates. Under enforcement a dropped (non-pow2) whole-arena grant leaves a privileged
  thread on the PRIVDEFENA background, which types the whole 1 GiB FlexSPI/SEMC aperture as
  Normal -- so the core prefetched past the populated 8 MiB into an AHB slave that never
  responds and stalled forever with **no fault** (NXP ERR011573). Fixed by a chip fixed-MPU
  table that wraps the unbacked apertures as Device + XN + no-access, programmed *before* the
  I-cache is enabled. Runs `SystemCoreClock` at the ROM-default **396 MHz** -- the 600 MHz
  CCM/PLL tree is a follow-up, not a regression. Full story: `../design-teensy-mpu-hang.md`.
- **`pizero2350`** -- Cortex-M33, but it reuses the **armv7m** arch backend verbatim (armv8-M is
  a superset for the switch/NVIC/SVC/PendSV path); only the MPU differs, and PMSAv8 has its own
  backend (`base+limit` RBAR/RLAR + MAIR, compile-gated so the v7-M/v6-M fleet is byte-identical).
  Console is **UART1 on GP4/GP5** -- UART0's pins are not brought out on the Pi-Zero header.
  BOOTSEL-recoverable, so a bad clock or boot-block config cannot brick it.
- **`xmc4800-relax` and `frdmk64f` under `-DKICKOS_HAVE_MPU=1` print no TAP by default, and
  that is not an enforcement failure.** Both boards default to a `KICKOS_SERVICE_LIST` that
  brings up their userspace UART driver and publishes stdout, which routes the TAP stream away
  from the kernel console; what reaches the wire is the banner, `[xmcuart|k64uart] driver up`,
  a stray `x`, then silence. The `x` is `selftest`'s `cap_index0` asserting that stdout is
  *not* published. Reproduced on both chips and from a pre-enforcement baseline, so it is a
  property of the service-list publish, not of the MPU. Build with
  `-DKICKOS_SERVICE_LIST=kickos_services_none` to get an observable verdict while keeping
  enforcement on -- that is how both boards were signed off (56/56 each, 2026-07-26).
- **RTT backend** -- generic and wired on the XMC (`KICKOS_CONSOLE=both`); the
  UART VCOM path is the one confirmed on hardware.
- The diagnostic LED is a kernel-owned facility (`kdiag_led_*`); a chip with no
  known LED (`qemu`, `microbit`, `frdmk64f`, `teensy41`, `pizero2350`) links the weak
  no-op and the LED silently does nothing -- not a failure.

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
  `arch_reserved_blocks`, which has no weak default on purpose, so an enforcing port that
  forgets its reserved set fails to link. What is still NOT covered in CI is
  **chip-specific** trapping: SYSMPU (K64F), the M7 anti-speculation wrap (i.MX RT1062) and
  PMSAv6 (M0+) have no QEMU model, and stay silicon-proven (see the matrix above and
  `../m2-readiness.md`).
- **Renesas RX has no CI gate at all.** RX72M needs `-misa=v3` and `-mdfpu`
  (`boards/rx72m/board.cmake`), and both exist only in the registration-gated Renesas GNURX
  build -- upstream `rx-elf` GCC rejects them. That toolchain cannot be fetched anonymously on a
  hosted runner, so RX is bench-validated only. A change that touches the arch seam is *not*
  covered for RX by a green CI run; build it locally.
- **microbit is the armv6m run gate, and the fleet's only board with a non-zero skip budget.**
  16 KiB SRAM and a 2-slot pool mean part of the suite genuinely cannot run here, so
  `microbit_selftest` sets `MAX_SKIPS`; every other board keeps the script's default of 0. That
  number is a **measurement, not slack** -- the ten tests and why each one skips are listed at the
  call site (`../../user/apps/common/selftest/CMakeLists.txt`), so raising it should mean a board
  capability changed, and a test that merely stopped running shows up as a breach.

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

Every non-sim build emits `<app>`, `<app>.hex`, `<app>.bin` next to the app ELF.
`blink` is the no-UART smoke test (built for xmc/f411/f302/picopi/bluepill-c8/due);
`hello` prints the banner + ping-pong. Fleet-wide apps build under
`build/<preset>/user/apps/common/<app>/`; a board's own demos (`xmcspi`, `k64drv`, `rxdrv`, ...)
build under `build/<preset>/user/apps/<board>/<app>/`.

### STM32 with an onboard ST-Link -- `f411disco`, `f302nucleo`

```sh
st-flash --connect-under-reset --reset write \
  build/<preset>/user/apps/common/hello/hello.bin 0x08000000
```
`--connect-under-reset` is needed to re-flash a *running* board: the idle thread
sits in `WFI`, so SWD can't halt a live core (a plain `write` on a fresh/erased
chip works without it). Nucleo consoles reach the ST-Link VCP (`ttyACM*`) with no
wiring; the F411-DISCO does **not** route USART2 to its VCP -- use an external
3.3 V USB-UART adapter (TX->RX crossed, GND, no VCC).

### `bluepill-c8` -- external ST-Link over SWD

The Blue Pill has no onboard debugger. Wire an ST-Link (a standalone V2 dongle,
or a Nucleo's ST-Link freed by pulling its **CN2** jumpers -- see below) to the
4-pin SWD header -- which carries only `3V3 / SWDIO / SWCLK / GND`, no NRST:
`SWDIO<->DIO`, `SWCLK<->CLK`, `GND<->GND` (power over USB or `3V3` -- one supply, shared
GND). A **fresh** chip needs no reset line -- SWD can halt it, so a plain:
```sh
st-flash --reset write build/bluepill-c8/user/apps/common/blink/blink.bin 0x08000000
```
works (`--reset` here is a software SYSRESETREQ over SWD). Only *re-flashing a
board already running KickOS* needs `--connect-under-reset` (the idle thread
sleeps in `WFI`, so SWD can't halt the live core) -- and that needs a physical
NRST wire. The Blue Pill's reset isn't on the SWD header: it's the **`R` pin** on
the long side header (or the reset button pad). LED = **PC13, active-low**.

**Using a Nucleo as the programmer:** pull **both CN2 (ST-LINK) jumpers** on the
Nucleo to detach its onboard target, then drive the Blue Pill from the Nucleo's
**CN4** SWD header -- `CN4.2=SWCLK`, `CN4.3=GND`, `CN4.4=SWDIO` (and `CN4.5=NRST`
-> the Blue Pill's `R` pin only if you need connect-under-reset later). Power the
Blue Pill from its own USB; restore the CN2 jumpers when done.

### `blackpill` (WeAct STM32F411) -- USB-DFU (no ST-Link)

The Black Pill has no on-board debugger; flash over USB with its ROM DFU
bootloader. Hold **BOOT0**, tap **NRST**, release BOOT0 (it enumerates as DFU),
then:
```sh
dfu-util -a 0 -s 0x08000000:leave -D build/blackpill/user/apps/common/blink/blink.bin
```
`:leave` runs the app after flashing. Or flash via SWD with an external ST-Link
(`st-flash write ... 0x08000000`) if you'd rather. LED = **PC13** (active-low).
Console = USART2 on **PA2 (TX)/PA3 (RX)**, 115200 -- wire a 3.3 V USB-UART there
(same pins as the Nucleos; no VCP on this board). It runs a **25 MHz** HSE
crystal -- the backend derives PLLM from it, so it reaches the same 84 MHz.

### `picopi` (RP2040) -- BOOTSEL + picotool

Hold **BOOTSEL** while plugging USB (mounts as mass storage / picoboot), then:
```sh
picotool load -x build/picopi/user/apps/common/blink/blink
```
`-x` runs it after loading. UART0 is GP0(TX)/GP1(RX) 115200 -- needs a 3.3 V
adapter, confirmed on silicon. LED is GP25 (not the Pico W, whose LED is on the
CYW43). picotool/BOOTSEL is the reliable flash path here: J-Link SWD of the RP2040
is flaky (DAP power quirks + boot2 isn't re-run on an SWD reset).

### `due` (SAM3X8E) -- bossac over the Programming port

Use the **Programming port** (the micro-USB next to the DC jack; enumerates as
Arduino PID `003d`). BOSSA needs the SAM3X in its SAM-BA ROM bootloader:
```sh
bossac -p ttyACMx --usb-port=0 -a -e -w -v -b -R \
  build/due/user/apps/common/blink/blink.bin
```
`-a` = the 1200-baud erase/reset hack, `--usb-port=0` = RS-232 (the prog port is
UART-bridged via the 16U2), `-b` = set boot-from-flash, `-R` = reset. If it says
"No device found", do it by hand: **hold ERASE ~2 s, tap RESET**, rerun without
`-a`; failing that, use the **Native port** (no `--usb-port=0`). `-b` is required:
the SAM3X latches its boot mode at NRST/power-on, so after flashing you must
**press RESET / power-cycle** -- the soft `-R` alone leaves it in the SAM-BA ROM
monitor. LED = PB27 ("L" amber). Its programming port *is* the console UART
(PA8/PA9), so `picocom -b 115200 /dev/ttyACMx` shows the console on the same cable.

### `esp32-wroom` and `esp32c6-wroom` -- esptool

Flash over the on-board USB-serial bridge with `esptool` (auto-enters the ROM
download mode via DTR/RTS; hold **BOOT** + tap **EN** if it doesn't):
```sh
esptool.py -p /dev/ttyUSB1 write_flash 0x1000 build/esp32-wroom/user/apps/common/hello/hello.app.bin
```
On the C6 use its CH343P port (`/dev/ttyACM0`); its native USB-Serial-JTAG is
flash-capable but useless as a console (see the note under the matrix). Console =
UART0 at 115200 on both (`esp32-wroom` GP1/GP3 -> CH340; `esp32c6-wroom`
GP16/GP17 -> CH343P). LED: `esp32-wroom` GP2 (D2), `esp32c6-wroom` WS2812B on GP8.

### `pizero2350` (RP2350) -- BOOTSEL + picotool

Same path as the Pico: hold **BOOT** while plugging USB, then

```sh
picotool load -x build/pizero2350/user/apps/common/hello/hello
```

`-x` runs it after loading. Console is **UART1 on GP4 (TX) / GP5 (RX)**, 115200 -- a 3.3 V
adapter, and note it is *not* UART0 (whose pins the Pi-Zero header does not bring out). No
diagnostic LED. BOOTSEL always recovers the board, so a wrong clock or boot-block config cannot
brick it.

### `teensy41` (i.MX RT1062) -- HalfKay

The Teensy has no SWD header exposed by default; flash the **`.hex`** over its HalfKay
bootloader with `teensy_loader_cli` (or the GUI Teensy Loader), tapping the on-board button to
enter it. The image to hand it is
`build/teensy41/user/apps/common/hello/hello.hex`; pass whichever `--mcu=` selector your
`teensy_loader_cli` build names for the 4.1 (it lists them with `--list-mcus`).

Console is **LPUART6** -- Teensy pin 1 (TX) / pin 0 (RX), "Serial1" in Teensyduino terms --
at 115200 on a 3.3 V adapter. No diagnostic LED is wired.

### `rx72m` (RX72M) -- rfp-cli over an E2 Lite

Renesas Flash Programmer CLI (`rfp-cli`, must be on `PATH` -- symlink the Renesas install),
driving an **E2 Lite** over the FINE 1-wire interface. The Intel HEX carries its own addresses
(reset vector + option memory), so load the `.hex`, not the `.bin`:

```sh
rfp-cli -device RX72x -tool e2l -if fine \
  -auth id FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF -run -a \
  build/rx72m/user/apps/common/hello/hello.hex
```

Every flag there is silicon-verified (`tools/flash-rfp.sh` wraps the same call): all-FF `-auth id`
authenticates blank/unlocked flash, `-a` is erase + program + verify, and **`-run` is required** --
without it the core stays in reset and the board looks dead. Do **not** pass `-osc`: it trips an
input-frequency error, and rfp's default is correct. Console is SCI6 on **PB1 (TXD6) / PB0
(RXD6)** at 115200, captured over an FT232 (`/dev/ttyUSB0`).

### `xmc4800-relax` and `frdmk64f` -- J-Link

Both use SEGGER J-Link (XMC = onboard J-Link-OB; K64F = OpenSDA reflashed with
J-Link firmware). Full `JLinkExe` / GDB / RTT recipes are in
[flashing.md](../flashing.md).
