<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# The bench chain

How a silicon capture is taken, and what a capture is allowed to claim. The scripts are
`tools/bench/`; the values that describe one particular rig are not in the repo at all.

## The chain

| script | job |
| --- | --- |
| `tools/bench/bench-present.sh` | READ ONLY: which machine the boards are on, which of them answer, each probe serial and each resolved console. Flashes nothing and is safe at any time |
| `tools/bench/bench-fleet.sh` | enumerates the bus, resolves each probe serial LIVE, runs every board and every service list it owes, then states coverage |
| `tools/bench/bench.sh` | ONE board: configure, build, locate the image, then hand off -- locally, or over ssh to the bench host |
| `tools/bench/bench-capture.sh` | ONE board, ONE built image: flash, capture, validate. THIS is the script that runs where the hardware is |
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

## The report has to survive the console

Every line the kernel side of the report prints goes out through `kprintf_paced`, which offers
a line the console TX ring refused again instead of losing it (see
[console.md](console.md), "Overflow policy"). The phase table is forty back-to-back lines and
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
| `BD_SWITCH` | the software register save, the swap and the software restore, stamped by `switch.S` |
| `BD_LOCK_HOLD` | the OUTERMOST `IrqLock` bracket, depth zero to depth zero |
| `BD_LOCK_WAIT` | the spin inside `arch_kernel_lock`, above one kernel core only |
| `BD_DOORBELL` | one whole doorbell round, above one kernel core only |
| `BD_IRQ_ENTRY` | a raise on the bench's own line to the handler's first instruction |
| `BD_IRQ_WCASE` | the same, with the raise at the START of a masked span of 0, 64, 256 or 1024 bytes. ONE slot for the four, cleared and re-reported per span |
| `BD_IRQ_E2E_LOCAL` | the raise, through delivery, dispatch and the wake, to the woken USERSPACE thread's first read of its device window, where that thread ran on the core that took the interrupt |
| `BD_IRQ_E2E_CROSS` | the same span where it did not, above one kernel core only |

`BD_SWITCH`, `BD_LOCK_HOLD` and `BD_LOCK_WAIT` are fed by the running workload. `BD_DOORBELL`
is fed by a probe that runs immediately BEFORE the report, so it prints with those three and
its figures belong to the same window. Every slot from `BD_IRQ_ENTRY` down is filled by a sweep
that runs AFTER the report and is printed by the op that ran it: printed with the others it
would report the window before its own sweep. That boundary is `DIST_SWEPT_FIRST`, and a slot
added on the wrong side of it reports the previous window with nothing saying so.

### What `BD_SWITCH` does not count

THE ROW IS A SUBSET OF THE PHYSICAL SWAPS IN ITS OWN WINDOW ON TWO ARCHES, and the report
states the denominator rather than leaving it to be worked out. `armv7m` and `rv32imac` both
carry an IPC fastpath inside the trap handler -- `.Lsvc_fp` and `.Lecall` -- which swaps
threads in place and returns through the exception epilogue. Neither reaches the deferred
switcher that stamps the bracket, PendSV on the one and `.Lswitch` on the other, so not one of
those swaps enters the distribution. A sweep of `qemu-riscv-bench`'s call/reply spans takes it
40000 times, twice `CALLREPLY_REPS` for the two spans inside `KOS_CALL_REG_BYTES`, and the
switch row's `n` counts none of them.

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
switch's cost on its way in, the windowed exit below it being unable to host a call, and only
that cooperative exit re-stamps the end cell. A switch resuming a thread through the interrupt
frame leaves the exit unrun, so the entry that follows it finds no end stamp and banks nothing
rather than banking a delta against a stale one. The counter there is 32 bits, so a stale pair
would land in the row as an ordinary sample with no `SAT` column to refuse it;
`tests/static/check_bench_xtensa_stamp.sh` reads the consumption out of the linked image,
there being no LX6 emulator in this tree to read the row from.

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

**THE CALLERS ARE ON DIFFERENT CORES AND ONE ATOMIC STATE CELL IS WHAT JOINS THEM.** The arm
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

### Locality, and how the two populations are produced

The tier-1 ISR stamps the core it runs on; the close compares that against the core the waiter
closed on.

**THE TWO BACKENDS DELIVER AN INJECTED LINE DIFFERENTLY, and a sweep that assumes either one is
wrong on the other.** On a GIC, `arch_irq_inject` sets an SPI pending in `GICD_ISPENDR` and the
distributor routes it to the core its target register names, whoever injected it. On rv64,
`raise_line` sets `sip.SSIP` on the CALLING hart, so the line is delivered on the hart that
injected it and nowhere else. Placing the WAITER alone therefore constructs a local wake on the
first and leaves it to coincidence on the second: it measured 1 to 3 local samples in 200 on the
four-hart rv64 board against 50 in 200 on the four-core GIC one.

So the sweep places BOTH ENDS. It spawns a raiser beside the waiter, and for each kernel core
runs two passes: the waiter on the raiser's own core, then the waiter on the next core. Where the
line follows its injector every same-core pass is local and every next-core pass is cross; where
the controller picks a fixed core, one waiter placement of each kind matches it. Each population
is therefore at least one pass in twice the core count, BY CONSTRUCTION, on both -- and that is
what makes a short row a finding rather than a property of the board. The gate refuses a
population below half that share, not merely an empty one: a row left to coincidence is not empty,
it is small.

Root cannot be the raiser: it holds no handle on itself, and placing a thread takes one.

### What the figures may claim

- `p50` and `p99` are bucket LOW EDGES at an eighth of an octave, so both are FLOORS and two
  runs compare only to that resolution. The line names its own statistic.
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
  bracket can pay. `BD_SWITCH` is not such a span: `switch.S` stamps both of its ends inside
  one `xtensa_switch`, and the entry that banks a sample finds no end stamp where the exit that
  writes one did not run.

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
locality split with an empty population above one kernel core, rows whose totals disagree with
what the probe says was closed, an unordered distribution on ANY row it emits, and either
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

- the banner carries the commit, and `-dirty` if the tree had uncommitted edits. A witness
  taken from a dirty tree that does not say so is unfalsifiable. For a `bench` capture the
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
- a two-image board (`f302nucleo`, `bluepill-c8`) restarts TAP numbering at 1 in each image,
  so a lone first plan line is HALF a run, not a short one.
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
