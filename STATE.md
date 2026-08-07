<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

**M4.7.7 is MERGED (PR #16), and `master` is `de2801d`.** Root is seated in the thread pool, so it
carries a kill tag of its own instead of sharing idle's boot tag, it is nameable by a handle and by a
reply capability, and an app's own `main` reaches every call/reply service API. It is witnessed on
six boards; see below.

**M4.7.8 is IN FLIGHT on `M4.7.8-timed-wait`.** Landed: a total tagged wait edge, so a parked
thread names what it waits for and the object owning the list it is on; timed send, call and
receive, the send closing a console-handover wedge that predates every release; and a join by
handle plus wait-until-last. **The reaper init is BLOCKED and the milestone cannot deliver it**:
`kos_wait_last()` means "last thread in the system" while a reaper needs "the app has finished",
and the init spawns the service threads itself, so it would park on its own children on every
board with a service list. `TODO.md` carries the analysis and the way forward, which is core-path
work needing its own number. **M4.7.x is not closed**; it is kernel-core work carrying an M4
number on purpose. Behind M4.7.6 (PR #15) on
`master`: M4.7.5 (PR #14), M4.7.4 (PR #13), M4.7.3 (PR #12), M4.7.2 (PR #11), M4.7.1 (PR #10),
M4.6.1 (PR #9), M4.5.9 (PR #8), M4.5.8 + M4.5.7 (PR #7), M4.5.6 + M4.5.7 (PR #6).

What M4.7.5 and M4.7.6 left behind is now ordinary tree shape and is documented where it
belongs, not here: the configuration mechanism in `README.md` and `docs/reference/porting.md`,
the C++20 level and its consumer rule in `cmake/kickos.cmake`, the measurement instruments in
their own script headers under `.session/`. This file keeps only what a command cannot
re-derive.

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
per job for that reason, and so must any local sweep.

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

1. **M4.7.7 -- root is a pool thread**, MERGED as PR #16 and witnessed on six boards.
2. **M4.7.8 -- the timed wait**, THIS milestone, feature-complete on its branch: an abortable and
   timed send, call and receive, a thread join, and wait-until-last. Its design spike is gitignored
   and never enters history, so `TODO.md`'s M4.7.8 section carries the settled facts rather than a
   pointer to it. The order was fixed and the reason is structural: **the tagged wait edge landed
   FIRST**, because a caller parked on a server's reply list has no edge back to that server, so no
   timed or aborted wait can revert its priority donation or unlink it without one.
   `KOS_ETIMEDOUT` is 110, clear of M4.8.1's `ENOTSUP` at 95. **The reaper init named in the
   original scope is NOT delivered** and is refused rather than deferred quietly; the next number
   for it, and the thread classification it actually needs, are in `TODO.md`.
3. **M4.8.1 -- the driver class layer**, the class layer the driver-model ruling requires and
   that SPI never got. IN FLIGHT on its own branch `M4.8.1-driver-class`, REBASED onto `de2801d`
   on 2026-08-06 (four commits; the cherry-picked worklist-retire commit dropped itself by
   patch-id, and the branch builds), carrying a per-driver-type class and a five-call UART class
   API. Keep it rebased as M4.7.8 lands: its SPI proxy marshals into `kos_call`, which M4.7.8
   changes.
4. **M4.8.2 -- USB CDC console**, continuing M4.6.2. Enumeration and bulk IN are witnessed on
   `pizero2350`; the production service list, bulk OUT and `teensy41` are not.
5. **M4.8.3..N -- the fleet-wide witness pass**, and the per-chip `arch_console_reclaim` bodies.
   **Nothing in-tree can catch a wrong `arch_mpu_region_pow2()` literal in a backend**
   (`cmake/boot_arena.cmake` scrapes the same file the link resolves), so `rx72m` silicon is the
   only check on that class for the RX MPU.

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
