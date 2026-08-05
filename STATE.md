<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

**M4.7.3 is complete on branch `M4.7.3-per-task-width`, squashed to one commit off `master`, and
awaits the maintainer's merge.** The pre-squash history is `backup/m473-presquash`; the squash left
the tree hash unchanged, so the silicon captures below still witness it. `TODO_FIX.md` is still the
untracked worklist and is **NOT gitignored, so `git clean -fd` destroys it**. Behind it on `master`: M4.7.2 (PR #11), M4.7.1 (PR #10), M4.6.1
(PR #9), M4.5.9 (PR #8), M4.5.8 + M4.5.7 (PR #7), M4.5.6 + M4.5.7 (PR #6).

### The bench pass: FIVE boards, FOUR ISAs, FOUR enforcement classes

Witnessed for M4.7.3, all five on one code tree.

| board | ISA / enforcement | plan | result |
| --- | --- | --- | --- |
| `xmc4800-relax` | armv7m / PMSAv7 enforce | `1..84` | 84 ok, 0 not-ok, 0 skip, 0 partial |
| `frdmk64f` | armv7m / SYSMPU enforce | `1..84` | 84 ok, 0 not-ok, 0 skip, 0 partial |
| `rx72m` | RXv3 / RX-MPU enforce | `1..84` | 84 ok, 0 not-ok, 0 skip, 0 partial |
| `esp32c6-wroom` | rv32imac / PMP NAPOT enforce | `1..84` | 84 ok, 0 not-ok, 0 skip, 0 partial |
| `esp32-wroom` | Xtensa LX6 / no-ring | `1..80` | 80 ok, 0 not-ok, 0 skip, 0 partial |

Logs: `.session/logs/m473-*.log`.

**This is the widest silicon coverage any M4.7 number has had**, and it is the answer to the
"only armv7m" gap M4.7.2 carried: four ISAs, and all four enforcement backends the fleet has
(PMSAv7, SYSMPU, RX-MPU, PMP) plus a no-ring board. The LX6 plan is 80 rather than 84 because a
no-ring board registers neither the three MPU-and-selftest arms nor `stackbase_arena` -- that is
the arm arithmetic working, not a shortfall.

**Width 11 had never been EXECUTED anywhere before this pass.** Every other segmented run in the
tree is width 10, so the geometry where `KCAP_ROOT_CHUNKS - KCAP_CHILD_CHUNKS` is non-zero was
compiled and never run. `xmc4800-relax` and `frdmk64f` are the two boards that close it; `rx72m`,
`esp32c6-wroom` and `esp32-wroom` all configure to 10.

**What the pass still does NOT witness:** the flat single-chunk path -- no bench board compiles it,
`microbit` being the only bootable flat board and a CI gate rather than a bench capture -- and
`KICKOS_CAP_REPLY_MAX` above 1, which nothing in the tree declares, so the reply-bound arms skip
above 1 rather than adapt.

### What M4.7.3 changed

**The width is per task.** Root gets the configure-time sum; every spawned task gets
`KICKOS_CAP_CHILD_WIDTH`, the grant-list floor plus the declared inbound-reply peak. That is what
makes M4.7.1's chunk directory load-bearing instead of provably inert. **No board's width moved** --
7 / 10 / 11 across all 36 presets, verified before and after against a worktree at `eb685b6`.

**The saving, measured fleet-wide:** 2304 -> 1216 B on 23 presets, 1280 -> 704 B on 8, and **zero on
the five flat supply-7 images**, whose arena is byte-identical (`_ebss == __kickos_ram_start ==
0x20001ae0` on `microbit`, before and after). Per-task width saves nothing on a 16 KiB part by
construction: a flat run is one chunk of the ceiling whatever a task declares. That is the opposite
of the naive expectation.

**The reply bound is a CAP, not a sub-range**, and the roadmap row that promised a sub-range was
updated to say so. A reservation collides with delegated placement -- grant 5 lands at index 6,
inside a top reservation on a width-7 child -- and would silently demote `cap_chunk_span` to PARTIAL
on every supply-16 board. The cap costs one counter, reserves no index, and moves no width.

**The generated header replaced three workarounds**, and they were deleted rather than tuned: the
`config/system.h` fallback, the provenance marker with its self-referential assert, and the
directory-tree walker that carried the width to subdirectories. `KCAP_CHUNK_TARGET` moved to `config/cap_geometry.h` so the probe reads
input headers only and never `cap.h` -- that is what removed the circularity at its root.

**A mechanism was shipped and then REMOVED inside the same milestone, and the reason generalises.**
`kos_thread_params::cap_width` plus `CAPABILITIES_WIDE_CHILDREN` let a parent name a wider child.
Four independent review angles converged on it: no driver, app or service used it; its only
declaration existed to feed the two arms testing it; its budget was a build-time promise nothing
enforced at runtime; an empty keyword value silently read as 0; it was unbounded (a 6.4 MB slab
configured clean); and the arm named for its guarantee did not test it. Removing it kept **100%** of
the saving, returned a further 64 B per segmented board, and deleted two defects at the root. The
design document's own opening criticises the design it replaced for "a per-spawn interface whose
only non-zero caller is the test of itself" -- and one milestone later it had grown another.

### What the ten-angle review found, and the two that matter most

**An unprivileged task could halt the kernel.** `kos_thread_spawn` checked only the upper edge of
`cap_width`; a lower value reached an unconditional `KICKOS_ASSERT` -- which is `kpanic`, not
debug-gated. Two angles reproduced the panic independently. Gone with the field.

**The reply bound's one-way guarantee was false for every server in the tree.** The reply term
widened ROOT, while the bound it sizes applies to every task and every in-tree server is a spawned
child on the child width. A reviewer measured three peers driving a child server's own-create budget
to zero. The term is now charged to the child width too, and configure refuses any combination
leaving a default-width child no slot of its own.

**`EXPECT_SKIPS` / `EXPECT_PARTIALS` are PERMISSION SETS, not budgets**, and the design record said
otherwise. `tests/check_tap_stream.sh` fails an UNLISTED skip and only NOTEs a listed arm that did
not skip. So a LOSS of arena slack is caught automatically and a GAIN is not: any change moving
`microbit`'s `.bss` must have its skip set diffed by eye. The false claim was corrected, but it had
already propagated into three review briefs before anyone checked it.

**Sweep by CLAIM, tree-wide, not by location.** It bit again: the same dead statement about the
deleted fallback stood in `cap.cc`, `cap.h`, `porting.md` and `roadmap.md`, and the FIRST commit of
this milestone created a fresh tombstone -- an assert whose message named the mechanism that commit
had just deleted, and which could no longer fail.

**The Reference tier had never stated the per-task law at all**, and carried three repealed `.bss`
formulas plus a `-KOS_EMFILE` definition false in both its clauses. `architecture.md` is the one
reference doc no reviewer had ever covered in full; it has now been covered.

**A comment in `k64uart.cc` called its own live grant inert** and "kept for spawn-signature parity".
It calls `kos_periph_enable`, for which MMIO possession is the sole authorisation, so deleting the
grant on that comment's word would have silenced the K64F console. The test for an inert grant is
whether the grantee calls a `kos_periph_*` syscall -- not whether the chip's MPU gates peripherals.

### Preparatory work banked for the next two numbers

**The configuration-mechanism spike is DONE** and recommends **no to one-app-per-build, yes to the
hybrid**. Measured: 11.8x wall-clock on `xmc4800-relax`, 13.2x on `sim`, ~8.3x across CI, to buy
640 B on a 128 KiB part and **0 B on the 16 KiB part**. The decisive fact is not the multiplier:
`ctest -R seam_defaults` in a one-app tree prints "No tests were found!!!" and **exits 0**, and five
CI steps gate whole boards on that shape. `--no-tests=error` now rides every ctest in the workflow.
Doc: `.session/spikes/design-config-mechanism.md`, deliberately outside master history.
**The one-app-per-build decision is the maintainer's and is NOT recorded in `roadmap.md` yet.**

**The M4.7.4 legacy sweep has been RUN**, and `TODO.md`'s section is now execution rather than
discovery. Classes 1 and 2 came back EMPTY; the workflow YAML is clean end to end; 17 of 32 class-4
items sit in source comments and CMake strings where `doc_names` is blind. Inventory:
`.session/spikes/legacy-audit.md`.

## What is next (locked order)

1. **The configuration-mechanism spike's DECISION.** The work is done; the one-app-per-build call
   and the number are the maintainer's.
2. **M4.7.4 -- delete the legacy management.** `TODO.md` carries the inventory to execute.
3. **M4.8.1 -- the driver class layer.** Branch `M4.8.1-driver-class` holds only its 102-line spec,
   parked; it was cut at `tree(4ad39a8)`, so check whether it now needs a rebase.
4. **M4.8.2 -- USB CDC console**, continuing M4.6.2. Enumeration and bulk IN are witnessed on
   `pizero2350`; the production service list, bulk OUT and `teensy41` are not.
5. **M4.8.3..N -- the fleet-wide witness pass**, and the per-chip `arch_console_reclaim` bodies.
   **Nothing in-tree can catch a wrong `arch_mpu_region_pow2()` literal in a backend**
   (`cmake/boot_arena.cmake` scrapes the same file the link resolves), so `rx72m` silicon is the only
   check on that class for the RX MPU.

Captures and records already stamped `M4.6.2` keep that name: a measurement is never renamed.

**`KICKOS_ENABLE_SELFTEST` is ON only on the `-st` presets.** A silicon selftest run needs
`--preset <board>-st -DKICKOS_HAVE_MPU=1`. Getting this wrong costs arms and still reads as a clean
pass. **Flash, WAIT for the flash script's own `r;g` to finish, arm exactly ONE reader by its
`by-id` symlink, then reset separately** -- `.session/m473-bench.sh` encodes the whole order and
refuses rather than producing a plausible-looking wrong log.

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
    cmake --preset <tree> -B <dir> [-DKICKOS_HAVE_MPU=0|1]
    cmake --build <dir> -j8 && ctest --test-dir <dir>

**`--preset` does NOT reset a cached `KICKOS_HAVE_MPU`**, so use a FRESH build dir per posture and
pass the value explicitly, then confirm it from `CMakeCache.txt`. **`sim` defaults to
`KICKOS_HAVE_MPU=1`** keyed on the arch, not the preset, which is why it has one posture only.

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
