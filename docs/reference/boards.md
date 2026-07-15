<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS board support

Status of every board target: what works, what only builds, how to flash it, and
where its console + LED live. For J-Link / RTT details see [flashing.md](../flashing.md).

## Status matrix

| Board (preset) | SoC / core | LED (blink) | Console | Flash tool | HW-validated |
|---|---|---|---|---|---|
| `sim` | host process | -- | host stdout | `ctest --preset sim` | [x] CI |
| `qemu` | mps2-an386 / M4 | -- | semihosting | `ctest --preset qemu` | [x] CI |
| `microbit` | nRF51822 / M0 | -- | semihosting | `ctest --preset microbit` | [x] CI |
| `qemu-riscv` | QEMU virt / RV32IMAC | -- | semihosting | `ctest --preset qemu-riscv` | [x] CI (first RISC-V) |
| `esp32c6-wroom` | ESP32-C6-WROOM-1 / RV32IMAC | GP8 (WS2812B, LED2) | UART0, GP16/GP17, 115200 -> CH343P VCOM (`/dev/ttyACM0`) | esptool | [x] selftest 14/14 + PMP NAPOT + diag-LED + bench |
| `esp32-wroom` | ESP32-D0WD / Xtensa LX6 @240 MHz | GP2 (D2, active-high) | UART0, GP1/GP3, 115200 -> CH340 (`/dev/ttyUSB1`) | esptool | [x] 8/8 apps incl fault dump + bench |
| `xmc4800-relax` | XMC4800 / M4F | P5.9 (LED1) | USIC0 ASC, P1.5/P1.4, 115200 -> VCOM; + RTT | onboard J-Link | [x] LED + VCOM console |
| `f411disco` | STM32F411 / M4F | PD12 (LD4 grn) | USART2, PA2/PA3, 115200 (ext adapter) | onboard ST-Link (`st-flash`) | [x] selftest 14/14 + all apps + fault dump + bench + LED |
| `blackpill` | STM32F411 / M4F | PC13 (active-low) | USART2, PA2/PA3, 115200 (ext adapter) | USB-DFU / SWD | [x] selftest 14/14 + bench (2nd F411; 25 MHz HSE) |
| `f302nucleo` | STM32F302R8 / M4 | PB13 (LD2 grn) | USART2, PA2/PA3, 115200 -> ST-Link VCP | onboard ST-Link (`st-flash`) | [x] selftest 13/14 (test 11 = 4 K alloc > 16 K RAM) + bench |
| `picopi` | RP2040 / M0+ | GP25 | UART0, GP0/GP1, 115200 | `picotool` (BOOTSEL) | [x] LED + UART0 + selftest |
| `bluepill` | STM32F103 / M3 (10 K clone) | PC13 (active-low) | USART1, PA9/PA10, 115200 (ext adapter) | external ST-Link (SWD) | [x] selftest 13/14 (test 11 = 4 K alloc > 10 K RAM) + hello; stress/bench don't fit 10 K |
| `bluepill-c8` | STM32F103C8 / M3 (20 K genuine) | PC13 (active-low) | USART1, PA9/PA10, 115200 | external ST-Link (SWD) | (!) build-only (20 K linker) |
| `frdmk64f` | MK64FN1M0 / M4F | -- (none) | UART0, PTB16/PTB17, 115200 -> OpenSDA VCOM | J-Link (OpenSDA) | [x] HW 2026-07-15 (full selftest over the buffered console ring, 120 MHz); SYSMPU sign-off = M2 |

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

- **`bluepill`** -- HW-tested 2026-07-14: selftest 13/14 (test 11's `kos_ram_alloc(4096)`
  can't fit the 10 K arena -- a genuine RAM limit, not a bug) + `hello`. `stress`/`bench`
  don't fit 10 K. Console needs an external adapter (no on-board USB-serial), on USART1
  PA9. Runs notably slow -- consistent with the clone's HSE not locking (HSI-fallback),
  though not confirmed via a clock read (bench doesn't fit to report it). No NRST on the
  SWD header: wire NRST->the `R` side pin for `--connect-under-reset`, else recover a
  `WFI`-locked board via BOOT0=1.
- **`bluepill-c8`** -- build-only (genuine 20 K variant of the HW-validated F103; only the
  10 K `bluepill` clone was physically run).
- **`due`** -- **retired** (see the table note above): SAM3X port proven 2026-07-09, but
  this unit now has a peripheral-I/O fault.
- **`frdmk64f`** -- **HW-revalidated 2026-07-15** (OpenSDA J-Link): full selftest streamed
  in-order over the buffered console ring; bench re-confirmed on silicon at **77 cyc / 641 ns**
  per switch (=> 120 MHz PLL) and 160 cyc / 1333 ns IRQ-entry; fault-dump path verified
  (UsageFault UNDEFINSTR escalated to HardFault, on PSP). Connect-under-reset was needed only
  once, to displace unknown prior firmware -- a KickOS-flashed board reflashes with a **plain
  connect** (KickOS leaves the SWD pins PTA0/PTA3 alone). Its distinguishing feature -- the
  **SYSMPU** -- is the M2 enforcement backend, where its formal M2 sign-off lands. No diagnostic
  LED wired (FRDM RGB not mapped; `blink` isn't built for it).
- **RTT backend** -- generic and wired on the XMC (`KICKOS_CONSOLE=both`); the
  UART VCOM path is the one confirmed on hardware.
- The diagnostic LED is a kernel-owned facility (`kdiag_led_*`); a chip with no
  known LED (`qemu`, `microbit`, `frdmk64f`) links the weak no-op and the LED
  silently does nothing -- not a failure.

## Flashing

Every non-sim build emits `<app>`, `<app>.hex`, `<app>.bin` next to the app ELF.
`blink` is the no-UART smoke test (built for xmc/f411/f302/picopi/bluepill/due);
`hello` prints the banner + ping-pong; both under `build/<preset>/user/apps/`.

### STM32 with an onboard ST-Link -- `f411disco`, `f302nucleo`

```sh
st-flash --connect-under-reset --reset write \
  build/<preset>/user/apps/hello/hello.bin 0x08000000
```
`--connect-under-reset` is needed to re-flash a *running* board: the idle thread
sits in `WFI`, so SWD can't halt a live core (a plain `write` on a fresh/erased
chip works without it). Nucleo consoles reach the ST-Link VCP (`ttyACM*`) with no
wiring; the F411-DISCO does **not** route USART2 to its VCP -- use an external
3.3 V USB-UART adapter (TX->RX crossed, GND, no VCC).

### `bluepill` -- external ST-Link over SWD

The Blue Pill has no onboard debugger. Wire an ST-Link (a standalone V2 dongle,
or a Nucleo's ST-Link freed by pulling its **CN2** jumpers -- see below) to the
4-pin SWD header -- which carries only `3V3 / SWDIO / SWCLK / GND`, no NRST:
`SWDIO<->DIO`, `SWCLK<->CLK`, `GND<->GND` (power over USB or `3V3` -- one supply, shared
GND). A **fresh** chip needs no reset line -- SWD can halt it, so a plain:
```sh
st-flash --reset write build/bluepill/user/apps/blink/blink.bin 0x08000000
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
dfu-util -a 0 -s 0x08000000:leave -D build/blackpill/user/apps/blink/blink.bin
```
`:leave` runs the app after flashing. Or flash via SWD with an external ST-Link
(`st-flash write ... 0x08000000`) if you'd rather. LED = **PC13** (active-low).
Console = USART2 on **PA2 (TX)/PA3 (RX)**, 115200 -- wire a 3.3 V USB-UART there
(same pins as the Nucleos; no VCP on this board). It runs a **25 MHz** HSE
crystal -- the backend derives PLLM from it, so it reaches the same 84 MHz.

### `picopi` (RP2040) -- BOOTSEL + picotool

Hold **BOOTSEL** while plugging USB (mounts as mass storage / picoboot), then:
```sh
picotool load -x build/picopi/user/apps/blink/blink
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
  build/due/user/apps/blink/blink.bin
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
esptool.py -p /dev/ttyUSB1 write_flash 0x1000 build/esp32-wroom/user/apps/hello/hello.app.bin
```
On the C6 use its CH343P port (`/dev/ttyACM0`); its native USB-Serial-JTAG is
flash-capable but useless as a console (see the note under the matrix). Console =
UART0 at 115200 on both (`esp32-wroom` GP1/GP3 -> CH340; `esp32c6-wroom`
GP16/GP17 -> CH343P). LED: `esp32-wroom` GP2 (D2), `esp32c6-wroom` WS2812B on GP8.

### `xmc4800-relax` and `frdmk64f` -- J-Link

Both use SEGGER J-Link (XMC = onboard J-Link-OB; K64F = OpenSDA reflashed with
J-Link firmware). Full `JLinkExe` / GDB / RTT recipes are in
[flashing.md](../flashing.md).
