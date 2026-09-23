<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The bench chain

How a silicon capture is taken, and what a capture is allowed to claim. The scripts are
`tools/bench/`; the values that describe one particular rig are not in the repo at all.

## The chain

| script | job |
| --- | --- |
| `tools/bench/bench-present.sh` | READ ONLY: which machine the boards are on, which of them answer, each probe serial and each resolved console. Flashes nothing and is safe at any time |
| `tools/bench/bench-fleet.sh` | enumerates the bus, resolves each probe serial LIVE, runs every board, every service list and every image it owes, then states coverage |
| `tools/bench/bench.sh` | ONE board: configure, build, locate the image, then hand off -- locally, or over ssh to the bench host |
| `tools/bench/bench-capture.sh` | ONE board, ONE built image: flash, capture, judge (the TAP stream through `check_tap_stream.sh`, a bench report through its own arms). THIS is the script that runs where the hardware is |
| `tools/bench/cap_esp.py` | the Espressif capture: reset-into-run and read on ONE serial handle |
| `tools/bench/rig.sh` | finds and reads the rig config; refuses by name when a required value is absent |
| `tools/bench/bench-host.sh` | sourced: which machine the boards are on, how to run a command there, and THE bus enumeration |
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
| `RIG_PROBE_<VID>_<PID>` | see below | which unit a probe `vid:pid` means, where the bus carries more than one of them. The vid:pid upper-cased with `:` as `_` |
| `RIG_BENCH_HOST` | remote mode | which machine is the bench. An ssh alias is preferred, and then no port or user belongs in the config |
| `RIG_BENCH_PORT` | no | the ssh port, only where `ssh_config` does not answer it. Empty passes no `-p` at all |
| `RIG_REMOTE_ROOT` | remote mode | the bench host directory holding the shipped tree and the run outputs |
| `RIG_REMOTE_PYBIN` | Espressif, remote | a python carrying pyserial ON the bench host, absolute |

`BENCH_HOST` in the environment is what SELECTS remote mode. `RIG_BENCH_HOST` names the
machine but does not select it: a config key must not move a flashing run from one box to
another, so `bench.sh` and `bench-fleet.sh` say when the two disagree and
`bench-present.sh` reports both. Which board is plugged into which of them stays a
question for an enumeration and is recorded nowhere.

HOW to reach that host is `ssh_config`'s answer and not the rig's. A port is passed only
where one is configured, and there is no fallback to 22, which would override the `Port`
an alias carries and reach a different machine. A user is never synthesised either.

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
enumeration `bench-present.sh` uses, so a `bench`-variant capture on a J-Link board needs
no fleet pass to learn a serial and no flashing to answer a read-only question. A hand
paired one is also the zsh trap: `for b in "xmc4800-relax 000591165808"` word-splits in
bash and does not in zsh, where the whole string arrives as one board name and the run
reads as skipped rather than failed.

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

Each list is then run in EVERY image the board's suite ships as, and the row naming a result
names the image. `DRY_RUN=1 tools/bench/bench-fleet.sh` prints that whole set and flashes
nothing; it asks no board either, so no line it prints is a witness.

## The report has to survive the console

Every line the kernel side of the report prints goes out through `kprintf_paced`, which offers
a line the console TX ring refused again instead of losing it (see
[console.md](console.md), "Overflow policy"). The phase table is one back-to-back line per phase and
the ring holds about eleven of them, so a plain `kprintf` loses the rest whole: three silicon
captures of the M8.7 campaign carried 6, 9 and 7 rows of 40, with the report continuing
afterwards as though the table were that short.

The table therefore states its own row count in its header, and
`tests/integration/check_bench_phase_table.sh` counts what arrived against it and against the
`sat-probe` line the printer ends on. It takes a recorded capture through `BENCH_CAPTURE=`, so
a silicon run is judged by the same parser as an emulated one.

## The in-kernel instrument: named distributions

`kernel/bench/bench.cc` keeps one row per kernel core, and inside each row a small set of NAMED
DISTRIBUTIONS declared in `kickos/bench.h` as `BD_*`. A distribution is an accumulator
(min/max/count/sum) plus a log-linear histogram, and one report path serves all of them, so a
new instrument is a `BD_` name, a format label and the site that calls `bench_dist_add`.

| slot | the span it samples |
| --- | --- |
| `BD_SWITCH` | the software register save, the swap and the software restore, stamped by `switch.S` (the LX6 closing one `retw` past it, in `arch_switch`) |
| `BD_LOCK_HOLD` | the OUTERMOST `IrqLock` bracket, depth zero to depth zero |
| `BD_LOCK_WAIT` | the spin inside `arch_kernel_lock`, above one kernel core only |
| `BD_DOORBELL` | one whole doorbell round, above one kernel core only |
| `BD_IRQ_ENTRY` | a raise on the bench's own line to the handler's first instruction |
| `BD_IRQ_WCASE` | the same, with the raise at the START of a masked span of 0, 64, 256 or 1024 bytes. ONE slot for the four, cleared and re-reported per span |
| `BD_IRQ_E2E_LOCAL` | the raise, through delivery, dispatch and the wake, to the woken USERSPACE thread's first read of its device window, where that thread ran on the core that took the interrupt |
| `BD_IRQ_E2E_CROSS` | the same span where it did not, above one kernel core only, and empty by construction (see "Locality" below) |

`BD_SWITCH`, `BD_LOCK_HOLD` and `BD_LOCK_WAIT` are fed by the running workload. `BD_DOORBELL`
is fed by a probe that runs immediately BEFORE the report, so it prints with those three and
its figures belong to the same window. Every slot from `BD_IRQ_ENTRY` down is filled by a sweep
that runs AFTER the report and is printed by the op that ran it: printed with the others it
would report the window before its own sweep. That boundary is `DIST_SWEPT_FIRST`, and a slot
added on the wrong side of it reports the previous window with nothing saying so.

THE PING-PONG PLAYERS ARE PINNED TOGETHER TO CORE 0, so the throughput row and the switch
distribution are the SAME-CORE HANDOFF, by placement and not by default. Their priority is equal,
and with a wider mask the placement invariant spreads such a pair across idle cores, which turns
every handoff into a cross-core wake.

THEY ARE ALSO PARKED FOR THE WHOLE REPORT. The player that posts the reporter's gate then waits on
a resume semaphore the reporter posts as the next window opens, and the other player waits on the
ping-pong behind it. The pin alone does not quiet them above one kernel core: the doorbell probe
and the sweeps place the reporter on other cores, and the pair runs again on its own core
meanwhile, switching and taking the kernel lock under the rounds being measured. So a throughput
window is exactly the rounds between the release and the gate, and no player runs between two
windows.

### The phase table's three calibration rows

`NULL`, `NEST` and `NEST_LOCK` are taken at one site, `kos_call`'s locked body, under the same
interrupt mask as the brackets they calibrate. They are what every other row is corrected
against, and the table header states the arithmetic: a leaf loses `NULL`, a composite loses
`NULL` plus `k` times `NEST - NULL`.

| row | the object it prices |
| --- | --- |
| `NULL` | two counter reads, issue to issue |
| `NEST` | the same pair inside an enclosing span, so it also carries the accumulator call the closing read cannot see |
| `NEST_LOCK` | an empty NESTED `IrqLock` inside an enclosing span |

`NEST_LOCK` IS WHAT PRICES A REMOVED NESTED LOCK, and it exists because a PERF-1 figure is
partly the instrument's own. Each nested acquisition that a change deletes also deletes a
`bench_lock_open`/`bench_lock_close` pair, and the close takes its counter read unconditionally,
ahead of the depth test, so a nested acquisition pays a read that buys nothing. `NULL` cannot
price that: it measures the distance between two reads and not what those reads charge the span
around them. `NEST_LOCK - NULL` is the whole nested acquisition as a bench build pays for it,
which is the quantity such a change removes; what a production build would have paid is the
`arch_irq_save`/`arch_irq_restore` pair inside it, read off the object code, and the difference
is the instrument-only share.

IT IS NESTED BY CONSTRUCTION AND NOT BY LUCK: the site sits inside `kos_call`'s own `IrqLock`,
so the depth rises from one and no `lock-hold` sample is taken. At depth zero the same body
would price the outermost bracket and feed the distribution a sample of its own.

**IT COSTS THE ROWS AROUND IT, and two of them by arithmetic.** `CALL_LOCKED` and `CALL_TOTAL`
enclose the calibration site, so each gained ONE nested bracket when `NEST_LOCK` landed: a
composite corrected at the old `k` reads high by one `NEST - NULL`. The lock it takes is real in
a bench image, so those two rows and the throughput row also carry the acquisition itself. And on
`qemu-riscv` a bracket added ANYWHERE inflates rows it is not inside, `rdcycle` there being
answered from the host tick source, so no row of that board is comparable across this change.
None of it reaches a production image: the site is compiled out with `KICKOS_BENCH`.

**THE PAIR INFLATES WHAT IT CALIBRATES, AND FOUR ROWS CARRY IT, NOT TWO.** The site sits inside
`kos_call`'s locked body (`kernel/syscall/syscall_ipc.cc`), which the fastpath closes as
`CALL_LOCKED` and `CALL_TOTAL` and the slowpath closes as `CALL_SLOW_LOCKED` and
`CALL_SLOW_TOTAL`. All four enclose it. What it charges each of them is four counter reads and
two accumulator calls for the pair, which is exactly `2 * (NEST - NULL)`, plus a third bracket
and a real nested acquisition for `NEST_LOCK`: the whole site is
`3 * (NEST - NULL) + (NEST_LOCK - NULL)`.

Those three brackets are inside `k`, so a composite corrected by the table's own rule is already
right and a reader subtracts nothing further. What SURVIVES the correction is
`NEST_LOCK - NULL`, the nested lock being real work in a bench image. A reader comparing one of
the four rows against a build carrying no calibration site subtracts the whole site.

MOVING THE PAIR TO AN `IrqLock` OF ITS OWN WOULD END THE INFLATION AND COST THE ARGUMENT that
the calibration runs under the same interrupt mask as the brackets it calibrates, and it would
move those four rows and the throughput row. It is not done, and this is what a reader gets
instead.

### Which composite rows may be corrected by hand

`k` BELONGS TO A SAMPLE AND NOT TO A ROW, which is why nothing prints it. A conditional arm, a
scan that popped a sender, a wake that seated a switch: each changes how many nested brackets
one sample carried, and a row's `avg` merges samples that carried different numbers. A single
printed `k` beside such a row would be a new false statement, which is worse than the silence.

SO FOR EVERY ROW BELOW MARKED `varies`, THE ROW IS NOT HAND-CORRECTABLE FROM THE OUTPUT ALONE:
its `min`, `avg` and `max` do not share one `k` and no single subtraction is right for the
three. The rows carrying a number carry it on every sample that reaches their close, and
`row - NULL - k * (NEST - NULL)` is exact for them.

| composite | `k` | what settles it |
| --- | --- | --- |
| `NEST` | 1 | `NULL`, which is the whole point of the row |
| `CALL_MINT` | 2 | `CALL_MINT_CAP` and `CALL_MINT_INFO`, both on every arm that closes it |
| `REPLY_WAKE` | 1 | the one `WAKE_UNPARK`, which now closes on the refusal arm too |
| `REPLY_LOCKED` | 5 | `REPLY_LOOKUP`, `REPLY_COPY`, `REPLY_FUNNEL`, `REPLY_WAKE`, and the one `WAKE_UNPARK` inside the last of them |
| `SWITCH_TO` | 4, or 5 under `KICKOS_LIBC_REENT` | `SWITCH_BOOK`, `MPU_APPLY`, `KTIME_REARM`, `ARCH_SWITCH`, and `REENT_SEAT` where that knob is on. ON A DEFERRED-SWITCH TARGET ONLY: where `arch_switch` swaps inline the span holds the suspension, and every bracket another thread closes on this core lands in it. A board whose deferred MPU commit does work adds `MPU_COMMIT` and the row then varies |
| `CALL_TOTAL`, `CALL_LOCKED` | varies | `CALL_DONATE` closes only where the caller outranks the server, and the wake's tail (`PICK_NEXT` and everything under `SWITCH_TO`) only where `resched_after_wake` seats a switch |
| `CALL_SLOW_TOTAL`, `CALL_SLOW_LOCKED` | varies | `CALL_SLOW_DONATE` is conditional on the bound server's priority, and `CALL_SLOW_PARK`'s block carries the same scheduling tail |
| `CALL_WAKE`, `CALL_SLOW_PARK`, `RECV_PARK` | varies | each encloses a reschedule that seats a switch or declines to |
| `RECV_SCAN` | varies, unbounded | one `WAKE_UNPARK` per sender the scan popped and rejected, and the loop runs over a queue |
| `RECV_LOCKED` | varies | `RECV_RESOLVE`, `RECV_SCAN` and `RECV_PARK`, plus whatever those two carried |
| `REPLY_TOTAL`, `REPLY_RECV_TOTAL` | varies | the reply half is optional, the receive can answer a notification without entering the receive body, and the deferred wake completes inside the span or does not |

Every row not in that table is a leaf and loses `NULL`, with one caveat carried elsewhere on
this page: where `arch_switch` swaps inline, `ARCH_SWITCH` holds the suspension, so whatever
another thread closes on that core lands in it and the row is a leaf on deferred-switch targets
only.

A CAPTURE WITNESSES TWO OF THOSE CONDITIONALS WITHOUT LEAVING THE TABLE: `CALL_DONATE`'s `n` is
a fraction of `CALL_TOTAL`'s, 19999 against 219999 in the M8.12 silicon captures, and
`PICK_NEXT`'s exceeds `SWITCH_TO`'s. A nested row whose `n` does not track its composite's is a
conditional arm. The converse does not hold: a bracket that also closes outside the composite
keeps a count of its own, which is why `SWITCH_BOOK` reads 520039 against `SWITCH_TO`'s 480039
on a board whose IPC fastpath swaps without entering `switch_to`.

### `CALL_RESUME` is a leaf, and the round trip is in no row at all on a deferred-switch target

Its mark is taken AFTER the `IrqLock` scope around the locked body has been left, so the release
that fires a pended switch has already run by then and `wq_confirm_resume`'s spin finds the
switch count already advanced. The frozen M8.12 captures say it outright: `CALL_RESUME` reads
`29/29 min=29` over 220000 samples on `f411disco-bench`, `28/28 min=28` on `esp32c6-wroom-bench`
and `31/282 min=31` on `xmc4800-relax-bench`, against a `CALL_TOTAL` of 2845 and up. A row
covering a server's processing cannot read like that.

WHERE THE ROUND TRIP LANDS IS THE TARGET'S SWITCH POSTURE. On a deferred-switch target the
pended switch fires inside the `IrqLock` destructor, after `CALL_TOTAL` closed and before
`CALL_RESUME` is marked, so the round trip is in NO phase row and the throughput row is the only
figure holding it. Where `arch_switch` swaps inline the switch happens earlier still, inside
`sched::wake` under the lock, and `CALL_WAKE` carries the server's whole processing together
with everything enclosing it: on `qemu-riscv64-bench` at one hart `CALL_WAKE`'s minimum is 26940
cycles of `CALL_TOTAL`'s 40380.

THE SPIN CAN COVER THE ROUND TRIP AND IS NOT WITNESSED DOING SO. Where the pend has not been
serviced by the time the mark is taken, the spin waits for the whole round trip and the sample
IS the round trip. No capture read here carries one, the three silicon boards' maxima sitting on
their minima.

### A bracket left open costs one counter read, and two returns leave one open on purpose

`KICKOS_BENCH_MARK` declares a local and reads the counter; the accumulator is touched only by
`KICKOS_BENCH_SPAN`. There is no pending-mark cell, so an abandoned mark cannot be consumed by a
later span, cannot overwrite anything and cannot corrupt a later sample. What it does is put one
counter read, one `NULL`, inside whatever span encloses it, where the closed bracket the
correction models costs `NEST - NULL`. Where the enclosing composite is abandoned on the same
arm, which is every refusal that returns out of `kos_call`, the read reaches no sample at all
and only `lock-hold` sees it.

`endpoint_recv_locked` CLOSES `RECV_LOCKED` AND `RECV_SCAN` ON THE PARKING PATH AND ON ITS ERROR
ARMS, AND NOT ON ITS TWO SERVED RETURNS. Closing those would put a scan that stopped on a hit
into rows that otherwise describe a scan run to empty, and it moves both rows' `n`: measured,
one sample in 260000 on `qemu-riscv64-bench` at one hart. `REPLY_RECV_TOTAL` does close on a
served receive, so it carries those two raw reads there, which is one of the reasons its `k`
varies.

### What `BD_SWITCH` does not count

THE ROW IS A SUBSET OF THE PHYSICAL SWAPS IN ITS OWN WINDOW ON TWO ARCHES, and the report
states the denominator rather than leaving it to be worked out. `armv7m` and `rv32imac` both
carry an IPC fastpath inside the trap handler -- `.Lsvc_fp` and `.Lecall` -- which swaps
threads in place and returns through the exception epilogue. Neither reaches the deferred
switcher that stamps the bracket, PendSV on the one and `.Lswitch` on the other, so not one of
those swaps enters the distribution. A sweep of `qemu-riscv-bench`'s call/reply spans takes it
40000 times, twice `CALLREPLY_REPS` for the two spans inside `KOS_CALL_REG_BYTES`, and the
switch row's `n` counts none of them.

ON `armv7m` THE ROW'S `max` IS AN INTERRUPT INDICATOR AND NOT A SWITCH COST. PendSV runs at
the lowest exception priority and does not mask until its `cpsid i`, about the first thirty
cycles of an eighty-cycle sample, so any NVIC line above it preempts inside the bracket and its
whole handler is charged to that sample. An interrupt-driven console is enough to do it on every
report window. The `rv32imac` and `rv64imac` brackets do not have this: they span a trap entry
where the hardware has already cleared `MIE`, which is why those boards print a stable `max`
across every window while an `armv7m` board's moves with what the console was doing. Read the
`p50` there, and read a moved `max` as a question about the window rather than about the swap.

IT IS REPORTED AS AN EXCLUSION AND NOT STAMPED, and the reason is the accumulator and not the
effort. A bracket at the fastpath site would enclose the whole IPC leaf -- the resolve, the
copy, the mint and the handoff -- and one accumulator cannot carry two enclosures, so the row
would stop being the register save, the swap and the restore. The fastpath's own cost is
already decomposed, by the `PH_CALL_*` phases that run over exactly that leaf.

So `switch-probe: fastpath-swaps=N` counts the swaps the report window held outside the row.
The reported windows are the ping-pong burst and the doorbell probe, neither of which issues an
IPC call, so the honest figure there is zero; `tests/integration/check_bench_lock.sh` refuses
anything else, and refuses the line's absence, a build that stopped printing it leaving the row
looking complete.

THE LX6's ROW IS THE COOPERATIVE WINDOWED SWAP ALONE. `xtensa_switch` banks the PREVIOUS
switch's cost on its way in, the windowed exit below it being unable to host a call. THE END
CELL IS STAMPED PAST THE `retw`, in `arch_switch`, where the windowed return lands, so the ps
write, the return itself and the underflow that reloads the incoming frame from its base save
area are inside the span; that is what makes this row the save, the swap and the restore, as
it is on `rv32imac` and `armv7m`. The ancestor frames below that one reload lazily as the
thread runs and belong to no switch. A switch resuming a thread through the interrupt frame
never returns there, so the entry that follows it finds no end stamp and banks nothing rather
than banking a delta against a stale one. The counter there is 32 bits, so a stale pair would
land in the row as an ordinary sample with no `SAT` column to refuse it;
`tests/static/check_bench_xtensa_stamp.sh` reads both halves out of the linked image, there
being no LX6 emulator in this tree to read the row from.

THE RXv3 ROW CLOSES WITH THE FRAME BACK. `kickos_rx_pendsw` stamps the open above the
register save and writes the save-and-swap half into a cell; `kickos_rx_restore` stamps its
own open past the deferred MPU commit and closes at the bottom of the frame reload, where the
DPFPU file, the accumulators, `FPSW` and R1-R15 are all back. The two halves are summed there
and banked by the NEXT switch, a call at either end standing inside the window it would
report. That is the same deferred bank `rv32imac` uses, and the same reason: the exit has no
register left. The pop is SPLIT for it, `popm r1-r13` then the close then `popm r14-r15`,
because nothing but the `rte` stands below a whole `popm r1-r15` and the `rte` has no scratch
at all. What the span leaves out is the MPU commit, priced separately as `PH_MPU_COMMIT`, and
the `rte` itself. The pending cell is CONSUMED ON READ: the IPC fastpath, `arch_start` and
the two USP-refusal arms all reach that restore without opening a window, and a cell left
standing would be subtracted from a later close and banked as a delta near the counter's
width. The time base is CMTW1 at 7.5 MHz, so a sample is a multiple of 32 ICLK cycles; the
cells hold raw ticks and the scaling happens once, at the bank.

TWO THINGS ON THAT ROW SURPRISE A READER COMPARING IT ACROSS ARCHES. Its `n` is one short of
the switches the window held, the last switch's sample still standing in the pending cell when
the report prints. And the cells, the stamps and the bank cost the SWINT handler something on
every switch, which the throughput line sees and NO phase row does: this is a deferred-switch
target, so `SWITCH_TO` and `ARCH_SWITCH` close before the handler ever runs.

`BD_LOCK_HOLD` IS NOT THE SAME OBJECT ON BOTH POSTURES, and a reader comparing two boards has
to know which. At one kernel core `klock_enter` and `klock_leave` are empty, so the sample is
the interrupt-masked window and nothing else. Above one core the same bracket also holds the
wait for the cross-core kernel lock and the lock itself. Its per-core MAX is therefore the
longest interrupt-masked window that core took under an `IrqLock`, which is the figure M9 asks
for by name.

WHAT THAT MAX DOES NOT COVER: a masked window opened by a bare `arch_irq_save` rather than by
`IrqLock`. Those are the arch line operations (`arch_irq_mask`, `arch_irq_unmask`,
`arch_irq_clear_pending`, `arch_irq_inject`), a few chip clock bodies, and the panic path, which
masks and never restores. Each is a straight-line body with no loop and no console, so none can
produce the millisecond-scale span this instrument exists to catch.

THAT MAX NAMES ITS OWN SITE. `lock-site: core=N site=0xADDR max=M` is one line per kernel core,
and the address is the return address of whatever function held the outermost bracket, taken at
the release that set that core's maximum and kept only while that sample is still the maximum.
So the site is filed off the accumulator's own `max` rather than beside it, and the two cannot
name different samples. Resolve it with `addr2line` against the image the capture names; the
bench boards are deterministic enough that one run names the path.

**IT IS ONE LEVEL OUT FROM THE LOCK, and that is what `__builtin_return_address(0)` means
here.** The bracket is always inlined, so the address belongs to the CALLER of the function that
held the lock, not to the acquisition. A body holding an `IrqLock` and called from several
places is therefore discriminated by the site and a body called from one is not.

`site=0x0` IS A CORE THAT TOOK NO SAMPLE, and nothing else can produce it: the cell is written
by the first sample whatever its width, and no return address on any target here is zero. That
makes the site cell its own authority, so the line carries no separate sample count.

**THE `max` ON THAT LINE MAY EXCEED THE AGGREGATED ROW PRINTED ABOVE IT, and both are correct.**
The report prints from a thread, and printing takes locks, so the row is still being fed between
the two lines. The pair on the `lock-site` line is one read of one row and is what binds the
address to a figure; the row above is the aggregate at the instant IT was read.

**AND THE MAX IS OFTEN THE CONSOLE RATHER THAN A KERNEL CRITICAL SECTION.** Measured on
`esp32c6-wroom-bench`: the site resolves inside `console_emit`, at the arm that hands a whole
line to `arch_console_write`. So a reader taking this row for the kernel's longest masked window
is reading how much the report printed. Resolve the site before drawing any conclusion from the
figure, and expect a row that moves when the console's output moves.

**AND ON THAT BOARD IT CARRIES A TWO-STATE ALIGNMENT TERM OF ABOUT 170 CYCLES, WHICH IS NOT A
COST AT ALL.** `console_tx_insert_line`'s entry address mod 4 decides which state a build
lands in: at 0 the row reads about 2013, at 2 about 2185, over thirteen captures and eleven
builds with no exception, each bit-exact across its own three windows. Two dead bytes of text
flip it either way. The instruction stream is identical; only its phase against a 32-bit fetch
word moves, which over an 87-byte copy is worth about two cycles a byte, and under the C
extension the linker owes the function only 2-byte alignment so the phase a build gets is
arbitrary. **So a `lock-hold` delta on this board is worth nothing until the two builds are
shown to share that address bit**, and a perturbation designed to test placement must not be a
multiple of four: four such controls found nothing here precisely because they were blind to
the one bit that matters. A single `nop` is the control that works.

### What a doorbell ROUND is, and what it is not

A round is ONE RAISE ACROSS EVERY PEER AND THE RENDEZVOUS THAT FOLLOWS IT: this core's request
cell bumped for each peer, the single write to the interrupt controller, each far side taking
the raise and storing its answer, and this core observing the LAST of those answers. That is
what `arch_ipi_send` followed by `arch_ipi_wait` costs a caller, and the tree has no
single-peer form of it: every caller passes the mask it needs, and the bring-up self-check's own
round (`arch/common/doorbell_protocol.cc`) passes every peer.

THE OTHER READING IS A DIFFERENT NUMBER AND SMALLER. Raise-to-one-peer's-answer shares the
raise with every other peer in the mask and ends at the first answer instead of the last, so it
omits the scan over the rest. On `qemu-arm64-benchsmp` a one-peer round measured p50 6144 cyc
against 16384 for the three-peer round, so the whole round is not three one-peer rounds either:
the raise is paid once and the peers answer in parallel.

BOTH ENDS ARE ON THE RAISING CORE AND THAT IS FORCED, NOT CHOSEN. A cycle counter is per core
and unsynchronised across cores on every backend here, so a span closed on the far side would
subtract two clocks. The consequence is that the distribution describes the INITIATOR, which is
why the kernel runs its bursts from each core in turn rather than from wherever the reporter sat.

NO SWITCH FALLS INSIDE THE BRACKET. The rounds run under an `IrqLock`, so this core cannot be
interrupted or migrated across one, and the far side answers out of its doorbell service body
without entering a scheduler. That matters on `armv8a`, `rv64imac`, the LX6 and the sim, where
`arch_switch` swaps inline and a bracket spanning a switch would close only on the next resume.
The probe's own window is ONE `lock-hold` sample, the `IrqLock` enclosing every round in a burst.

WHAT FEEDS IT. A shared-kernel SMP board performs no raise-and-rendezvous in steady state: the
kernel's rendezvous sites are the bring-up self-check, address-space maintenance on the
translating backends, and the LX6's cross-core line routing. The bring-up rounds are wiped by
the reset that opens each report window, so the distribution is driven by a probe
(`KOS_BENCH_OP_DOORBELL_PROBE`) that runs a fixed burst of real rounds through the same seam,
once per kernel core, before each report.

### The IRQ block, and what its two halves discriminate

`irq-probe` names the line the board declared free (`KICKOS_IRQ_FREE_BASE`, a chip constant in
`chip_limits.h`) and how many times the sweep raised it; `wcase-probe` says the same for each
masked span, which is its own sweep. **The denominator is not decoration.** `n=0` beside
`raised=0` is a line the kernel never attached; `n=0` beside `raised=100` is a raise that
reached no handler. The row alone shows neither, and a bench that named a line its controller
could not raise printed a clean `0/0/0` for as long as it did.

BOTH ROWS ARE A SUBTRACTION ACROSS A CYCLE COUNTER, AND A CYCLE COUNTER IS PER CORE. The
counters have no common zero, so a span opened on the raising core and closed on a handler that
ran somewhere else is an offset, not a latency, and it reads as a perfectly plausible figure:
six digits where one-core silicon reports tens. The probe therefore says which core the sweep
ran on (`on`), which core the handler stamped on (`hcore`), how many samples were refused for
disagreeing (`foreign`), and the widest raise-to-observation window the raising core measured
(`win`). `hcore` reads `none` where the line never fired and `mixed` where the sweep's samples
came from several cores, which is a mix of several clocks however each one was classified.

A sample counts only when the handler names the raiser's own core AND its stamp falls inside
that window. The two are independent: an offset can land inside the window by luck on a board
whose counters start together, and the window is the one a classifier stuck on "local" cannot
satisfy. A sample that fails either is refused, enters no statistic, and is counted in
`foreign`.

REFUSING IS THE RIGHT ANSWER ON A BOARD THAT DELIVERS THE LINE ELSEWHERE. The sweep first
places itself on each core it may run on in turn and keeps the first that takes the interrupt
itself, which is all the placement a kernel can do: the routing is the controller's, not the
scheduler's. Where no core takes its own raise the rows report nothing rather than a number
they cannot mean, and the end-to-end span below is then the only IRQ figure the board has --
which is the instrument built for the cross-core case, and it reads one clock for the whole
image.

THE ENTRY ROW AND THE END-TO-END ROW MEASURE DIFFERENT MACHINES, and each says something the
other cannot:

- `BD_IRQ_ENTRY` and `BD_IRQ_WCASE` spin for the handler INSIDE the syscall that raised the
  line. That works only where the kernel runs with interrupts unmasked; `armv8a` enters at
  EL1 with `PSTATE.I` set and never clears it, so both rows are empty there AT EVERY CORE
  COUNT. Above one core a peer takes the line while the raiser spins and the rows appear to
  fill; that peer stamps its own cycle counter, so what fills them is an offset between two
  clocks and the sweep refuses it. The GIC routes the free SPI to one core and a raise from any
  other reaches only that one, so no placement makes the delivery local.
- `rv64imac` reports nothing there either, and first of all for the same reason: at ONE hart the
  entry row is empty beside fifty closed end-to-end spans, so the line delivers and it is the
  syscall that raised it which runs masked. Above one hart the rows APPEARED to fill, and the
  path they filled by is this arch's own: `sip.SSIP` is double-booked between the kernel
  doorbell and the injected-line channel, and the raised set the dispatch drains is one word for
  the image, so whichever hart next takes a doorbell swallows the bit and runs the handler
  there. The probe says `hcore=mixed`, one sweep's samples having come from several harts.
- `rv32imac` and `armv7m` are where these two rows carry a reading, their syscalls running
  unmasked, and both are held to one core.
- the end-to-end span LEAVES the kernel between the raise and the wake, so it is indifferent to
  that. An empty entry row beside closed end-to-end spans is that backend; an empty entry row
  beside no closed span is a board that cannot deliver an injected interrupt at all.

`bench_irq_raise` is not uniform and the probe line says which it took: a direct `STIR` write on
`armv7m`, which works while `PRIMASK` holds a span masked, and the `arch_irq_inject` seam
everywhere else.

### The end-to-end span

It opens where `bench_irq_raise` fires and closes in the woken USERSPACE thread, on the trap it
takes immediately after reading the device window it holds. What it encloses: the controller's
delivery, the trap entry, the dispatch, the tier-1 ISR's mask and post, the wake and reschedule,
the switch, the return to userspace, that read, and one trap back in.

**IT IS REPORTED IN NANOSECONDS AND NOT IN CYCLES, and the machine is why.** A span that opens
on one core and closes on another cannot be timed with a per-core cycle counter: two cores' PMU
counters have no common zero, and a cross-core sample taken that way reads as a wrapped negative.
Both ends therefore read `arch_clock_now`, which is one counter for the image.

WHAT THE SPAN DOES NOT ENCLOSE: a device REGISTER access. The window the waiter reads is a block
of the arena that root reserved, granted and handed over, which is the shape
`user/apps/common/selftest`'s own IRQ-driver arm uses. No board the bench runs under an emulator
can grant a user thread a real DEV window at all -- `armv8a`, `rv64imac` and `x86_64` refuse
every `arch_mpu_region_encodable` outright -- and blind-reading a peripheral block on a part
whose clock gating is unknown is a bus fault, not a measurement.

`tare` on the probe line is the instrument's own tail: the same userspace read and the same trap
with no interrupt in it, taken once before the first window. Every end-to-end figure sits above
it, and the gate refuses one that does not.

`closed` and `dropped` are the sweep's own accounting. A sample is DROPPED, and lands in no row,
when the closer is not the armed waiter, when the waiter's switch count did not move (a wait
that returned on the fast path without ever leaving the CPU), when no ISR ran, when the
waiter woke on something that opened no span at all, or when the span is wider than the ns
columns carry. That last one is 4.29 s and over, and the close REFUSES it rather than capping
it to the widest value the row holds: capped, it is a stall the accumulator accepts as a wake,
because the cap is exactly the widest sample `acc_add` does not refuse. The rule is on the
64-bit nanosecond difference and so is the same at either tick width; the `tare` row is refused
by it on the same terms as the locality rows.

`e2e-passes: asked=N raised=M` IS THE DENOMINATOR `closed` AND `dropped` CANNOT SUPPLY,
because the raise loop is the app's. `asked` is the pass count the app handed
`KOS_BENCH_OP_E2E_PRINT`; `raised` is the raises the kernel accepted. The raise answers
`-KOS_EBUSY` until the waiter has published its park, and the app retries a bounded number of
times before abandoning the pass -- and an abandoned pass opens no span, so it moves neither
`closed` nor `dropped` and the rows read as a complete sweep at whatever size they reached.
`tests/integration/check_bench_irqspan.sh` refuses `raised` below `asked`, refuses a `closed`
that does not equal `raised`, and refuses a capture carrying no such line at all.
`irq-probe`'s own `raised=` is the same job one level in, and the kernel can count that one
alone because that sweep's loop is its own.

**THE CALLERS MAY BE ON DIFFERENT CORES AND ONE ATOMIC STATE CELL IS WHAT JOINS THEM.** The arm
writes the line, the waiter, the epoch and the ISR cell and then RELEASES the armed state; the
waiter RELEASES the parked state from inside its own park; the raise ACQUIRES that, which is
what makes those four readable there, reads `arch_clock_now` and RELEASES the opened state; the
close ACQUIRES that before it subtracts. Nothing else in the protocol is atomic: every other
cell is ordinary data one of those releases carries. The ISR's own stamp is the exception and is
relaxed on purpose, the wake path's kernel lock already ordering it against the close. Above one
kernel core `tests/static/check_bench_e2e_publish.sh` reads those instructions out of the linked
image, TCG modelling no store buffer and no run in this tree being able to tell a relaxed image
from an ordered one.

**A REFUSED TRANSITION IS NOT A NO-OP, AND THE POSTURE IS WHAT KEEPS THAT HARMLESS.** The
protocol is a plain read-then-write on one cell, never a compare-and-swap, and three of its
transitions change state before or without establishing that the caller owns the span:

- `close` stores IDLE as its second statement, ahead of the test that the closer IS the armed
  waiter. A close from any other thread therefore takes a live span to IDLE, and every raise
  after it answers `-KOS_EBUSY`. It also counts itself into `dropped`.
- `arm` writes the line, the waiter, the epoch and the ISR cell and RELEASES ARMED whatever the
  state was, so a second armer overwrites a span already raised and the close that follows
  subtracts a `t0` belonging to nobody's wake.
- `raise` tests PARKED and then stores RAISED with nothing between the two, so two raisers can
  both find PARKED, both stamp `t0` and both count into `raised`, and only the second stamp
  survives for the close to subtract.

None of the three is reachable in the posture the sweep runs: ONE armer, ONE raiser, ONE closer
and ONE reporter, serialised by the `go`/`pass` semaphore handshake in
`user/apps/common/bench/main.cc`, so no two of these calls are ever in flight together. The
protocol is left as it is because the alternative buys a compare-and-swap on the sample path for
a race the instrument's own shape excludes; what may not stand is a page saying the cells are
safe when the ordering is what makes them unnecessary.

**THE RAISE READS NOTHING OF THE WAITER'S BUT WHAT THE WAITER PUBLISHED.** It answers
`-KOS_EBUSY` until the parked state is up, and the app retries. The mark that puts it up is
taken on the waiter's own way into the block and under the kernel lock the waking post has to
take, so a line injected once the raiser has seen it cannot be delivered before the park it
announces. That placement is not an optimisation: with the raise let through earlier the rv64
board still closed 200 of 200 and dropped none, and its locality split moved from 95/105 to
175/25, the classification following where the waiter happened to be rather than where the
sweep placed it.

### Locality, and why every wake is local

The tier-1 ISR stamps the core it runs on; the close compares that against the core the waiter
closed on.

**THE LINE IS DELIVERED ON ITS CLAIM CORE, AND THE THREAD SERVING IT IS PINNED THERE.** That is
the claim rule (N3 in [design-multicore.md](../design-multicore.md)): a claim is refused unless
the claimer's mask is exactly its own core, a wait unless the waiter's mask is exactly the claim
core, and the line is routed to the claim core whoever injects it. So root pins itself to core 0
for its claim, and the waiter is spawned pinned to core 0 and never moved. A waiter on any other
mask cannot wait at all: the refusal ends its loop, and every raise after it answers
`-KOS_EBUSY` until the sweep abandons it.

The raiser walks every kernel core, two passes on each, and that moves only where the raise is
injected: the ISR runs on the claim core and wakes a waiter that can run nowhere else. EVERY
WAKE IS THEREFORE LOCAL, BY CONSTRUCTION, AT EVERY CORE COUNT. The local row carries the whole
sweep, and the cross row, still declared above one kernel core, is empty: a cross-core wake of
a line's waiter is a shape the claim rule does not admit. A cross sample is a wake the pin did
not hold, and the gate refuses it.

### What the figures may claim

- `p50` and `p99` are bucket LOW EDGES at an eighth of an octave, so both are FLOORS and two
  runs compare only to that resolution. The line names its own statistic.
- A `p99` IS STATED ONLY WHERE THE ROW CARRIES 1000 SAMPLES, and below that the middle column
  carries the row's own MAXIMUM and the statistic label reads `p50/max/max`. Ruled at M8.12,
  where the choice was between refusing the figure and printing the maximum outright. Under 100
  samples the 99th nearest rank IS the largest sample, and a few hundred put one or two samples
  above it, so what a swept row was publishing is its top sample under a percentile's name: the
  `irq` and `wcase-irq` rows carry 100, `e2e-local` 50 and `e2e-cross` 150. At the floor the top
  percent is ten samples rather than one. The workload-fed `switch`, `lock-hold` and `lock-wait`
  rows carry tens of thousands and are unaffected. Two things follow. A capture taken before
  M8.12 labels those rows `p50/p99/max` and its middle column is a bucket floor rather than a
  maximum, so the two eras' middle columns are not one figure; and no gate keys on the label,
  which is what lets the archived captures still be read. A board that declares
  `KICKOS_CHIP_CYCCNT_GLITCHES` reports `min/avg/max` and has no percentile to withhold, so no
  row of one is ever relabelled.
- A part declaring `KICKOS_CHIP_CYCCNT_GLITCHES` keeps MIN as its headline and compiles the
  histograms out entirely. That board is also the one with least RAM, which is the point: a
  histogram is 672 bytes per core per slot, against 24 for the accumulator alone.
- A distribution that means nothing at a core count is not declared there and costs no byte.
- Every accumulator call runs AFTER its own closing read, so it is invisible to its own sample
  and charged to whatever encloses it. `BD_LOCK_WAIT` is therefore inside `BD_LOCK_HOLD`.
- An aggregated line and the per-core rows under it come from ONE read of each row. They are
  not two snapshots, so they can be compared to each other.
- ABOVE ONE KERNEL CORE, `avg` IS READ FROM A LIVE PEER ROW. A peer's `Acc` is copied field by
  field while that peer keeps writing, so the `sum` and the `count` a per-core `avg` divides can
  come from different instants; the phase table's `avg` merges every core's row the same way. The
  percentiles do not share this -- their rank is the snapshot's own total -- and `min`, `max`, `n`
  and `SAT` are single loads of single-writer cells. Read a four-core `avg` as indicative and
  difference a `p50`.
- A SPAN TOO WIDE FOR THE ROW IS COUNTED, NOT AVERAGED IN. The statistics are 32 bits, and
  where `arch_switch` swaps inline a bracket closes on the thread's next resume, which is
  unbounded; above one kernel core it can close on another core, whose cycle counter shares no
  zero with the opening core's. Where the counter is 64 bits (`armv8a`, `rv64imac`, `x86_64`)
  the subtraction is too, and a delta that will not fit lands in the row's trailing `SAT`
  column and in no statistic. Every other column on that row is over `n` and never over
  `n + SAT`. On `qemu-arm64-benchsmp` a third of the samples on a switch-enclosing row land
  there; on the same image at one kernel core, not one does.
- THE LX6 IS NOT COVERED AND CANNOT BE. It counts 32 bits and swaps inline, so a span that
  waits for a resume still wraps into the row there and reads as a cost of about 2^32 cycles.
  Widening it needs a reader running more often than CCOUNT wraps, which is not a price a
  bracket can pay. `BD_SWITCH` is not such a span: its two ends stand one `retw` apart, inside
  one transit of the switcher on one core, and the entry that banks a sample finds no end stamp
  where the resume that writes one did not run.

### The controls

`tests/unit/benchdist` feeds `kickos/bench_hist.h` synthetic sample sets with known quantiles
and checks that each reported value is a floor within an eighth of an octave of the
nearest-rank quantile it names. `tests/integration/check_bench_doorbell.sh` reads the `doorbell-probe` lines and the doorbell
report: one burst per kernel core, each run ON THE CORE IT WAS ASKED FOR and moving that core's
OWN sample count by exactly the rounds it ran, every per-core row carrying one burst, the rows
totalling the aggregate, and each row's MINIMUM a multiple of the raise floor that core measured
inside the same burst. The last of those is the only arm that sees a bracket closed inside the
raise: such a bracket still reports a full, ordered, correctly indexed distribution.
`tests/integration/check_bench_lock.sh` reads the `lock-probe`
line the kernel prints from three nested `IrqLock`s: the counts must move by exactly one, on the
probing core, from a starting depth of zero. Zero says the bracket never accumulated or wrote a
peer's row; three says it sampled every nesting level.

`tests/integration/check_bench_saturate.sh` reads the `sat-probe` line, and WHAT THAT LINE
CARRIES IS SCOPED TO THE TICK WIDTH. Where the tick is 64 bits (`riscv64`, `aarch64`,
`x86_64`) it feeds the shipped accumulator one delta it can hold and one it cannot and reports
what each did to the count and to `SAT`: a representable sample moves the count alone, a wider
one moves `SAT` alone and no statistic. Below that width there is no saturation rule in the
shipped accumulator to witness: `acc_add`'s saturating branch is itself compiled out at a
32-bit tick (`kernel/bench/bench.cc`, guarded on `KICKOS_BENCH_TICK_BITS`), no delta a 32-bit
counter forms being too wide for a 32-bit row, so the line carries a width and no counts and
the two partition arms skip. What a 32-bit board gets from the probe is the width arm alone:
the width the build declares, checked against what the image's own ELF header says the machine
is, which refuses a tick type that disagrees with its counter whether or not any span happened
to be long enough to wrap. The last arm reads the table itself and refuses a row
whose maximum sits just under 2^32 with no `SAT` beside it.

The same gate reads the `ns-probe` line beside it, which is the NANOSECOND column's own
witness: the conversion is handed 4294967295 cycles, the widest count a 32-bit accumulator can
hold, and reports the nanoseconds it produced and whether the answer was capped. A cap there is
a RATE fact and not a defect -- above about 1 GHz that many cycles converts inside the field
and below it does not -- so what the pair states is that the cap and the value AGREE. A build
that truncates instead prints a small number beside `capped=0`, which on its own reads as an
ordinary figure. A board converting nothing prints `ns-probe: rate=0` and no ns column at all.

A COMPLETE REPORT IS NOT A SURVIVED RUN, so the saturation gate and
`tests/integration/check_bench_cyccnt.sh` each end on an assertion that the image did not
panic. Every other arm in both reads text the image printed, which an image that prints the
whole report and then faults answers in full. The assertion is weak above one kernel core,
where an interleaved line can hide the marker from a grep, and what carries it there is a
positive arm: the saturation gate's own first arm reads the `sat-probe` line that ends the
phase table, and the counter gate's reads the LAST switch line of the run, neither of which a
run that faulted inside the sweep reaches.

`tests/integration/check_bench_irqspan.sh` reads the IRQ block. It refuses a raise denominator
of zero, a dropped sample, a
cross-core sample at any kernel core count, rows whose totals disagree with
what the probe says was closed, an unordered entry, local or masked row, and a local
end-to-end p50 at or below the tare. A SWEEP THAT CLOSED NO SPAN AT ALL is refused under that
same end-to-end clause, which carries the two diagnoses of an empty block: an empty entry row
with nothing refused as foreign beside it is a board that delivers no injected interrupt, and
anything else is a waiter that never woke. The entry row's own condition is a DESCRIPTION and
not a verdict -- it carried samples, or the sweep refused every one as foreign, or the
end-to-end span shows the backend masks its syscalls -- because where none of the three holds
no span closed either, and that is what the end-to-end clause refuses. It also reads the four masked spans as a distribution and
not as four sample counts: the labels must ascend, and at ONE kernel core the p50s must grow
with the span and the widest must clear the narrowest by more than a bucket, which is what an
ignored `span_bytes` or a counter that does not move fails. Above one kernel core that
comparison is reported and not judged: no board here delivers an injected line to the core that
raised it, so every sample is refused as foreign and the four rows arrive empty. A board that
did deliver locally would populate them honestly and the arm is kept for it, but nothing on this
bench has ever witnessed that, and judging a shape no capture has shown asserts the emulator. And it refuses a populated entry or masked row whose probe names a handler
core other than the one the sweep ran on or names several, a row whose maximum exceeds the
raise-to-observation window its own sweep measured, a masked row printed without its probe, a
populated row whose own count and its own probe's foreign do not sum to that probe's raised, and
any refused sample at one kernel core, where there is one counter and a disagreement is the
instrument rather than the board.

EVERY CLAUSE OWES A PLANT NO OTHER CLAUSE REFUSES, one plant per arm and one arm per plant. A
plant that trips two arms proves neither: delete the arm it was written for and the report is
still refused, so the control stays green and the hole it was covering does not show. Every
refusing plant is therefore a report that passes but for the single field its name points at.
Two things in the file cannot meet that standard and say so where they sit. The guard on a
missing probe LINE is one: a report with no `irq-probe` carries `raised=0` and one with no
`e2e-probe` carries `closed=0`, so it can never refuse a report the two denominator arms would
let through, and it is proven as a shape rather than as an arm, for a single finding instead of
a cascade read off handed zeros. The other is the six-figure report the four-core boards printed
before the domain arms existed, kept verbatim as a whole-shape control because it is a real
report rather than one written for an arm.

A BOARD THAT DELIVERS ITS OWN RAISE LOCALLY ABOVE ONE KERNEL CORE has never printed here, and
the clock-domain arms are kept for it: they are what says what a populated four-core row owes,
and a plant is the only place that shape exists. Each of those plants is that report with one
field moved, so the arm it names is the only thing that can refuse it.

IT JUDGES EVERY REPORT WINDOW OF A RECORDED CAPTURE AND THE FIRST WINDOW OF A POLLED RUN. A
capture holds one report per `throughput` row -- three in the M8.7 baseline -- and read
globally the first window's accounting answers for every later one that dropped its own, which
is the same per-window rule `tools/bench/bench-capture.sh` applies on its side. A poll has no
complete capture to read: it stops the image on the first complete block, so what follows that
block is a window the run was cut off inside. The `--controls` registration plants a
three-window CRLF capture whose LAST window alone is short of its denominator, which is exactly
the shape a first-window reader reports clean.

THAT POPULATION ARM IS HELD TO POPULATED ROWS BY MEASUREMENT AND NOT BY CAUTION, and the figure
is what says so. Over the thirty-six captures of the M8.7 baseline there are 540 probe-and-row
pairs; 233 of them do not satisfy `n + foreign == raised`, and EVERY ONE of those carries `n=0`,
while every row with `n` above zero satisfies it exactly. The 233 are overwhelmingly the
`n=0 foreign=0 raised=100 hcore=none` shape this page already describes as a raise that reached
no handler, plus two rows refused all but a few samples. So an arm applied to empty rows as well
would refuse a documented-legitimate state on every board that has one, and the emptiness is
already discriminated above by a better signal than the row's own count: an empty entry row
beside CLOSED end-to-end spans is a backend whose syscall runs masked, and an empty one beside no
closed span is a board that cannot deliver an injected line, which is refused.

## What a capture may claim

- the banner carries the build label, and `-dirty` if the tree had uncommitted edits. A witness
  taken from a dirty tree that does not say so is unfalsifiable. THE LABEL IS A COMMIT HASH ONLY
  WHILE NO TAG IS REACHABLE: `cmake/build_stamp.cmake` stamps `git describe --dirty --always`,
  which answers `<tag>` on a tree sitting on one and `<tag>-<n>-g<hash>` past it. A tagged tree
  is what a re-measurement of an archived campaign runs on, so the capture chain recognises all
  three shapes; where the console ate the word `commit` the label is recovered by holding the
  surviving bytes against the one the build stamped. For a `bench` capture the
  label is COMPARED, against the one the build stamped into that image
  (`cmake/build_stamp.cmake`, carried to the capture in `EXPECT_COMMIT`), and the whole label
  including the suffix must match. A label the recovery could not read the suffix off is
  refused rather than compared. A capture from a dirty tree is not refused for being dirty;
  one whose banner disagrees with the image is.
- a `bench` capture owes a bench REPORT, checked as one AND READ OVER ONE BOOT. The slice is
  cut ONCE and every arm reads it, the cycle judgement included, so a board that restarted
  inside the window cannot lend the truncated run the markers, the table, the accounting or
  the live counter of the complete run that preceded it.

  THE SLICE IS CUT AT THE LAST BOOT START AND NOT AT THE LAST BANNER, because the banner is
  the thing that goes missing. The console emits whole lines or nothing, so a reboot can drop
  its commit line entire; anchored on the label, the search then reaches back to the EARLIER
  boot and every arm reads a complete run that is not the one the board last ran. The anchor
  is the banner block's title, which carries no label, and the commit line stands in only
  where the title went too. What then proves the slice is a count: the header, the rate line,
  the phase table and `bench: done` are each printed exactly once by a boot, so a slice
  holding two of any of them spans a boundary the anchor missed and is refused rather than
  read. A last boot carrying no label of its own is refused; an earlier boot's may not stand
  in for it.

  The report owes the header, the rate line, the phase table, a throughput row, the switch row
  the distribution print writes whatever it sampled, and the `bench: done` sentinel the app
  writes last. Its absence is a capture the window cut short, which holds every earlier line
  and reads as complete on any of them.

  EVERY CLAUSE ABOUT A REPORT WINDOW HOLDS FOR EVERY WINDOW, never for at least one. A boot
  prints one window per report, and read across the slice a single accounted window covers
  every later one that dropped its accounting. A window opens at the `throughput` row each
  report begins with and runs to the next one or to the end of the slice.

  A WINDOW OWES EXACTLY ONE SWITCH ROW, because that row is the condition every clause below
  it reads. None means the line was lost whole and each of those clauses is then decided by a
  sample count of zero the window never printed; two means nothing says which of them the
  end-to-end block was conditioned on. Both are refused.

  So per window: the end-to-end denominator is
  owed CONDITIONALLY, on THAT window's switch row `n`, the app skipping the end-to-end block
  whole where the distribution print hands back a switch count of zero -- a board that
  brackets no switch prints no `e2e-passes` line and owes none, while a window whose switch
  row sampled and prints none has rows answering to nothing. What is owed under that one
  condition is the whole end-to-end body and not the accounting line alone: the block is one
  print, so the window owes exactly one `e2e-probe` beside its accounting, and a probe that
  closed spans owes a locality row to carry them. `raised` must EQUAL `asked`:
  below it the sweep abandoned passes that opened no span, above it one of the two counts is
  not counting what it names. `closed` must equal `raised`, the locality rows must total
  `closed`, and a dropped sample is refused. An accounting line that is present and does not
  read as counts decides nothing, and may not resolve to satisfied. Each side of every one of
  those has its own plant.

  The phase table's declared row count is reconciled against the rows that arrived, which is
  the other half of the same class: a table that lost rows to a busy console still ends with
  every line after it. A rate line that is missing outright is refused; a rate of `0 Hz` is a
  declaration and is stated, not judged.
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
  ONE ROUTE REACHES NO VERDICT: a USB CDC console is the device, so it loses the head of the
  capture and with it the plan line. That capture says so and is reconciled by hand.
- a SPLIT board restarts TAP numbering at 1 in each image, so a lone first plan line is a
  FRACTION of a run and not a short one, and whatever reports the pass must say which image a
  figure came from. How many images a board ships as is a property of ITS configure, published
  by `user/apps/common/selftest/CMakeLists.txt` as `KICKOS_SELFTEST_IMAGES` and written beside
  the build as `kickos-selftest-images.txt`; `bench-fleet.sh` asks for it through
  `LIST_IMAGES=1 bench.sh <board>` rather than naming boards. It named them once, went two
  splits stale on two of them and named a third nowhere at all, and a fleet pass then flashed
  one image of three while reading green.
- a board absent from the bus is REPORTED as absent. It is never silently skipped, and an
  absent board is not a pass.
- a cycle figure is claimable only where the counter MOVED, and the capture says so or
  refuses. `tests/integration/check_bench_cyccnt.sh` registers through an emulator, so it
  covers every board that has one and no board that does not -- which is every board on this
  bench. The capture judges the report against itself instead: a board that converts no
  reading prints `cycle counter: 0 Hz` and its zeros settle nothing, while a board that NAMES
  a rate and then reports a span of zero cycles over samples it counted is refused.

  THE LIVENESS HALF OF THAT JUDGEMENT IS PER REPORT WINDOW, for the same reason the
  completeness clauses are: summed over a boot, one window whose rows spanned answers for
  every window after it, and a counter that stopped partway through the run then reads as a
  live one. So a window that sampled rows and carries not one row spanning more than a single
  value is refused, and the refusal names the window. Every cycle row a boot prints lies
  inside a window: the phase table, the `ns-probe` and the `sat-probe` come ahead of the first
  one and carry no `lo/avg/max cyc` row, so the group before window 1 is judged only for a log
  that opens no window at all. The `cyc:` line stays a reading of the whole boot, which is
  what the capture reports rather than what it decides.

  A part whose counter is known to
  glitch is not exempt -- declaring that keeps MIN as its headline, which says which statistic
  to trust and not that zero is a reading. The refusal is the capture's and not the board's:
  the log is written and fetched anyway, so the round-trip, throughput and end-to-end figures,
  which come from the wall clock, stand.
