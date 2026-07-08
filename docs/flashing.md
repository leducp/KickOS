<!-- SPDX-License-Identifier: CECILL-C -->
# Flashing KickOS to hardware

> For the full board matrix (per-board LED / console / flash tool / what's
> HW-validated vs build-only) and the non-J-Link flash paths (ST-Link, picotool,
> bossac), see [boards.md](boards.md). This page is the J-Link / RTT deep-dive.

Every non-sim build emits three images next to the app ELF (via
`kickos_emit_image`, see `cmake/kickos.cmake`):

- `<app>` — the ELF (addresses + symbols; best for a debugger).
- `<app>.hex` — Intel HEX (addresses embedded).
- `<app>.bin` — raw binary (no addresses; you supply the load address).

e.g. `build/xmc4800-relax/user/apps/blink/blink.{elf-less name,hex,bin}`.
Espressif boards additionally emit `<app>.app.bin` (the bootable image).

## One command: `tools/flash.sh`

For the common case, don't hand-craft the tool invocation — let the dispatcher do it:

```sh
tools/flash.sh <board> [app]      # app defaults to hello
tools/flash.sh --list             # every board + the backend it would use
```

It resolves the board → chip (from `boards/<board>/board.cmake`), finds the emitted
image, then runs the **first suitable backend whose tool is on your `PATH`** — the
only assumption is that the tool is on `PATH` (no hardcoded install locations):

| chip | backend(s), priority order | image / notes |
|---|---|---|
| `esp32c6` / `esp32` | `esptool` | `<app>.app.bin` @ `0x0` (C-series) / `0x1000` (esp32); port auto-detected |
| `stm32f1/f3/f4` | `stlink` → `jlink` | `<app>.bin` @ `0x08000000` (stlink) |
| `rp2040` | `picotool` (hold BOOTSEL) | ELF |
| `nrf51` | `pyocd` → `jlink` | `<app>.hex` |
| `sam3x8e` | `bossac` | `<app>.bin`; double-tap RESET for SAM-BA |
| `mk64f` | `jlink` → `pyocd` | `.hex` |
| `xmc4800` / `rx72m` | `jlink` | `.hex` (addresses embedded) |
| `mps2` / `virt` / sim | — | not flashed; run in QEMU/host (`ctest --preset <board>`) |

Each backend is a standalone script you can also run directly — same
`<board> [app]` interface, sharing `tools/flash-common.sh`:

```sh
tools/flash-jlink.sh   bluepill        # or flash-esptool.sh / flash-stlink.sh / …
```

Knobs (honored by the dispatcher and every backend):

- `FLASH_TOOL=esptool|stlink|jlink|picotool|pyocd|bossac` — **force a backend** when
  a chip has several (e.g. use your own J-Link on a Blue Pill instead of an ST-Link:
  `FLASH_TOOL=jlink tools/flash.sh bluepill`).
- `FLASH_PORT=/dev/ttyACMx` — force the serial port (else first `ttyACM*`/`ttyUSB*`).
- `DRY_RUN=1` — print the command without running it.

Example — flash the C6 over its native USB with the esp-idf env active:

```sh
. $IDF_PATH/export.sh                       # puts esptool on PATH
cmake --build build/esp32c6-wroom --target hello
tools/flash.sh esp32c6-wroom                # -> flash-esptool.sh: esptool --chip esp32c6 …
```

The sections below are the per-tool deep-dives.

## XMC4800 Relax Kit — onboard SEGGER J-Link

The Relax Kit carries a genuine **SEGGER J-Link-OB**, so the same USB that powers
the board flashes and debugs it (and exposes a `ttyACM0` VCOM). Install SEGGER's
*J-Link Software and Documentation Pack* (`.deb` from segger.com) — it provides
`JLinkExe`, `JLinkGDBServer`, `JLinkRTTClient`, and the udev rules (no `sudo`).

The XMC4800-2048's flash is at **`0x08000000`** (cached alias; that is the link
base — see `arch/arm/chip/xmc4800/xmc4800.ld`).

### Flash the raw `.bin`

```sh
cat > /tmp/xmc.jlink <<'EOF'
loadbin build/xmc4800-relax/user/apps/blink/blink.bin 0x08000000
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
J-Link> loadbin build/xmc4800-relax/user/apps/blink/blink.bin 0x08000000
J-Link> r
J-Link> g
J-Link> q
```

### Debug with GDB

```sh
JLinkGDBServer -device XMC4800-2048 -if SWD -speed 4000      # terminal 1
arm-none-eabi-gdb build/xmc4800-relax/user/apps/blink/blink \      # terminal 2
  -ex 'target remote :2331' -ex load -ex 'monitor reset' -ex continue
```

### Console

The `ttyACM0` VCOM is wired to an XMC USIC UART; a USIC console driver is being
brought up (until then `arch_console_write` is a stub and the VCOM is silent).
Independently, a **SEGGER RTT** backend gives a UART-free console over the debug
probe — `JLinkRTTClient` (or `JLinkRTTViewer`) shows the KickOS banner + TAP with
no wiring at all. See `arch/arm/chip/xmc4800/` and `lib/rtt.*`.

## Other J-Link targets

Any board with a J-Link works the same — only the `-device` string changes:

| Board | `-device` | Notes |
|-------|-----------|-------|
| XMC4800 Relax | `XMC4800-2048` | onboard J-Link-OB; flash `0x08000000` |
| FRDM-K64F | `MK64FN1M0xxx12` | OpenSDA reflashed with SEGGER's J-Link-OpenSDA firmware; flash `0x00000000`. Its VCOM (UART0, PTB16/PTB17) *is* driven — open at 115200 for the banner + TAP. |

**Helper:** `tools/flash-jlink.sh <image> [board]` wraps the `JLinkExe` dance and
picks the right device + flash base per board (so a raw `.bin` lands at `0x00000000`
on K64F but `0x08000000` on XMC). Pass the linked ELF or `.hex` and the address is
taken from the file (no base needed); pass a `.bin` and the per-board base is used.

```sh
tools/flash-jlink.sh build/frdmk64f-rtt/user/apps/hello/hello.bin        # k64f (default)
tools/flash-jlink.sh build/xmc4800-relax/user/apps/blink/blink   xmc4800 # ELF -> loadfile
```

For **RTT** (over the probe, needs a `KICKOS_CONSOLE=rtt|both` image), the loop is
sequential — one probe, one connection at a time.

**Console (channel 0):**
```sh
tools/flash-jlink.sh build/frdmk64f-rtt/user/apps/hello/hello.bin  # JLinkExe flashes + quits
tools/rtt-server.sh                                                # JLinkExe hosts RTT :19021 (core running)
JLinkRTTClient                                                     # another terminal: channel 0 console
```

**Telemetry (channel 1, binary).** The binary trace is channel 1; pull it with
`JLinkRTTLogger` (its own connection — *not* alongside `rtt-server.sh`). Needs a
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

Boards without a J-Link use their native path instead: RP2040 via
`picotool load -x <elf>` (BOOTSEL), STM32 via ST-Link + `openocd`/`st-flash`,
Arduino Due via `bossac`.

> udev: SEGGER's pack installs J-Link rules. For the VCOM, add yourself to the
> `dialout` group to read `ttyACM*`.
