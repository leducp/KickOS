<!-- SPDX-License-Identifier: CECILL-C -->
# Flashing KickOS to hardware

> **This page holds every flash recipe.** For the board matrix -- per-board LED / console /
> flash tool and what is HW-validated vs build-only -- and for the silicon record, see
> [boards.md](reference/boards.md). Validation status is never claimed here.

Every non-sim build emits three images next to the app ELF (via
`kickos_emit_image`, see `cmake/kickos.cmake`):

- `<app>` -- the ELF (addresses + symbols; best for a debugger).
- `<app>.hex` -- Intel HEX (addresses embedded).
- `<app>.bin` -- raw binary (no addresses; you supply the load address).

The build tree mirrors `user/apps/`: a fleet-wide app emits under
`build/<board>/user/apps/common/<app>/`, a board-specific one under
`build/<board>/user/apps/<board>/<app>/` -- e.g.
`build/xmc4800-relax/user/apps/common/blink/blink.{elf-less name,hex,bin}` and
`build/xmc4800-relax/user/apps/xmc4800-relax/xmcspi/xmcspi.hex`. A board's own demos
(`xmcspi`, `k64dspi`, `rxdrv`, `c6blink`, `f411spi`, ...) are the second form.
Espressif boards additionally emit `<app>.app.bin` (the bootable image).

`blink` is the no-UART smoke test (built for xmc/f411/f302/picopi/bluepill-c8/due);
`hello` prints the banner + ping-pong.

## One command: `tools/flash.sh`

For the common case, don't hand-craft the tool invocation -- let the dispatcher do it:

```sh
tools/flash.sh <board> [app]      # app defaults to hello
tools/flash.sh --list             # every board + the backend it would use
```

It resolves the board -> chip (from `boards/<board>/board.cmake`), finds the emitted
image, then runs the **first suitable backend whose tool is on your `PATH`** -- the
only assumption is that the tool is on `PATH` (no hardcoded install locations):

| chip | backend(s), priority order | image / notes |
|---|---|---|
| `esp32c6` / `esp32` | `esptool` | `<app>.app.bin` @ `0x0` (C-series) / `0x1000` (esp32); port auto-detected |
| `stm32f1/f3/f4` | `stlink` -> `jlink` | `<app>.bin` @ `0x08000000` (stlink) |
| `rp2040` / `rp2350` | `picotool` (hold BOOTSEL) | ELF |
| `nrf51` | `pyocd` -> `jlink` | `<app>.hex` |
| `sam3x8e` | `bossac` | `<app>.bin`; double-tap RESET for SAM-BA |
| `mk64f` | `jlink` -> `pyocd` | `.hex` |
| `imxrt1062` | `teensy` (`teensy_loader_cli`) | `.hex`; tap the button for HalfKay |
| `xmc4800` | `jlink` | `.hex` (addresses embedded) |
| `rx72m` | `rfp` (`rfp-cli` + E2 Lite) | `.hex` (carries the reset vector + option memory) |
| `mps2` / `virt` / sim | -- | not flashed; run in QEMU/host (`ctest --preset <board>`) |

Each backend is a standalone script you can also run directly -- same
`<board> [app]` interface, sharing `tools/flash-common.sh`:

```sh
tools/flash-jlink.sh   bluepill-c8     # or flash-esptool.sh / flash-stlink.sh / ...
```

Knobs (honored by the dispatcher and every backend):

- `FLASH_TOOL=esptool|stlink|jlink|picotool|pyocd|bossac|rfp|teensy` -- **force a backend** when
  a chip has several (e.g. use your own J-Link on a Blue Pill instead of an ST-Link:
  `FLASH_TOOL=jlink tools/flash.sh bluepill-c8`).
- `FLASH_PORT=/dev/ttyACMx` -- force the serial port (else first `ttyACM*`/`ttyUSB*`).
- `DRY_RUN=1` -- print the command without running it.
- `FLASH_BUILD=<dir>` -- override the build dir (default `build/<board>`).
- `FLASH_IMAGE=<path>` -- name an image outright, skipping resolution entirely.

**The resolver cannot find a SECOND target built in a shared source dir.** `flash_resolve`
(`tools/flash-common.sh`) looks for `<app>/<app>.hex` -- the directory and the image must share a
name. A `CMakeLists.txt` that adds two executables from one directory breaks that: `inprstormmax.hex`
is emitted into `inprstorm/`, so `tools/flash.sh xmc4800-relax inprstormmax` fails to resolve it. Use
`FLASH_IMAGE=<path> tools/flash.sh <board>` for those, or split the app directories. This is in
`flash_resolve`, so it applies to **every board and every backend**, not just the J-Link ones.

**Build-dir hazard: the sim presets put their binary dir INSIDE the source tree.**
`cmake/presets/host.json` sets `"binaryDir": "${sourceDir}/build/sim"` for the `sim` preset (and
`build/sim-telem` for `sim-telem`). Two source trees configured with the preset and no explicit `-B`
therefore collide on the same path, and a stale in-tree `build/sim` has been observed doing exactly
that. Pass `-B <dir>` (or point `FLASH_BUILD` at the one you mean, for the board presets) when more
than one tree is in play.

Example -- flash the C6 over its native USB with the esp-idf env active:

```sh
. $IDF_PATH/export.sh                       # puts esptool on PATH
cmake --build build/esp32c6-wroom --target hello
tools/flash.sh esp32c6-wroom                # -> flash-esptool.sh: esptool --chip esp32c6 ...
```

## Per-board recipes

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

**One image per BOOTSEL state, and no SWD escape** -- unless the image hands itself back. Once an
ordinary KickOS image runs the board is no longer a boot device, so the next `picotool load` needs a
physical BOOT press. `flash-jlink.sh` carries an `RP2350_M33_0` row, but on this bench it does not
help: the J-Link Pro (SN `000177003338`) reports `VTref=0.000V` / `ITarget=0mA` against this board,
i.e. the probe is not wired to the Pi-Zero's SWD pads at all. **SWD remains unavailable here**, so
BOOTSEL is the only channel; budget captures accordingly, or wire SWD first.

`kos_reboot()` at the end of `main` closes that loop for an app that reaches it, and
`-DKICKOS_SHUTDOWN_TO_BOOTLOADER=ON` extends it to any image, including one that ends in a fault:
the knob (default OFF, requires `KICKOS_ENABLE_SELFTEST`) puts the handover in the kernel's two
terminal dead-ends, `kickos_terminate` (shutdown syscall, last-thread-out, `kickos_isr_fault`) and
`kfault_terminate` (`kpanic`, and every ARM MemManage/HardFault; the handover sits in its
fallback body, `arch/common/kfault_terminate_default.cc`). Both drain the console
first. **Leave it OFF for a bench or soak image**, which must stay resident. The silicon witness for
both dead-ends is in `reference/boards.md`.

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
flash-capable but useless as a console (reason in `reference/boards.md`, under the status matrix).
Console = UART0 at 115200 on both (`esp32-wroom` GP1/GP3 -> CH340;
`esp32c6-wroom` GP16/GP17 -> CH343P). LED: `esp32-wroom` GP2 (D2), `esp32c6-wroom` WS2812B on GP8.

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
J-Link firmware). The full `JLinkExe` / GDB / RTT recipes are the sections below.

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

## XMC4800 Relax Kit -- onboard SEGGER J-Link

The Relax Kit carries a genuine **SEGGER J-Link-OB**, so the same USB that powers
the board flashes and debugs it (and exposes a `ttyACM0` VCOM). Install SEGGER's
*J-Link Software and Documentation Pack* (`.deb` from segger.com) -- it provides
`JLinkExe`, `JLinkGDBServer`, `JLinkRTTClient`, and the udev rules (no `sudo`).

The XMC4800-2048's flash is at **`0x08000000`** (cached alias; that is the link
base -- see `arch/arm/chip/xmc4800/xmc4800.ld`).

### Flash the raw `.bin`

```sh
cat > /tmp/xmc.jlink <<'EOF'
loadbin build/xmc4800-relax/user/apps/common/blink/blink.bin 0x08000000
r
g
qc
EOF
JLinkExe -device XMC4800-2048 -if SWD -speed 4000 -autoconnect 1 -CommanderScript /tmp/xmc.jlink
```

`loadbin` needs the address because a `.bin` carries none; `0x08000000` is the
flash origin. (`.hex` carries its own addresses: `loadfile <app>.hex` instead,
no address.) `r` resets, `g` runs, `qc` quits. LED1 (P5.9) should blink.

### Interactive equivalent

```sh
JLinkExe -device XMC4800-2048 -if SWD -speed 4000 -autoconnect 1
J-Link> loadbin build/xmc4800-relax/user/apps/common/blink/blink.bin 0x08000000
J-Link> r
J-Link> g
J-Link> q
```

### Debug with GDB

```sh
JLinkGDBServer -device XMC4800-2048 -if SWD -speed 4000      # terminal 1
arm-none-eabi-gdb build/xmc4800-relax/user/apps/common/blink/blink \      # terminal 2
  -ex 'target remote :2331' -ex load -ex 'monitor reset' -ex continue
```

### Console

The `ttyACM0` VCOM is driven by USIC0 in ASC (UART) mode on P1.5/P1.4 at 115200 --
open it for the KickOS banner + TAP. `arch_console_write` is buffered (the console
ring drains via the USIC TX interrupt); panics/faults use the bounded polled
`arch_console_write_sync`. The XMC also defaults to `KICKOS_CONSOLE=both`, so a
**SEGGER RTT** backend runs alongside the VCOM, giving a UART-free console over the
debug probe -- `JLinkRTTClient` (or `JLinkRTTViewer`) shows the same banner + TAP
with no wiring at all. See `arch/arm/chip/xmc4800/` (incl. `usic_uart.cc`) and
`lib/rtt.*`.

## Other J-Link targets

Any board with a J-Link works the same -- only the `-device` string changes:

| Board | `-device` | Notes |
|-------|-----------|-------|
| XMC4800 Relax | `XMC4800-2048` | onboard J-Link-OB; flash `0x08000000` |
| FRDM-K64F | `MK64FN1M0xxx12` | OpenSDA reflashed with SEGGER's J-Link-OpenSDA firmware; flash `0x00000000`. Its VCOM (UART0, PTB16/PTB17) *is* driven -- open at 115200 for the banner + TAP. |

**Helper:** `tools/flash-jlink.sh <board> [app]` (app defaults to `hello`) wraps the
`JLinkExe` dance: it maps the board to the right `-device` string and `loadfile`s the
emitted `.hex` (whose addresses are embedded, so no load base is needed -- the same
`.hex` works regardless of the per-board flash origin). It finds the app under
`build/<board>/user/apps/<board>/<app>/` or `build/<board>/user/apps/common/<app>/`. The
`FLASH_BUILD` / `FLASH_IMAGE` overrides and the resolver's one-image-per-directory limitation live
in `flash_resolve` and apply to every backend -- see *One command* above.

```sh
tools/flash-jlink.sh frdmk64f              # build/frdmk64f/.../hello.hex  -> MK64FN1M0xxx12
tools/flash-jlink.sh xmc4800-relax blink   # build/xmc4800-relax/.../blink.hex -> XMC4800-2048
```

For **RTT** (over the probe, needs a `KICKOS_CONSOLE=rtt|both` image), the loop is
sequential -- one probe, one connection at a time. The K64F defaults to `chip`, so
build it with RTT enabled first (the XMC4800 already defaults to `both`):

**Console (channel 0):**
```sh
cmake --preset frdmk64f -DKICKOS_CONSOLE=both   # RTT console into build/frdmk64f
cmake --build build/frdmk64f --target hello
tools/flash-jlink.sh frdmk64f hello  # JLinkExe flashes build/frdmk64f/.../hello.hex + quits
tools/rtt-server.sh                  # k64f is the default; JLinkExe hosts RTT :19021 (core running)
JLinkRTTClient                       # another terminal: channel 0 console
```

**Telemetry (channel 1, binary).** The binary trace is channel 1; pull it with
`JLinkRTTLogger` (its own connection -- *not* alongside `rtt-server.sh`). Needs a
`KICKOS_TELEMETRY=rtt` image.

```sh
# record to a file, then decode (the script prints the exact --clock-hz command):
tools/telemetry-record.sh trace.bin            # k64f (default); Ctrl-C to stop
python3 tools/kicktrace.py trace.bin --summary --clock-hz 120000000  # CPU%/latency in ns

# or LIVE per-record scroll via a fifo:
mkfifo /tmp/rtt1
JLinkRTTLogger -device MK64FN1M0xxx12 -if SWD -speed 4000 -RTTChannel 1 /tmp/rtt1 &
python3 tools/kicktrace.py --follow /tmp/rtt1
```

`--clock-hz` is the trace-clock (DWT/core = `SystemCoreClock`): **K64F 120000000**
(PLL; 20971520 only if the PLL bring-up is skipped), **XMC4800 120000000**. Without
it, ns needs a SESSION anchor in the capture (only emitted at boot/shutdown); a
mid-run capture reports raw ticks.

> udev: SEGGER's pack installs J-Link rules. For the VCOM, add yourself to the
> `dialout` group to read `ttyACM*`.
