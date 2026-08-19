<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The bench chain

How a silicon capture is taken, and what a capture is allowed to claim. The scripts are
`tools/bench/`; the values that describe one particular rig are not in the repo at all.

## The chain

| script | job |
| --- | --- |
| `tools/bench/bench-fleet.sh` | enumerates the bus, resolves each probe serial LIVE, runs every board and every service list it owes, then states coverage |
| `tools/bench/bench.sh` | ONE board: configure, build, locate the image, then hand off -- locally, or over ssh to the bench host |
| `tools/bench/bench-capture.sh` | ONE board, ONE built image: flash, capture, validate. THIS is the script that runs where the hardware is |
| `tools/bench/cap_esp.py` | the Espressif capture: reset-into-run and read on ONE serial handle |
| `tools/bench/rig.sh` | finds and reads the rig config; refuses by name when a required value is absent |

`bench.sh` never flashes. `bench-capture.sh` never builds and knows nothing about ssh.
That split is what makes a remote pass honest: every refusal fires where the hardware is
and travels back as output plus an exit code, so a remote failure cannot read as a local
success.

## What is tracked, and what is not

TRACKED, because it is what the project knows about its own boards:

- the flash-and-capture ORDER per board class. Arm-after-flash on a J-Link, arm-first on
  an ST-Link, one handle for an Espressif part, no separate reset on the f302nucleo.
- every refusal: 0-byte log, held port, dead reader, vanished probe, a write that failed,
  no SWD speed that identifies the core, an app that owes a TAP plan and announced none.
- the TAP validation: the LAST plan line to end of file is the authoritative run, counts
  are never summed across plan lines, and the banner must keep its `-dirty`.
- the service-list coverage derivation and its refusal (below).

NOT TRACKED, because it describes one rig and would be a lie in any other checkout: which
physical cable is on which board, this box's absolute paths, the bench host and its port,
and where a python carrying pyserial lives. Those come from a gitignored config.

## The rig config

`.session/rig.conf`, or wherever `KICKOS_RIG` points. `tools/bench/rig.conf.example` is
the committed template -- copy it and fill in your own values.

    cp tools/bench/rig.conf.example .session/rig.conf

A git worktree has no `.session/` of its own, so from one you must pass `KICKOS_RIG`
naming the main checkout's copy.

| key | required | what it names |
| --- | --- | --- |
| `RIG_SESSION` | always | the directory holding `env.sh` and receiving the logs |
| `RIG_TREE` | always | the tree built when the caller sets no `TREE` |
| `RIG_PYBIN` | Espressif, local | a python carrying pyserial; empty falls back to `$PY` from `env.sh` |
| `RIG_CONSOLE_<BOARD>` | see below | the console cable, as a by-id path or glob |
| `RIG_BENCH_PORT` | no | ssh port default when `BENCH_PORT` is unset |
| `RIG_REMOTE_ROOT` | remote mode | the bench host directory holding the shipped tree and the run outputs |
| `RIG_REMOTE_PYBIN` | Espressif, remote | a python carrying pyserial ON the bench host, absolute |

`BENCH_HOST` in the environment is what SELECTS remote mode, and it is deliberately not a
config key: the host a board is plugged into is a question for an enumeration, not a
recollection.

### Consoles, and why one guess is forbidden

A console is resolved by SERIAL, never by a `ttyACM`/`ttyUSB` number -- flashing
re-enumerates a probe, so a number resolved before the flash can name a different device
after it.

Where the instance comes from depends on the board:

- a **J-Link VCOM** is derived from the probe serial the caller already resolved live, so
  it needs no config.
- an **ST-Link or CH34x** has a by-id prefix that names the probe on its own, so the
  script carries the pattern.
- an **FTDI console** (`rx72m`, `picopi`, `pizero2350`, `teensy41`) has NO tracked pattern and refuses
  by name until `RIG_CONSOLE_<BOARD>` says which cable. A bench carries other people's
  FT232s and CP210x cables, so a vendor-keyed glob resolves to whichever enumerated first,
  and the capture that follows is complete, plausible, and the wrong board.

Any row may be pinned in the config, and a pin always wins -- do that the day a second
ST-Link or a second CH34x joins the bus. A pattern matching MORE than one device is
refused rather than resolved by taking the first.

The key is the board name uppercased with dashes turned into underscores:
`esp32c6-wroom` becomes `RIG_CONSOLE_ESP32C6_WROOM`.

## Service-list coverage

A driver is only in the image if `KICKOS_SERVICE_LIST` puts it there, so a green run of a
board's DEFAULT list says nothing about that board's drivers. `bench-fleet.sh` derives the
lists a board owes from the tree -- every `kickos_services_<board-ish>[_variant]` provider
declared in a `CMakeLists.txt` -- so a provider added tomorrow is owed tomorrow, and prints
a coverage table naming each one as `captured` or `NOT RUN`. Any `NOT RUN` makes the pass
`INCOMPLETE` and the script exits nonzero.

Each list gets its own TAG, because TAG keys the log and two captures of one app at one TAG
overwrite each other.

## What a capture may claim

- the banner carries the commit, and `-dirty` if the tree had uncommitted edits. A witness
  taken from a dirty tree that does not say so is unfalsifiable.
- a witness belongs to a TREE, not to a run: never re-message or rebase past a capture and
  keep calling it evidence.
- a two-image board (`f302nucleo`, `bluepill-c8`) restarts TAP numbering at 1 in each image,
  so a lone first plan line is HALF a run, not a short one.
- a board absent from the bus is REPORTED as absent. It is never silently skipped, and an
  absent board is not a pass.
