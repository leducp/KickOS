<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

**M4.7.2 is complete on branch `M4.7.2-review-findings`, and awaits the maintainer's merge.** It
worked the A-list in the untracked `TODO_FIX.md`, which is still the worklist and now also carries a
Part C of findings raised while doing the work and deliberately deferred. **`TODO_FIX.md` is
untracked and NOT gitignored, so `git clean -fd` destroys it.**

Behind it on `master`: M4.7.1 (PR #10, the capability-table rework), M4.6.1 (PR #9, the IRQ
substrate and the buffered userspace UART), M4.5.9 (PR #8), M4.5.8 + M4.5.7 (PR #7), M4.5.6 +
M4.5.7 (PR #6).

### The bench pass, and why it is the first one

**Two boards at one TREE, 2026-08-04 (`e74933d`, stamped `da716a8` / `15fdd82` -- identical trees), both `1..79` with zero not-ok, zero
skipped and zero partial.** Wire values and the full non-witness list:
`docs/reference/boards.md`.

**It is the FIRST silicon witness of the merged capability table, covering M4.7.1 and M4.7.2
together.** M4.7.1's own pass was taken at `c82af2c`, which is **not an ancestor of `master`** -- it
survives only on `backup/m47-presquash`, and 26 code files changed between it and the merged
`4ad39a8`, `cap.h` / `cap.cc` / `thread.h` / `syscall_thread.cc` / `sync.cc` / `slotpool.h` /
`cmake/cap_table.cmake` and the selftest among them. That run is evidence the design boots under two
MPU classes. It is not a witness of what merged, and no record may quote it as one. This mattered
because I believed the old pass-of-record line in THIS FILE and propagated it into a tracked
reference doc before an agent failed to corroborate it.

**What the new pass does NOT witness**, which is the part worth carrying:
- **only the 2-chunk geometry.** The flat run is a supply-7 board's shape and `cap_chunk_span`
  reports PARTIAL there; no bench board reproduces it.
- **only armv7m.** The rework is arch-neutral and only one ISA ran it.
- **the fourth provisioning term is 0 in every configure in the tree**, so its summing path, the
  service-list read and its diagnostic are unexercised everywhere, on silicon and off.
- **`cap_chunk_span` cannot be mutation-proved even in principle**: a consistent bijective mis-decode
  in `cap_slot` relabels slots and every install/lookup pair still agrees, while every non-injective
  mutation also corrupts `cap_run_free_build` and breaks BOOT rather than one arm. Its evidence is
  coverage, not detection. `cap_gen_reuse` DOES have a clean kill: deleting the generation test fails
  it, and the same mutation on the pre-milestone tree leaves the whole suite green, which is the
  proof that branch had never once executed.
- the A5 crowding scenario (concurrent callers filling a server's window) has no test anywhere.

### What M4.7.2 changed

Three latent code defects, all latent rather than live and all committed as such: `cap_slab_detach`
left `cap_free_head` naming a slot in a run it had returned; `g_stdout_target` was an endpoint
HANDLE sign-tested as if it were an index; and a `static_assert` cited as a guarantee could not fail.
Then the smaller cap.h/cap.cc items, the provisioning model (the grant-list floor RAISES the width
instead of refusing it, a fourth declared term for inbound reply capabilities defaulting to 0, an
out-of-tree warning, a fallback-provenance marker), two selftest arms, and a reference/Book/design
resync.

**The ten-angle review found more in the NEW work than in the old**, which is the lesson to carry.
Angle 9 found three MAJOR defects in interfaces this milestone had just added: the out-of-tree
warning could not fire for an `add_subdirectory`/`FetchContent` consumer (it HAS `boards/`, so it
read as in-tree while its declaration landed after the resolve), a misspelled keyword was dropped
silently, and `INBOUND_REPLY_CAPS -1` passed CMake truthiness and NARROWED the summed width. All
three are fixed and each refusal is proven to bite.

**The same false claim lives in several places, and fixing one instance does not find the others.**
That bit three times here: the `CAP_REPLY` packing, the three-term sum, and a `KICKOS_MAX_HANDLES=9`
guard. Sweep by CLAIM, tree-wide, including source comments and CMake strings -- `doc_names` reads
tracked markdown only and cannot see those.

**Provisioning widths, measured 2026-08-04** (re-derive with `cmake --preset <board>` and read the
`KickOS: cap table =` line; do not trust these once anything declares): **7** on the three supply-7
boards, **10** on supply-16 boards, **11** on `xmc4800-relax` and `frdmk64f` UNDER ENFORCEMENT, where
`CMakeLists.txt` resolves the service list after `KICKOS_HAVE_MPU` and picks the retaining one. So 11
is the normal silicon posture on both flagship boards, not an opt-in. **No board's width changed in
this milestone**, floor and reply term both being inert on today's declarations.

**`KICKOS_ENABLE_SELFTEST` is ON only on the `-st` presets.** A silicon selftest run needs
`--preset <board>-st -DKICKOS_HAVE_MPU=1`. Getting this wrong costs 17 arms and still reads as a
clean pass: the plain preset gave `1..62` where the `-st` one gives `1..79`.

**Flash, WAIT, arm, reset.** The documented J-Link order is flash-then-arm-then-reset, but the flash
script's own `r;g` runs the suite immediately, so arming straight after captures its headless tail
and the log looks doubled. Let that run finish first.

## What is next (locked order)

1. **M4.7.3 -- per-task table width, and the reserved reply sub-range.** Removes the one-width law,
   which is what makes M4.7.1's chunk directory load-bearing rather than inert, and partitions the
   run so client reply traffic cannot crowd out a server's own creates. `roadmap.md`'s ledger assigns
   the number and argues the split; `docs/design-capability-table.md` section 7 states the bound
   honestly (bounded, not impossible). **Fix the configuration mechanism INSIDE this number**, not
   after: per-task width adds more computed outputs, so the workarounds compound exactly here.
2. **The configuration-mechanism spike -- Kconfig before M5.** AGREED, number unassigned. The
   deciding question is not Kconfig, it is whether KickOS becomes one-app-per-build: the width is a
   max over APP TARGET PROPERTIES, a build-graph fact Kconfig cannot express, and Zephyr only escapes
   it because one app per build makes the app's own fragment the whole answer. `roadmap.md` carries
   the analysis and the two options.
3. **M4.7.4 -- delete the legacy management.** Unreleased before M6, so there is no compatibility to
   carry and anything existing only because something else USED to is cost. `roadmap.md` defines the
   class and `TODO.md` lists the sweeps; sequence it after M4.7.3, whose generated header removes the
   `config/system.h` fallback rather than tuning it.
4. **M4.8.1 -- the driver class layer.** Branch `M4.8.1-driver-class` holds only its 102-line spec,
   parked; `tree(master) == tree(4ad39a8)` when it was cut, so it needs no rebase.
5. **M4.8.2 -- USB CDC console**, continuing M4.6.2. Enumeration and bulk IN are witnessed on
   `pizero2350`; the production service list, bulk OUT and `teensy41` are not.
6. **M4.8.3..N -- the fleet-wide witness pass**, and the per-chip `arch_console_reclaim` bodies.
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
