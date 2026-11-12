<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The bench chain

How a silicon capture is taken, and what a capture is allowed to claim. The scripts are
`tools/bench/`; the values that describe one particular rig are not in the repo at all.

## The chain

| script | job |
| --- | --- |
| `tools/bench/bench-present.sh` | READ ONLY: which machine the boards are on, which of them answer, each probe serial and each resolved console. Flashes nothing and is safe at any time |
| `tools/bench/bench-fleet.sh` | enumerates the bus, resolves each probe serial LIVE, builds every judged image `LIST_IMAGES` names for each board, captures them in one worker per board, then states image coverage |
| `tools/bench/bench.sh` | ONE board: configure, build, locate the image, then hand off -- locally, or over ssh to the bench host |
| `tools/bench/bench-capture.sh` | ONE board, ONE built image: flash, capture, judge (the TAP stream through `check_tap_stream.sh`, a bench report through its own arms). THIS is the script that runs where the hardware is |
| `tools/bench/cap_esp.py` | the Espressif capture: reset-into-run and read on ONE serial handle |
| `tools/bench/stamp_lines.py` | stamps each line every capture route appends to the log with its host arrival time, into `<log>.times`, which `bench.sh` fetches beside the log; `wallclock`'s judge times a kernel sleep by it |
| `tools/bench/stamper.sh` | sourced: starts the stamper with no earlier `<log>.times` left, refuses a capture whose stamper is not running or failed, and truncates the log only with no stamper over it |
| `tools/bench/rig.sh` | finds and reads the rig config; refuses by name when a required value is absent |
| `tools/bench/bench-host.sh` | sourced: which machine the boards are on, how to run a command there, and THE bus enumeration |
| `tools/bench/amp_peers.sh` | sourced: the other nodes' flash text windows of an own-image AMP node's build, which a capture of that image alone erases before the load so the node runs with no peer; only the RP2 boards' picotool load erases them, and it needs picotool 2.x and whole 4 KiB sectors, so any other board's capture refuses them |
| `tools/bench/board-rows.sh` | sourced: THE per-board table, the probe row that decides presence and the console row a capture opens |

`bench.sh` never flashes. `bench-capture.sh` never builds and knows nothing about ssh.
That split is what makes a remote pass honest: every refusal fires where the hardware is
and travels back as output plus an exit code, so a remote failure cannot read as a local
success.

The two sourced files are there so the three scripts cannot disagree. One enumeration
answers every presence and serial question, local or remote; one table says what each
board puts on the bus and which device its console is. A capture opens what
`bench-present.sh` says it will open because they read the same row.

## Asking the rig

WHICH BOARDS ARE ON THE BENCH IS A COMMAND, NEVER A NOTE. A written inventory of cables,
ports and boards has a shelf life, and a stale one reads exactly like a fresh one.

    tools/bench/bench-present.sh                  every board this tree can flash
    tools/bench/bench-present.sh rx72m f411disco  named boards, nonzero if one is absent
    tools/bench/bench-present.sh reach            does the bench host answer at all
    tools/bench/bench-present.sh bus              the raw enumeration and the by-id devices
    tools/bench/bench-present.sh consoles         every console row, resolved
    tools/bench/bench-present.sh holders          which process holds each console device

It names the machine it asked and what ssh resolves that name to, because a bench reached
through an alias and a laptop with an empty bus otherwise produce the same board table.
An UNREACHABLE bench and a bench with NO BOARDS are different answers: the first refuses
and quotes the ssh error, the second is a clean report whose boards all say ABSENT. A board
this chain cannot decide, because its probe is the same USB device another board's is, says
so rather than guessing.

A board is never silently skipped. Anything the rig will not answer is a row to add to
`tools/bench/board-rows.sh`, and every refusal in the chain names the command to run rather
than a note to read: the console rows, the probe serials and the process holding a port are
all questions this answers, so none of them needs a shell on the bench host.

## What is tracked, and what is not

TRACKED, because it is what the project knows about its own boards:

- the flash-and-capture ORDER per board class. Arm-after-flash on a J-Link, arm-first on
  an ST-Link, one handle for an Espressif part, no separate reset on the f302nucleo.
- every refusal: 0-byte log, held port, dead reader, vanished probe, a write that failed,
  no SWD speed that identifies the core, an app that owes a TAP plan and announced none, a
  report that names a cycle rate and then reads zero or one constant.
- the TAP validation: the LAST plan line to end of file is the authoritative run, counts
  are never summed across plan lines, and the banner must keep its `-dirty`.
- the image coverage derivation and its refusal (below).

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
| `RIG_PROBE_<VID>_<PID>` | see below | which unit a probe `vid:pid` means, where the bus carries more than one of them. The vid:pid upper-cased with `:` as `_` |
| `RIG_BENCH_HOST` | remote mode | which machine is the bench. An ssh alias is preferred, and then no port or user belongs in the config |
| `RIG_BENCH_PORT` | no | the ssh port, only where `ssh_config` does not answer it. Empty passes no `-p` at all |
| `RIG_REMOTE_ROOT` | remote mode | the bench host directory holding the shipped tree and the run outputs |
| `RIG_REMOTE_PYBIN` | Espressif, remote | a python carrying pyserial ON the bench host, absolute |
| `RIG_WIRED_<BOARD>` | no | the bench fittings on that board, space-separated names; see below |

`BENCH_HOST` in the environment is what SELECTS remote mode. `RIG_BENCH_HOST` names the
machine but does not select it: a config key must not move a flashing run from one box to
another, so `bench.sh` and `bench-fleet.sh` say when the two disagree and
`bench-present.sh` reports both. Which board is plugged into which of them stays a
question for an enumeration and is recorded nowhere.

HOW to reach that host is `ssh_config`'s answer and not the rig's. A port is passed only
where one is configured, and there is no fallback to 22, which would override the `Port`
an alias carries and reach a different machine. A user is never synthesised either.

### Bench fittings

Some verdicts rest on hardware fitted on the bench rather than on the board: a loopback jumper, or
a device on a bus. Which of them THIS rig carries is `RIG_WIRED_<BOARD>`, the board name uppercased
with dashes turned into underscores, holding the names below. `bench.sh` hands each one to the image's judge as
an `@<fitting>` argument, and a judge evaluates a clause resting on a fitting only where an argument
names it, never from its environment.
Elsewhere the capture must still show the transfer, and the judge prints `NOT EVALUATED` for its
values, so the image is captured but partly owed; a refused open or a failed transfer fails it
either way. Unset declares nothing: a missing key never reads as a fitted wire, and a misspelt
name owes the clause rather than passing it. Fittings that occupy the same pins are exclusive,
and a rig declaring two of them is refused.

| board | fitting | what it is | the clause it carries |
| --- | --- | --- | --- |
| `f411disco` | `spi1-loopback` | a PA7-to-PA6 jumper | `f411spi`'s four echoed words and its loopback verdict |
| `frdmk64f` | `lan9252` | the EasyCAT LAN9252 shield, chip select on PTC4 | `k64dspi`'s `BYTE_TEST` signature |
| `frdmk64f` | `dspi0-loopback` | a PTD2-to-PTD3 (SOUT-to-SIN) jumper, shield off; exclusive with `lan9252` | the loopback cases of a `K64DSPI_LOOPBACK` build |

    RIG_WIRED_FRDMK64F="lan9252"

The XMC4800's SSC loopback is internal to the channel and needs no fitting.

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

A PROBE `vid:pid` MATCHING MORE THAN ONE ENUMERATED DEVICE IS THE SAME REFUSAL one level in,
and `RIG_PROBE_<VID>_<PID>` is the pin that answers it: more than one XMC and more than one
K64F are in rotation and every unit of a kind presents the same identity. A pin is checked
against what is enumerated, so a stale one is refused too. `bench-present.sh` reports that
board as SERIAL REFUSED and counts it NOT DECIDABLE, which is the read-only half of the same
verdict the flashing path refuses on -- an ambiguous bus may not be answered confidently by
the tool whose whole job is answering what is on the bench.

A DERIVED VCOM IS CHECKED AGAINST THE BUS, because a serial that names no attached probe
builds a perfectly plausible path. Unchecked it reads as a resolved console and the failure
arrives later as a 0-byte log, which is what a silent board looks like.

NOBODY PAIRS A BOARD WITH A SERIAL BY HAND. `bench.sh` takes the serial as an OPTIONAL
argument and, given none, reads it off the bus itself through the same row and the same
enumeration `bench-present.sh` uses, so a variant's capture on a J-Link board needs
no fleet pass to learn a serial and no flashing to answer a read-only question. A hand
paired one is also the zsh trap: `for b in "xmc4800-relax 000591165808"` word-splits in
bash and does not in zsh, where the whole string arrives as one board name and the run
reads as skipped rather than failed.

The key is the board name uppercased with dashes turned into underscores:
`esp32c6-wroom` becomes `RIG_CONSOLE_ESP32C6_WROOM`.

## Image coverage

`bench.sh` names an image with `APP`. A driver is in the image
whose composition names it, so a green run of a board's kernel-console `selftest` images says
nothing about that board's drivers, which run in the `selftest_<driver>` images.

    LIST_IMAGES=1 tools/bench/bench.sh <board>    every image the configure emits

It configures and flashes nothing, and prints `<build>/kickos-images.txt`, one
`<image>|<stdout>|<judge>|<args>` row per image. The stdout is `kernel`, the driver serving
stdout, or `-` for an image composing no system. The judge is the gate script that holds the
capture: the TAP validator `tests/integration/check_tap_stream.sh` for a selftest image, which the
capture runs against the image's row of `<build>/kickos-selftest-manifest.txt`, or the script an
image's `kickos_app_judge` names, run with the row's `;`-separated args. `JUDGE_ARGS`, which goes
with a caller's own `JUDGE`, is split at `;` the same way, so `a b` is one argument.

A capture judge reads what an image printed and nothing else. Where its verdict also has a clause
a capture does not carry, an exit status or the system ending, or one resting on a bench fitting the
rig does not declare (*Bench fittings* above), the judge prints `NOT EVALUATED: <clause>` for each,
and the image is captured but partly owed.

The other judges are marks:

- `emulator` or `emulator-owed`, set by `kickos_emulator_judged`: an image whose verdict is an
  exit status, a panic, a reboot or a deliberate fault, which no capture carries. It reads
  `emulator` where a test the build runs (neither a `host` gate nor declined) names the image,
  and `emulator-owed` where none does. Naming is running because a test that reads an image
  without booting it is a `host` gate.
  Owed is decided per build: an emulator gate on another board runs another image, so it
  witnesses nothing of this one.
- `human`, set by `kickos_human_judged`: an image whose only verdict a person reads, an LED; the
  args say what.
- `inapplicable`, set by `kickos_inapplicable`: an image whose claim this build's posture voids,
  a memory-fault arm on a board that enforces no memory; the args say why.
- `-`: an image nothing judges.

`bench-fleet.sh` captures every judged image `LIST_IMAGES` names for each board, then every other
variant the tree declares as a `<board>-<variant>` preset: of the flat build, every image only it
ships (`c6txidle`, which drives the console's UART) and each image its `FLAT_ALSO` names for that board;
of the `amp2-n0` build, the AMP partition; of any other variant (`smp`),
every image, each labelled `(<variant>)`. It prints an "image coverage"
table naming each image as `captured`, `captured, partly owed: <clauses>`, `NOT RUN`, `NO JUDGE`,
`emulator-judged`, `emulator-judged, no emulator: owed`, `human-judged (<what>): owed` or
`inapplicable (<why>)`. Nothing owed is counted as covered, and the pass states how many images
and clauses it owes. It exits 1 on a failed capture, an `ABSENT` board, or any `NOT RUN` or `NO JUDGE`, which
make the pass `INCOMPLETE`; 3 when every capture passed and something is still owed; and 0 only when
nothing is. One TAG covers the whole pass: a capture's log is keyed by tag, board and image
(`<session>/logs/<tag>-<board>-<image>.log`), the tag of a variant's capture being TAG with the
variant appended (`<tag>smp`, `<tag>flat`, `<tag>amp2n0`), so the captures of one pass never share
a log.

Every image is built (`BUILD_ONLY=1 bench.sh`) before the first is flashed. The captures then run
in one worker per board, a board's in the order above, so a pass takes as long as its slowest
board. Boards that share a probe or a console cable share a worker: `board_resources` in
`board-rows.sh` names each probe and console a board holds, and two boards naming one (the RP
boards and the Teensy, whose `RIG_CONSOLE_<BOARD>` pins name one FTDI) are captured one after the
other. Each worker's output is printed under its own heading once all have finished, and the table
above merges them.

`DRY_RUN=1 tools/bench/bench-fleet.sh` prints that whole set and flashes nothing; it asks no board
either, so no line it prints is a witness.

    AMP_PARTITION=1 APP=ampping_n0 VARIANT=amp2-n0 tools/bench/bench.sh <board>

builds the board's AMP partition, every node's image merged into one by the node 0 build's
`amp_partition` target, flashes it, and judges the capture with the judge `ampping_n0`'s row
names: `tests/integration/check_c6_amp_capture.sh` on `esp32c6-wroom`, which reads both node
images' ROM load lines, the LP core's APM witness and the four rounds across, and
`tests/integration/check_pizero_amp_gate.sh` on `pizero2350`, which reads the ACCESSCTRL readback,
node 0's read of its UART0 and the four rounds across. No emulator runs either partition.

The console route is derived from the image. Every system target defines
`kickos_usb_device_console` (`<kickos/usb_console.h>`), 1 where its stdout driver is a USB device
console, `rpusb` or `rt1062usb`, which is also what brings the USB clock tree up in its chip init;
`usb_console_image` in `tools/bench/board-rows.sh` reads its value from the ELF, and such an image
is captured over the device's own ACM. The selftest's `selftest_rpusb` on `picopi` and `pizero2350`
and `selftest_rt1062usb` on `teensy41`, `usbcdcwit` and `wallclock_usb` are those images.
`CONSOLE_PIN=1` forces the pin console back, for a device-controller backend that dies before it
publishes.

The kernel banner never reaches that console, so the driver opens the host's stream, at each
configuration, with the banner's identity rows: title, board, build, app and commit, in `kbanner`'s
formats from the same version, board, app stamp and stamped build time and label
(`<kickos/sys/banner_identity.h>`); an image with no app stamp prints no app row. `bench.sh`
refuses a capture on that route whose last identity block does not name the image it flashed, its
build and app stamps read out of that image (`identity_verdict`, `tools/bench/banner.sh`), so
another image of the same build and commit is refused too, the app stamp naming its image beside
the second it was compiled in; and a judge's capture reads from that block as it would from a
banner.

## `chaincheck`, the whole-chain app, on every board

`chaincheck` (`user/apps/common/chaincheck/main.c`) checks what the compiler, libgcc, the C
library and libm make of common patterns: 32- and 64-bit arithmetic, float and double compares
and classification, conversions, the libm basics with exact answers, and printf of every format.
It is one image per half, each carrying only its own arms: `chaincheck` (integer, every board),
`chaincheck_float` (float compute, whose arms report their answers as bits) and
`chaincheck_full` (float and C99 printf formats). The float image is not built on a chip whose
flash is 64 KiB or less, which it overflows by 3 to 5 KiB. On such a chip the integer half is
two images, `chaincheck` (arithmetic and memory arms, half `core`) and `chaincheck_print`
(integer printf arms, half `print`); half `int` names both together. The full image is not built where
newlib is the nano profile, which lacks doubles, `long long` and the C99 length modifiers.
`tests/integration/check_chaincheck.sh <image> <half>` reads the half's arm list off the source
and requires each arm's line by name, under the emulator gate
(`tests/integration/gates/chaincheck.cmake`) and over a bench capture alike.

rxv3 has no QEMU machine, so the DFPU compare patch's silicon witness is the RX72M capture, owed
under the enforcing and the flat preset. `FLAT_ALSO` in `bench-fleet.sh` names
`rx72m:chaincheck_float` and `rx72m:chaincheck_full`, the two images that read the bit, so a
fleet pass captures both builds and its table shows `chaincheck_float (flat)`. By hand:

    VARIANT=flat TAG=<tag> APP=chaincheck_float tools/bench/bench.sh rx72m

## `mutexbench`, the mutex as an application pays for it

`mutexbench` (`user/apps/common/mutexbench/main.cc`) times an uncontended lock and unlock, the C
library's locks under `malloc` and `getenv`, the bare system calls (`cpu_clock_hz`, `clock_now`,
`core_current`; each is a user-mode read where the board has one), a semaphore's post and
wait (`sem: post+wait`, an uncontended pair of compare-and-swaps in user mode with no kernel
entry, so it times no system call and compares only with captures whose semaphore is a user
word), a semaphore round trip between two threads, and the waiter's wait in a mutex handoff: alone, beside every other thread slot parked in pairs, and from another task through a
memory window where the board has address spaces. It reads `kos_clock_now`, so it builds on every
board, the 64 KiB-flash ones included. Each handoff row prints its p50, p99 and max and the
round it came from, and a clock too coarse to time one handoff prints the round alone. Its rows
are read, not judged (`kickos_unbooted`), so a capture takes no judge:

    APP=mutexbench JUDGE=none CAP_SECS=90 VARIANT= tools/bench/bench.sh <board>

A handoff is timed by a `kos_clock_now` on each side, a system call except where
`KICKOS_CLOCK_USER` makes it a counter read (porting.md), so its figures compare only between
trees whose clock read costs the same.

## `ucasrewind`, the rewind witness

On armv6m and rxv3 the switcher resumes a thread it took off the core inside `kickos_user_cas`'s
window at the routine's load (`user-cas-fails-across-a-return` in
[invariants.md](invariants.md)). `ucasrewind` (`user/apps/common/ucasrewind/main.cc`) is built only
in a tree configured with `KICKOS_WITNESS_REWIND`, whose kernel counts each rewind that moves a
stacked PC: two round-robin adders on one word must see at least one, and the word must equal the
adds both saw succeed. `tests/integration/check_rewind.sh` holds it under the emulator gate on
armv6m and over a capture alike. rxv3 has no QEMU machine, so its witness is the RX72M capture:

    EXTRA_CMAKE="-DKICKOS_WITNESS_REWIND=ON -DKICKOS_WITNESS_REWIND_SCRATCH=check_rewind.sh" \
        APP=ucasrewind JUDGE=tests/integration/check_rewind.sh VARIANT= tools/bench/bench.sh rx72m

## `userbench`, the performance bench

The kernel carries no bench instrument. `userbench` (`user/apps/common/userbench/main.cc`) times
every row from user space, on the plain kernel every other image runs, with the clock an
application reads (`kos_clock_now`, and `kos_cpu_clock_hz` for the `cpu clock` line).
`tests/integration/check_userbench.sh` judges it under the emulator gate and over a capture
alike: every row the stated posture owes is printed in the shape its clock owes, nothing is
refused, and the run ends in one boot. The figures themselves are read, not judged.

    APP=userbench JUDGE=tests/integration/check_userbench.sh tools/bench/bench.sh <board>

The `bench: posture:` line states what the board owes: kernel cores, the clock (`fine` prints
p50/p99/max per sample, `average` a mean), the dirty-vector row, address spaces, flash text, and
main's thread budget.

| row | what it times |
|---|---|
| `throughput` | semaphore ping-pong switches, three windows |
| `call/reply` | a call and its reply, 8 to 256 B, the register form and the generic path, and a caller that outranks its server and donates |
| `xtask ping-pong`, `xtask call/reply` | the same across two tasks |
| `spawn/exit` | a thread, and a task |
| `flash text` | a call into task text in external flash, a cache hit and a miss (flash posture) |
| `word-wake`, `owner-exit`, `reach-lost` | the walks under the kernel lock, as a slope in ns per waiter over three waiter counts (`reach-lost` with address spaces) |
| `e2e-local` | a raised line to its woken waiter, stamped before `kos_irq_raise` and on the wake, less a tare |
| `xcore-wake 0-<n>` | a wake of a thread on another core, the round trip halved (above one core) |
| `w2`, `w3`, `w4-push`, `w4-reseat` | the scheduler workloads above one core |
| `switch-vec` | the switch with every vector register dirty, less the clean one (armv8a, rv64imac, x86_64) |

What the clock an application reads resolves, per family:

| family | user clock | resolution |
|---|---|---|
| armv6m, armv7m, armv8-M | none (`kos_clock_now` is a system call) | coarse |
| rv32imac virt, rv64imac virt | the time CSR (`KICKOS_CLOCK_USER`) | 100 ns |
| rv32imac ESP32-C6 | none | coarse |
| armv8a | `CNTPCT_EL0` | 16 ns under QEMU |
| x86_64 | `rdtsc` | about one cycle |
| lx6 | `CCOUNT`, per core and not synchronised | one cycle |
| rxv3 | none | coarse |

What no user-space row can see is taken from the trace (`kernel/include/kickos/ktrace.h`,
`tools/kicktrace.py`) where a board carries a trace clock and RTT: the cost of each system call,
and an interrupt's entry to exit and its entry to the switch it causes. On silicon that is
`xmc4800-relax`; under emulation, the `-telem` presets.

Figures published before this bench (the switch and switch-mean rows, the lock-hold, lock-wait
and scheduler counters, the phase table, the IRQ and masked-span rows) were taken by an
instrument inside the kernel, whose own brackets cost a measured share of every figure. They
are in-kernel era figures and do not compare with `userbench` rows.

## What a capture may claim

- the banner carries the build label, and `-dirty` if the tree had uncommitted edits. A witness
  taken from a dirty tree that does not say so is unfalsifiable. THE LABEL IS A COMMIT HASH ONLY
  WHILE NO TAG IS REACHABLE: `cmake/build_stamp.cmake` stamps `git describe --dirty --always`,
  which answers `<tag>` on a tree sitting on one and `<tag>-<n>-g<hash>` past it. A tagged tree
  is what a re-measurement of an archived campaign runs on, so the capture chain recognises all
  three shapes; where the console ate the word `commit` the label is recovered by holding the
  surviving bytes against the one the build stamped.
- a witness belongs to a TREE, not to a run: never re-message or rebase past a capture and
  keep calling it evidence.
- a TAP capture is judged by `tests/integration/check_tap_stream.sh`, the same verdict the sim
  and QEMU gates run, and not by this chain's count of the `ok` lines it can see. THAT IS THE
  WHOLE POINT: a console that dropped lines under producer pressure shrinks the count with the
  loss, so the count agrees with itself while the harness's trailer, computed inside the image
  before anything went missing, says the run failed. Measured on `f411disco`: `ok: 123` against
  a plan of `1..125`, `# 1 test(s) failed` in the same log, and a summary reading `not ok: 0`.
  The arm count and the skip, partial and fault permission sets come out of the build's own
  `kickos-selftest-manifest.txt`, one row per image, written by
  `tests/integration/gates/selftest.cmake` from the very variables it hands its own entries. A
  TAP app with no row, or no arm count, is REFUSED rather than checked against itself.
  One repair is made before it judges: an IRQ console drains the stream well behind the run
  while the kernel writes a thread-fault record straight to the device, so an expected fault
  can land inside a status line. A line split by a WHOLE record, in the exact form
  `kernel/init/fault.cc` emits, of a thread on the image's fault list, whose two halves make a
  well-formed test-point line, is re-joined and named in a NOTE; any other split stays refused.
  ONE ROUTE REACHES NO VERDICT: a USB CDC console is the device, so it loses the head of the
  capture and with it the plan line. That capture says so and is reconciled by hand.
- a SPLIT board restarts TAP numbering at 1 in each image, so a lone first plan line is a
  FRACTION of a run and not a short one, and whatever reports the pass must say which image a
  figure came from. How many images a board ships as is a property of ITS configure, published
  by `user/apps/common/selftest/CMakeLists.txt` as `KICKOS_SELFTEST_IMAGES` and written beside
  the build as `kickos-selftest-manifest.txt`; `bench-fleet.sh` asks for it through
  `LIST_IMAGES=1 bench.sh <board>` rather than naming boards. It named them once, went two
  splits stale on two of them and named a third nowhere at all, and a fleet pass then flashed
  one image of three while reading green.
- an image is judged by the gate script its app CMake or its gate fragment
  (`tests/integration/gates/`) names through `kickos_app_judge` (`user/apps/CMakeLists.txt`),
  with the arguments it passes, which configure writes as the third and fourth fields of the
  image's row of `kickos-images.txt`. The script is the image's emulator gate where it has one,
  reading the capture through `KOS_CAPTURE` by every clause but an exit status. `bench.sh` runs
  it over the capture unless `JUDGE` names another. An image that neither its row nor `JUDGE`
  judges is refused, so is one its row marks emulator-judged, and `JUDGE=none` takes its capture
  unjudged. `tests/static/check_app_judges.sh` holds each judge to a planted capture it passes
  and to damaged copies of it that it refuses, each with the token its row names, and every app
  a board directory builds to a judge or its waiver list.
- a board absent from the bus is REPORTED as absent. It is never silently skipped, and an
  absent board is not a pass.
