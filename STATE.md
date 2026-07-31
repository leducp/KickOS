<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

## Where we are

On `master`. **M4.5.6 and M4.5.7 are MERGED**, squashed to `dde73ca` (PR #6). M4.5.6 deleted the
root-privilege knob and brought the privileged-write seam, the per-entry value mask, the esp32c6
`.data` LMA fix, honest thread-pool provisioning, and the ring and sim seam gates. M4.5.7 removed
the weak-symbol seam mechanism, and is summarised below the root list.

Captures and records across `TODO.md` and `docs/` stamp the pre-squash tips they ran on:
`c5d9b0d`, `270b6fa`, `124b68c`, `989af16`, `16e4af0`, `788b1d8`. All six folded into `dde73ca` and
reach no branch, so they resolve only where those objects still exist. The stamps stay as written.

**Root is unconditionally unprivileged on every board.** `KICKOS_ROOT_PRIVILEGED` is DELETED,
with no replacement and no porting escape hatch, and it appears in **no code or build file at all**
-- not even a configure-time guard, so a stale `-D` is silently ignored. (Docs discuss the name
historically, correctly; records taken on `master` before this merge ran it `ON`, which is why they
say so.) The invariant that arrives with it is **"exactly one privileged
thread, and it is `idle`"** (`docs/reference/invariants.md`,
`root-unprivileged-idle-alone-privileged`). What travelled with it:

1. **The privileged-WRITE seam.** `arch_periph_reg_write` (`KOS_SYS_PERIPH_REG_WRITE = 42`),
   class 2 of the family: POSSESSION-gated on a live DEV window based exactly at the block base,
   bounded so that region also CONTAINS the target word, narrowed by a per-chip allowlist of
   exact `(base, offset)` pairs, and narrowed again by a per-entry VALUE MASK -- out-of-mask bits
   are refused `-KOS_EINVAL` before the store, never trimmed. Two backends: `xmc4800` and the sim.
2. **Two real defects, fixed.** `kmain` called `kpanic` in root's unprivileged frame, so the
   panic path faulted on itself and lost the diagnostic -- now an ungated `kos_panic` syscall
   (`= 41`) with positive wire coverage. And `arch_user_text_readable`'s non-MPU arm admitted
   anything outside the arena, so a bad user pointer hard-faulted the kernel on the shipping
   default of every hardware board -- now a whitelist of ROM plus static RAM.
3. **`mpu_privileged_guard` retired, `rootfault` promoted** to an always-built gate on six
   images; the axiom the retirement rests on gained `privileged_spawn_refused`.
4. **The panic-path console reclaim was WIDENED** to reclaim-from-any-state, once, with
   `RECLAIMED` stored before the body. Safe only because every chip reclaim body is idempotent
   absolute stores -- that requirement is now load-bearing.
5. **A REAL LINKER BUG on both ESP boards.** `esp32c6.ld` linked `.data` with an `AT` clause
   while the load counter kept counting from `.text`, so `_sidata` sat outside every loaded
   segment and `Reset_Handler` copied uninitialised SRAM over the `.data` the ROM had already
   placed. The `AT` is gone and `ASSERT(_sidata == _sdata)` pins it; `esp32.ld` carried the same
   latent construct. **`virt.ld` keeps its `AT` and must not gain the assert** -- QEMU honours
   PhysAddr there, so the copy does real work. Loader-dependent, not arch-dependent:
   `docs/reference/porting.md`.
6. **`f302nucleo` advertised four thread slots its arena backed three of.**
   `cmake/boot_arena.cmake` and `arch/common/boot_arena.ld.h` now model the thread-stack pool, so
   an overcommit is a LINK error. `KICKOS_POOL_ARENA_ASSERT` is opt-in per chip `.ld` and only
   `stm32f302.ld` invokes it (see *Open blockers*).

**M4.5.7: no KickOS seam is a weak symbol.** 33 `__attribute__((weak))` definitions went, and
`.weak NMI_Handler` became a file-local label in 11 `startup.S` files. An optional seam's fallback
body sits ALONE in `<symbol>_default.cc`, in `kickos_arch_*` and never in `kickos_kernel`; a
backend satisfies the reference from its own archive member, so the fallback is never extracted.
LINKER behaviour, not language semantics, but behaviour every Unix-like linker and MSVC `.lib`
shares. The load-bearing rule -- a backend's definition must live in an ALWAYS-ANCHORED member or
it SILENTLY DECLINES at runtime -- is what `seam_defaults` enforces (`arch/CMakeLists.txt:11-71`).

**Behind us on `master`**: M4.5.5 gave the alloc/MPU seam a third region-encoding mode and added
`rootauth`; stage 4 cut authority to six bits and made root hand the app only what it declared;
stage 3 gave `arch_periph_enable` its possession gate.

**Silicon.** Wire values, per-capture tips and what each capture does NOT witness:
`docs/reference/boards.md`, *M4.5.6*. Boards reached: `xmc4800-relax`, `frdmk64f`, `pizero2350`,
`rx72m`, `esp32c6-wroom`, `f302nucleo`, `esp32-wroom`. Headlines: the mask refuses WHOLE rather
than trimming; the storm verdict now holds for a structural reason rather than one operating
point; the RING arm is witnessed for the first time in the project; `rx72m` closed all three of
its owed items in one visit. **The process rule this milestone wrote down, it then broke** --
only two second-half captures stamp a committed tip, the rest a dirty tree. Commit before a
witness pass.

**Still owed.** `f411spi` on `f411disco` is the ONLY remaining mux-write debt, and that board is
not on the current bench. The `f302nucleo` fault-reporter root cause is open, blocked on a
physical ST-Link replug. `frdmk64f` and `bluepill-c8` need right-sizing before the pool assert
can go fleet-wide.

## What is next (locked order)

Arranged so **nothing waits on bench access**; bench-gated debt is recorded and explicitly
non-blocking, the `rx72m` precedent from M4.5.5.

1. **M4.6.1 -- IRQ.** Reclaim on thread teardown, and gate `irq_register` on the existing
   `AUTH_IRQ` bit. Console visibility and handover ordering belong here too.
2. **M4.6.2 -- USB CDC console** on `picopi`, `pizero2350` and `teensy41`, the boards whose
   console needs an external adapter. One shared CDC class over two controller backends. After
   M4.6.1 because it needs the IRQ-driven substrate and the reclaim fix.
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

## Gates (ctest, re-measured at the half-two tip, zero failures)

sim 20/20, `qemu` 19/19, `microbit` 13/13. Under `KICKOS_HAVE_MPU=1`: `qemu` 22/22, `qemu-m7`
21/21, `qemu-m33` 21/21, `qemu-m3` 20/20, `qemu-riscv` 16/16. Eight configurations, each +1 on
M4.5.6: `seam_defaults` runs everywhere. Fleet links 33/33, MPU variants 14/14. Per-commit
numbers: one `panicgate` case or `ringpriv` registration moves several at once, so re-derive
rather than carrying a tally forward.

`selftest` plans **68** on `sim`, **67** under enforcement, **63** without -- the sim's extra
case is the seam backend it alone can answer. Skips 0 everywhere except `microbit`'s 9, all
pinned by name in its gate. New this milestone: `panicgate` (five images, one case each, on every
preset that enables the selftest, so all eight measured above), `privileged_spawn_refused`,
`ringpriv`, `ringppb`.

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
- **ring-only** (3): `f302nucleo` -- the only physically present no-MPU ARM board, and now the
  ring arm's silicon witness; `bluepill-c8` (no unit, build-only); `due` (unit retired).
- **no-ring** (3): `microbit`, `esp32-wroom`, `sim`.

Per-board chips, cores and the fact that decides each class: `docs/reference/boards.md`.

## Open blockers

- **`kos_print` does not survive a published console**, and that is the whole of it -- NOT "app
  output is invisible". `printf` and `std::cout` DO reach a published driver via three
  publish-aware writers kept in step (`user/include/kickos/sys/emit.h`). What drops is
  `kos_print` / `kos_kconsole_write`, and RTT still carries it. Real exposure: a freestanding app
  using `kos_print`, and a genuine DARK WINDOW between publish and the driver serving cap 0.
  M4.6.1.
- **The `f302nucleo` fault reporter produces no dump, cause OPEN, DEFERRED past M4.6.2** (no board
  access before then). The hardware faults exactly as ARM ARM B3.1.1 requires, but no byte reaches
  `USART2_TDR`; the pre-existing `fault` app truncates identically, so it is not the probers' bug.
  The unresolved span is the WHOLE dump, not the narrow reporter-to-TDR gap once recorded: the
  earlier "no lockup" and "MSP peak" readings both came from a fixed 120-step trace that stopped
  while the reporter was still healthy, and stack exhaustion is now arithmetically excluded. The
  emitted code on that path is instruction-identical to `pizero2350`'s, which dumps. First action on
  board return needs no probe: read LD2 (PB13), which `kfault_terminate` blinks 3-and-pause.
  `TODO.md` carries the arithmetic, the disassembly result and the breakpoint recipe.
- **No emulated gate can exercise a buffered-ring panic flush, and the sim cannot substitute** (its
  ring is provably empty at panic time; deleting `console_tx_flush_sync()` leaves the sim suite
  green). The drain is now WITNESSED on `pizero2350` with a measured non-empty ring
  (`used_at_panic=419` of 511, 0 after the flush) and a negative control that strands all 419, so it
  is live and load-bearing on silicon while being dead code on the host. `TODO.md`, M4.5.6. Still
  no automated gate: the in-env hole is open, the behaviour is not in doubt.
- **Two boards advertise thread slots their arena cannot back**, which is why
  `KICKOS_POOL_ARENA_ASSERT` stays opt-in. Worst image per config: `frdmk64f-st +MPU` -28,992 B,
  `frdmk64f +MPU` -28,960 B, `bluepill-c8-st` -4,096 B, `bluepill-c8` -4,000 B. Headroom is
  PER-IMAGE, not per-preset -- each app's static footprint moves the arena base, so never quote
  one number per board.
- **`bluepill-c8-st` has exactly ZERO boot-arena slack** (2,560 B needed, 2,560 available), the
  only image in the fleet at or below zero, so any static-RAM growth in a SHARED test breaks its
  link -- and it has no ctest gate and no unit, so only a full-fleet build catches it.
- **IRQ non-reclaim**: `irq_detach` has one caller, nothing in thread teardown releases a
  binding, and `irq_register` is ungated -- any thread can squat any line, permanently. M4.6.1.
- **FOUR in-tree apps grant a DEV window a live board-service driver already holds**, which the
  one-holder-per-window check refuses. Silicon-only: no in-env gate covers any of them.
- **A missed `KICKOS_APP_AUTHORITY` declaration surfaces only at runtime.** The kernel cannot
  know what an app will call, so there is no configure-time equivalent of the root-MMIO
  `FATAL_ERROR`; the failure is a checked `-KOS_EPERM`. The nastiest shape is an app that ignores
  a failed `pinmux_set` and then drives an unmuxed pin.
- **`Debug` is not a supported configuration on the 64 KiB boards** (decided in M4.5.5, not a
  blocker). It is the whole class, and the overflow moves every milestone that grows the suite,
  so re-measure rather than quoting -- `docs/reference/porting.md` holds the current figures. No
  gate builds them in `Debug` deliberately: the link failure already names the overflow in bytes.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
