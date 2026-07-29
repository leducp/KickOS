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
| `microbit` | nRF51822 / M0 | -- | semihosting | `ctest --preset microbit` | [x] CI (armv6m run gate; the fleet's only measured expected-skip list -- see *microbit* below) |
| `qemu-riscv` | QEMU virt / RV32IMAC | -- | semihosting | `ctest --preset qemu-riscv` | [x] CI (first RISC-V) |
| `esp32c6-wroom` | ESP32-C6-WROOM-1 / RV32IMAC | GP8 (WS2812B, LED2) | UART0, GP16/GP17, 115200 -> CH343P VCOM (`/dev/ttyACM0`) | esptool | [x] full selftest + PMP NAPOT enforcement + `mpu_fault` trap + diag-LED + bench; the `c6blink` granted-GPIO window is the canonical per-thread PMP proof. **Second board with an UNPRIVILEGED root, and the first on RISC-V PMP** (2026-07-28) -- see *Unprivileged root* below |
| `esp32-wroom` | ESP32-D0WD / Xtensa LX6 @240 MHz | GP2 (D2, active-high) | UART0, GP1/GP3, 115200 -> CH340 (`/dev/ttyUSB1`) | esptool | [x] 8/8 apps incl fault dump + bench |
| `rx72m` | RX72M / RXv3 @240 MHz | P80 (LED6, active-low) | SCI6 ASC, PB1/PB0, 115200 -> FT232 (`/dev/ttyUSB0`); ring | `rfp-cli` (Renesas Flash Programmer) | [x] full selftest + stress + `RX EXCEPTION` dump (2026-07-09); RX-MPU enforcement selftest + `mpu_fault` cross-domain trap + `rxdrv` granted peripheral window (2026-07-17); DPFPU switch + bench. **Fourth board with an UNPRIVILEGED root, and the only one on the RX MPU** (2026-07-28) -- see *Unprivileged root* below. **No CI gate** -- see *CI coverage* below |
| `xmc4800-relax` | XMC4800 / M4F | P5.9 (LED1) | USIC0 ASC, P1.5/P1.4, 115200 -> VCOM; + RTT | onboard J-Link | [x] full selftest + stress + `HARD FAULT` dump (2026-07-09, 144 MHz); PMSAv7 enforcement selftest + `mpu_fault` cross-domain trap + the `xmcspi` granted-USIC window (2026-07-17) -- the canonical per-thread PMSA proof; console handover to a userspace driver, panic-path reclaim and clock retune all silicon-passed. **First board with an UNPRIVILEGED root** (2026-07-27) -- see *Unprivileged root* below |
| `f411disco` | STM32F411 / M4F | PD12 (LD4 grn) | USART2, PA2/PA3, 115200 (ext adapter) | onboard ST-Link (`st-flash`) | [x] full selftest + all apps + fault dump + bench + LED; **PMSAv7 enforcement silicon-witnessed 2026-07-29** -- enforcement selftest 62/62 + `mpu_fault` cross-domain MemManage denial, closing the `stm32f411` MPU HW debt for the chip. **Fifth board with an UNPRIVILEGED root, and the second on PMSAv7** (2026-07-29) -- see *Unprivileged root* below |
| `blackpill` | STM32F411 / M4F | PC13 (active-low) | USART2, PA2/PA3, 115200 (ext adapter) | USB-DFU / SWD | [x] full selftest + bench (2nd F411; 25 MHz HSE); MPU backend is the shared `stm32f411` one, silicon-witnessed on `f411disco` 2026-07-29 (not re-run on this board) |
| `f302nucleo` | STM32F302R8 / M4 | PB13 (LD2 grn) | USART2, PA2/PA3, 115200 -> ST-Link VCP | onboard ST-Link (`st-flash`) | [x] selftest minus the 4 KiB-alloc test (16 K RAM) + bench; **not an enforcement target -- the F302R8 (`x8` line) has no MPU** (the F302xB/xC line does). **A bench board** (onboard ST-Link, own VCOM, no external adapter), and the fleet's only physically-present **no-MPU ARM** board -- the sole possible silicon witness for the privilege-ring arm, and no such run has happened; see *Unprivileged root* below. The `[x]` above dates to 2026-07-14: **the `-st` image is computed not to boot at this branch tip** (arena 96 B short of root's stack), an M4.5.2 regression found by arithmetic and not by any gate, because this board has **none** -- see *CI coverage* below |
| `picopi` | RP2040 / M0+ | GP25 | UART0, GP0/GP1, 115200 | `picotool` (BOOTSEL) | [x] LED + UART0 + full selftest with `sched_exit` (2026-07-09, 125 MHz PLL); PMSAv6 cross-domain denial silicon-proven 2026-07-19 (M0+ has no MemManage -- it escalates to HardFault) -- the fleet's only armv6m enforcement proof; U-mode `cxxtest` still awaits a bench re-flash |
| `bluepill-c8` | STM32F103C8 / M3 (64 K/20 K genuine) | PC13 (active-low) | USART1, PA9/PA10, 115200 | external ST-Link (SWD) | (!) build-only, and **no unit exists** -- there is no genuine F103C8 on the bench, so nothing here can be silicon-witnessed at all (64 K/20 K linker; links the full app set incl selftest + stress) |
| `frdmk64f` | MK64FN1M0 / M4F | -- (none) | UART0, PTB16/PTB17, 115200 -> OpenSDA VCOM | J-Link (OpenSDA) | [x] HW 2026-07-15 (full selftest over the buffered console ring, 120 MHz); SYSMPU enforcement + `mpu_fault` trap silicon-proven at M2 |
| `teensy41` | i.MX RT1062 / M7 @396 MHz | -- (none wired) | LPUART6 ("Serial1", pins 0/1), 115200 | `teensy_loader_cli` (HalfKay, `.hex`) | [x] full selftest + soak under PMSAv7 enforcement, after the M7 anti-speculation fix (ERR011573; `../design-teensy-mpu-hang.md`) |
| `pizero2350` | RP2350 / M33 @150 MHz (armv7m backend) | -- (none on the Pi-Zero header) | UART1, GP4/GP5, 115200 | `picotool` (BOOTSEL) | [x] full selftest under PMSAv8 enforcement + `mpu_fault` cross-domain MemManage denial + bench/soak. **Third board with an UNPRIVILEGED root, and the first on PMSAv8** (2026-07-28) -- see *Unprivileged root* below. Also the first silicon witness for `kos_reboot` (BOOTSEL handover) and for `KICKOS_SHUTDOWN_TO_BOOTLOADER` on both terminal dead-ends -- see its flashing section |

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
- **`bluepill-c8`** -- build-only **because no unit exists**: there is no genuine 64 KiB/20 KiB
  F103C8 here, and the F103 port was physically run only on the now-retired 10 K clone. So its
  unwitnessability is hardware absence, not a verdict about the part -- and it costs no coverage,
  since `f302nucleo` is the same class (64 KiB-flash armv7m, no MPU, real privilege ring) and is on
  the bench. Links the full app set.
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
- **`xmc4800-relax` and `frdmk64f` under `-DKICKOS_HAVE_MPU=1` now print TAP through their
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
  use `kickos::emit` (`user/include/kickos/sys/emit.h`).
- **`kpanic_enter`'s UART reclaim clips bytes the userspace driver had in flight.** Reproducible on
  `xmc4800-relax`: the report always reaches the wire (that is the point of the reclaim), but roughly
  the last 8 bytes queued by the polled TX writer are garbled, eating the tail of the line before the
  dump. Announce-before-poke lines should therefore not be the *only* record of an address.
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
- **`f302nucleo` has no gate of any kind.** It is in the `build-boards` sweep and nothing else: no
  CTest, and no QEMU run gate because **no emulator models the part**. So the only thing CI says
  about this board is that it links, and a regression that stops it booting is invisible until
  somebody flashes it. That matters more now than it did, because it is a bench board and the
  fleet's only physically-present no-MPU ARM part (see *Unprivileged root* below).

  **That gap has a concrete instance: the `f302nucleo-st` `selftest` image no longer leaves enough
  arena for root's stack.** `__kickos_ram_end - __kickos_ram_start`, read with `arm-none-eabi-nm` on
  the built ELF, same toolchain and same command on both refs:

  | Ref | `__kickos_ram_start` | `__kickos_ram_end` | Arena |
  | --- | --- | --- | --- |
  | `181540e` (master) | `0x20002c40` | `0x20003800` | 3,008 B |
  | `176109e` (this branch) | `0x20002e60` | `0x20003800` | 2,464 B |

  `kmain` takes both bootstrap stacks from that arena through `boot_stack_alloc` --
  `KICKOS_IDLE_STACK_SIZE` then `KICKOS_ROOT_STACK_SIZE`, 512 and 2048 on this chip at those refs --
  so it needs **2,560 B**. Master had 448 B of headroom; the branch is **96 B short**, which means
  the second allocation cannot be satisfied, and an unsatisfied one is
  `kpanic("kmain: no arena for the root stack")` (`kernel/init/kmain.cc:224`) rather than a degraded
  boot. M4.5.2's static growth consumed 544 B of arena and crossed the line,
  which makes this a **regression introduced on this branch**, not a pre-existing condition --
  consistent with the board's genuine 2026-07-14 silicon pass at 13/14 (`../m2-readiness.md`) and
  with nothing having flashed it since.

  **This is arithmetic over the allocator, not an executed panic.** No emulator models the
  stm32f302 and the boards are physically disconnected, so nobody has run it: the evidence is the
  two arena figures against the 2,560 B requirement and nothing else. It is exactly the shape the
  missing gate cannot catch -- the image links cleanly, and the linker script's own arena `ASSERT`
  passes because it only checks that the arena is non-negative, not that it can hold what `kmain`
  will ask for.
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
chip works without it). It needs NRST reaching the probe, which the onboard debuggers
wire by construction but `bluepill-c8`'s 4-pin header does not carry. So
`tools/flash-stlink.sh` defaults it **on** for `f411disco` and `f302nucleo` and off
elsewhere; `STLINK_UNDER_RESET=1` forces it on, `=0` off.
```sh
FLASH_BUILD=$PWD/build/f411disco-mpu tools/flash.sh f411disco selftest
```
Nucleo consoles reach the ST-Link VCP (`ttyACM*`) with no
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

`-x` runs it after loading. `picotool` keys the file type off the extension, so hand it a path
ending in `.elf`. Console is **UART1 on GP4 (TX) / GP5 (RX)**, 115200 -- a 3.3 V adapter, and note
it is *not* UART0 (whose pins the Pi-Zero header does not bring out). No diagnostic LED, so the
console is the only channel. BOOTSEL always recovers the board, so a wrong clock or boot-block
config cannot brick it.

**One image per BOOTSEL state, and no SWD escape** -- unless the image hands itself back (the knob
below). Once an ordinary KickOS image runs the board is no longer a boot device, so the next
`picotool load` needs a physical BOOT press. `flash-jlink.sh` carries an `RP2350_M33_0` row, but on
this bench it does not help: the J-Link Pro (SN `000177003338`) reports `VTref=0.000V` /
`ITarget=0mA` against this board, i.e. the probe is not wired to the Pi-Zero's SWD pads at all.
**SWD remains unavailable here**, so BOOTSEL is the only channel; budget captures accordingly, or
wire SWD first.

**`kos_reboot` closes that loop, and is silicon-witnessed here** (2026-07-28, `6857df3`, the
first execution of syscall 38 and its RP2350 backend on any chip). `rebootdemo` announced, waited
out its 3 s arming timeout, and never printed the `reboot declined: rc=` line -- the call did not
return, which is the contract for the BOOTSEL-type `reboot(0x0102, 10, 0, 0)`:

```
[rebootdemo] KickOS reboot-to-bootloader demo
[rebootdemo] handing the chip to its bootloader in 3s
```

The board then re-enumerated as `2e8a:000f Raspberry Pi RP2350 Boot` on a fresh USB device
number, and `picotool info` answered again (`target chip: RP2350`, `image type: ARM Secure`). So
the bootrom magic check at `0x10`, the lookup-helper halfword at `0x16` and the `'R','B'` lookup
under `RT_FLAG_FUNC_ARM_SEC` all resolved on real silicon. A diagnostic image that ends in
`kos_reboot()` therefore hands its own next BOOTSEL state back.

**`-DKICKOS_SHUTDOWN_TO_BOOTLOADER=ON` extends that to any image, including one that ends in a
fault.** A per-app `kos_reboot()` at the end of `main` cannot help `rootfault` or `mpu_fault` --
they never reach it. The knob (default OFF, requires `KICKOS_ENABLE_SELFTEST`) instead puts the
handover in the kernel's two terminal dead-ends, `kickos_terminate` (shutdown syscall,
last-thread-out, `kickos_isr_fault`) and the weak `kfault_terminate` (`kpanic`, and every ARM
MemManage/HardFault -- the arm this board's fault captures actually take). Both drain the console
first. Leave it OFF for a bench or soak image, which must stay resident.

**Both dead-ends are silicon-witnessed here (2026-07-28, `3204121`), and the evidence is the USB
device number.** The bootrom re-enumerates on every handback, so a number that rises at each step
proves a fresh enumeration rather than a stale node. Seven images ran off the ONE human press that
produced device 022:

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

So `kickos_terminate` is witnessed four times and `kfault_terminate` three, each fault image having
printed its complete `=== MPU FAULT ===` register dump through `MMFAR` before handing over -- the
drain does run ahead of the reboot. Steps 5-7 are re-captures of 2, 4 and 3 (the first pass's logs
were lost to a capture-tooling fault, not a board fault), and they cost nothing: no press was needed
at any point after 022.

**The handler-mode call is now witnessed, which was this knob's one unverified risk.**
`kfault_terminate` runs in MemManage **handler** mode with the faulting context still live, and
`kos_reboot`'s witness above covers only thread mode on an ordered exit -- a bootrom call from a
fault handler had never been shown on this part. Steps 1, 3 and 7 are exactly that call returning
the board to BOOTSEL.

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

`-run` is the only reset available here, so start the console capture **before** the flash rather
than after it. More than one FT232 is usually attached; the way to tell which is this board is the
`board rx72m` banner line, so capture every candidate and pick by content (2026-07-28: the other
FT232 was a `pizero2350` console). The long-standing "watch for a TX/RX cabling swap" note did
**not** reproduce in the 2026-07-28 session -- thirteen flashes, every one printed on `ttyUSB0`
first try -- so treat it as a possible cause of silence, not an expected step.

### `xmc4800-relax` and `frdmk64f` -- J-Link

Both use SEGGER J-Link (XMC = onboard J-Link-OB; K64F = OpenSDA reflashed with
J-Link firmware). Full `JLinkExe` / GDB / RTT recipes are in
[flashing.md](../flashing.md).

Drive `JLinkExe` **headless**: `-CommanderScript <file>` (never the interactive prompt),
`-NoGui 1` (V9.58 otherwise forks a GUI server and hangs), `-AutoConnect 1`, `-ExitOnError 1`,
explicit `-if SWD -device <dev> -speed <n>`, and `-SelectEmuBySN <sn>` because more than one probe
is attached. Closing stdin (`< /dev/null`) makes an unanticipated prompt fail fast instead of
hanging. Capture the VCOM with a scripted reader, not `minicom`/`screen`, which need a keystroke to
exit.

**OpenSDA/J-Link stability on the K64F: nothing anomalous observed** (2026-07-27). Six
flash-plus-reset cycles across two images, every connect first-try, no replug or power cycle needed.
The older "K64F wedges its SWD and needs a physical power cycle" note is not corroborated by this
session and should not be treated as a bench fact.

### Unprivileged root

`KICKOS_ROOT_PRIVILEGED=OFF` creates root unprivileged, holding a `CAP_AUTHORITY` instead of the
whole arena. Five boards are flipped: `xmc4800-relax` (PMSAv7), `esp32c6-wroom` (RISC-V PMP),
`pizero2350` (PMSAv8), `rx72m` (RXv3 RX-MPU) and `f411disco` (PMSAv7), each with its own
subsection below. That completes the declared stage-2 set; `frdmk64f` waits for stage 3.

**The knob itself is decided for deletion, with no replacement** -- unprivileged root becomes the
only posture on every board, and no board is marked as unable to enforce it
(`design-unprivileged-root.md` sections 4 and 10). The captures below are what they always were;
what changes is that "flipped" stops being a per-board property. The witness a board can carry
still depends on its hardware, in three arms: **authority** (a capability gate refusing a thread
that lacks the bit -- pure kernel logic, so every target), **confinement** (a cross-domain fault --
needs an MPU), and **ring** (an unprivileged thread refused a privileged-only register -- needs a
privilege ring and no MPU). Every subsection below is a confinement witness. **No board has yet
witnessed the ring arm on silicon**, and `f302nucleo` is the only one that could.

The honest asymmetry, because it is easy to over-read a green run: on a board with no ring the
authority arm still passes, and it never implies confinement. Nothing there stops a thread walking
past the syscall and touching the peripheral directly.

#### `xmc4800-relax` -- PMSAv7

`xmc4800-relax` is the first board flipped, and the first silicon witness for the
boundary (2026-07-27, **`22e1c5a`**, PMSAv7, 144 MHz, console-only service list). Both arms were
re-captured on the post-rebase tree; the earlier pre-rebase capture (`a463ab9`) is superseded and
its hash no longer exists on this branch. **Re-witnessed at the M4.5.1 tip (2026-07-28,
`75227d4`)**: both arms re-run, and the table below is the tip capture (captures under
`.session/n33-rewitness/`, machine-local). The hashes in this section are what the silicon
banners stamped; after the M4.5.1 squash they resolve against `backup/m4.5.1-pre-squash`, not
the live branch.

| Evidence | Root privileged (control) | Root unprivileged |
| --- | --- | --- |
| Banner `mpu` line | `enforce` | `enforce, root unprivileged` |
| Console handover to `xmcuart` | up, TAP via driver | up, TAP via driver |
| `selftest` | 61 cases, 61 `ok`, **0 skips**, 0 fail | 61 cases, 60 `ok`, 1 skip, 0 fail |
| `rootfault` cross-domain write | completes, reports "root is NOT confined" | **MemManage trap** |

The control column previously read "58 `ok`, 1 skip". That was wrong, and the way it was wrong is
worth keeping: the 1 was `mutex_deadlock # SKIP pool too small` from the **FRDM-K64F full-service-list**
run, transcribed into the XMC row. Under the console-only list this board skips nothing in the
privileged arm, so the honest control is 0 -- which also makes the A/B cleaner than it looked, since
the entire skip delta between the columns is now posture-driven rather than partly provisioning.

The confinement fault, byte-for-byte from the wire:

```
[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x20013000 (expect fault
=== MPU FAULT ===
  PC=0x800060c LR=0x800071d xPSR=0x1000000 (PSP)
  R0=0x52 R1=0x3 R2=0x2222 R3=0x20013000 R12=0x0
  CFSR=0x82 HFSR=0x0
  MMFAR=0x20013000
```

`MMFAR` matches the address the app announced, `R3` holds it and `R2` holds the stored `0x2222`,
`CFSR=0x82` is DACCVIOL + MMARVALID, and `(PSP)` puts the fault in thread mode. The `75227d4`
re-run reproduces PC, LR, CFSR and MMFAR identically, including the reclaim clip. The child had
already written the same region and was still parked, so the page belonged to a live foreign domain
rather than being unmapped. The truncated `(expect fault` is the `kpanic_enter` reclaim clipping
noted above, reproduced identically across resets.

The same run with `KICKOS_ROOT_PRIVILEGED=ON` completes the write and prints `root is NOT confined`,
which is what makes the trap attributable to the flip rather than to anything else on the board.

The two selftest skips were named on the wire: `irq_as_event` (root plays the device, writing a page
it allocated but was never granted) and `mpu_privileged_guard` (its subject is the privileged
posture). **This capture predates `kos_mem_self_grant`**, which is why there are two. `irq_as_event`
was not posture cost but a missing capability, and it now runs in both postures; only
`mpu_privileged_guard` is genuinely posture-driven. Re-measured on this board at `75227d4`: the
flipped arm skips exactly `mpu_privileged_guard`, and `irq_as_event`, `mem_self_grant` and
`mem_self_grant_nonpow2` all run `ok` under enforcement in both postures.

#### `esp32c6-wroom` -- RISC-V PMP

The second board flipped, and the first silicon witness for the boundary on **PMP** rather than
an ARM MPU (2026-07-28, **`e5c651b`**, PMP NAPOT, ~160 MHz, kernel console, service list
`kickos_services_none`). Both arms were captured at that tip on the plugged board, over the
CH343P (captures under `.session/`, machine-local).

| Evidence | Root privileged (control) | Root unprivileged |
| --- | --- | --- |
| Banner `mpu` line | `enforce` | `enforce, root unprivileged` |
| `selftest` | 62 cases, 62 `ok`, **0 skips**, 0 fail | 62 cases, 61 `ok`, 1 skip, 0 fail |
| `rootfault` cross-domain write | completes, reports "root is NOT confined" | **PMP store fault naming `root`** |
| `c6blink` granted-GPIO window | blinks, pad tracks, ungranted poke faults | identical |

The one skip is `mpu_privileged_guard`, named on the wire and posture-driven exactly as on the
XMC. The confinement fault, byte-for-byte from the wire:

```
[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x40834000 (expect fault)

MPU FAULT: task 'root' attempted write at 0x40834000 -- reported
```

The address in the kernel's line matches the one the app announced, and the child had already
written that region and was still parked, so it belonged to a live foreign domain. Unlike the
XMC the report is not clipped: the C6 runs the kernel console, so there is no userspace-driver
reclaim in the panic path. Nothing here witnesses a console handover under the flip -- that arm
is XMC-only, since this board has no userspace console driver.

**What had to close first: the GPIO matrix.** A pad on this family passes two mux stages, and
`arch_pinmux_set` mediated only the IO_MUX pad word -- so the matrix out-sel was reachable only
as raw MMIO, and `c6blink` did that out-sel write plus its `GPIO_ENABLE_W1TS` direction write
from `main`, i.e. from root. Both stages now ride one `kos_pinmux_set` call (`4947e6e`) and
`c6blink` does no MMIO from root at all (`e5c651b`); its unprivileged driver holds the 64 B pin
bank and does direction, drive and readback itself. The silicon proof that both stages really
landed is the pad readback: `GPIO_IN` tracked the drive on all ten cycles in both postures,
which it cannot do unless the pad was muxed to the matrix *and* the matrix out-sel set to 128.

`c6blink`'s isolation oracle moved with the widened window and now pokes ungranted
`GPIO_FUNC10_OUT_SEL_CFG` -- the matrix escalation surface itself:

```
[c6blink] PASS (pad tracked the drive on every cycle)
[c6blink] poking UNGRANTED out-sel @ 0x6009157c (expect MPU FAULT)

MPU FAULT: task 'c6blink' attempted write at 0x6009157c -- reported
```

Identical in both postures, which is the point: the app is posture-independent, so its
enforcement signal is attributable to PMP and not to the root posture.

#### `pizero2350` -- PMSAv8

The third board flipped, and the first silicon witness for the boundary on **PMSAv8** -- a
Cortex-M33 running the `armv7m` arch backend with `KICKOS_ARM_PMSAV8_SOURCE` linked in
(2026-07-28, **`6857df3`**, 150 MHz, kernel console, service list `kickos_services_none`).
Captured on the plugged Waveshare Pi-Zero over an FTDI on GP4 (captures under `.session/logs/`,
machine-local).

| Evidence | Root privileged (control) | Root unprivileged |
| --- | --- | --- |
| Banner `mpu` line | `enforce` | `enforce, root unprivileged` |
| `selftest` | 62 cases, 62 `ok`, **0 skips**, 0 fail | 62 cases, 61 `ok`, 1 skip, 0 fail |
| `rootfault` cross-domain write | completes, reports "root is NOT confined" | **MemManage trap**, `CFSR=0x82` |
| `mpu_fault` domain-A -> domain-B write | n/a: the app's subject is an unprivileged child either way | **MemManage trap**, `CFSR=0x82`, `MMFAR=0x20027000` |

The `rootfault` row is the `6857df3` capture. The `selftest` pair and `mpu_fault` were measured at
**`3204121`** with `KICKOS_SHUTDOWN_TO_BOOTLOADER=ON`, and all three banners stamp that commit, so
the two `selftest` columns are the same tree and differ only in the knob (captures
`clean-selftest-{flip,ctl}.log` and `clean-mpufault-flip.log` under `.session/logs/`, machine-local).

The one skip is `mpu_privileged_guard`, named on the wire and posture-driven exactly as on the XMC
and the C6:

```
ok 54 - mpu_privileged_guard # SKIP root unprivileged: no privileged caller exists; the inverse claim is apps/rootfault
# skipped: 1
# all tests passed (1 skipped)
```

The control arm runs that same case and passes it, with nothing else moving in the stream:

```
ok 54 - mpu_privileged_guard
# skipped: 0
# all tests passed
```

That one line is the whole A/B inside the TAP stream: same plan (`1..62`), same 61 other verdicts,
and the entire skip delta between the columns is posture-driven rather than provisioning.

The confinement fault, byte-for-byte from the wire:

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
flash at `0x10000530`, which is where root's code executes from on this part. The child had
already written the same region and was still parked, so the page belonged to a live foreign
domain rather than being unmapped.

**The report carries no task name, and cannot on this family.** ARM MemManage goes straight to the
armv7m reporter, which prints the register dump and labels it `=== MPU FAULT ===` only when the
CFSR MMFSR byte is set; the `MPU FAULT: task 'root'` form comes from `kickos_isr_fault`, which is
the RISC-V/chip-hook route. `tests/check_qemu_rootfault.sh` already encodes exactly this two-family
split, and this capture is its ARM arm. Attribution to root therefore rests on the
announce-before-poke ordering plus the `MMFAR` match, as it does for the XMC. Unlike the XMC the
announce line is **not** clipped -- this board runs the kernel console, so there is no
userspace-driver reclaim in the panic path, the same reason the C6 prints cleanly.

**The privileged control was measured, so the trap is attributable to the flip.** The same binary
built with `KICKOS_ROOT_PRIVILEGED` at its default ON, on the same tip and the same board, completes
the write and says so:

```
   mpu     enforce
[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x20026000 (expect fault)
[rootfault] cross-domain write completed: root is NOT confined (expected with KICKOS_ROOT_PRIVILEGED=1 or no enforcement)
```

No fault, no register dump, and the banner reads `enforce` without `root unprivileged`. The region
address is `0x20026000` in **both** arms, so the two runs put root's write at the same place and
differ only in the knob. That makes this board's A/B as strong as the XMC's and the C6's, rather
than resting on construction alone.

**`mpu_fault` adds a second, independent PMSAv8 denial under the flip** -- child-to-child rather
than root-to-child, so it checks the descriptor programming without root's posture in the picture at
all. Its subject is an unprivileged domain-A thread in either posture (the region base arrives
through the thread ARG by value precisely so root never touches domain A), which is why the control
column above reads `n/a` instead of naming a missing run:

```
[domain] A: writing my own region
[domain] A: my region ok; writing domain B (expect fault)

=== MPU FAULT ===
  PC=0x10000418 LR=0x100004d7 xPSR=0x1000000 (PSP)
  R0=0x3a R1=0x3 R2=0x2222 R3=0x20027000 R12=0x0
  CFSR=0x82 HFSR=0x0
  MMFAR=0x20027000
```

Same signature shape as the `rootfault` arm -- `CFSR=0x82`, `(PSP)`, `R3`/`MMFAR` agreeing on the
denied address, `R2` holding the `0x2222` never stored -- at a different address and from a
different faulting PC. This board had a pre-flip `mpu_fault` witness (see the status matrix); this
is the run under an unprivileged root.

**What this does not cover.** Nothing here witnesses a console handover under the flip -- that arm
stays XMC-only, since this board has no userspace console driver. The bench limit that used to keep
the control `selftest` and `mpu_fault` off this list is gone:
`KICKOS_SHUTDOWN_TO_BOOTLOADER=ON` retires the press-per-image cost, so both are measured above.

#### `rx72m` -- RXv3 RX-MPU

The fourth board flipped, and the only witness for the boundary on the **RX MPU** -- the fleet's
one CPU-side unit that checks the peripheral/SFR aperture as well as RAM (2026-07-28,
**`d71b313`**, 240 MHz, kernel console, service list `kickos_services_none`). Both arms were
captured at that tip on the plugged board, over the FT232 (`/dev/ttyUSB0`; captures under
`.session/logs/`, machine-local). Both arms' banners stamp `commit d71b313`, so the two columns
are the same tree.

**There is NO emulator model for RXv3 anywhere** -- no QEMU machine, no CI job (see *CI coverage*
above, RXv3 row: `none`). This flip is silicon-only and unfalsifiable off the bench: nothing in
the repo can re-derive the table below.

| Evidence | Root privileged (control) | Root unprivileged |
| --- | --- | --- |
| Banner `mpu` line | `enforce` | `enforce, root unprivileged` |
| `selftest` | 62 cases, 62 `ok`, **0 skips**, 0 fail | 62 cases, 61 `ok`, 1 skip, 0 fail |
| `rootfault` cross-domain write | completes, reports "root is NOT confined" | **access exception naming `root`** |
| `rxdrv` granted-port window | blinks, pad tracks, `-KOS_EBUSY` refusal, ungranted poke faults | identical |

The one skip is `mpu_privileged_guard`, named on the wire and posture-driven exactly as on the
XMC and the C6:

```
ok 54 - mpu_privileged_guard # SKIP root unprivileged: no privileged caller exists; the inverse claim is apps/rootfault
# skipped: 1
# all tests passed (1 skipped)
```

The confinement fault, byte-for-byte from the wire:

```
[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x14000 (expect fault)

MPU FAULT: task 'root' attempted write at 0x14000 -- reported
```

The address in the kernel's line matches the one the app announced, and the child had already
written that region and was still parked, so it belonged to a live foreign domain. This is the
**named** reporter, not the nameless `=== RX EXCEPTION (access exception) ===` dump:
`kickos_rx_fault_report` routes vector `0x54` to `kickos_isr_fault` only when the faulting
`PSW.PM` is set, and a flipped root runs in user mode, so it qualifies. That branch had never
been reachable from root before the flip. Like the C6 the report is not clipped -- this board runs
the kernel console, so there is no userspace-driver reclaim in the panic path, and nothing here
witnesses a console handover under the flip (that arm stays XMC-only).

**What had to close first: the mux had no mediation at all.** `rx72m` had no `arch_pinmux_set`
backend, so the weak `-KOS_ENOSYS` applied and any privileged caller could re-point the SCI6
console pins; and `rxdrv` wrote `PORT8.PMR`, `PODR` and `PDR` raw from `main`, i.e. from root.
Those split two ways. `PMR` selects peripheral-vs-GPIO per pin and is therefore an escalation
surface, so it is now kernel-mediated (`f891ea0`); `PDR` and `PODR` are not, once the pin is
owned, so they moved in-window (`a5a70b6`). The backend covers **both** RX mux stages in one call
-- the MPC `PmnPFS` function select and `PORTm.PMR` -- because `PSEL` can re-point a pin already
at `PMR=1`, which the console pins are, without `PMR` ever being written; mediating `PMR` alone
would have left the refusal bypassable. `PB1/TXD6` and `PB0/RXD6` are refused `-KOS_EBUSY`, and
`rxdrv` asks for exactly that refusal on the wire:

```
[rxdrv] pinmux P80 -> general I/O rc 0
[rxdrv] pinmux PB1/TXD6 refused (-KOS_EBUSY): console pin is kernel-owned
```

That second call requests `PSEL=000000b` with `PMR=0` on the console's TX pin -- the write that
would dark the console if the refusal ever regressed -- so a run that stops at that line is
itself the failure signal.

A `PmnPFS` write needs the MPC `PWPR` unlock (`B0WI=0`, then `PFSWE=1`) and is legal only while
the pin's `PMR` bit is 0 (UM r01uh0804ej0120 sec.23.4.1 steps 1-6, sec.23.4.2 (1)); miss either
and the write is silently dropped. `PORTm.PMR` itself needs no unlock -- `PRCR` gates only the
clock / operating-mode / low-power / LVD registers (sec.13.1.1) and does not cover the PORT or
MPC blocks. **The unlock is not independently witnessed by `rxdrv`**: `P80PFS`'s reset value is
already the `0x00` the app writes, so a dropped write would look identical. It is witnessed
instead by the console itself -- `sci6_console_init` now goes through the same unlock helper, and
its `PSEL=001011b` writes to `PB1PFS`/`PB0PFS` are what put the banner on the wire at all.

`rxdrv`'s isolation oracle moved with the widened window and now pokes ungranted `PORT8.PMR` --
the mux escalation surface the backend just took over:

```
[rxdrv] blink 10 pad=0/0 pad=1/1
[rxdrv] PASS (pad tracked the drive on every cycle)
[rxdrv] poking UNGRANTED PORT8.PMR @ 0x0008C068 (expect MPU FAULT)

MPU FAULT: task 'rxdrv' attempted write at 0x8c068 -- reported
```

Identical in both postures, which is the point: the app is posture-independent, so its
enforcement signal is attributable to the RX MPU and not to the root posture. The poke is a plain
store, not a read-modify-write -- an RMW faults on its read half and the report named a read,
which understates the escalation. The `pad=0/0 pad=1/1` columns are `PORT8.PIDR`, the pin state,
which reads regardless of `PDR`/`PMR` (UM sec.22.3.3); it tracked the drive on all ten cycles in
both arms, and it cannot unless `PMR` really cleared and `PDR` really set output. The window
widened from the 16 B `PODR` block to 80 B at the port base (`PDR` + `PODR` + `PIDR` up to
`PORTF`), ending at `0x0008C04F` -- 16 B, one full register row, short of the `PMR` block at
`0x0008C060` -- still one RX-MPU descriptor, since the unit wants a 16-aligned base and a
16-multiple size and no power of two. Covering `PDR`/`PODR` for every port
is an unavoidable over-grant (the RX interleaves ports inside each register block rather than
blocking per port) but not an escalation: a pin at `PMR=1` ignores `PDR`/`PODR` entirely (UM
Table 23.47), so the console pins stay unreachable through the window.

**`stress` was NOT run under the flip.** RX sign-off historically paired "selftest + stress", but
`apps/common/stress` spawns privileged children at three sites and structurally cannot work with
an unprivileged root -- that is stage-4 work, not a regression of this flip.

#### `f411disco` -- PMSAv7

The fifth and last board of the declared stage-2 set, and the **second** PMSAv7 witness after the
XMC (2026-07-29, **`6646c8e`**, 84 MHz, kernel console, service list `kickos_services_none`). Both
arms were captured at that tip on the plugged 32F411E-DISCO, flashed over the onboard ST-Link and
read on USART2/PA2 through an FT232 (`/dev/ttyUSB1`; captures under `.session/logs/`,
machine-local). Both arms' banners stamp `commit 6646c8e`, so the two columns are the same tree and
differ only in the knob.

**This board's enforcement had never been witnessed at all before this run** -- the status matrix
carried `stm32f411` as build-and-link validated with the MPU HW pending. So the debt was closed
first, in the default posture, and only then was the knob flipped; the two are separate captures
below for exactly that reason. Since the MPU backend is the shared `stm32f411` one, this closes the
HW debt for the chip, `blackpill` included.

| Evidence | Root privileged (control) | Root unprivileged |
| --- | --- | --- |
| Banner `mpu` line | `enforce` | `enforce, root unprivileged` |
| `selftest` | 62 cases, 62 `ok`, **0 skips**, 0 fail | 62 cases, 61 `ok`, 1 skip, 0 fail |
| `rootfault` cross-domain write | completes, reports "root is NOT confined" | **MemManage trap**, `CFSR=0x82`, `MMFAR=0x2000a000` |
| `mpu_fault` domain-A -> domain-B write | **MemManage trap**, `CFSR=0x82`, `MMFAR=0x2000b000` | n/a: the app's subject is an unprivileged child either way |

The `mpu_fault` column sits on the control side here, the mirror of `pizero2350`'s table, because
that run is the pre-flip enforcement witness -- the debt closure -- and not a flip arm. The app is
posture-independent either way.

**Phase 1, the enforcement witness in the default posture.** `selftest` under
`-DKICKOS_HAVE_MPU=1` with `KICKOS_ROOT_PRIVILEGED` left at its default ON:

```
   board   f411disco
   arch    armv7m
   mpu     enforce

1..62
...
ok 54 - mpu_privileged_guard
...
ok 62 - mem_self_grant
# skipped: 0
# all tests passed
```

62 of 62, nothing skipped -- including `mmio_grant`, `domain_share`, `grant_reserved`,
`confused_deputy`, `mem_self_grant` and `mem_self_grant_nonpow2`, the cases that only mean anything
with a live MPU. Then the cross-domain denial, byte-for-byte from the wire:

```
[domain] A: writing my own region
[domain] A: my region ok; writing domain B (expect fault)

=== MPU FAULT ===
  PC=0x800048c LR=0x800054b xPSR=0x1000000 (PSP)
  R0=0x3a R1=0x3 R2=0x2222 R3=0x2000b000 R12=0x0
  CFSR=0x82 HFSR=0x0
  MMFAR=0x2000b000
```

`CFSR=0x82` is DACCVIOL + MMARVALID, `(PSP)` puts the fault in thread mode, `R3` and `MMFAR` agree
on the denied address, and `R2` holds the `0x2222` that was never stored. `kos_ram_alloc` handed
domain A `0x2000a000` and granted it the low 4 KiB, so `0x2000b000` is one region past the grant --
the app does not print the number, so the `R3`/`MMFAR` agreement plus that arithmetic is what ties
the trap to the announced write. That closes the `stm32f411` MPU HW debt: PMSAv7 on this chip
enforces, and it is no longer inferred from the XMC.

**Phase 2, the flip.** The one skip is `mpu_privileged_guard`, named on the wire and posture-driven
exactly as on the other four boards:

```
ok 54 - mpu_privileged_guard # SKIP root unprivileged: no privileged caller exists; the inverse claim is apps/rootfault
# skipped: 1
# all tests passed (1 skipped)
```

The control arm above runs that same case and passes it, with nothing else moving in the stream:
same plan (`1..62`), the same 61 other verdicts in the same order, and the entire skip delta between
the columns is posture-driven rather than provisioning. That one line is the whole A/B inside the
TAP stream.

The confinement fault, byte-for-byte from the wire:

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
`CFSR=0x82` is DACCVIOL + MMARVALID, and `(PSP)` puts the fault in thread mode. The child had
already written the same region and was still parked, so the page belonged to a live foreign domain
rather than being unmapped. The announce line is **not** clipped -- this board runs the kernel
console, so there is no userspace-driver reclaim in the panic path, the same reason the C6 and the
Pi print cleanly and the XMC does not.

**The report carries no task name, and cannot on this family.** ARM MemManage goes straight to the
armv7m reporter, which prints the register dump and labels it `=== MPU FAULT ===`; the
`MPU FAULT: task 'root'` form comes from `kickos_isr_fault`, the RISC-V/chip-hook route.
`tests/check_qemu_rootfault.sh` encodes that two-family split. Attribution to root rests on the
announce-before-poke ordering plus the `MMFAR` match, as it does for the XMC and the Pi.

**The privileged control was measured, so the trap is attributable to the flip.** The same app built
with `KICKOS_ROOT_PRIVILEGED` at its default ON, on the same tip and the same board, completes the
write and says so:

```
   mpu     enforce
[rootfault] child: wrote my own granted region
[rootfault] root: writing the child's granted region at 0x2000a000 (expect fault)
[rootfault] cross-domain write completed: root is NOT confined (expected with KICKOS_ROOT_PRIVILEGED=1 or no enforcement)
```

No fault, no register dump, and the banner reads `enforce` without `root unprivileged`. The region
address is `0x2000a000` in **both** arms, so the two runs put root's write at the same place and
differ only in the knob.

**What this does not cover.** Two things, both pre-existing and neither blocking the flip.

`f411spi` -- this board's own diagnostic app, the STM32-family peripheral-window reference -- does
its SPI1 bring-up (RCC clock-enable, GPIOA/GPIOE mux, SPI1 master config) from `main`, i.e. from
root, before spawning the unprivileged driver that holds the 32 B SPI1 grant. Under the flip root
has no MMIO authority, so the app faults on its very first bring-up store, at `RCC_AHB1ENR`:

```
   mpu     enforce, root unprivileged

=== MPU FAULT ===
  PC=0x8000622 LR=0x8000621 xPSR=0x1000000 (PSP)
  R0=0x40023830 R1=0x0 R2=0x0 R3=0x40023830 R12=0x0
  CFSR=0x82 HFSR=0x0
  MMFAR=0x40023830
```

`0x40023830` is `RCC_AHB1ENR` (RCC base `0x40023800` + `0x30`), the first line of `main`. This is
the same shape as `c6blink` and `rxdrv` before their windows were reworked, and it is a **known
follow-up, not a flip blocker**: the stage-2 gate is `selftest` + `rootfault`, both of which are
green above, and `f411spi` is a `kickos_add_diagnostic_app` that is never a production image.
Restructuring it into a mediated bring-up plus a widened grant is stage-3 work
(`arch_periph_enable`), tracked with the other board apps.

`f411spi`'s own subject -- a granted SPI1 window driving a real PA7->PA6 loopback -- stays
**unwitnessed on this board in either posture**. It needs the loopback jumper fitted, and it was not
run in the control posture here. The chip's peripheral-window proof therefore remains open; the
canonical PMSA peripheral proof stays `xmcspi` on the XMC. Nothing here witnesses a console handover
under the flip either -- that arm stays XMC-only, since this board has no userspace console driver.

#### Coverage boundary -- what this silicon witness covers

**Re-established 2026-07-28 at the tip, `75227d4`**: the XMC A/B re-run plus the `frdmk64f`
SYSMPU regression, six flash-and-capture runs, every signature matched, zero reflashes. The gap
this section used to record -- commits green under emulation but never run on hardware -- is
closed for everything at or before `75227d4`. **Extended 2026-07-28 to `e5c651b`** by the C6 A/B
plus the `esp32-wroom` LX6 regression, seven more flash-and-capture runs, zero reflashes.
**Extended again 2026-07-28 to `6857df3`** by the `pizero2350` A/B (PMSAv8) and the `kos_reboot`
witness, four flash-and-capture runs on three BOOTSEL states, both `rootfault` arms measured.
**Carried to `3204121`** on that board under `KICKOS_SHUTDOWN_TO_BOOTLOADER=ON`: seven images off
one human press, closing its control `selftest` (62/62, 0 skips) and its `mpu_fault` arm under the
flip, and witnessing both terminal dead-ends. Its console-handover arm stays unwitnessed (no
userspace console driver on this board).
**Extended once more 2026-07-28 to `d71b313`** by the `rx72m` A/B: thirteen flash-and-capture runs
in one session (`selftest`, `rootfault`, `rxdrv` per arm, an `rxdrv` re-run after the oracle poke
became a plain store, then all six re-run at `d71b313` so both arms stamp the same commit), zero
reflashes, every signature matched. The RX arm is the one that cannot be cross-checked anywhere
else -- no RXv3 emulator exists -- so for `arch/rx/**` the boundary IS this bench run.
**Extended 2026-07-29 to `6646c8e`** by the `f411disco` phase-1 enforcement witness plus its A/B:
five flash-and-capture runs (control `selftest` + `mpu_fault`, flipped `selftest` + `rootfault`,
control `rootfault`), zero reflashes, every signature matched. That run also closes the last
never-witnessed enforcement backend in the fleet -- `stm32f411` PMSAv7, shared with `blackpill`.

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

**`frdmk64f` is deliberately NOT flipped** and stays privileged-root: its service list's bring-up
writes peripheral registers directly from root, which needs stage 3's `arch_periph_enable`. Pairing
that list with the flip is a configure-time error, not a runtime surprise. It was instead
re-regressed under SYSMPU enforcement with a privileged root, first at `22e1c5a` and again at
`75227d4` (2026-07-28): selftest 61 cases / 60 `ok` / 1 skip (`mutex_deadlock`, pool too small) /
0 fail, and `mpu_fault` still traps a child's cross-domain write with
`SYSMPU ISOLATION FAULT: port=3 addr=0x2001b000 master=0 W EDR=0x80000003` -- byte-identical to
the prior witness (SYSMPU surfaces as an imprecise bus fault, so the core label reads
`HARD FAULT` and the chip hook adds the real line).

**`esp32-wroom` is not a flip target at all** -- the LX6 has no MPU, so there is no privilege
boundary to enforce and the banner reads `mpu off`. It is regressed instead: at `e5c651b`
(2026-07-28) its selftest ran 58 cases / 58 `ok` / 0 skips / 0 fail on silicon, unchanged by the
C6 work, which is what the run is for (the two Espressif ports share nothing but the flasher).

**`microbit` witnesses no arm on silicon, for two independent reasons.** There is **no physical
unit** -- it is a QEMU armv6m vehicle, not a bench board (`../m2-readiness.md`) -- and the
nRF51822's Cortex-M0 implements no privilege axis anyway, so the kernel's `unprivileged` marking is
discarded by the hardware and such a thread runs privileged (see the caveat under *Per-board
caveats*). Even a unit would therefore witness the authority arm and nothing about the ring. The
armv6m privilege boundary is `picopi`'s alone -- an M0+, which does implement the extension.

**`bluepill-c8` carries no witness because no unit exists.** That is the binding reason, not RAM
(heap policy, `design-flash-footprint.md` section 7), not the 9-handle provisioning (the authority
cap costs zero dynamic slots), and not the missing MPU. It costs nothing either: `f302nucleo` is the
same class -- a 64 KiB-flash armv7m part with no MPU and a real privilege ring -- and is on the
bench, so it carries the hardware coverage for both.

**`f302nucleo` is the fleet's only physically-present no-MPU ARM board, so it is the sole possible
silicon witness for the ring arm.** Having no MPU rules out the confinement arm and nothing else;
the ring arm wants exactly a ring with no MPU, which the F302R8's M4 has. **Nothing has been run on
it under an unprivileged root** -- no flash, no capture, no result of any kind. This is future work,
and it needs a gate built for it too, since the board has none (see *CI coverage* above) -- which is
also where the arena regression that its `-st` image is computed to hit at this branch tip is
recorded, and that has to clear before any capture here means anything.
`design-unprivileged-root.md` sections 9 and 10 carry the arms and the arithmetic.
