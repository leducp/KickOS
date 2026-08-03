<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

On branch `M4.6.1-irq`, off `master` at `b56ceff`. **M4.6.1 is COMPLETE in both halves, and the
second half now runs on silicon.** Behind it on `master`: M4.5.9 (PR #8, the comment purge and the
design-doc cleanup), M4.5.8 + M4.5.7 (`844bee9`, PR #7, the gates that fail and the weak-symbol
removal), M4.5.6 + M4.5.7 (`dde73ca`, PR #6, unprivileged root).

**The first half** was the IRQ substrate: the line is a `CAP_IRQ` from a generational pool, the
mint takes `AUTH_IRQ`, `wait`/`ack`/`notify` take possession plus the matching right, reclaim rides
`cap_teardown` so every death path releases the line, and a claim no longer arms -- the first wait
does, in the thread that will consume the event. Driver-death console reclaim landed with it, and
so did handover ordering: root proves the driver is serving before any client runs.

**The second half** is the buffered userspace UART -- an SPSC byte ring, the `kos_uart_*` wire ABI,
and a two-thread driver (service thread parks in `recv`, IRQ thread owns the device) whose only
link is a doorbell. Five per-chip consumers exist. **Four of them now carry the entire selftest
over the userspace driver, on silicon, with nothing left in the kernel's path.** Every capture
below shows `# tap route: stdout endpoint -> console driver (service list published)`, so the
bytes provably crossed `printf` -> `_write` -> `kos_send(0)` -> endpoint -> service thread -> ring
-> doorbell -> IRQ thread -> the peripheral's TX register.

| board | ISA / enforcement | plan | result | capture (`.session/logs/`) |
| --- | --- | --- | --- | --- |
| `xmc4800-relax` | armv7m / PMSAv7 | `1..78` | all pass, 0 skip, 1 partial | `m461n-xmc-ktime.log` |
| `frdmk64f` | armv7m / SYSMPU | `1..78` | all pass, 0 skip, 1 partial | `m461n-k64-ktime.log` |
| `esp32c6-wroom` | rv32imac / PMP | `1..78` | all pass, 0 skip, 1 partial | `m461n-c6-ktime.log` |
| `rx72m` | RXv3 / **RX-MPU** | `1..78` | all pass, 0 skip, 1 partial | `m461n-rx-ktime-mpu.log` |
| `esp32-wroom` | Xtensa LX6 / no MPU in silicon | `1..74` | all pass, 0 skip, 1 partial | `m461n-lx6-ktime.log` |
| `f302nucleo` part 1 | armv7m / no MPU in silicon | `1..44` | all pass, 3 skip, 1 partial | `m461n-f302-p1.log` |
| `f302nucleo` part 2 | armv7m / no MPU in silicon | `1..30` | all pass, 4 skip, 2 partial | `m461n-f302-p2.log` |

**All seven images are the same clean committed tip, `9a00e73`** -- HEAD -- and the plans are the
current ones. `m461n-*` is the pass of record. Every capture was checked for the two silent capture
failures this bench has produced before: all ten stamp `20f6d43` with no `-dirty`, none contains a
`not ok`, all reach the completion marker, and none shows an interleaved half-line (the two-reader
clobber tell).

The single partial on each board is `cap_capacity` reporting that the board has one capability
class, which is true of every hardware board. `f302nucleo`'s extra skips are PROVISIONING, not
defects: `KICKOS_MAX_THREADS` is 3 there, so `mutex_chain_boost`, `mutex_deadlock` and
`call_infoless_revert` skip as `pool too small`, and part 2 adds `domain_share`, `confused_deputy`
and `mem_self_grant` for the same reason plus `irq_as_event` (4 KiB MMIO-page alloc).

**`rx72m` runs under MPU ENFORCEMENT for the first time**, and that closes a real gap rather than
adding a row: the chip has an MPU (`arch/rx/chip/rx72m/mpu.cmake` -- "KICKOS_HAVE_MPU=1 actually
faults a cross-domain access"), its preset never set the knob, and every prior capture and every
prior record described it as "no MPU". It is `1..78` with the four MPU-gated arms genuinely
executing -- `endpoint_bound` (48), `stackbase_arena` (74), `grant_reserved` (75),
`dev_window_exclusive` (76) -- none of which appear in a non-enforced capture. `esp32-wroom` and
`f302nucleo` are the only remaining boards at `1..74`/`1..44`+`1..30`, and those two chips have no
MPU at all.

**What this pass witnessed that no earlier one could**, all six previously unproven:
the console reclaim keyed on the device window and the `KOS_SYS_THREAD_KILL` cancellation primitive
(`20f6d43`); the Xtensa CCOMPARE equality-match fix (`b4e888d`, on `esp32-wroom`); the LX6 demux
silencing an unroutable UART0 sub-source; **the selftest suite split, on `f302nucleo`, whose two
images had never been booted**; and the three arms that split restored, which had never executed on
that silicon -- `cap_dest` (part 1, ok 8) PASS, `cap_capacity` (part 1, ok 9) PARTIAL exactly as
predicted, `irq_discard` (part 2, ok 16) PASS. 44 + 30 = 74 matches the configure prediction.

All five `*_uartirq` service lists are on the wire with their first-light markers -- `[xmcuartirq]`,
`[k64uartirq]`, `[c6uart]`, `[lx6uart]`, `[rxsci]` -- so the whole suite ran THROUGH the userspace
driver on every one, which is what exercises the bring-up paths this change touched.

All five first-light markers are on the wire (`[xmcuartirq] device up (IRQ TX)` and its four
siblings), so on every board the whole suite ran THROUGH the userspace driver. The CRLF cook holds on
silicon too: driver-carried lines end `\r\n` where an earlier capture from the same board shows a
bare `\n`, so the published console and the kernel console agree byte for byte.

**`rx72m`'s stop at `ok 51` is FIXED, and the doorbell is not involved.** Bench-confirmed on this
chip: an image with zero doorbell posts truncates identically, and one with 200 posts at bring-up
plus three on every write completes cleanly -- so the design gate's counting-semaphore argument
(sec.7.5) stands as written.

Two independent things were happening.

The truncation was never a mid-run wedge. The short stream is a **byte-exact prefix** of the
complete one, short by exactly **511 bytes -- one TX ring's usable capacity**. Every test ran and passed; the tail was
produced and then lost at SHUTDOWN, because root sends two zero-length flush requests after `main`
returns and then halts with interrupts masked. `rxsci` was the one driver not implementing that
request, so root released instantly and halted on a full ring. The "fix" was the driver finally
honouring the protocol, not a workaround -- and no driver-side change can rescue a missing flush,
because by then root has stopped asking.

The second defect is the real one and the tip did NOT fix it. `service_irq` read `TDRE` **before**
arming `TIE`, and the only raise this chip offers is a TDR-to-TSR transfer taken with `TIE` already
1. If the in-flight transfer completed inside that window it landed with `TIE` clear: no edge, and
the pass then armed a source that could never fire. Terminal state -- ring non-empty, `TDRE` set,
`TIE` set, no edge left -- which is a RULE T1 violation and why the drain intermittently fell a ring
behind. Arming before observing closes it.

**And it is a RACE.** A 3-of-3 A/B either way is luck: the same tip measured 0-of-5 in another pass.
The window is a few instructions against an 87 us byte time, so any scheduling shift flips it, and
any perturbation -- an extra sleep, a polled probe, a raised budget -- can look like a fix.

**What the bench pass cost, and what it bought.** The five drivers were written from manuals and
had never run; getting four of them green found real defects rather than typos, and every one of
them was in shared code:

1. **The TX doorbell was conditional** on an observed idle, which is a lost wakeup: the IRQ thread
   can drain and park between the emptiness test and the push (`b129a65`, `1233516`).
2. **A short accept silently dropped the remainder** (`ef14ab2`). A plain send has no reply, so the
   sender cannot be told and cannot retry; the retry has to live driver-side, which is what
   `console_write_all` is. All five drivers now go through that one pump.
3. **Shutdown drained the kernel's ring and not the driver's** (`09fafd9`).
4. **`rxsci` discarded every stdout byte and never quiesced TXI** (`ecd3995`), then had its
   `console_write` collapsed onto the shared pump (`fb739fc`).
5. **Round-robin never expired a slice under preemption, and the tick charged the wrong thread**
   (`66339ee`) -- a scheduler bug, surfaced by a driver that finally kept two threads busy.
6. **The LX6 had no `__getreent`**, so no stdio call could work at all (`16662b2`).

**The capability table gained three stages, and they are landed.** Stage 0 and stage 1 at
`264beae`: the authority word moved out of the table into `Thread::authority` (it names no pool
object, holds no refcount and bumps no generation), the dead `KOS_CAP_CLOCK` and the reserved spare
went, `KCAP_INDEX_BITS` is now derived from `KICKOS_MAX_HANDLES` with the 15-bit sign boundary
pinned by `static_assert`, and `KICKOS_MAX_SPAWN_GRANTS` was split out so the four caller-stack
arrays in `syscall_thread.cc` stop scaling with the ceiling. Stage 2 at `6be8220` bounded the two
interrupt-masked windows that an elastic table would have turned into milliseconds: the
effective-priority funnel **no longer reads the capability table at all** (reply donors come off an
intrusive `HeadList` on the TCB, served endpoints off the endpoint pool's existing back-pointer),
and `cap_teardown` now takes and releases its own `IrqLock` every 4 slots behind a new
`Thread::dying` marker. Both were prospective rather than live -- `KICKOS_MAX_HANDLES` is 7 to 12
today -- but the measured waste was 405,504 table slots visited to find 20 donors.

**Stage 3 is LANDED, and the capability table is no longer inline in the TCB.** A task's table is a
run taken at spawn from a statically partitioned slab of fixed size classes and returned at slot
reclaim; the TCB keeps a pointer, a capacity and a class id. The parent declares the child's
capacity, narrow-only -- a **ceiling, not a conserved budget**, so it bounds a child's WIDTH and the
per-class counts are the only wall against draining the slab. A request takes the smallest class
that fits and is **refused, never spilled** into a larger one, so every refusal names one board
knob. Fixed classes cannot fragment, so "refuse when full" stays truthful. The runtime path never
touches the slab: attach at spawn, detach at reclaim, and `cap_install` / `cap_lookup` /
`cap_teardown` work only inside the task's own run -- which is what keeps a client driving a
server's reply mint bounded by the server's declared run and not one byte wider than today.

**The default is behaviour-identical but not free.** One class at the full ceiling, one run per
possible task: no board's refusal points moved, but the mechanism costs **+196 B of text and +8 B
per TCB**. A board comes out ahead only once it declares a mix, and two do -- `sim` and `qemu`,
the two that can be run, the latter giving 192 B of `.bss` back on its selftest image. Every
hardware board still ships the default; tuning them is a measurement, not an inheritance.

**Silicon.** Wire values, per-capture tips and what each capture does NOT witness:
`docs/reference/boards.md`. The process rule this project kept breaking is honoured this time: all
five images stamp one clean committed tip.

**Still owed, and none of it is speculative.**

- **Both kernel defects found while chasing driver behaviour are FIXED**: `ktime_rearm` re-deriving
  the deadline it programs (see *Open blockers*), and tier-1 EDGE lines unable to clear a stale
  pending from userspace, closed by `KOS_SYS_IRQ_DISCARD` / `kos_irq_discard` and gated by selftest
  `irq_discard`.
- **M4.6.2's CDC console is witnessed only under the DIAG service list.** See *Open blockers*.

## What is next (locked order)

1. **Per-board capability-class mixes.** The mechanism is landed and two trees declare a mix; every
   hardware board still ships the single-class default and pays +196 B text / +8 B per TCB for
   nothing. Tuning one is a measurement -- the counts must cover every concurrently ALLOCATED slot,
   not every live thread, because a run returns at reclaim rather than at exit -- and a wrong count
   is a clean spawn refusal, not a corruption. Start from the demand counts in `TODO.md`.
2. **M4.6.2 -- USB CDC console.** Enumeration and bulk IN are witnessed on `pizero2350`; the
   production service list, bulk OUT, and `teensy41` are not.
3. **M4.6.3..N -- the fleet-wide witness pass**: every capture before M4.5.2 was taken at `-O0`,
   and `f411spi` lands here. **Nothing in-tree can catch a wrong `arch_mpu_region_pow2()` literal
   in a backend** (`cmake/boot_arena.cmake` scrapes the same file the link resolves), so `rx72m`
   silicon is the only check on that class for the RX MPU -- and it passed.

## Build posture

The fleet including the sim builds `MinSizeRel` (`-Os`, `-g` re-added). It shipped `-O0` until
M4.5.2, roughly 2x the footprint, so **every silicon witness taken before it needs re-running**
-- on the K64F `-Os` dropped a PIT clock-gate-race write that `-O0` had masked. `-Os` is a preset
default, invisible through `find_package(KickOS)`, and the floors in `docs/reference/porting.md`
assume it. The one commit to revert when bisecting a footprint or timing regression is
`build: optimise the fleet (MinSizeRel)`.

## Gates (ctest, re-derived on this tree, zero failures)

| tree | enforcing | ring-only / no MPU |
| --- | --- | --- |
| `sim` | **25/25** (its default posture) | -- |
| `sim-telem` | **27/27** | -- |
| `qemu` | 23/23 | 20/20 |
| `qemu-m7` | 22/22 | 19/19 |
| `qemu-m33` | 22/22 | 19/19 |
| `qemu-m3` | 21/21 | 18/18 |
| `qemu-riscv` | 17/17 | 15/15 |
| `qemu-telem` | -- | 21/21 |
| `microbit` | -- | 14/14 |

**`sim` defaults to `KICKOS_HAVE_MPU=1`** (`CMakeLists.txt`, keyed on the arch, not on the preset),
which is why it has no second column. **`--preset` does NOT reset a cached `KICKOS_HAVE_MPU`**, so
an earlier `-DKICKOS_HAVE_MPU=1` in the same build dir silently keeps the MPU posture and its
higher tally; pass the value explicitly when measuring either one. Per-commit numbers: one
`panicgate` case or `ringpriv` registration moves several at once, so re-derive rather than
carrying a tally forward.

**The fleet links with zero failures** across four ISAs: every preset at its default posture, the
MPU variants, the gate trees at both postures, and the 5 `*_uartirq` service lists on their own
boards. The Gates table above is 9 trees and 14 posture-configurations, and the 5 service lists are
exact. **No other partition of that total is derivable** from `cmake/presets/*.json` or `ci.yml`, so
re-derive the breakdown from a sweep before quoting one, or quote only the sweep's own pass/fail.

`doc_names` catches a cross-reference that no longer resolves, and it has already caught two
regressions since being wired up, one of them 33 references reverted by a rebase. Run it after any
doc-heavy rebase; a clean `git rebase` is not evidence. It reads TRACKED MARKDOWN ONLY, so the same
citations in source comments, CMake strings and workflow YAML rot silently -- M4.6.1 removed one
such comment citation by hand for exactly that reason.

`selftest` plans **79** on `sim`, **78** under enforcement, **74** without, **73** on `microbit`
and **74** on the two 64 KiB chips, the last of those SPLIT ACROSS TWO IMAGES as `1..44` plus
`1..30` -- the sim's extra case is the seam backend it alone can answer, and `microbit` alone still
refuses `cap_capacity` by name (its 16 KiB arena). Both silicon plan counts are confirmed on
hardware and match the capture table above; of the 64 KiB pair only `f302nucleo` is witnessed there,
`bluepill-c8` having no unit. **Skips are 0 on every bench board**, and
`microbit`'s **13** are the only ones left in the fleet -- `xmc4800-relax` and `frdmk64f` used to
skip `mutex_deadlock` as `SKIP pool too small` when no pool was ever full: their SPI service keeps a
request-endpoint cap in ROOT'S cap table for the life of the image, and the fix was sizing
`KICKOS_MAX_HANDLES` on those two chips (14 at `8f47990`, re-derived to 12 at `264beae` once
`KICKOS_CAP_FIRST_DYNAMIC` dropped to 2, which is the same 8 usable dynamic slots either way).
The 13 are all pinned by name in `microbit`'s gate:
`uart_service` (its 1 KiB ring block does not fit a 16 KiB part's arena), `domain_share`,
`irq_as_event`, five mutex choreographies, and five `call_*` cases -- `call_infoless_revert` plus
the four donation ones, three of which `6be8220` added. That is 1+1+1+5+5 = 13. `-KOS_ENOMEM` cannot distinguish a full cap table from an empty pool, so its
message is misleading on every board. The arm count is an **exact floor per posture** in
`user/apps/common/selftest/CMakeLists.txt`, so adding a case without raising it fails rather than
passing quietly. **Adding a `static` to a shared test is not free on the tiny boards**: it comes out
of `microbit`'s 16 KiB arena, which is why a new case reports its results back over its own
endpoint rather than through file-scope state.

**`ringpriv` and `ringppb` are permanent CI, not bench captures**: `cmake --preset qemu` IS the
ring-only posture, so the M3/M4/M7/M33 arms carry them with no MPU. `microbit` asserts the
OPPOSITE outcome with one arm (`CONTROL.nPRIV == 0`, no privilege axis) rather than skipping,
machine-checking the armv6m classification, and does not build `ringppb` -- on a no-ring core the
PPB read legitimately succeeds.

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
  the dead driver left. Per-chip bodies are M4.7.
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
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
