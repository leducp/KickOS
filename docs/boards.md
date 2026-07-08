<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS board support

Status of every board target: what works, what only builds, how to flash it, and
where its console + LED live. For J-Link / RTT details see [flashing.md](flashing.md).

## Status matrix

| Board (preset) | SoC / core | LED (blink) | Console | Flash tool | HW-validated |
|---|---|---|---|---|---|
| `sim` | host process | — | host stdout | `ctest --preset sim` | ✅ CI |
| `qemu` | mps2-an386 / M4 | — | semihosting | `ctest --preset qemu` | ✅ CI |
| `microbit` | nRF51822 / M0 | — | semihosting | `ctest --preset microbit` | ✅ CI |
| `qemu-riscv` | QEMU virt / RV32IMAC | — | semihosting | `ctest --preset qemu-riscv` | ✅ CI (first RISC-V) |
| `esp32c6-wroom` | ESP32-C6-WROOM-1 / RV32IMAC | (WROOM, TBD) | UART0 (ROM), 115200 | esptool (TODO) | ⚠️ build-only |
| `xmc4800-relax` | XMC4800 / M4F | P5.9 (LED1) | USIC0 ASC, P1.5/P1.4, 115200 → VCOM; + RTT | onboard J-Link | ✅ LED + VCOM console |
| `f411disco` | STM32F411 / M4F | PD12 (LD4 grn) | USART2, PA2/PA3, 115200 (ext adapter) | onboard ST-Link (`st-flash`) | ✅ LED + UART + ping-pong |
| `blackpill` | STM32F411 / M4F | PC13 (active-low) | USART2, PA2/PA3, 115200 (ext adapter) | USB-DFU / SWD | ⚠️ build-only (2nd F411 board; 25 MHz HSE) |
| `f302nucleo` | STM32F302R8 / M4 | PB13 (LD2 grn) | USART2, PA2/PA3, 115200 → ST-Link VCP | onboard ST-Link (`st-flash`) | ✅ LED + console |
| `picopi` | RP2040 / M0+ | GP25 | UART0, GP0/GP1, 115200 | `picotool` (BOOTSEL) | ✅ LED; ⚠️ UART untested |
| `bluepill` | STM32F103 / M3 (10 K clone) | PC13 (active-low) | USART1, PA9/PA10, 115200 (ext adapter) | external ST-Link (SWD) | ✅ LED + UART (10 K LD part) |
| `bluepill-c8` | STM32F103C8 / M3 (20 K genuine) | PC13 (active-low) | USART1, PA9/PA10, 115200 | external ST-Link (SWD) | ⚠️ build-only (20 K linker) |
| `due` | AT91SAM3X8E / M3 | PB27 ("L" amber) | UART, PA8/PA9, 115200 → prog-port VCOM | `bossac` | 🧪 experimental (LED only; console dead) |
| `frdmk64f` | MK64FN1M0 / M4F | — (none) | UART0, PTB16/PTB17, 115200 → OpenSDA VCOM | J-Link (OpenSDA) | ⚠️ build-only |

Console pins are TX/RX in that order. STM32/XMC flash base is `0x08000000`; K64F is
`0x00000000`; the Due's `bossac` handles the offset itself.

### Built but NOT hardware-tested (know before you trust it)

- **`picopi` UART** (GP0/GP1) — the driver builds and the clocks are set, but it
  has never been exercised (no adapter was on hand); only the LED is confirmed.
- **`bluepill`** — LED confirmed on hardware; console (needs an external adapter,
  no on-board USB-serial) not yet tested. Target is sized to the **10 KiB / 32 KiB
  low-density floor** since many boards are LD parts/clones (see stm32f103.ld). No
  NRST on the SWD header: wire NRST→the `R` side pin for `--connect-under-reset`,
  else recover a `WFI`-locked board via BOOT0=1.
- **`due` (EXPERIMENTAL)** — builds + flashes, but the clock bring-up is marginal
  on hardware: `blink` runs *intermittently* and the UART console is dead (MCK
  almost certainly isn't the 84 MHz the baud assumes). Needs a scope on PA9 / SWD
  to resolve, not blind edits. Treat as unsupported until then.
- **`frdmk64f`** — entirely build-only; also still runs at the reset FEI clock
  (~20.97 MHz, no PLL yet), and has **no diagnostic LED** wired (the FRDM RGB LED
  isn't mapped — `arch_diag_led_*` falls back to the weak no-op, so `blink` does
  nothing there; `blink` isn't built for it).
- **RTT backend** — generic and wired on the XMC (`KICKOS_CONSOLE=both`); the
  UART VCOM path is the one confirmed on hardware.
- The diagnostic LED is a kernel-owned facility (`kdiag_led_*`); a chip with no
  known LED (`qemu`, `microbit`, `frdmk64f`) links the weak no-op and the LED
  silently does nothing — not a failure.

## Flashing

Every non-sim build emits `<app>`, `<app>.hex`, `<app>.bin` next to the app ELF.
`blink` is the no-UART smoke test (built for xmc/f411/f302/picopi/bluepill/due);
`hello` prints the banner + ping-pong; both under `build/<preset>/user/apps/`.

### STM32 with an onboard ST-Link — `f411disco`, `f302nucleo`

```sh
st-flash --connect-under-reset --reset write \
  build/<preset>/user/apps/hello/hello.bin 0x08000000
```
`--connect-under-reset` is needed to re-flash a *running* board: the idle thread
sits in `WFI`, so SWD can't halt a live core (a plain `write` on a fresh/erased
chip works without it). Nucleo consoles reach the ST-Link VCP (`ttyACM*`) with no
wiring; the F411-DISCO does **not** route USART2 to its VCP — use an external
3.3 V USB-UART adapter (TX→RX crossed, GND, no VCC).

### `bluepill` — external ST-Link over SWD

The Blue Pill has no onboard debugger. Wire an ST-Link (a standalone V2 dongle,
or a Nucleo's ST-Link freed by pulling its **CN2** jumpers — see below) to the
4-pin SWD header — which carries only `3V3 / SWDIO / SWCLK / GND`, no NRST:
`SWDIO↔DIO`, `SWCLK↔CLK`, `GND↔GND` (power over USB or `3V3` — one supply, shared
GND). A **fresh** chip needs no reset line — SWD can halt it, so a plain:
```sh
st-flash --reset write build/bluepill/user/apps/blink/blink.bin 0x08000000
```
works (`--reset` here is a software SYSRESETREQ over SWD). Only *re-flashing a
board already running KickOS* needs `--connect-under-reset` (the idle thread
sleeps in `WFI`, so SWD can't halt the live core) — and that needs a physical
NRST wire. The Blue Pill's reset isn't on the SWD header: it's the **`R` pin** on
the long side header (or the reset button pad). LED = **PC13, active-low**.

**Using a Nucleo as the programmer:** pull **both CN2 (ST-LINK) jumpers** on the
Nucleo to detach its onboard target, then drive the Blue Pill from the Nucleo's
**CN4** SWD header — `CN4.2=SWCLK`, `CN4.3=GND`, `CN4.4=SWDIO` (and `CN4.5=NRST`
→ the Blue Pill's `R` pin only if you need connect-under-reset later). Power the
Blue Pill from its own USB; restore the CN2 jumpers when done.

### `blackpill` (WeAct STM32F411) — USB-DFU (no ST-Link)

The Black Pill has no on-board debugger; flash over USB with its ROM DFU
bootloader. Hold **BOOT0**, tap **NRST**, release BOOT0 (it enumerates as DFU),
then:
```sh
dfu-util -a 0 -s 0x08000000:leave -D build/blackpill/user/apps/blink/blink.bin
```
`:leave` runs the app after flashing. Or flash via SWD with an external ST-Link
(`st-flash write ... 0x08000000`) if you'd rather. LED = **PC13** (active-low).
Console = USART2 on **PA2 (TX)/PA3 (RX)**, 115200 — wire a 3.3 V USB-UART there
(same pins as the Nucleos; no VCP on this board). It runs a **25 MHz** HSE
crystal — the backend derives PLLM from it, so it reaches the same 84 MHz.

### `picopi` (RP2040) — BOOTSEL + picotool

Hold **BOOTSEL** while plugging USB (mounts as mass storage / picoboot), then:
```sh
picotool load -x build/picopi/user/apps/blink/blink
```
`-x` runs it after loading. UART is GP0(TX)/GP1(RX) 115200 — needs a 3.3 V
adapter (untested so far). LED is GP25 (not the Pico W, whose LED is on the CYW43).

### `due` (SAM3X8E) — bossac over the Programming port

Use the **Programming port** (the micro-USB next to the DC jack; enumerates as
Arduino PID `003d`). BOSSA needs the SAM3X in its SAM-BA ROM bootloader:
```sh
bossac -p ttyACMx --usb-port=0 -a -e -w -v -b -R \
  build/due/user/apps/blink/blink.bin
```
`-a` = the 1200-baud erase/reset hack, `--usb-port=0` = RS-232 (the prog port is
UART-bridged via the 16U2), `-b` = set boot-from-flash, `-R` = reset. If it says
"No device found", do it by hand: **hold ERASE ~2 s, tap RESET**, rerun without
`-a`; failing that, use the **Native port** (no `--usb-port=0`). LED = PB27 ("L"
amber). Its programming port *is* the console UART (PA8/PA9), so `picocom -b
115200 /dev/ttyACMx` shows the console on the same cable.

### `xmc4800-relax` and `frdmk64f` — J-Link

Both use SEGGER J-Link (XMC = onboard J-Link-OB; K64F = OpenSDA reflashed with
J-Link firmware). Full `JLinkExe` / GDB / RTT recipes are in
[flashing.md](flashing.md).
