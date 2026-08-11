<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

**M4.8.1 is MERGED (PR #19), and `master` is `d4d250bc`.** One generic driver service replaced
twelve per-(class x chip) bring-ups: a POD descriptor plus one `bring_up`, twelve `constexpr`
validity legs, and the thread bodies in the class substrate. `driver_bringup.h` is gone. Per-chip
service code went from 2307 lines to about 1039, and the bring-up from eight hand-written copies to
one.

Behind it on `master`: M4.7.9 (PR #18), M4.7.8 (PR #17), M4.7.7 (PR #16), M4.7.6 (PR #15),
M4.7.5 (PR #14), M4.7.4 (PR #13), M4.7.3 (PR #12), M4.7.2 (PR #11), M4.7.1 (PR #10), M4.6.1 (PR #9),
M4.5.9 (PR #8), M4.5.8 + M4.5.7 (PR #7), M4.5.6 (PR #6).

**Three defects that predate the driver work were fixed alongside it**, and each is the kind that
only silicon or a sanitizer finds:
- **PendSV read `g_arch_next` and the deferred MPU stash unmasked** on both ARM ports. The pair is
  written together under the kernel lock, so a device IRQ between the two reads resumed a thread with
  another thread's regions programmed. It presented as a USB CDC hard fault on `picopi` about 75% of
  runs, and the fault frames were FOSSILS of an earlier SVC, which is why the dump read like an
  escalated `svc`.
- **Three selftest IRQ arms hardcoded NVIC line numbers with no per-chip free-line set.** Inert on
  QEMU, wired peripherals on RP2040. `KICKOS_IRQ_SOFT_ONLY_BASE` lets a chip declare the fact.
- **`thread_join` read its start time after the spawn**, so a higher-priority target could reach its
  sleep first and the measured interval no longer contained it.

**Two arms filed as unreliable were not flakes.** `rr_interleave` was a symptom of the PendSV race
(4 failures in 6 runs before, 0 in 6 after) and `thread_join` was 80% on one board and 0 across
twelve runs on four others. Both had been written down as marginal and left alone, and that is what
kept a genuine scheduler race hidden. **Measure a rate on both sides of a change before applying a
flake label.**

### What the captures do NOT witness

**The M4.7.6 and M4.7.7 silicon debts are PAID.** Both were taken together on 2026-08-06 at
`de2801d`, the M4.7.7 merge, on a rare day when both benches were plugged at once. One run
discharges both because M4.7.6 is an ancestor of that tree, so a board that boots at all has
exercised the `.init_array` deletion that was the whole of the M4.7.6 risk; a separate `m476` run
would only have earned something had this one failed. Six boards, four ISAs, every enforcement
class the fleet has, zero `not ok` anywhere. Logs `.session/logs/m477-*.log`.

| board | plan | result |
| --- | --- | --- |
| `rx72m` (RXv3, RX MPU) | `1..84` | 84 ok, 0 skip, enforce |
| `xmc4800-relax` (PMSAv7) | `1..84` | 84 ok, 0 skip, enforce |
| `frdmk64f` (SYSMPU) | `1..84` | 84 ok, 0 skip, enforce |
| `esp32c6-wroom` (PMP NAPOT) | `1..84` | 84 ok, 0 skip, enforce |
| `esp32-wroom` (LX6, no unit) | `1..80` | 80 ok, 0 skip, off |
| `f302nucleo` (ring-only) | `1..43` + `1..37` | 43 + 37 ok, 3 + 7 skip, 0 + 4 partial, off |

**M4.8.1 is witnessed on EVERY enforcement class the fleet has**, each with its own converted
driver seen coming up, and `picopi` gives the project its FIRST clean armv6m enforcement capture:

| board | class | plan | driver witnessed |
| --- | --- | --- | --- |
| `esp32c6-wroom` | PMP NAPOT | `1..95` | `c6uart` |
| `rx72m` | RX MPU | `1..95` | `rxsci`, the 3-thread 2-line outlier |
| `xmc4800-relax` | PMSAv7 | `1..95` | `xmcuartirq`, `xmcuart`, `xmcssc` |
| `frdmk64f` | SYSMPU | `1..95` | `k64uart`, `k64dspi`, `k64uartirq` |
| `picopi` | PMSAv6, armv6m | `1..95` | -- |
| `esp32-wroom` | LX6, no unit | `1..91` | `lx6uart` |
| `f302nucleo` | ring-only | `1..51` + `1..40` | -- |

**M4.8.2 is witnessed on SIX boards at `b77a3ef4`**, which is every enforcement class the fleet can
currently reach: `picopi` is the only gap and it is not on any bus. A scheduler change is shipped
kernel code on every board, which is why the whole fleet ran rather than one representative. Logs
`.session/logs/m482-*.log`, and **all seven streams were piped through `tests/check_tap_stream.sh`
by hand** rather than read off the printed counts. `f302nucleo` is the exception that proves the rule
and its permission sets are NOT declared anywhere in the tree: they were taken from
`CONTEXT.local.md`'s provisioning list, so for that board alone the check is only as good as that
list. `microbit` is the only board whose skip set the tree states.

| board | class | plan | result |
| --- | --- | --- | --- |
| `frdmk64f` | SYSMPU | `1..95` | 95 ok, enforce |
| `esp32c6-wroom` | PMP NAPOT | `1..95` | 95 ok, enforce |
| `xmc4800-relax` | PMSAv7 | `1..95` | 95 ok, enforce |
| `rx72m` | RX MPU, and the fleet's only board with no CI gate at all | `1..95` | 95 ok, enforce |
| `esp32-wroom` | LX6, no unit, and an IMMEDIATE-switch port | `1..91` | 91 ok |
| `f302nucleo` | ring-only | `1..51` + `1..40` | 51 + 40 ok, 3 + 7 skip, 0 + 4 partial |

**The witness survived the review fixes that followed it.** The primary evidence is the source diff:
those fixes are comment-only, which `git diff` shows outright. The artifact cross-check is worth
knowing for its traps. Every kernel object AND the `text` size changed, which looks alarming and is
not: `-g` embeds the build path in DWARF, and a worktree path is longer than the main checkout's;
the `text` delta is `tests/tap/tap.h`'s `__FILE__`, which is APP-side. Comparing normalised
`objdump -d` instead, with an untouched file as the control, every instruction stream is identical.
**Two limits on that method.** It disassembles no data, so a changed `.rodata` initialiser passes
silently. And it was run on `frdmk64f`, where `KICKOS_ASSERT` expands to a stringified condition; on
a `KICKOS_DIAG_TERSE` board (`f302nucleo` is the only one) the same macro emits `__LINE__` as an
immediate, so inserting a comment line above an assert legitimately moves a constant there and must
not be read as a code change. An object-size delta alone never means the code moved.

**`f302nucleo`'s second image needed the log SPLIT before the checker would accept it.** Both its
captures contain TWO plan lines, a board restart inside the capture window that
`.session/bench-capture.sh` flags itself, and p2's whole-file stream reconciles as "plan claims 40
but 44 were reported". Feeding the last run alone passes. p2's truncated fragment had reached 4 arms
with zero failures and p1's had reached none, which is what says restart rather than fault: check the
fragment before believing a reconciliation failure on this board.

**A DRIVER IS ONLY IN THE IMAGE IF THE SERVICE LIST PUTS IT THERE.** `rx72m-st`, `esp32-wroom-st` and
`esp32c6-wroom-st` all default to `kickos_services_none`, so a green run on a default preset says
NOTHING about that board's driver. This was got wrong twice in one session. `bench.sh` takes
`SERVICE_LIST=` for exactly this, and the IRQ UART services live only in the `*_uartirq` providers and
are never a default. The polled default lists also claim no IRQ line at all, which is why a timing arm
can pass there and fail under `_uartirq` on the same board.

**M4.7.8 is witnessed on ALL SIX boards** at the FINAL tree, zero `not ok` anywhere: `rx72m`,
`xmc4800-relax`, `frdmk64f` and `esp32c6-wroom` 95 ok enforcing, `esp32-wroom` 91 ok,
`f302nucleo` 51 + 40 ok across its two images. Logs `.session/logs/m478c-*.log`. The captures
carry TWO commit hashes, `19b548c` on the first five boards and `d00e342` on the last two,
because the commits were reworded mid-pass; the trees are byte-identical and the enforcing
boards report the same 95 under both, so it is one witness of one tree.
The K64F needed a second attempt earlier in the day for a reason worth knowing rather than
retrying blindly: its OpenSDA probe shows a SEGGER licence dialog once per calendar day, the
acknowledgement is date-stamped, and that pass crossed midnight. The board was on the bus and
healthy, and `bench.sh`'s preflight refused by name instead of capturing a 0-byte log.
`CONTEXT.local.md` carries the detail.

**`f302nucleo` is the one board whose capture is not self-validating**, and its skips are
provisioning (a 3-thread pool, a 7-slot cap table), not defects. `bench.sh` prints counts but does
not run `tests/check_tap_stream.sh`, so both images were piped through it by hand against the arm
counts derived from `user/apps/common/selftest/CMakeLists.txt` rather than from their own plan
lines. Do the same for any future silicon capture on a board with no ctest gate: a plan that
reconciles with itself proves nothing about an arm that was deleted.

**`.session/bench-fleet.sh` is the instrument for a fleet pass now**, and it exists because a
caller must never pair a board with a probe serial by hand. `for b in "board sn"; do bench.sh $b`
is correct in bash and silently wrong in zsh, which does not word-split: the pair arrives as one
board name and `bench.sh` dies at its configure line before printing anything, so the board reads
as skipped rather than failed. The fleet script resolves serials itself from sysfs by `idProduct`,
reports an absent board as absent, and handles the two-image boards.

**A batch `ctest` across all suites is not a valid instrument for `sim` and `qemu`.** They have no
silicon clock, fail under the load of a back-to-back run, and pass standalone. CI runs one board
per job for that reason, and so must any local sweep. **That constraint is now MECHANICAL rather
than remembered**: every gate that executes no KickOS image carries `LABELS host`, so `ctest -L host`
is the batchable set and `ctest -LE host` is exactly the set that must run standalone. The label is
defined once in the root `CMakeLists.txt`. It is not a synonym for "runs on the build host":
`oot_export` runs the app it built and deliberately does not carry it.

**`EXPECT_SKIPS` and `EXPECT_PARTIALS` are PERMISSION SETS, not budgets.**
`tests/check_tap_stream.sh` fails an UNLISTED skip but only NOTEs a listed arm that did not skip.
A LOSS of arena slack is therefore caught automatically and a GAIN is not: any change that moves
`microbit`'s `.bss` needs its skip set diffed by eye.

**A silicon selftest run is `--preset <board>-st`.** `KICKOS_ENABLE_SELFTEST` is ON only on the
`-st` variants and on the boards whose base variant is itself a run gate. Getting the variant
wrong costs arms and still reads as a clean pass. Flash, WAIT for the flash script's own `r;g` to
finish, arm exactly ONE reader by its `by-id` symlink, then reset separately;
`.session/bench.sh <board> [sn]` with `TAG=<milestone>` encodes the whole order and refuses
rather than producing a plausible-looking wrong log.

## What is next (locked order)

1. **M4.8.1 -- the generic driver service**, MERGED as PR #19 and witnessed on every enforcement
   class the fleet has.
2. **M4.8.2 -- the host unit-test layer**, and the `sched::wake()` dying-guard repair it is the tool
   to prove. Its record is `docs/design-m4.8.2-host-unit-tests.md`; section 8 is what landing it
   found. The layer has TWO seams, not one: a **U-seam** at the syscall boundary, which needs no
   kernel and no fixture (`tests/drvbringup`, landed inside M4.8.1), and a **K-seam** at the arch
   boundary, which resets the whole singleton (`tests/kfixture`, plus its first gate
   `tests/schedwake`). **The K-seam is SIXTEEN functions, and the real `cap_teardown` rides it for
   free**: adding `syscall/cap.cc` trades two stubs for two others and does not widen the seam, so a
   fixture with a stubbed-out sweep would have been strictly worse for nothing.
   **The repair is THREE clauses, not the one `TODO.md` proposed.** A priority comparison alone
   strands `exit_current`'s own waiter loop, which wakes joiners after its `on_remove` when the
   dying thread can never be picked again; the `EXITED` clause is what keeps that loop's single final
   reschedule the only switch. **TWO of the five backends take a thread-context switch IMMEDIATELY,
   and one of them is silicon**: the sim (`swapcontext`) and Xtensa/LX6 (`esp32-wroom`). ARM, RISC-V
   and RX pend it until the enclosing IrqLock releases and would have survived by luck of the port.
   The same fact widens the mid-chunk exposure the narrowing introduces from a host curiosity to a
   silicon one, and `kernel/include/kickos/cap.h` is where it is stated portably.
   Still owed: the `class_backend` widening, the blocking-call trap's mechanism, an arm for the
   concurrent sweep, the migration, and one SILICON obligation (below).
3. **M4.8.3 -- the task layer**, if `docs/design-task-layer.md` rules for it. A task is a set of
   threads; the address space attaches to Domain, not Task. The spike's motivation is now partly
   discharged: the single bring-up tail takes a thread SET, which is what the old one-handle
   parameter could not express.
4. **M4.9.1 -- USB CDC console**, continuing M4.6.2. The console now **enumerates and carries payload
   on an RP2040** (`picopi`, 5.4-5.8 KiB per run), where every earlier witness was RP2350. What it
   does not do is deliver its tail: `main` returns and the teardown drops about 2.7 KiB still queued
   in the PUBLISHED console's ring, because that path drains only the kernel transport.
5. **M4.9.2..N -- the fleet-wide witness pass**, and the per-chip `arch_console_reclaim` bodies.
   **Nothing in-tree can catch a wrong `arch_mpu_region_pow2()` literal in a backend**
   (`cmake/boot_arena.cmake` scrapes the same file the link resolves), so `rx72m` silicon is the only
   check on that class for the RX MPU.

Captures and records already stamped `M4.6.2` keep that name: a measurement is never renamed.

## Build posture

The fleet including the sim builds `MinSizeRel` (`-Os`, `-g` re-added). It shipped `-O0` until
M4.5.2, roughly 2x the footprint, so **every silicon witness taken before it needs re-running**
-- on the K64F `-Os` dropped a PIT clock-gate-race write that `-O0` had masked. `-Os` is a preset
default, invisible through `find_package(KickOS)`, and the floors in `docs/reference/porting.md`
assume it. The one commit to revert when bisecting a footprint or timing regression is
`build: optimise the fleet (MinSizeRel)`.

The build identity is **`0.4.7`**, in both `project()` and `KICKOS_VERSION` (root `CMakeLists.txt`).
The scheme is `0.<milestone>.<submilestone>` and **the bump belongs to the milestone**: it sat at
`0.4.5-2` across all of M4.6, so every M4.6 banner shipped a submilestone behind.

## Gates

**Do not carry a tally forward from this file.** One `panicgate` case or `ringpriv` registration
moves several at once, so the number here rots every milestone and has misled repeatedly. Re-derive:

    source .session/env.sh          # MANDATORY: all four cross families need it
    cmake --preset <tree> -B <dir>
    cmake --build <dir> -j8 && ctest --test-dir <dir>

**The posture is part of the preset**, so there is no flag to pass and no cached value to
reset: a board that can enforce does so on its base variant and carries a `<board>-flat`
preset for the ring-only one. `sim` has no flat variant at all -- it enforces through host
mprotect, so enforcement is the only posture it has.

**Swept 2026-08-04 at tree `e74933d`: zero failures, every tree, both postures, and all 26 presets
configure and build.** That sweep is the measurement; the shape worth remembering is that the
enforcing and ring-only postures differ by a handful of arms and that `microbit` is the only board
in the fleet with skips.

Two facts a re-derive will not tell you:

- **`oot_export_mcu` fails deterministically when `ctest` runs without `.session/env.sh` sourced** --
  it falls through to the picolibc C-only `/usr/bin` twin. 100% reproducible without the env, 100%
  absent with it. It is an environment artifact, not a code failure, and it re-configures in a
  subprocess so the toolchain vars must be exported at ctest time too.
- **`qemu-riscv` under enforcement is the only posture reporting zero partials** where every ARM
  enforcing posture reports one. Both are "all expected"; it is an encoded per-arch difference, not a
  defect, and it reads as a bug later if nobody says so.

**The sim has no virtual time and its gates are not deterministic**: `arch_clock_now` reads
`CLOCK_MONOTONIC` and the tickless one-shot is a real `timer_create` delivering SIGALRM, so
preemption lands at an arbitrary host instruction and a sim gate can fail load-dependently.

`doc_names` catches a cross-reference that no longer resolves and has already caught two regressions,
one of them 33 references reverted by a rebase. Run it after any doc-heavy rebase; a clean
`git rebase` is not evidence. **It reads TRACKED MARKDOWN ONLY**, so the same citations in source
comments, CMake strings and workflow YAML rot silently -- and it validates a PATH and an IDENTIFIER,
never a LINE NUMBER, which is why this tree cites path + symbol and `design-capability-table.md` now
carries no `path:line` at all.

**The selftest arm count is an EXACT FLOOR per posture** in `user/apps/common/selftest/CMakeLists.txt`
(`_tap_arms`, plus the independent partition `_tap_arms_p1` + `_tap_arms_p2` for the two-image split,
with a totality FATAL if they disagree), so adding a case without moving both fails loudly rather
than passing quietly. **Adding a `static` to a shared test is not free**: it comes out of `microbit`'s
16 KiB arena, which is why a new case reports over its own endpoint rather than through file-scope
state. `-KOS_ENOMEM` cannot distinguish a full cap table from an empty pool, which is why
`-KOS_EMFILE` exists.

**`ringpriv` and `ringppb` are permanent CI, not bench captures**: `cmake --preset qemu` IS the
ring-only posture. `microbit` asserts the OPPOSITE outcome with one arm (`CONTROL.nPRIV == 0`) rather
than skipping, machine-checking the armv6m classification, and does not build `ringppb` -- on a
no-ring core the PPB read legitimately succeeds.

## Board matrix

**Every board runs an unprivileged root by construction**, so a row records which ENFORCEMENT
backend it can witness with: **enforcing** (a ring AND an MPU/PMP backend -- the only class that
can witness memory confinement), **ring-only** (a real ring, no MPU backend), **no-ring** (no
privilege axis, but the authority word is software and still bites, which is why `microbit` runs
`rootauth`).

- **enforcing** (14): `xmc4800-relax` (PMSAv7, the flagship and the only board carrying the
  class-2 write seam), `frdmk64f` (SYSMPU), `pizero2350` (PMSAv8), `f411disco` + `blackpill`
  (shared PMSAv7), `teensy41` (PMSAv7 + ERR011573), `picopi` (PMSAv6, the fleet's only armv6m
  enforcement proof), `rx72m` (RX MPU, no CI gate), `esp32c6-wroom` (PMP NAPOT), and the five run
  gates `qemu` / `qemu-m3` / `qemu-m7` / `qemu-m33` / `qemu-riscv`.
- **ring-only** (3): `f302nucleo` -- the only physically present no-MPU ARM board, and the ring
  arm's silicon witness; `bluepill-c8` (no unit, build-only); `due` (unit retired).
- **no-ring** (3): `microbit`, `esp32-wroom`, `sim`.

Per-board chips, cores and the fact that decides each class: `docs/reference/boards.md`.

## Open blockers

- **OWED BY M4.8.2, AND ONLY SILICON CAN PAY IT: narrowing the dying guard puts MORE traffic through
  the preemptible window between a fault redirect and its stub.** The coupling runs the wrong way, so
  `fault-record-is-printed-only-by-its-owner` carries the weight now and no host gate can discharge
  it. Wants an enforcing board with a fault arm under teardown pressure. Related and NOT independent:
  `endpoint_wait_timeout` already deflates a dying thread's priority from the timer, in the chunk
  gap, with no `dying` test, which lowers the very quantity the new guard compares against.

- **FIXED 2026-08-02: `ktime_rearm` re-derived the deadline it programmed.** It applied the
  min-delta floor itself against a fresh clock read, on every context switch, so a deadline inside
  the window was a DIFFERENT value each call -- exactly what every backend dedups on. The dedup
  never hit, the one-shot restarted before its compare, and such a deadline starved. The floor now
  lives where the deadline is BORN (`ktime_sleep_until`; `arm_slice` already had it) and
  `ktime_rearm` passes the absolute deadline through. Proven on all six boards (`m461n-*`) and by
  the `ktime_rearm` ctest, REGISTERED because it passes; mutation-proved by restoring the floor,
  which prints the drift directly (1017500, 1020000, 1022500 ... where 1015000 is wanted).
  **The two non-obvious boards are both green.** `esp32-wroom` needed a separate arch fix first --
  Xtensa `CCOMPARE0` is an equality match, so a compare landing behind the counter is MISSED rather
  than late and the next match is a 2^32-cycle wrap away, which presents as a hang at `sleep_order`
  (`b4e888d`). `rx72m` runs `rr_interleave` green with `rr order: ABABAB` under MPU enforcement.

- **FIXED 2026-08-02: the console reclaim was keyed on the endpoint's last RECEIVER.** `recv_holders`
  counts WAIT-bearing caps, so on a two-thread driver it hit zero when the SERVICE thread died --
  and the registers belong to the IRQ thread, which parks on a line cap and is not counted. The
  reclaim reprogrammed the UART under a live owner and silenced its source. It is now asked of the
  DEVICE: `console_on_driver_death` defers while any live domain holds `arch_console_reclaim_window()`.
  **"The driver is gone" is not expressible from kernel state, by design** -- `domain_for` skips the
  dedup loop for an MMIO grant and the dedup loop requires one region, so a driver's threads sit in
  different `Domain` objects: one grant, one domain, one thread. The isolation principle is what
  made the driver invisible to the kernel, so this defect was a consequence of it, not an oversight.
  The window comes from the ARCH rather than the publisher, because asking the publisher means
  trusting an unvalidated userspace address for the kernel's own registers -- and because storing
  the pair costs 8 bytes of `.bss`, which is enough to red `microbit_selftest` (measured).
  `KOS_SYS_THREAD_KILL` is what makes the deferred reclaim terminate: cooperative cancellation,
  gated on **spawn parenthood** rather than an authority bit or a capability, since parenthood
  grants the caller nothing it did not already have and cannot be delegated. Gated by
  `sim_driver_death` case 3, mutation-proved four ways. **No silicon**: the two real drivers are
  compile-verified only.

- **`kos_print` does not survive a published console**, and that is the whole of it -- NOT "app
  output is invisible". `printf` and `std::cout` DO reach a published driver via three
  publish-aware writers kept in step (`user/include/kickos/sys/emit.h`). What drops is
  `kos_print` / `kos_kconsole_write`, and RTT still carries it. **Silence from a `kos_print`-only app
  is not evidence of a dead driver**: the write is dropped before it reaches the ring, so a working
  driver and a dead one look identical on the wire -- pick a `printf` app (`gpioblink`) to probe one.
  The remaining exposure is a freestanding app using `kos_print`. What is INHERENT and not a bug: on
  a chip where the driver takes the UART the kernel was using, the kernel must let go first, so
  kernel-console writes in that span are dropped by construction -- one device, one owner.
- **M4.6.2's CDC console is witnessed only under the DIAG service list.** On `pizero2350` the host
  enumerates the device and binds `cdc_acm`, `lsusb -v` parses both interfaces, and bulk IN carried
  918 bytes byte-exactly with `drop=0 used=0` -- which validates the descriptor tables, the chapter
  9 request machine and the multi-packet EP0 data stage, the parts flagged highest-risk because no
  USB 2.0 or CDC/PSTN specification is in the local reference set. Under the PRODUCTION service
  list the device still reaches `[rpusb] host configured the device`, and then **not one byte
  reaches the ACM tty** across three attempts. Bulk OUT has never been exercised at all, `teensy41`
  is a marked seam rather than a half-built backend, and `Shared::configured` does not clear on
  unplug because no backend arms a disconnect or suspend source.
- **`f302nucleo`: the `udf` EXECUTES and the HardFault handler is NEVER ENTERED.** Answered on the
  bench with an LED probe after the UART markers proved to be a confounded instrument -- every
  marker that fires runs with `CR1.TXEIE` clear, so every reading past that point was a false
  negative. LD2 driven by a raw `GPIOB->BSRR` store with cycle-counted dwells shows one short flash,
  solid ~2 s, then dark forever: the app ran, `kos_print` returned, the `udf` was reached, and the
  fault-reporter entry -- which lights LD2 before any UART store -- never ran. **Exception entry
  itself fails**: a bad vector fetch, or LOCKUP during hardware stacking. The `[faul` truncation is
  explained and is not a defect. The panic path is separately witnessed healthy on a
  debugger-resumed boot. The `CFSR=0x00008200`/`BFAR=0xE000ED00` reading quoted as this defect's
  evidence since filing belongs to `ringpriv`, not to `fault`. Root cause is open;
  `arch_console_reclaim` is the empty default here and was never a candidate.
- **No emulated gate can exercise a buffered-ring panic flush, and the sim cannot substitute** (its
  ring is provably empty at panic time; deleting `console_tx_flush_sync()` leaves the sim suite
  green). The drain is witnessed on `pizero2350` with a measured non-empty ring (`used_at_panic=419`
  of 511, 0 after the flush) and a negative control that strands all 419. Still no automated gate:
  the in-env hole is open, the behaviour is not in doubt.
- **Two boards advertise thread slots their arena cannot back**, which is why
  `KICKOS_POOL_ARENA_ASSERT` stays opt-in. Headroom is PER-IMAGE, not per-preset -- each app's
  static footprint moves the arena base, so never quote one number per board.
- **`bluepill-c8-st` has 96 B of boot-arena slack** (measured at `6be8220`, up from zero because
  stage 0 handed back two reserved cap slots), so any static-RAM growth in a SHARED test still
  breaks its link -- and it has no ctest gate and no unit, so only a full-fleet build catches it.
- **A per-chip `arch_console_reclaim` body exists only on `mk64f`, `xmc4800` and `esp32`**, so
  elsewhere a driver death flips the state and the polled route works but the DEVICE is whatever
  the dead driver left. Per-chip bodies are fleet work; `roadmap.md`'s sub-milestone ledger says
  which number that is.
- **FOUR in-tree apps grant a DEV window a live board-service driver already holds**, which the
  one-holder-per-window check refuses. Silicon-only: no in-env gate covers any of them.
- **A missed `KICKOS_APP_AUTHORITY` declaration surfaces only at runtime.** The kernel cannot know
  what an app will call, so there is no configure-time equivalent of the root-MMIO `FATAL_ERROR`;
  the failure is a checked `-KOS_EPERM`. The nastiest shape is an app that ignores a failed
  `pinmux_set` and then drives an unmuxed pin.
- **`Debug` is not a supported configuration on the 64 KiB boards** (decided in M4.5.5, not a
  blocker). It is the whole class, and the overflow moves every milestone that grows the suite,
  so re-measure rather than quoting -- `docs/reference/porting.md` holds the current figures.

## History that must not be garbage-collected

**`c296feb` is reachable only from the local unpushed branch `m4.2-presquash`.** It holds
`git show c296feb:docs/design-m4-rx-irq-demux.md`, which `docs/design-m4.6-irq-driver.md` section 6
cites rather than reproduces for the RX routing-class taxonomy, the group-register table and the
level-versus-edge semantics. One `git branch -D m4.2-presquash` destroys an M4.6.1 prerequisite.

Captures and records across `TODO.md` and `docs/` stamp pre-squash tips (`c5d9b0d`, `270b6fa`,
`124b68c`, `989af16`, `16e4af0`, `788b1d8`) that folded into `dde73ca` and reach no branch. The
stamps stay as written.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `roadmap.md` -- the milestone plan, and the sub-milestone ledger: the only place a number is
  ASSIGNED. This file carries the locked ORDER and cites those numbers.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
