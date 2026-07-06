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
