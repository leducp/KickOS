<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS TODO

**M1 VALIDATION COMPLETE (2026-07-14)** -- 10 boards on silicon (5 ISAs) + 3 emulator gates
green; every board boots, has a console, runs the selftest, panics visibly, and runs at its
true (or safely-degraded) clock. Full record in `docs/archive/M1_state.md`. The items still
open below are either optional perf, deferred to M2, or non-gating HW-unverified notes --
none block M1.

Living checklist for **M1** (uniformity / bring-up). Check items off as they land -- this file,
not memory, is the source of truth for "where are we". M2 (MPU enforcement) and M3
(capabilities + clock-select) items are parked at the bottom so they aren't lost.

This file is the **granular, actionable** status. The milestone-level plan (the general idea
per milestone) is `roadmap.md`; validated end-state + per-board detail is
`docs/archive/M1_state.md`; the board/console readiness matrix is `docs/m2-readiness.md`.

## Session record -- 2026-07-27 integration (READ THIS FIRST IF RESUMING)

Branch state and the decisions taken in conversation over the 2026-07-27 integration.
**Everything below is maintainer-confirmed**; do not re-open a decision here without new
information.

### Where the branch is

`M4.5.1-ci-hardening`, squashed to nine commits on top of `master` (`64410b7`). Linear, no merges.
Re-derive the count with `git rev-list --count master..HEAD` rather than trusting a figure here.
No upstream is configured, so `git push` needs the refspec named.

Every commit hash and subject cited in this file and in `docs/audit/` resolves against
`backup/m4.5.1-pre-squash`, the full pre-squash history, not against this branch. Older strata
resolve against `backup/m4.5.1-pre-msg-trim` and the `refs/backup/integration-*` refs. **These refs
are local-only**; pushing them is what makes the citations resolvable for anyone else.

**One branch only.** Worktree branches are transport: replay them, delete the branch, remove the
worktree with `git worktree remove`. The `.claude/worktrees/*` entries still listed (m4.6, spi-bug,
cleanup-regs, three stale `agent-*`) belong to other efforts and are all based on the pre-M4.5.1
master `64410b7` -- rebase before any work in them.

### Decisions taken (do not re-open without new information)

- **`kos_reboot` shares `kos_shutdown`'s authority** (`AUTH_SYSTEM`), rather than taking a capability
  at a reserved index. `KOS_CAP_RESERVED3` therefore **stays free**, which is worth more than bit
  granularity: spending the last well-known index forces the next one to raise
  `KICKOS_CAP_FIRST_DYNAMIC` and costs a dynamic slot on all four 9-handle boards. **LANDED** as
  `KOS_SYS_REBOOT`; recorded in full in the `kos_reboot` section below and in
  `docs/design-unprivileged-root.md` section 9. The stage-4 re-cut renamed that shared bit from
  `AUTH_DEVICE` without splitting reboot from shutdown.
- **`kos_ram_alloc` gets an explicit self-grant**, not an implicit one at alloc: `AUTH_MEMORY`-gated,
  bounded by `KICKOS_MPU_MAX_REGIONS`, failing loud with `-KOS_ENOMEM`. **LANDED** as
  `KOS_SYS_MEM_SELF_GRANT`.
- **The selftest gate asserts a named, posture-dependent expected-skip list**, not a skip budget.
  **LANDED** as `EXPECT_SKIPS`.
- **`KOS_CAP_SERVICE` is retired.** The ABI is not stable yet, so a superseded spelling is deleted
  rather than deprecated. **LANDED.**
- **clang-format is decided against** as a gate -- see the CI-hygiene section.
- **The record cites closing commits by SUBJECT, not by hash.** Hashes move under rebase; this
  branch proved it (the A/B witness hash `a463ab9` had to be re-resolved to `22e1c5a`).
  **SUPERSEDED** by the squash: subjects do not survive one either. The record names a single
  resolution target instead, `backup/m4.5.1-pre-squash`.
- **The canvas is mirrored into the repo** so git carries its history, the Cursor path staying the
  live file. **SUPERSEDED**: the record is `docs/audit/kickos-codebase-audit.html`, edited in
  place. No live copy outside the tree, no mirror. The `.canvas.tsx` survives only on
  `backup/m4.5.1-pre-squash`.
- **Non-goals are appended to the existing `## North star` section of
  `docs/reference/architecture.md`**, not given a new document, because that section already states
  all three goals. **LANDED** as `### Non-goals -- seL4 machinery deliberately NOT adopted`
  (`m4.5.1: state the seL4 machinery KickOS does not adopt, and the arithmetic refuting it`,
  79b7a37): all four, each with its arithmetic.
- **Sequencing: M4 driver breadth and M5 SMP wait behind goal 1** (fleet flip,
  `arch_periph_enable`, `kos_cap_narrow`). Both multiply capability and memory complexity on a
  fleet that still defaults to privileged root, so doing them first widens the surface that goal 1
  then has to confine. **LANDED** in `roadmap.md` under `## Next` (`m4.5.1: put M4 driver breadth
  and M5 SMP behind goal 1`, a5fc422).
- **Selftest tiering (core tier + optional tiers) was to be considered for the two 64 KiB boards --
  but MEASURE FIRST, and the measurement says it is not needed today.** See below.

### The tiering measurement (this is the "measure first" result)

The premise was that the fleet defaults to `Debug` with no optimisation, inflating flash ~26%. It
is **already handled**: `CMakeLists.txt:137-145` applies `-Os` across the whole tree for exactly
`f302nucleo` and `bluepill-c8` when `KICKOS_ENABLE_SELFTEST` is on, described in-file as a holding
measure pending N16. At branch tip (`text + data`, against 64 KiB of flash):

| Board | flash used | free | headroom |
| --- | --- | --- | --- |
| `bluepill-c8-st` | 51,540 B | 13,996 B | 21.4% |
| `f302nucleo-st` | 51,716 B | 13,820 B | 21.1% |

**Tiering is unnecessary: both boards carry the fleet-uniform suite with ~14 KiB spare.** The
revisit trigger ("only if headroom actually erodes") has **partly fired** -- 1,636 bytes went on
`bluepill-c8-st` and 1,644 on `f302nucleo-st` against the M4.5.1 merge baseline, attributed
symbol-by-symbol in `design-flash-footprint.md` section 9: two new selftest cases
(`t_bus_device_slots` + helpers, `t_reboot_denied`), a real +88 in `domain_for` from the
one-holder-per-MMIO-window check, and `MAX_TESTS` 64->128, whose 512 bytes land on RAM rather than
flash. 21% is still ample, so tiering stays unbuilt.

**The narrower N16 question now has a measured answer: per-preset build types**
(`design-flash-footprint.md` section 10). `CMAKE_BUILD_TYPE=MinSizeRel` is byte-identical in
`.text` to an explicit `-Os`, and its `-DNDEBUG` is inert in this tree -- there is no `NDEBUG`
reference anywhere and no `assert()` outside the `KICKOS_DEBUG` guards -- so its only cost is
losing `-g`, recoverable with `-DCMAKE_C_FLAGS_MINSIZEREL="-Os -DNDEBUG -g"`. It also **inverts the
block's stated cost**: `CMakeLists.txt:139-140` gives that cost as "the `-st` kernel is no longer
codegen-identical to the same board's non-st build", and measured kernel-side on `bluepill-c8`
`hello` the two differ by **13,505 bytes as shipped** and by **114 at `-Os`** (the flag's genuine
content). The unoptimised default is what creates the divergence; widening `-Os` nearly closes it,
which matters because silicon witnesses are taken on `-st` images. **LANDED**: the four MCU base
presets and the sim preset build `MinSizeRel`, `-g` is re-added under that config, and the RP2040 /
RP2350 optimised-build defect that blocked it is fixed. The consequence for the existing witnesses
is the fleet re-witness pass under M4.5.5.

### One new finding: guards that exist but assert almost nothing

Filed as one item because the **pattern** is the point -- a check that is present, green, and
carrying almost no information is worse than an absent one, because it consumes the attention that
would have gone to writing a real check.

- [ ] **Replace the `.bss`-emptiness linker assert and the vacuous `kernel_ctor_placement` with one
      post-link ELF check.**
      - `ASSERT(_ebss > _sbss)` in the linker scripts only fires when kernel `.bss` is **entirely
        empty**, which needs all four archive selectors to fail at once. It misses the far likelier
        **partial** case: one KickOS archive renamed, or a new one added and not listed in all
        **eleven** scripts. That library's writable state then sits silently inside the app's
        granted window -- an isolation hole that the assert reports as fine.
      - `kernel_ctor_placement` passes **vacuously fleet-wide** (same class: green, asserting
        nothing).
      - **Proposed fix, in an idiom the project already uses** (`check_kernel_ctor_placement.sh`,
        `check_oot_export.sh`): a post-link ELF check asserting that **no symbol from a
        kernel-owned object lands inside `[__kickos_appdata_start, __kickos_appdata_end)`**. That
        catches the partial case the linker script structurally cannot.
      - **Why not fix it in the linker script:** GNU ld cannot be asked whether an input selector
        matched anything, so the in-script version can only ever approximate. This is a limitation
        of the tool, not of the attempt -- worth recording so the next person does not retry it.

### Remaining queue, in dependency order

Ordered so each step's input exists when it starts. Items 1-2 are cheap and unblock judgement;
the sweeps go last because they touch everything and would conflict with any of the above. Closed
items keep their number and are struck through rather than removed, because they are cited by
number: the record and the XMC entry under Blockers below both point at **item 5**.

1. ~~**Measure `-Os` on the tight boards**~~ -- DONE above. Outcome: **tiering not needed.** The
   narrower question of how `-Os` should be expressed is answered above too, and landed:
   per-preset build types (`MinSizeRel`).
2. **Selftest tiering** -- **do not build it.** Headroom has since eroded by ~1.6 KiB to 21.4%,
   which is a partial fire of that trigger and still ample. Kept in the queue only so the next
   reader sees it was considered and refuted by measurement, not forgotten.
3. ~~**Non-goals into `docs/reference/architecture.md`, appended to the existing `## North star`
   section.**~~ -- DONE (79b7a37). All four landed with the arithmetic that refuted them (no untyped
   memory / `Retype`; no CNodes; no derivation tree; no per-instance capabilities), under
   `### Non-goals -- seL4 machinery deliberately NOT adopted`, and the common thread is stated: the
   16-slot ceiling with 9-handle boards under it. **That section's `read`/`open`/`socket` sentence
   stays alone** -- the maintainer reads it as a design rule, not a status claim. It was not
   touched, and should not be by a later pass.
4. ~~**Record the sequencing note** (M4 driver breadth and M5 SMP behind goal 1) in `roadmap.md`.~~
   -- DONE (a5fc422), as a block quote under `## Next`.
5. **Move the XMC USIC bring-up into the granted driver thread, and add the privileged configure
   seam it needs for FDR/BRG/CCR.** Both halves, not one: the placement move is necessary and the
   three PV-write-only stores are a measured hardware refusal -- see the entry under Blockers
   below. Unblocks `xmcssc` on a flipped board.
6. **`stm32f103` `arch_mpu_min_region()` override.**
7. **Re-point `kernel_ctor_placement` at the `cxxtest` ELF** (it is vacuous where it is now; see
   the finding above -- these two are the same problem and can land together).
8. **CI hygiene set, minus clang-format** (which is decided against).
9. **Branch-wide comment sweep** -- last, because it touches everything.
10. **Commit-message reword as a SEPARATE step after the sweep**, not folded into it.
11. **Record citation pass** -- after the record edits, so it runs over the final text.

## M4.5.1 -- kernel audit follow-ups (2026-07-26) -- COMPLETE except S4 (2026-07-27)

Findings from a code audit of the kernel, all rated **Medium or below** -- none is a live
escalation or a fleet blocker. They are bound-the-unbounded / be-honest-about-the-error
hardening, roughly ordered by exposure. Six of seven landed; the seventh turned out not to
be implementable where it was filed, and says so in code.

Verified on the host sim and QEMU only, each with a gate checked to FAIL first. **These commits
have not yet been through CI** -- but the branch under them has: the maintainer reports **CI green
at `16d89a0`**, the tip before this batch, covering the branch's first 33 commits. Every commit
that touches `.github/workflows/` is at or before it, so the fleet-wide `-Werror` and the rest of
the pipeline are now observed on CI's pinned 15.2.rel1 rather than argued from a local 15.3.rel1.
Uncovered: everything after `16d89a0` -- stage 0/1 of unprivileged-root, this batch, the book
work, the stage-2 flip and its silicon record, and the record passes over all of it. That is
**49 commits as of this commit**, derived as `git rev-list --count 16d89a0..HEAD` on a branch that
stands 82 commits from `master`, with `16d89a0` at position 33 by the same count against
`master..16d89a0`. **Take the command over the number**: it grows with every commit until CI runs
again. The figure this replaces (21) was correct at `3e8ed10` and went stale when the two transport
branches were replayed in, which is the whole reason the method is written down here. CI status
itself is the maintainer's report, not checked from here (`gh` unauthenticated, and `ci.yml`
triggers `push` only on `master`).

- [x] **Bound the semaphore `count`** -- `m4.5.1: bound the semaphore count` (66280c1). The hole
      was **sharper than filed**: `sem_create` validated nothing,
      so the overflow was not "enough posts" but *one* -- `kos_sem_create(INT_MAX)` then a
      single post. Now `sem_create` refuses an initial outside `[0, KOS_SEM_COUNT_MAX]`
      (-KOS_EINVAL) and `sem_post` refuses at the ceiling (-KOS_EOVERFLOW, a new code); the two
      ISR posters ignore the refusal, matching their coalescing contract. Gate: the two arms of
      `sem_destroy`, each checked to fail on its own.
- [x] **Read `console_chip_writers()` under `IrqLock`** -- `m4.5.1: read the console writer
      count under IrqLock` (127dae2). One load under the lock; no livelock,
      since the drain yields between polls rather than spinning inside a critical section.
      NOT gated: reproducing a torn read needs a race the suite cannot schedule -- the change
      is an argument, not a demonstration, and that is worth saying.
- [x] **Split `domain_for`'s refusal** -- `m4.5.1: split domain_for's refusal reasons`
      (296e030). An out-parameter errno (EPERM inadmissible / EINVAL malformed /
      ENOMEM exhausted), forwarded verbatim by `thread_spawn`. The spawn-boundary pre-check
      that existed only to recover the errno the chokepoint could not express is gone, so the
      duplication went with the fix. Gate: `grant_reserved`, checked to fail on ENOMEM.
- [x] **Debug asserts on the intrusive list** -- `m4.5.1: assert list membership on push_back
      and unlink` (0022c82). New `KICKOS_DEBUG` knob, default OFF (board
      images byte-identical); **the sim preset turns it ON**, so the guards run against the
      whole suite on every sim run instead of rotting unbuilt. Both checked to fire.
- [x] **Bound the spin in `wq_confirm_resume`** -- `m4.5.1: bound the resume spin` (6961989).
      Measured while gating it: the loop takes **zero
      iterations on the sim AND on qemu armv7m**, so the barrier the comments describe at
      length has never been observed to spin even once -- see the new finding below. Proven by
      holding the epoch so the switch never lands: an infinite hang becomes a panic on both.
- [ ] **Implement `__malloc_lock`/`__malloc_unlock`** -- **NOT DONE, and it cannot be done
      here.** `m4.5.1: document why the malloc lock stays a no-op` (291815c)
      replaces the vague FOOTGUN comment with three measured facts: newlib takes this lock
      RECURSIVELY (in the linked `cxxtest` image `_free_r` holds it and calls
      `_malloc_trim_r`, which takes it again), so a non-recursive lock self-deadlocks and a
      re-entry detector fires on a legitimate free; a recursive lock needs thread identity and
      userspace has none; and capabilities are per-task, so there is no lock object two threads
      can name and no reserved index left to seat one at. "An IrqLock-equivalent" is not
      available -- this file is userspace, which is the whole point of the surrounding
      milestone. Real fix: the per-thread libc state / TLS item under "Later -- not M1", or a
      kernel-held lock behind a syscall (a designed change).
- [x] **Guard the `uint16_t` domain refcount** -- `m4.5.1: bound the domain refcount at the
      thread pool` (b72e9e5). The count is live threads and nothing else, so
      a `static_assert` proves the wrap unreachable, as the object-refcount arrays already do.
      Bound against the thread HANDLE INDEX ceiling, not `KICKOS_MAX_THREADS`: a board sets the
      latter to 2..16, so an assert on it could never fire. Checked live by widening
      `INDEX_BITS`. Plus a `KICKOS_DEBUG` assert for the way the bound would break first.

## M4.5.1 -- found during the CI / out-of-tree hardening work (2026-07-26)

- [x] **Split `_sbrk` into its own TU.** -- `m4.5.1: move _sbrk out of the force-linked TU`
      (c539d1c). **The split alone is necessary but not sufficient, which
      the plan did not anticipate**: the g++ driver appends libc and libstdc++ AFTER everything
      CMake emits, so an on-demand `_sbrk` member sits behind the linker by the time `_sbrk_r`
      asks -- measured as `undefined reference to _sbrk` on EVERY allocating image fleet-wide.
      The toolchain runtime therefore joins the rescan group, which moves the group from
      `kickos_core` onto the two posture leaves (the freestanding leaf must NOT name libstdc++,
      and CMake forbids one target's closure carrying a library in two groups); `kickos_cxx_rt`
      names its include providers directly as a result.
      Fail-loud PROVEN by construction, with both heap bounds deleted from `mps2.ld`: an app
      calling `malloc` fails the link naming `_kickos_heap_start`, an app using `new` fails the
      same way on the full-C++ leaf, a non-allocating app still links (the property that was
      impossible before -- the force-linked reference broke those too), and a real board is
      unaffected. Both out-of-tree export gates pass, so the exported package still links.
      Follow-on now unblocked: nrf51 can drop the zero-length `.userheap` it only kept for this.
- [x] ~~**Override `arch_mpu_min_region()` to 0 in `chip_stm32f103.cc`.**~~ DONE (`6d49e14`),
      and `chip_stm32f302.cc` with it: both parts have no MPU, so they were paying the v7-M pow2
      alignment tax for regions nothing programs.
- [ ] **Re-point `kernel_ctor_placement` at the `cxxtest` ELF.** The gate passes fleet-wide, but
      vacuously: every app it inspects links an empty `.kickos_app_init_array` window, so the
      script takes its early-out without ever dereferencing a pointer. `cxxtest` is the one image
      with real app ctors -- point the gate at it so it actually asserts something.
- [ ] **Console bytes lost on shutdown.** On a service-list board, root returning while the
      userspace driver still holds queued bytes loses them: `console_tx_flush_sync()` is a no-op
      (the ring was disarmed by `console_tx_deinit`) and `arch_shutdown` then spins forever.
      Shutdown has to drain through the owning driver, not the retired kernel ring.
- [x] **Give `thread_spawn`'s two READ checks the same static-data fallback the write side just
      got.** -- `m4.5.1: use user_readable_ok for thread_spawn's two read checks`
      (fe68c72). One word at each site; byte-identical on an enforcing backend, where the
      fallback arm returns false. Gate: the `authority_cap` worker's params struct and grant
      array became GLOBALS -- exactly the shape that failed -- and the comment recording the bug
      as a local workaround is gone with it. A **fourth** spawn probe was needed for the array:
      the other three are all refused by an authority check that runs before the delegation
      loop, so none of them read `caps[]` at all and the array site would have shipped
      unexercised. Each of the two sites checked to fail on its own.
- [ ] **The fleet-uniform selftest image no longer fits the smallest flash -- NEEDS A DECISION,
      and the kernel-audit batch forced it.** Re-measured 2026-07-27 at `7eb9592`: the headroom
      was **104 bytes** on `f302nucleo-st` and **292** on `bluepill-c8-st`, not the 96/284
      recorded here (measure from the program headers, not `size`'s text+data). The seven
      kernel-hardening items then cost **184 bytes** on f302nucleo, so the board stopped linking
      on KERNEL code, before a single new test -- which is not the shape this item predicted.
      **The measurement that should decide it:** the same f302nucleo image is **48,848 bytes at
      `-Os`** against 65,720 at the fleet default -- and the fleet default is `CMAKE_BUILD_TYPE=
      Debug`, i.e. `-g` with **no `-O` at all**. So the ceiling is ~74% real code and ~26%
      unoptimised codegen, and the choice this item poses (shed coverage / accept build-only)
      has a third answer nobody had costed: 16.7 KiB comes back for free.
      **Holding measure landed** (`m4.5.1: build the two 64 KiB boards at -Os`, e946003): `-Os`
      across the selftest tree for these two boards only, `-g`
      kept, no test dropped, one block to revert. What it costs, stated plainly: the `-st`
      image's kernel is no longer codegen-identical to the same board's non-st build, which the
      selftest `-Os` block deliberately preserved when it optimised the app's own TUs only. Both
      boards are build-only for the suite (no bench unit, no QEMU model), so what they provide
      is a link check and this keeps them providing one. **Widening `-Os` to the fleet, or
      accepting either board as suite-exempt, is the maintainer's call.**
      Unchanged: `TAP_CHECK` embeds `__FILE__` plus its stringified condition, so assertion
      count is a flash cost; CI does not catch any of this, because its ARM matrix builds the
      PLAIN board presets, not the `-st` ones. Same class as `tests/tap/tap.cc`'s
      `MAX_TESTS = 64` (`tap::add` drops silently past the ceiling rather than failing the
      build).
## M4.5.1 -- found during the kernel-audit batch (2026-07-27)

- [ ] **The resume barrier has never been observed to spin.** Bounding
      `wq_confirm_resume` (6961989) needed a gate, and calibrating one measured that the loop
      takes **zero iterations on the host sim AND on qemu armv7m** across the whole suite: a cap
      of one iteration does not fire. The sim is expected (its switch is synchronous inside
      `wq_block`), but ARM is not -- the long comment at `sync.h` explains that `arch_switch`
      only PENDS PendSV and `arch_irq_restore` has no ISB, so 1-2 instructions retire on the
      not-yet-switched thread. Either the switch always lands before the caller reaches the
      barrier (a call and a return later), or the window is narrower than the comment implies.
      Worth settling, because the barrier is on the mutex/endpoint wake path on every board and
      is currently justified by an argument nothing exercises. Silicon witness still owed --
      timing is exactly what an emulator does not reproduce.
- [x] **The whole fleet builds unoptimised: CLOSED.** The four MCU base presets and the sim preset
      set `CMAKE_BUILD_TYPE=MinSizeRel` and every board inherits it; `-g` is re-added under that
      config (`CMakeLists.txt:139`), because an image with no debug info cannot be witnessed on
      silicon. The two-board `-Os` holding block is deleted, subsumed by the fleet default. The gap
      it closed, measured on f302nucleo's selftest image: 64,408 bytes unoptimised against 47,120 at
      `-Os`, a 26.8% reduction with no source change (`design-flash-footprint.md` section 3, which
      measures all fourteen boards both ways). One caveat stands: the switch's I-cache footprint
      plausibly moved too, but **no instruction-cache or cycle measurement was ever taken**, so that
      is not to be cited as measured (`design-flash-footprint.md` section 10). Consequence: every
      published bench figure measured unoptimised code, and every silicon witness except the
      2026-07-29 `f302nucleo` captures, which are the first taken on `-Os` -- see the fleet
      re-witness pass under M4.5.5 below. No bench has been run optimised.
- [x] **`picopi` and `pizero2350` cannot build an optimised image: FIXED**, which is what unblocked
      the fleet build type above.
      `arch/arm/chip/rp2040/chip_rp2040.cc:80-81` and `arch/arm/chip/rp2350/chip_rp2350.cc:96-97`
      -- the bootrom-header `r8`/`r16` accessors, `*reinterpret_cast<volatile uint8_t*>(addr)` --
      fail `-Werror=array-bounds` ("array subscript 0 is outside array bounds of
      `volatile uint8_t [0]`") at `-O2` and at `-Os`, and compile clean at `-O0` and `-O1`.
      Reproduced both by compiling each TU directly and as a `pizero2350-st` build at
      `-DCMAKE_BUILD_TYPE=MinSizeRel`; the diagnostic count varies with toolchain version, the
      pass/fail split by optimisation level does not. The accessors exist only under
      `KICKOS_ENABLE_SELFTEST` (they serve `arch_reboot`), which is why the default build and the
      flag-off `-Os` build both pass -- flag-gated code is exactly the code no default build
      compiles. **Cause, confirmed by flag bisection:** GCC treats the first
      `--param=min-pagesize` bytes of the address space as unmapped, so a constant-address
      dereference down there is an out-of-bounds access to it -- and on both chips the bootrom
      the accessors read *is* at address 0. `--param=min-pagesize=0` silences all four
      diagnostics on the rp2350 TU with nothing else changed. The remedy is a
      `#pragma GCC diagnostic ignored "-Warray-bounds"` scoped to the two accessor bodies
      (`chip_rp2040.cc:80-82`, `chip_rp2350.cc:96-98`), which compiles clean at `-Os` and `-O2` on
      both TUs; a global `--param` would blind the whole tree to a real diagnostic class. The
      optimised accessors are **disassembly-verified only, never executed** -- folded into the
      M4.5.5 re-witness pass below.
- [ ] **LTO does not link, on any board.** `-flto` fails every app with
      `(.isr_vector+0x4): undefined reference to Reset_Handler`. The handler is defined in a C++ TU
      and referenced **only** from the vector table in an assembly object, so the LTO plugin sees
      no reason to keep the definition (measured on `bluepill-c8`, `design-flash-footprint.md`
      section 12). So LTO is not an available footprint recovery today. It is not on the `-Os` path
      N16 needs, so it gates nothing -- filed so it is not rediscovered. **Record only: no fix
      attempted.**
- [ ] **`kickos_core` no longer carries the archive group.** c539d1c moved the RESCAN group onto
      the `kickos` / `kickos_cxx` leaves, because the two postures need different toolchain
      runtimes in it and CMake forbids one target's closure carrying a library in two groups. A
      consumer linking `kickos_core` DIRECTLY now gets usage requirements but no archives. The
      documented contract already says consumers link a leaf and never core, and both
      out-of-tree export gates pass -- but core is still in the export set, so the contract is
      now load-bearing where it used to be advice. Either state it in the exported package or
      make linking core alone a configure-time error.
- [x] **The record cites commit hashes a rebase has rewritten: CLOSED BY CONVENTION (2026-07-28).**
      Found while re-resolving the audit record: of the 56 hashes it cited, 37 named commits
      unreachable from `HEAD` after the message-trim rebase. 32 were remapped (patch-id, or a
      unique exact subject); 16 squash casualties stayed flagged. The M4.5.1 squash to eight
      theme commits then made per-citation conversion a losing game -- a subject survives a
      reword but not a squash -- so the convention is a resolution TARGET instead: every hash
      and subject this branch's records cite resolves against `backup/m4.5.1-pre-squash`
      (tree-identical full history), stated once in the audit page's header. The standing
      practice: create the backup ref BEFORE any history edit, and say in the record which ref
      citations resolve against. This file's own six M3-era hashes resolve against
      `backup/m3-pre-squash` the same way.
- [ ] **Reclaim `arch_ram_alloc`'s alignment run-up** (M5 allocator work). The bytes skipped
      ahead of each allocation to satisfy its alignment are dropped on the floor -- which is why
      boot-stack allocation *order* is load-bearing today (idle must be allocated before root).
      Folding the run-up back into the free space removes that ordering constraint. Subsumed by
      the general freeing allocator under "Later -- not M1".

## M4.5.1 -- CI hygiene (2026-07-26)

- [ ] **Reduce `--repeat until-pass:4` to 2 on the four QEMU gates** -- or better, fix the timing
      root cause that made 4 look necessary. Four attempts hides a gate that fails most runs.
- [ ] **Tighten the sim `mpu_fault` failure regex** to match what `tests/check_qemu_mpu_fault.sh`
      already asserts; the sim side accepts more than it should, so it can pass on the wrong fault.
- [ ] **Add link-only CI jobs for `f302nucleo-st` and `bluepill-c8-st`** (maintainer-confirmed
      2026-07-27). CI builds only the plain presets, so a selftest image that overflows 64 KiB of
      flash goes unnoticed until someone builds the `-st` preset by hand. These two boards are
      build-only for the suite anyway -- a link check is exactly and only what they provide, so a
      link-only job is the whole value at none of the runtime cost. Both link today with ~14 KiB
      spare (measured in the session record above, and shrinking), and the job is what keeps that
      true. Note for whoever adds it: `-Os` is applied to precisely these two boards under
      `KICKOS_ENABLE_SELFTEST` (`CMakeLists.txt:141`), so the job must configure with the selftest
      **on** or it will not measure the image that actually risks overflow. A local sweep of all
      thirteen `-st` presets found one real link break that the seven emulator gates could not
      (`esp32-wroom-st`, Xtensa, missing `kickos_arch_mpu_commit`), which is the argument for
      widening this beyond the two tight boards later.
- [x] ~~**Pin a clang-format version, reformat, add a gate.**~~ **DECIDED AGAINST 2026-07-27
      (maintainer).** Not wanted as a CI gate. The checked-in `.clang-format` is a **per-file
      starting point** -- something to run on a file you are already editing if you want it -- and
      **not a target state the tree is supposed to converge to**. So the measurement that prompted
      this item (144 of 289 tracked C++ files diverge under clang-format 21.1.8) is not evidence of
      drift; it is the expected state of a config used that way, and re-measuring it will not change
      the answer.
      Recorded so it is not re-filed. The reformat would also not have been mechanical: the config
      says `IndentExternBlock: NoIndent`, `user/src/syscall_stubs.cc` obeys it and
      `kernel/init/kmain.cc` does the opposite, so gating would have restyled every `extern "C"`
      block in the kernel as a side effect of a formatting decision nobody made deliberately.
      Record finding **T9** closes with the same reasoning.
- [ ] **Add a licence-header gate.** The premise that it could wait -- "coverage is 100%, so this
      is cheap to hold" -- turned out to be false: re-measuring on 2026-07-27 found
      `docs/design-rp2350-mpu-armv8m.md` carrying no SPDX identifier while all 26 sibling design
      records did. Header added, so coverage is 534 of 536 tracked non-binary files (the rest are
      `.gitignore` and the six JSON presets, none of which can carry a comment). The drift the gate
      exists to catch had already happened unnoticed, which is the argument for adding it now.
- [ ] **Pin GitHub Actions to commit SHAs**, not moving tags -- a tag is a supply-chain seam
      controlled by someone else.
- [ ] **Wire the telemetry runtime gates into CI.** The `sim-telem` / `qemu-telem` presets exist
      and work, but no job runs them, so telemetry can rot without anything going red.
- [ ] **Move `.claude/` from `.git/info/exclude` into `.gitignore`** -- `.git/info/exclude` is
      per-clone, so every other clone sees the directory as untracked noise.

## `kos_reboot` (reboot-to-bootloader) -- BUILT (2026-07-28)

`KOS_SYS_REBOOT = 38`, `AUTH_SYSTEM`-gated, behind an `arch_reboot` seam whose weak default is
`-KOS_ENOSYS`. Case, weak symbol, wrapper and app are all inside `KICKOS_ENABLE_SELFTEST`, so a
production image carries none of it. The decision to share shutdown's bit, and its counter-argument,
are recorded in `docs/design-unprivileged-root.md` section 9.

Backends: rp2040 `'UB'` -> `_reset_to_usb_boot(0, 0)`; rp2350 `'RB'` -> `reboot` with
`BOOTSEL | NO_RETURN_ON_SUCCESS`; imxrt1062 `bkpt #251` -> the MKL02 presents HalfKay. Every
other chip declines through the weak seam. The earlier instruction to read `*(uint8_t*)0x13` and
branch on it is **refuted**: both datasheets forbid using that ROM build byte to locate
functions, and the three magic bytes at `0x10` are the whole validity test they give.

Witnessed: the refusal path -- selftest `reboot_priv` (an unprivileged caller gets `-KOS_EPERM`)
plus `sim_reboot_declined` / `qemu_reboot_declined` on `apps/rebootdemo`. The reboot itself is
**witnessed on RP2350** (pizero2350, see `docs/reference/boards.md`); the RP2040 and imxrt1062
backends are still bench debt (see the bench item below).

## Unprivileged ctors and `main` -- start unprivileged, holding capabilities (2026-07-27)

**Reasoning, blockers and the boards this does not work on are now filed as
`docs/design-unprivileged-root.md` (ACTIVE).** This section stays the actionable checklist.

A design investigation superseded stages 2-8 of the old plan: root should **start unprivileged
holding capabilities** rather than start privileged and demote, so there is no demotion to build.
`thread_regions_recompose`, `KOS_SYS_DROP_PRIV`, its per-arch backends and Xtensa-last are
**deleted, not deferred** -- the region set is composed once in `thread_create`
(`kernel/thread/thread.cc:94-134`) from a privilege that never changes, and the rest existed only
to manage a transition this design does not have. The reason, recorded once: **every ISA with a
ring split already encodes thread privilege in the fabricated first frame and restores it on the
first switch-in** -- armv7m `ctx.npriv` (`arch/arm/armv7m/arch_armv7m.cc:111-119`), armv6m
(`arch_armv6m.cc:102-108`), rv32imac `MSTATUS_MPP` (`arch_rv32imac.cc:143-158`), rxv3
`PSW_THREAD_USER` (`arch_rxv3.cc:279-289`). The two ports without a ring split store nothing:
xtensa (`arch_xtensa.cc:271-273`) and the sim, whose `arch_context_init` takes `privileged` and
**discards** it (`sim.cc:758-761`) -- privilege there is the thread's region set plus a
per-context mid-syscall raise counter, so the sim has no CPU-mode axis at all. Starting root
unprivileged therefore needs **zero new assembly on any port**, and Xtensa comes along free
rather than last. `drop_priv` survives only as a **contingent, much smaller** item: it is the
only mechanism giving "privileged bring-up then self-confinement for life", which is what the
blocked bring-up bodies below want -- in scope only if the `arch_periph_enable` seam (stage 3)
proves insufficient.

The old stage 1 (arena-allocated boot stacks) LANDED on the M4.5.1 branch -- see
`m4.5.1: take the root and idle stacks from the arena, not .bss`. The new stages, in dependency
order:

**Stage 0 -- independent prerequisites, no behaviour change. COMPLETE** (all three were real bugs;
each landed with its own gate, and the whole stage costs 276 B of flash and 8 B of `.bss` on
frdmk64f+blink -- the 8 B being the argv struct itself).
- [x] **Move the argv handoff out of kernel-stack storage** -- see `m4.5.1: move the init argv
      handoff off the boot stack`. `root_entry` read `argc`/`argv` from a `kmain` frame local on the
      boot stack, *outside* the arena, so an unprivileged root would fault on its first statement
      after the ctor walk on every enforcing board -- and **the sim cannot reproduce it** (`kmain`'s
      frame is host stack). Now `kickos_init_args` in `libkickos_user.a`, which every enforcement
      linker script routes into the `.appdata`/`.appbss` grant. NOT the init provider's archive: a
      build naming its own `KICKOS_INIT_PROVIDER` must not be able to remove the definition.
      Placement verified by symbol address on all five enforcement images, not assumed.
- [x] **Add `KOS_SYS_SHUTDOWN(status)`** -- see `m4.5.1: end the system through a syscall, not a
      direct kernel call`. Syscall **36**; privileged-only for now, which is exactly who can end the
      system today, and stage 1 widens it to `AUTH_DEVICE`. Still the natural home for the "console
      bytes lost on shutdown" item above, which now has one owner instead of two call sites. Gate:
      selftest `shutdown_priv`, checked to FAIL (run truncates mid-suite) with the gate removed.
- [x] **Add a writable arm to `user_writable_ok`** -- see `m4.5.1: give user_writable_ok the
      static-data arm its read twin has`. New `arch_user_data_writable` seam. **The hole was wider
      than recorded here:** it is not just the five chips with no MPU *backend* (stm32f103,
      stm32f302, nrf51, sam3x8e, esp32 -- and `sam3x8e` **has** an MPU on silicon, a Cortex-M3
      revision 2.0 unit; what is missing is the `mpu.cmake` port, and the `due` unit is retired so
      it cannot be witnessed either way) -- **the host sim has it too**, despite building
      `KICKOS_HAVE_MPU=1`, because its globals live in the host image rather than the mprotect'd
      arena. So the fix could not key
      on `KICKOS_HAVE_MPU` alone and the sim carries its own arm. Gate: selftest `writable_global`,
      confirmed failing on both broken postures beforehand. Also note the suite had already
      *worked around* this bug in `ep_recv_worker`'s comment without it being filed.

**Stage 1 -- the authority capability, root still privileged. COMPLETE** (see
`m4.5.1: gate the eight authority syscalls on a capability, not only on privilege`; +374 B flash
on frdmk64f+blink, no `.bss`).
- [x] **Added `CapType::CAP_AUTHORITY`**, seated at `KOS_CAP_AUTHORITY` (index 2, already
      reserved, and now spelled for what it holds) carrying the **five unused bits of
      `CapEntry.rights`**: `AUTH_MEMORY` (ram_alloc + MMIO grant), `AUTH_PINMUX`, `AUTH_CLOCK`,
      `AUTH_IRQ`, `AUTH_DEVICE` (console publish, shutdown, and later reboot -- **not** periph
      enable, which is possession-gated; see stage 3). Zero dynamic slots on
      every board. Poolless, so it resolves by reading the reserved slot and never via
      `cap_resolve_e`; `obj_ref_inc`/`obj_ref_drop`/`obj_close_protocol` each gained an explicit
      no-op arm rather than relying on a `default:` that asserts.
      **Seated WITHOUT `CAP_TRANSFER`**, which makes it non-delegable rather than merely
      undelegated -- so index 2 has exactly one writer, the kernel, and the delegation-packing
      collision below is unreachable instead of unlikely.
- [x] **Converted the gates to `cap_check_authority(caller, AUTH_*)`** -- that call and nothing
      else, with the privileged-implies-everything arm inside the function, so the rule is stated
      once instead of at every site. Behaviour-neutral: root is privileged, so every
      gate takes the same arm as before. **Eight sites converted, and there turned out to be
      nine authority decisions**: `grant_region_admissible`'s DEV arm was missed here and found by
      stage 2 below. Enumerate the decisions, not the call sites -- a count that is right the day
      it is written is what lets the next one hide. `kos_thread_params` gained an `authority`
      byte (in the padding after `cap_count`, so the struct does not grow) that narrows only --
      refused together with `cap_count >= 2`, since a second delegated cap would land on the
      authority slot.
- [x] **Gate: selftest `authority_cap`.** An unprivileged child holding `AUTH_PINMUX` and nothing
      else gets PAST the pinmux gate (which then answers for itself) while being refused at a
      gate it holds no bit for -- so the bits are shown independent, not one lump. Confirmed to
      FAIL with the grant removed, which is what makes it cover the arm that would otherwise ship
      unexercised until stage 2 (the `kernel_ctor_placement` vacuity trap).

**Stage 2 -- flip per board**, behind a build-enforced `KICKOS_ROOT_PRIVILEGED` knob (default ON,
**NOT a weak symbol**: opting out of the boundary must be visible in the board's build, not
silently satisfied by a link-time override).
- [x] **The knob** -- see `m4.5.1: add KICKOS_ROOT_PRIVILEGED, and seat root's authority cap when it
      is off`. CMake option, always emitted as `0`/`1` (so `#if` is `-Wundef`-clean), printed at
      configure time and carried to out-of-tree consumers as a usage requirement of `kickos_core`.
      OFF creates root unprivileged and seats `CAP_AUTH_ALL` at `KOS_CAP_AUTHORITY` after
      `thread_create` (which zeroes the TCB) and before `sched::start`. The banner reports the
      posture on the `mpu` line as a *concatenated literal*, so the default posture adds no string
      and no runtime branch there. Cost to the no-MPU tight boards, measured against `ed78926`
      rather than assumed: f302nucleo+selftest **-4 B** of text, bluepill-c8+selftest **+8 B**. Not
      byte-identical, as first claimed here: the `+8` is `syscall_thread.cc` calling
      `cap_check_authority` where it read `Thread::privileged`, and the `-4` is one store dropped
      from the selftest. Both still link.
- [x] **A ninth authority gate that stage 1 missed, and it blocks the first board** -- see
      `m4.5.1: gate the MMIO grant on AUTH_MEMORY, not on the caller's privilege`.
      `grant_region_admissible`'s DEV arm (`kernel/grant/grant.cc`, Choice 5A) read the caller's raw
      `Thread::privileged`. It is not in the design doc's list of surviving privilege reads, and it
      sits directly on the console-handover path: an unprivileged root holding `AUTH_MEMORY` clears
      the `syscall_thread.cc` gate and is then refused here, so the board goes dark. Now
      `caller_authorized`, resolved at both call sites as `cap_check_authority(current, AUTH_MEMORY)`.
- [x] **`xmc4800-relax` FLIPPED and witnessed on silicon.** Console-only service list added
      (`kickos_services_xmc4800relax_console`), selected automatically for the flipped posture;
      `KICKOS_SERVICE_LIST_ROOT_MMIO` makes the combined list a **configure-time `FATAL_ERROR`** in
      that posture rather than a dark board. Evidence in `docs/reference/boards.md`. The A/B was
      **re-captured post-rebase at `22e1c5a`**, so it witnesses the rebased combination of stage 2
      with the kernel-audit batch, not just the pre-rebase branch.
- [x] **Re-witness the tip on silicon: DONE 2026-07-28 at `75227d4`.** The XMC A/B re-run plus
      the `frdmk64f` SYSMPU regression, six flash-and-capture runs, all signatures matched. The
      two enforcement-path commits the boards left the bench before (`af696e6`, `3c772b9`) plus
      the alignment-gate repair are witnessed by `mem_self_grant` and `mem_self_grant_nonpow2`
      running `ok` under PMSAv7 in both postures and under SYSMPU. Updated boundary table in
      `docs/reference/boards.md`; captures under `.session/n33-rewitness/` (machine-local).
- [x] **`esp32c6-wroom`, `pizero2350` and `rx72m` FLIPPED and witnessed on silicon**, which puts
      the flip across all four enforcement backends (PMSAv7, RISC-V PMP, PMSAv8, RXv3). Evidence
      per board in `docs/reference/boards.md`. `rx72m` needed two prerequisites first: `rxdrv`
      moved onto the fleet driver pattern, and an `arch_pinmux_set` backend covering `PmnPFS` plus
      `PORTm.PMR`.
- [x] **`f411disco` FLIPPED and witnessed on silicon 2026-07-29 at `6646c8e`**, which closes the
      declared stage-2 set. What blocked it was a **pre-existing bench debt rather than flip work**:
      PMSAv7 had never been witnessed on that board at all, so a flip would have had no enforcing
      baseline to discriminate against. Done in two passes for that reason -- enforcement first in
      the default posture (selftest 62/62 with 0 skips, plus an `mpu_fault` cross-domain MemManage
      denial, `CFSR=0x82`, `MMFAR=0x2000b000`), then the A/B. The backend is the shared `stm32f411`
      one, so this also closes the MPU HW debt for the chip and for `blackpill`; `docs/m2-readiness.md`
      no longer carries an unwitnessed enforcement backend. Evidence in `docs/reference/boards.md`.
      Found on the way, NOT fixed: `f411spi` does its SPI1 bring-up (RCC/GPIOA/GPIOE) from `main`,
      so under the flip it faults MemManage on the first store (`RCC_AHB1ENR` @ `0x40023830`,
      witnessed). It is a diagnostic app and not on the gate, so it is stage-3 follow-up
      (`arch_periph_enable`), the same treatment `c6blink` and `rxdrv` needed.
- [x] **`frdmk64f` FLIPPED and witnessed on silicon at `127efb5`, on stage 3 rather than stage 2.**
      Its `k64uart` and `k64dspi` PACR writers (`AIPS0_PACRN` at `0x4000_0064`, `AIPS0_PACRF` at
      `0x4000_0044`) both fall inside `arch_reserved_blocks`'s AIPS0 entry
      `[0x4000_0000, 0x4000_1000)`, so no grant can ever reach them and `arch_periph_enable` was the
      only way in. **The first board to run its FULL service list under the flip** (console
      `k64uart` + SPI `k64dspi`), where every other flipped board is console-only or serviceless:
      `selftest` `1..65` `# all tests passed (2 skipped)`, and `rootfault` denies root's cross-domain
      write (`SYSMPU ISOLATION FAULT: port=3 addr=0x2001a000 master=0 W EDR=0x80000003`,
      `CFSR=0x400`, `HFSR=0x40000000`). Skips are `mpu_privileged_guard` (posture) and
      `mutex_deadlock # SKIP pool too small` (pre-existing, `docs/reference/boards.md`). Root writes
      no MMIO at all on this board now. Evidence in `docs/reference/boards.md`.
- [x] **Per-board gate, and what it actually cost.** Both halves met on `xmc4800-relax` silicon
      under PMSAv7. But the gate as worded is not reachable by the *unmodified* suite, and the
      reasons are worth keeping:
      - The cross-domain half needed a **new** app. `apps/mpu_fault` confines a spawned CHILD and
        says nothing about root; nothing covered root, the thread that runs the ctors, the bring-up
        and `main`. `apps/rootfault` does, and is discriminating in both postures (privileged root
        completes the write and says so), so the fault is evidence rather than a symptom.
      - The selftest half cost **2 skips** when this was written, named on the wire
        (`irq_as_event`, `mpu_privileged_guard`), so a flipped image did **not** satisfy the
        selftest gate. Both halves are now closed, and the split is worth keeping because the two
        skips had different causes. `irq_as_event` was a missing capability, not a posture cost:
        `kos_mem_self_grant` lets root ask for the page it allocated, so it **runs** in both
        postures. Only `mpu_privileged_guard` is genuinely posture-driven -- its subject is the
        privileged posture. The gate now takes an expected-skip list **by name**
        (`EXPECT_SKIPS`) instead of a count, since a budget of 2 would have admitted any 2 skips
        rather than these 2. Measured after both: flipped `sim` skips exactly
        `mpu_privileged_guard`, 59/60 run.
- [x] **`kos_ram_alloc` grants its caller nothing, which left it near-useless to an unprivileged
      root.** Allocation and grant are separate acts: a region becomes reachable by being handed to
      a spawn, so an `AUTH_MEMORY` holder could allocate a page and then not touch it. A privileged
      root never noticed. This was the root cause of BOTH selftest skips and of the `mpu_fault`
      restructure below -- a gap in the capability story rather than three test defects.
      **Decided: an explicit `kos_mem_self_grant` (37), not an implicit grant at alloc.** Alloc must
      stay usable for the allocate-then-hand-off pattern that spends no region, and the caller's
      region table is a hard, small budget (`KICKOS_MPU_MAX_REGIONS`), so spending a slot has to be
      something the caller *asks* for. It is gated on `AUTH_MEMORY`, bounded by that budget, and
      fails loudly with `-KOS_ENOMEM` rather than silently not enforcing -- proved both ways in
      `t_selfgrant` (returning `-KOS_EPERM` fails the test; accepting silently faults the worker).

**Stage 3 -- the blocked bring-up bodies. COMPLETE** (2026-07-29, `127efb5`).
- [x] **Added `arch_periph_enable(uintptr_t base)`**, weak `-KOS_ENOSYS` in
      `kernel/time/clock_select.cc`, covering "ungate the clock and drop the bus-side
      supervisor-protect for the block at this base". Syscall `KOS_SYS_PERIPH_ENABLE = 39`, wrapper
      `kos_periph_enable`. Backends: mk64f (UART0, DSPI0) and stm32f411 (SPI1, clock gate only --
      that bus exposes no privilege-classification register in this tree). ESP32-C6 and RX72M
      deliberately have none: their windows need nothing from the seam, the C6's APM open being a
      boot-time act in `arch_init`. Each backend is a hand-curated table keyed on the **exact** block
      base, never a range, and both writes are DERIVED from `base`, so no caller can name a shared
      block's register or the bit inside it. Retires `k64uart` and `k64dspi`'s root MMIO entirely, so
      `kickos_services_frdmk64f` came off `KICKOS_SERVICE_LIST_ROOT_MMIO`.
- [x] **NOT gated `AUTH_DEVICE`, which is what this checklist previously said. The gate is
      possession.** `caller_holds_mmio_block(base)` (`kernel/syscall/syscall_mem.cc`) requires a live
      `ARCH_MPU_DEV` region whose base matches exactly; privileged callers bypass, as in
      `cap_check_authority`. Deliberately not `user_range_ok` (that funnel asks whether the kernel
      may dereference a user pointer, and passes trivially at `len == 0`); exact base rather than
      containment is what stops a sub-block window reaching a whole-block entry. `AUTH_DEVICE` would
      have handed `kos_shutdown` and reboot-to-bootloader to every unprivileged bus driver in the
      fleet, since the seam's callers ARE the drivers, and every other existing bit carries
      collateral just as unwanted with no free bits left. Possession is sufficient because a granted
      window already means "may enable, configure and disable this device" wherever the silicon
      permits it directly -- the XMC `KSCFG` case is the precedent, and the seam brings K64F and F411
      to that parity. Rationale in full: `docs/design-unprivileged-root.md` sections 7 and 9.
- [x] **The call site is the DRIVER, not root**, which is what makes the seam's bound a fact rather
      than an aspiration: root holds no DEV region on any board (`ARCH_MPU_DEV` is attached only in
      `domain_for`, reached with MMIO only from `thread_spawn`, and `KOS_SYS_MEM_SELF_GRANT`
      hardcodes `R|W`), so a holder of one window can only ever ask the kernel to configure that
      window's device.
- [x] **No PIT entry on K64F, refused by design.** One AIPS `PACR` slot covers a whole 4 KiB block,
      so opening slot 55 for the legitimately granted PIT ch2 window (`0x40037120`) would also expose
      the chained ch0+ch1 pair carrying `arch_clock_now` -- the registers `arch_reserved_blocks`
      protects by address (`{PIT_BASE, 0x120}`). That base answers `-KOS_EINVAL`. An entry exists
      only where the bus gate's granularity is CONTAINED by the block the window covers. The
      system-wide reach of an opened slot is the already-documented hardware ceiling
      (`docs/book/peripheral-isolation-and-the-hardware-ceiling.md`,
      `docs/reference/architecture.md`, `docs/design-m3-console-handover-stageii.md`), not new.
- [x] **New `arch/arm/chip/mk64f/regs/aips.h`** gives the three formerly open-coded slot -> (`PACR`
      register, `SP` bit) derivations one home, with `static_assert`s pinning slots 106 (UART0), 44
      (DSPI0) and 55 (PIT, derivation coverage only, no entry). It caught that `PACR` offsets are
      **not** contiguous: groups 0..3 at `0x20`..`0x2C`, `0x30`..`0x3C` reserved, groups 4..15 at
      `0x40`..`0x6C`, so a naive `0x20 + group * 4` names the wrong register for slot 44.
- [x] **`stm32f411` pinmux gained an encoding field.** `func` bit 8 `PINMUX_OUT_HIGH` presets an
      output high; a non-output mode carrying the bit is refused `-KOS_EINVAL`. `BSRR` is written
      BEFORE `MODER` (proven in disassembly: `str [r3,#24]` precedes `str [r3,#0]`), because a `BSRR`
      set on a still-input pin is inert while the reverse order asserts the `ODR` reset level first.
      `f411spi` needs it to hold the onboard gyro's `PE3` chip-select deasserted so the gyro's SDO
      stays tri-stated.
- [x] **Gate: selftest `periph_enable_unheld`** (`ok 47`), the negative arm. The POSITIVE arm has no
      in-env carrier at all -- the host sim can never hold a DEV region, `arch_mpu_region_encodable`
      returning false unconditionally (`arch/sim/sim.cc`) -- so it is witnessed by driver bring-up on
      silicon instead, and by the two-arm possession probes in `c6blink` (ESP32-C6, PMP NAPOT) and
      `rxdrv` (RX72M, RXv3), each negative in `main` and positive as the driver's first act, both
      printing rc and want. Those two are **not yet run on silicon**.

**Stage 4 -- the app story. COMPLETE** (three commits: the delegation type guard, the re-cut plus
`kos_cap_narrow`, then the narrow site plus the per-app declarations).
- [x] **Re-cut the authority set into SIX bits, and delete `AUTH_DEVICE` and `AUTH_CLOCK` as names.**
      `AUTH_DEVICE` in its old shape ("console publish, shutdown, reboot") held together only
      while root does all three: once root is only a spawner, `console_publish` and `shutdown` go to
      DIFFERENT threads and no single bit can carry both. The cut: `AUTH_MEMORY` (`ram_alloc`, MMIO
      grant, `mem_self_grant`; root the spawner), `AUTH_PINMUX` (`pinmux_set`; root's board pin map
      plus apps muxing their own pins), `AUTH_PSTATE` (`cpu_clock_set`; a governor service and nobody
      else), `AUTH_IRQ` (drivers with lines), `AUTH_SYSTEM` (`shutdown`, `reboot`; root/init),
      `AUTH_CONSOLE` (`console_publish`; the console driver). `cpu_clock_set` keeps its own bit
      specifically because a CPU governor needs clock-rate authority and nothing else -- folding it
      with shutdown would hand the governor the power to end the system. `AUTH_CONSOLE` must be a bit
      and **cannot** be possession-gated the way `arch_periph_enable` is: `KOS_SYS_ENDPOINT_CREATE` is
      completely ungated, so any thread can mint the endpoint it would publish, and
      `cap_console_publish` has no owner check either (filed below under the M4.5.3 findings). Full
      reasoning in `docs/design-unprivileged-root.md` section 5.
- [x] **Funded the sixth bit by moving the authority word out of `CapEntry.rights` into the poolless
      `obj` field.** `CapEntry` stays 8 bytes; `rights` is 0 on such an entry. The two families now
      have separate enums (`CapRights` bits 0..2, `CapAuthority` bits 0..5) and separate numbering,
      which costs one thing worth recording: an object right is no longer a *distinguishable* wrong
      value in an authority mask, so the "non-authority bits" spawn refusal catches only bits above
      the six, and the selftest's bad-bits probe moved to `1 << 6`.
- [x] **The delegation type guard landed FIRST, as its own commit.**
      `kernel/syscall/syscall_thread.cc` copies `se->obj` *and* `se->type` verbatim, so with the word
      in `obj` a delegable authority cap would forge a seat at child index `ci+1` -- index 2 whenever
      `cap_count >= 2`. Refused by TYPE, deliberately not by rights, so it does not rest on the byte
      the same change repurposes. Behaviour-neutral on landing (`cap_seat_authority` masks to the
      authority bits, so such a cap never carried `CAP_TRANSFER`).
- [x] **Added `kos_cap_narrow(cap, mask)`** -- `KOS_SYS_CAP_NARROW = 40`, ungated (an authority
      needed to drop authorities could never be given up), refuses any non-`CAP_AUTHORITY` cap with
      `-KOS_EINVAL` because narrowing an endpoint cap's `CAP_WAIT` needs `obj_close_protocol`'s
      `recv_holders` accounting. Narrowing to 0 empties the slot, type included.
- [x] **`kickos_default_init_run` narrows root after bring-up**, so the pin map and the console
      publish still have their bits. It lives in the RUN BODY, not the entry, because `init.h`
      advertises that body as the delegation reuse point: a custom `KICKOS_INIT_PROVIDER` composing
      pinmux + service list + run body would otherwise have run the app with root's full authority,
      silently. Found by the review pass, not by a test. The mask comes from
      `kickos_app_authority()` (default `AUTH_MEMORY | AUTH_SYSTEM`), overridden per app by
      `KICKOS_APP_AUTHORITY` in the app's own TU. **Per app, not per build tree**: one tree links
      every app against one kernel, so no CMake variable can express it. **NO weak symbol** -- the
      fallback is alone in `system/init/app_authority_default.cc`, so an app that defines the symbol
      resolves it locally and that member is never extracted. Weak was tried first and rejected: GCC
      carries a weak attribute from a declaration onto the definition in the same TU, so every app's
      override compiled `W` and link order decided it (`nm` confirmed). The macro emits the
      `extern "C"` itself, because a bare definition in a C++ app TU would mangle and leave the app
      silently on the default mask. Declared: `selftest` (five bits, not `AUTH_PSTATE`),
      `initdemo` (`CONSOLE`), `clockretune` (`PSTATE`), `c6blink`/`rxdrv`/`f411spi` (`PINMUX`).
      `AUTH_SYSTEM` is **not** forced back on: `kmain`'s refused shutdown panics
      `"root: shutdown refused"`, so the mistake is already legible, and forcing it would deny a
      never-returning app the ability to hold nothing.
- [x] **`stress` is NOT privileged-root: its three spawns were the same leftover.** Nothing in
      `ping`/`pong`/`churner` touches a privileged or authority-gated path (semaphore ops and
      `kos_sleep_ns` are ungated; the globals are ordinary `.appdata`), none of them spawns anything,
      and `sleeper` -- unprivileged in that same app, same round -- already does a superset of
      `churner`. Same origin commit as `selftest`'s two. Now unprivileged, and this **fixed a real
      failure**: under `KICKOS_ROOT_PRIVILEGED=OFF` the old flags were refused `-KOS_EPERM`, so
      `sim_stress` FAILED there on master and passes now.
- [x] **Gate: the selftest `authority_cap` narrow arms** -- the worker drops its only authority and
      the gate that had just answered for it returns `-KOS_EPERM`, plus the non-authority-cap
      refusal. In-env on every target, sim included. The ROOT narrow has no in-env carrier, so it was
      witnessed by deleting `AUTH_CONSOLE` from `initdemo`'s declaration: `console_publish` then fails
      from root on `qemu` at `KICKOS_ROOT_PRIVILEGED=OFF`, and the identical source passes at `ON`.
      That pair is also what proves the per-app override actually overrides.

Opened by stage 4:
- [ ] **The ROOT narrow has no automated test, in any environment.** `authority_cap` witnesses a
      CHILD narrowing its own cap; nothing exercises the path the default init actually takes. It
      cannot be witnessed under the default posture either: `KICKOS_ROOT_PRIVILEGED=ON` leaves the
      slot unseated, so `kos_cap_narrow` returns the tolerated `-KOS_EBADF` whatever the app declared,
      which also means **a silently-ignored `KICKOS_APP_AUTHORITY` override is unobservable on every
      standing gate**. It was witnessed once by hand, by deleting `AUTH_CONSOLE` from `initdemo` and
      watching its publish fail at `KICKOS_ROOT_PRIVILEGED=OFF`. Turning that into a gate is the same
      work as CI item (a) below: a preset that builds the flipped posture and asserts a declared-mask
      app fails when its bit is removed.
- [ ] **The `CAP_AUTHORITY` delegation refusal has zero test coverage, and cannot easily get any.**
      `syscall_thread.cc` refuses the type ahead of the `CAP_TRANSFER` check, but an authority cap
      always carries `rights == 0`, so the older check refuses the same delegation with the same
      `-KOS_EPERM`. The two are indistinguishable from userspace, which is why the guard is
      defense-in-depth rather than a behaviour change -- and why no black-box test can pin it. A
      kernel-side unit hook, or an assertion that the refusal fires for the type reason, is the only
      way to keep it from being silently deleted as redundant.
- [ ] **The sim selftest gate cannot run an unprivileged root**, which is part of CI item (a) below.
      `ctest` matches `# skipped: [1-9]` as a failure regex, but under `KICKOS_ROOT_PRIVILEGED=OFF`
      `mpu_privileged_guard` reports a legitimate named SKIP ("no privileged caller exists"), so the
      gate goes red for the right behaviour. `tests/check_qemu_selftest.sh` already solved this with
      its `EXPECT_SKIPS` permission-list; the sim registration has no equivalent. Measured on the
      branch: `sim` at `ROOT_PRIVILEGED=OFF` is 12/13 with that as the only failure (master is 11/13
      there, the second being `sim_stress`, now fixed).
- [ ] **`f302nucleo-st` does not link in Debug, and has not for a while.** `.text` overflows FLASH by
      4,548 B on `ba38b22` and 4,988 B with stage 4 -- so it is pre-existing, not stage 4's, but
      nothing catches it because no gate builds that board in Debug. At `-Os` the same image has
      **12.7 KiB free** (52,512 B of 65,536), which is why three comments in
      `user/apps/common/selftest/main.cc` claiming "96 bytes free" / "at the f302nucleo ceiling" were
      removed rather than updated -- four in total, the fourth found by the review pass after the
      first three were called complete: they were measured under the superseded `-O0` default and
      were being used to justify keeping `.rodata` down in a configuration that has ample room. Decide
      whether Debug is a supported configuration for the 64 KiB boards; if it is, this is a real
      regression to size down.
- [ ] **A per-service authority declaration in `kos_service_cfg`.** The struct has `rsv[2]`, so a
      byte fits with no layout change, and the runner could then narrow *between* entries -- hold
      `AUTH_CONSOLE` only while the `KOS_SVC_CONSOLE` entry runs. Deliberately NOT done in stage 4:
      root holds `CAP_AUTH_ALL` for the whole list run either way, so the only window it closes is
      between one bring-up entry and the next, with no app code running, and the app-level narrow
      already strips the bit before `main`. It becomes worth doing when service bring-up moves off
      the root thread -- i.e. with the item directly below.

Carried over from the old plan, untouched by this design:
- [ ] **Move app bring-up into the service lists**, so an app is started the way a driver is.

Blockers and limits:
- **One service bring-up body still pokes MMIO directly from root** --
  `system/driver/xmc4800/xmcssc/xmcssc.cc:281-324` (USIC kernel clock, baud, protocol) -- on
  `xmc4800-relax`, the enforcement flagship. The two K64F bodies are **retired by stage 3**:
  `system/driver/mk64f/k64uart/k64uart.cc` (AIPS PACR) and
  `system/driver/mk64f/k64dspi/k64dspi.cc` (clock gates, pin mux, GPIO, DSPI config) each call
  `arch_periph_enable` from the driver thread that holds the window. Stage 3 does **not** cover the
  XMC, which needs USIC-specific FDR/BRG/CCR programming rather than
  "ungate a clock, drop supervisor-protect", so `xmc4800-relax` stays console-only under the flip.
  **The XMC blocker is hardware, measured on silicon
  2026-07-28** by `user/apps/xmc4800-relax/pvprobe`: an unprivileged thread holding the MPU grant
  for the U0C1 window (`0x4003_0200`) has its writes to FDR/BRG/CCR **silently discarded** (no
  fault, read-back unchanged), while `SCTR` (`U,PV`) in the same window in the same run lands
  exactly and an ungranted SCU poke MemManages. So the window is grantable and the *transfer* path
  works unprivileged -- `xmcssc` already proves that -- but `xmc_spi0_start`'s three PV-write-only
  stores need a privileged executor, and the flip needs a seam for them. Given that seam the
  bring-up moves **wholesale** into the granted driver and must, because no path exists by which a
  post-flip root holds a DEV region: `ARCH_MPU_DEV` is attached only in `domain_for`, reached with
  MMIO only from `thread_spawn`, and `KOS_SYS_MEM_SELF_GRANT` hardcodes `ARCH_MPU_R | ARCH_MPU_W`.
  So the driver is the only possible caller of the seam. The
  earlier entry here said the opposite ("contradicted, not untested", from `consoledemo`'s scrambler
  garbling the UART); that was **invalid inference** -- the scrambler also writes SCTR/TCSR/PCR (`U,PV`) and
  gates `KSCFG`, any one of which garbles the UART on its own. Also corrected: Table 18-20 marks
  exactly three registers `Write = PV` (FDR, BRG, CCR); `INPR` is `U,PV` and its earlier inclusion
  was a transcription slip (untested here). **Now enforced
  at configure time** (`KICKOS_SERVICE_LIST_ROOT_MMIO`) rather than left to fail on the hardware:
  pairing such a list with `KICKOS_ROOT_PRIVILEGED=OFF` is a `FATAL_ERROR`, because the runtime
  failure is a fault mid-bring-up *after* the console has been relinquished, i.e. a silent dark
  board. The XMC flip drops `xmcssc` from the image entirely instead of linking it and not calling
  it.
- **A published console silences an app's own diagnostics** -- `kos_print` hands bytes to the kernel
  console, and `console_emit` drops all of them once the UART is `USER_OWNED`. Found on silicon: the
  first `rootfault` capture held a fault dump with nothing to check it against, and `mpu_fault`'s
  captures had been marker-only on every service-list board. Fixed for both via
  `kickos::emit` (`user/include/kickos/sys/emit.h`), which is the **third** copy of the
  try-index-0-then-fall-back policy (`tests/tap/tap.cc`, libc `_write`). The open question of which
  other worker-printing diagnostics share the problem is **answered: `pvprobe` and `inprstorm` do**
  -- filed below.
- **The panic-path UART reclaim clips bytes in flight.** `kpanic_enter` takes the UART back from the
  userspace driver so the report always reaches the wire, which works, but on `xmc4800-relax` it
  reproducibly garbles roughly the last 8 bytes the driver had queued (the polled TX word pending in
  `TBUF0`). Cosmetic for a terminal report, but it eats the tail of the line preceding the dump.
- **`bluepill-c8` and `f302nucleo` are held by the absence of a RING-ARM witness, not by RAM or
  handles.** `f302nucleo` now has silicon: `hello` and `stress` both pass at 2 KiB of heap
  (`docs/reference/boards.md`). What is unwitnessed is the ring arm, and no prober app exists.
  Both are armv7m, so the flip's mechanism (`ctx.npriv` in the fabricated first frame) is present;
  neither part has an MPU (`stm32f103` none, and `f302nucleo` is the R8 `x8` line, which has none
  either), so stage 2's gate -- selftest green *under enforcement* plus a cross-domain `rootfault`
  -- cannot be met on either. The 9-handle provisioning costs the flip nothing: the authority cap
  sits at reserved index 2 and spends zero dynamic slots. The arena is heap policy, not the part:
  measured 6,560 B (`bluepill-c8`, production image), 2,592 B (its selftest image), 14,752 B with
  the heap carve at zero; 8,512 and 4,512 on `f302nucleo` since its carve went to 2 K
  (`design-flash-footprint.md` section 7, `docs/reference/porting.md` minimum-requirement). The "barely 3 KiB" reading is a selftest-image figure.
- **`Thread::privileged` survives**, with narrowed meaning: it selects the memory posture (kernel
  domain + permissive background), it is the confused-deputy bypass at `syscall_mem.cc:37`, and it
  stays the home for "may spawn a privileged child" -- which should NOT be a capability, since
  holding it is equivalent to holding everything forever. Consequence: on a root-unprivileged
  board, **no privileged thread can come into existence after boot**.
- **`idle` stays privileged and holds no capabilities** -- it runs no app code, and RXv3 `WAIT` is a
  privileged instruction while RISC-V U-mode `WFI` is optional per spec.
- **The reserved cap index range is full after this** (0 stdout, 1 clock, 2 authority, 3 spare --
  reboot shares shutdown's bit, so index 3 stays free). **The five-bit authority ceiling is gone**:
  the word now lives in the poolless `CapEntry.obj`, `CapEntry` is still 8 bytes, and the width is
  bounded by `kos_thread_params::authority` (a `uint8_t` in padding) rather than by the entry. Two
  more authorities cost nothing; a ninth needs that params field widened.
- **Delegation packing collides with reserved names** -- spawn delegation puts cap *i* at child
  index *i+1*, so a delegated cap lands at index 1 (`KOS_CAP_CLOCK`) and a second at index 2. The
  authority cap can no longer be the one that collides (refused by type at the delegation site), but
  the `KOS_CAP_CLOCK` aliasing still blocks the narrowed hand-off to a driver manager until the
  deferred explicit-destination-index work lands.
- **Cap-gen is a `uint16_t`** with no object generation behind a poolless cap, so 65536
  close/re-seat cycles wrap it. Unreachable in-tree; same unbounded-counter class as the
  domain-refcount item above.

## M4.5.5 -- MPU region-encoding classes

Ordered after stage 4 (`kos_cap_narrow`) and before the `KICKOS_ROOT_PRIVILEGED` deletion, but
**not a blocker for it** -- the knob goes away on the strength of the flip, not of region shaping.
One general fleet re-witness pass closes the step; M4.6 (consoles/UART) follows it.

- [ ] **Give the alloc/MPU seam a third region-encoding mode.** `arch_ram_region_size`
      (`arch/include/kickos/arch/arch.h:234`) and `arch_ram_region_align` (`:262`) are `static
      inline` and keyed SOLELY on `arch_mpu_min_region()`, so they offer exactly TWO modes:
      `min == 0` gives 16-byte granularity, and any nonzero `min` gives a power-of-two size with
      the base NATURALLY ALIGNED to that size. The hardware has THREE classes, so one of them has
      no representation:
      - **Power-of-two REQUIRED.** PMSAv7 (`stm32f411`, `xmc4800`, `mps2`), and RISC-V PMP NAPOT
        (`arch/riscv/rv32imac/arch_rv32imac.cc:332` returns 8, "RISC-V PMP NAPOT minimum region
        size"). NAPOT folds the size into the trailing address bits, so pow2 there is the
        encoding itself, not a convention.
      - **Granular at N, power-of-two NOT required.** NXP SYSMPU 32 B (`mk64f`), RX MPU 16 B
        (`rx72m`), and ARM PMSAv8 32 B (`rp2350`, and `mps2-an505` via `qemu-m33`), which is
        base/limit rather than base+size. **This is the unrepresentable class**, and it
        over-aligns today on `frdmk64f`, `rx72m` and `pizero2350`.
      - **No MPU.** `arch/arm/chip/nrf51/chip_nrf51.cc:110` overrides to 0. `stm32f103` and
        `stm32f302` override to 0 as well (`6d49e14`), so this item covers only the class that
        remains: a granule that is right while the MODE is wrong.
      **For PMSAv8 the `min_region` VALUE of 32 is correct; the MODE is wrong.** 32 is the PMSAv8
      granule, which is why `arch/arm/common/arch_arm_pmsav8.cc:156` deliberately keeps the weak
      32 and overrides encodability alone. `mk64f` is the same shape: it overrides
      `arch_mpu_region_encodable` (`arch/arm/chip/mk64f/chip_mk64f.cc:559`) and nothing on the
      size/align path, so it still gets power-of-two shaping. That distinction is what makes this
      ONE seam change rather than a set of per-chip patches.
      **This is a known trade made explicit, not a newly found bug.**
      `arch/rx/rxv3/arch_rxv3.cc:653` already records the pow2 shaping as "a describable superset,
      not a requirement", and `arch_ram_region_size` already carries a `SEAM (MMU era)` marker
      naming itself the SINGLE point that couples allocation size to MPU descriptor geometry. The
      third mode belongs at that marker.
      **Scope and risk.** The change alters the region descriptors actually programmed on
      `frdmk64f`, `rx72m` and `pizero2350`. Two of those (`rx72m`, `pizero2350`) are flipped to
      unprivileged root AND silicon-witnessed, so it requires a silicon re-witness. Review it
      against `arch_mpu_region_encodable` and the real descriptor programming -- PMSAv8
      `MPU_RBAR`/`MPU_RLAR`, SYSMPU `RGD`, RX `RSPAGEn`/`REPAGEn` -- not only the allocator.
      `qemu-m33` (PMSAv8) is the one in-env gate the change moves.
      **The motivation is not bytes.** All three affected boards have RAM to spare (262 K on
      `frdmk64f`, 512 K on each of `rx72m` and `pizero2350`), so the expected recovery is small in
      absolute terms. What it buys is correct hardware modelling, and the parts ahead --
      Cortex-M23/M33/M55/M85 are all PMSAv8.
- [ ] **One general fleet re-witness pass, and it closes the step.** Every silicon witness in
      `docs/reference/boards.md` was captured from an UNOPTIMISED binary: the MCU presets built
      `CMAKE_BUILD_TYPE=Debug`, which is `-g` with no `-O` flag at all, and the fleet now builds
      `MinSizeRel` (`-Os -g`), which moves each image by roughly 17 to 23 KB. Per-board figures in
      `docs/design-flash-footprint.md`.
      **What does not transfer, and must be re-captured:** fault addresses, disassembly offsets,
      symbol sizes, stack-depth observations, and every timing figure -- the bench numbers, and
      `inprstorm`'s measured ~37,700 ISR invocations/second
      (`user/apps/xmc4800-relax/inprstorm/main.cc:25`). Least transferable of all: `pvprobe`'s
      privileged-write measurements, whose whole subject is whether an individual store lands.
      **What stands:** `bluepill-c8-st` and `f302nucleo-st`. The deleted two-board holding measure
      already built those two `-Os`, and their `.text` is byte-identical under `MinSizeRel`.
      **What the move buys, which changes what a witness is worth:** a board's `-st` kernel and its
      non-`-st` kernel differed by **13,505 bytes** as shipped, so an `-st` witness never
      transferred to the shipped image at all. Both now build `-Os` and differ by under 200 bytes
      -- the flag's genuine content (`arch_reboot`, `arch_irq_inject`, `arch_mpu_probe_addr`,
      `irq_spurious_count`) -- so an `-st` witness finally does transfer.
      **The same pass clears this milestone's silicon debt.** Each item is unwitnessed for its own
      reason and none of them justifies a separate bench trip:
        - The UART-FIFO drain on the reboot path (`arch_console_flush_sync` before `arch_reboot`,
          `kernel/syscall/syscall.cc:389`). Only `mk64f` and `xmc4800` implement the seam and
          neither has an emulator gate, so the sim and QEMU gates exercise only the weak no-op
          (`kernel/init/console.cc:347`) -- the truncation fix itself has never run against a real
          UART.
        - `dev_window_exclusive` and `bus_device_slots`: both postdate every silicon capture, so no
          chip has ever run them. Already recorded under the five-apps DEV-window item below.
        - The optimised `arch_reboot` path on `picopi` and `pizero2350`: verified by disassembly
          only, never executed, and neither RP part has an emulator gate. Distinct from the
          never-run RP2040 and imxrt1062 reboot BACKENDS under *Needs hardware* below.
        - `f302nucleo` joins the bench this round: it has an onboard ST-Link and a VCOM console,
          and `tools/flash-stlink.sh:18` already defaults `--connect-under-reset` on for it. It is
          the **only physically-present no-MPU ARM board**, which makes it the sole possible
          silicon witness for the claim that unprivileged root is real on a part with no MPU --
          root starting unprivileged, the ctors and `main` running, selftest green. That is a
          declared objective of the pass, not a by-product. It is NOT the stage-2 enforcement gate,
          which no MPU-less part can meet (see the `bluepill-c8` / `f302nucleo` bullet above).

## Found during the M4.5.2 stage-2 flip work (2026-07-28/29)

- [ ] **An IRQ line is never released, so a driver that exits cannot be respawned. Owner: M4.6**,
      whose design gate already names reclaim/teardown on driver death. `irq_detach` has exactly
      **one** caller in the whole tree (`kernel/init/console_tx.cc`, the console handover path), and
      nothing in thread teardown touches IRQ bindings: `exit_current` runs `cap_teardown` plus
      `domain_release` plus `on_remove` and no more, and `cap_teardown` walks the handle table only.
      Two leaks follow from one omission. The line keeps the dead driver's handler, so
      `irq_register` returns `-KOS_EBUSY` for it **forever** (one-driver-per-line is doing exactly
      what it is meant to; nothing ever tells it the driver is gone). And the binding pool is
      bump-allocated with no free path (`k.irq_binding_count++`), so the slot leaks too.
      **This contradicts a documented path**: `user/include/kickos/sys/spi_service.h` says
      `serve_loop` returns when the endpoint dies (`EPIPE`) so the driver thread can exit and "let
      root respawn". The respawn cannot work -- the new driver thread fails at
      `kos_irq_register`. The contract is written, the mechanism to honour it is not.
- [ ] **`kos_bus_cfg.cs_index` is accepted and never interpreted.** `k64dspi` drives one hardwired
      GPIO CS (`PTC4`) and `xmcssc` one fixed `SELO0`, so neither `fold()` reads the field, neither
      bounds it, and neither refuses an out-of-range value. Harmless while every driver has one CS
      line, and a trap the moment one has two: the M4.5.2 device slots let a client configure slot 0
      and slot 1 with different `cs_index` values and get the same physical line. `bus-service.md`
      and `bus.h` now say so; a multi-CS driver has to read and bound the field, and that is when
      the `-KOS_EINVAL` refusal the contract wants becomes real.
- [ ] **`irq_register` is completely ungated**, which is the sharper half of the same area. The
      `KOS_SYS_IRQ_REGISTER` dispatch arm (`kernel/syscall/syscall.cc`) calls straight through with
      no `cap_check_authority`, while its tier-2 neighbours `IRQ_ATTACH` and `IRQ_UNMASK` both check
      `AUTH_IRQ`. Combined with the no-reclaim finding above, any unprivileged thread can
      permanently squat any line on the chip -- one syscall, irreversible, no authority needed.
      Gating it on `AUTH_IRQ` is the obvious move; whether tier-1 should instead take a narrower
      per-line authority is the part that needs a decision.
- [ ] **Five in-tree apps grant a DEV window a live board-service driver already holds, so the
      M4.5.2 one-holder-per-window check (`domain_for` -> `-KOS_EBUSY`) now refuses their spawn.
      Silicon-only: no in-env gate covers any of them** (all are `kickos_add_diagnostic_app` or a
      hardware-observable demo, none has a CTest gate), so nothing goes red until the next bench run.
      The same gap covers the suite itself: `dev_window_exclusive` and `bus_device_slots` postdate
      every silicon capture, so the case totals stamped in `docs/reference/boards.md` are right for
      their commits and neither new case has ever run on a chip.
      Verified statically on `xmc4800-relax-st -DKICKOS_HAVE_MPU=1`, whose service list resolves to
      `kickos_services_xmc4800relax` (`xmcuart` U0C0 + `xmcssc` U0C1) -- the `xmcspi` and
      `consoledemo` ELFs both carry `kickos_board_services`, so both drivers are up before `main`.
        - `xmcspi`, `xmccshold`, `pvprobe`, `inprstorm` each grant `U0C1_BASE`/`0x200` =
          `[0x40030200,0x400303FF]`, the exact window the `xmcssc` bus service holds. This is a REAL
          pre-existing conflict, not a false positive: two drivers configuring one USIC channel. The
          four predate `xmcssc` joining the service list (M4.4) and silently became conflicting then.
          Build them `-DKICKOS_SERVICE_LIST=kickos_services_xmc4800relax_console` (console only, an
          existing provider) so U0C1 has no other holder.
        - `consoledemo -DKICKOS_SCRAMBLE_TEST=ON` grants `0x40030000`/`0x200` = the exact window the
          unprivileged `xmcuart` driver holds. Here the double grant is the POINT (garble a live
          console, prove `arch_console_reclaim` recovers it), so the check structurally obsoletes the
          way it was staged. Build it `-DKICKOS_SERVICE_LIST=kickos_services_none` (kernel-driven
          console, no DEV holder anywhere) and the scrambler is the sole holder again.
      `KICKOS_SERVICE_LIST` is one global cache variable per build tree, so this is a per-image build
      discipline, not something an app's CMakeLists can set for itself. Decide whether to encode it
      (a per-app configure-time refusal, or splitting the diagnostics into their own build trees).
- [ ] **Respawn vs `-KOS_EBUSY` on the device window -- documented, cannot bite today, revisit with
      SMP or a higher-priority supervisor.** `spi_service.h` says `serve_loop` returns on EPIPE so
      root can respawn; a respawn issued while the dying driver still references its domain would now
      earn `-KOS_EBUSY`. Two independent reasons it cannot happen now: (a) `sched::exit_current`
      calls `cap_teardown` (which EPIPE-wakes the parked respawner) and `domain_release` in the SAME
      `IrqLock` critical section, `cap_teardown` first, so a woken supervisor always observes the
      window already free; (b) root runs at `KICKOS_PRIO_MIN + 1` = 2, below every service driver
      (11-12), so on single-core it cannot preempt a driver between `serve_loop` returning and
      `exit_current`. Opens if a supervisor ever outranks a driver, on SMP, or if death is detected
      any other way (watchdog/timeout/a future join) -- then join before respawning, or retry on
      `-KOS_EBUSY`. Note the respawn path is ALREADY broken for an unrelated reason (the IRQ-line
      entry above), so no in-tree caller exercises this yet.
- [ ] **`pvprobe` and `inprstorm` print via `kos::print`, not `kickos::emit`**, so their output is
      silently dropped on any board whose console has been published to a userspace driver. Only
      `rootfault`, `mpu_fault` and `rebootdemo` include `emit.h`. The fix is one include and a call
      swap, and it matters out of proportion to its size: these two are the probes the
      unprivileged-root design's evidence rests on, so a silent probe reads as a probe that found
      nothing.
- [~] **`f411spi` cannot run under the flip: its bring-up shim writes MMIO from `main`. ADDRESSED by
      stage 3, silicon-unwitnessed.** The `stm32f411` `arch_periph_enable` backend covers the SPI1
      clock gate and the pinmux encoding covers `PE3`, but `frdmk64f` was the only board on the bench
      for stage 3, so the `f411disco` run is bench debt. Found 2026-07-29 while flipping `f411disco`, and witnessed
      rather than inferred -- the app faults MemManage on the first store of `main`
      (`RCC_AHB1ENR` @ `0x40023830`, `CFSR=0x82`, `MMFAR=0x40023830`), before it ever spawns the
      unprivileged driver that holds the 32 B SPI1 grant. Same shape as `c6blink` and `rxdrv` before
      their windows were reworked: the escalation surfaces (RCC clock-enable, GPIOA/GPIOE mux) are
      deliberately kept out of the driver's window, which is exactly why they need kernel mediation
      instead of a wider grant. NOT a flip blocker -- it is a `kickos_add_diagnostic_app`, never a
      production image, and the stage-2 gate is `selftest` + `rootfault`, both green on that board.
      Its loopback arm is also still unwitnessed in the default posture (needs the PA7->PA6 jumper),
      so the chip's peripheral-window proof stays open either way.

## Found during the M4.5.2 review (2026-07-29)

- [ ] **A user-facing test suite does not exist.** The kernel selftest tests the KERNEL through the
      syscall surface and deliberately does not test the user-facing surface, so nothing anywhere
      checks that `printf`, `std::cout`, heap behaviour and libc integration work per board.
      `hello` passing is the entire coverage, and on some boards `hello` has no gate at all: QEMU
      models no `stm32f302`, so `f302nucleo` carries **no CI gate of any kind**
      (`docs/reference/boards.md`). **The two suites cannot merge**, and the `-st` presets are why --
      the kernel suite is provisioned FOR the kernel. `f302nucleo-st` (`cmake/presets/arm.json:137`)
      now runs it with `KICKOS_USER_HEAP_SIZE=0` and `KICKOS_USER_STACK_SIZE=1024`, which is
      precisely the opposite of what a user-API suite has to exercise. Sharp consequence of that
      preset: with the heap carve at zero the `-st` gate on that board no longer exercises the heap
      at all, so a heap regression on a 16 KiB part would go unseen.
- [ ] **No CI job exercises an unprivileged root, on any target.** `KICKOS_ROOT_PRIVILEGED` appears
      zero times in `.github/workflows/ci.yml`: every gate builds and runs the privileged posture.
      Six boards are witnessed flipped by hand on silicon, so the posture M4.5 exists to deliver has
      no automated coverage at all, which is the same unexercised-arm shape as the
      `kernel_ctor_placement` vacuity trap. Cheapest real arm is **qemu with `KICKOS_HAVE_MPU=1`
      plus the flip** -- a genuine armv7m `npriv` boundary and a real MPU, and that configuration
      already builds and its gate runs green. A flipped **sim** arm is cheaper still and the
      `EXPECT_SKIPS` plumbing already handles it (flipped sim runs 59/60, skipping exactly
      `mpu_privileged_guard`), but the sim discards the privilege flag, so it witnesses the
      authority logic and region composition, never a CPU-mode boundary. Owned by 4.5.5, whose
      re-witness pass is the natural home.
      Deliberately NOT done instead: flipping `frdmk64f`'s preset default. It would break the locked
      order (the knob's deletion is step 4, after the 4.5.5 re-witness), give the fleet a third
      posture alongside XMC's console-only special case, and silently break `k64drv`, which cannot
      run flipped by design. It would also change only what is BUILT, not what is TESTED, since no
      CI job runs frdmk64f with MPU non-vacuously.

- [ ] **CI builds the enforcement gates unoptimised -- NEEDS A DECISION.** The "ARM PMSA enforcement
      run gates (v7 + v8)" step (`.github/workflows/ci.yml:171`) passes `-DCMAKE_BUILD_TYPE=Debug`
      explicitly (`:176`), which overrides the fleet's `MinSizeRel` default. So the four gates
      covering the enforcement posture -- the posture that matters most -- still compile `-O0`, on
      the one job that runs `qemu`, `qemu-m33`, `qemu-m7` and `qemu-m3` under `KICKOS_HAVE_MPU=1`.
      Filed as a decision rather than a defect: dropping the pin makes those gates test what ships,
      and it also changes what has been gated until now. **The same pin is why CI cannot catch the
      `-Os` gate-then-configure bug class.** `build-boards-mpu` does build `MinSizeRel`, but its only
      ctest, `kernel_ctor_placement`, is documented as passing vacuously for that matrix. So no CI job
      exercises `KICKOS_HAVE_MPU=1` at `MinSizeRel` with real driver code paths -- exactly the
      configuration the K64F PIT lost write was found in.
- [ ] **`sam3x8e` over-alignment: PARKED on hardware absence, not open.** The chip HAS an MPU on
      silicon (Atmel SAM3X/SAM3A datasheet, Cortex-M3 revision 2.0) but KickOS ships no `mpu.cmake`
      backend for it, so it builds `KICKOS_HAVE_MPU=0` while still inheriting ARM's weak
      `arch_mpu_min_region()` of 32 (`arch/arm/common/arch_arm_common.cc:348`) -- costing 3,808 bytes
      of measured over-alignment on a part that enforces nothing. It is the third member of the class
      `stm32f103` and `stm32f302` just left, both of which now override to 0 in
      `arch/arm/chip/stm32f103/chip_stm32f103.cc` and `arch/arm/chip/stm32f302/chip_stm32f302.cc`.
      **PARKED by the maintainer for a concrete
      reason: the physical Arduino Due unit is dead** (`docs/reference/boards.md`), so nothing on this
      chip can ever be witnessed -- do not pick it up expecting to validate it. The class itself is
      handled by the region-encoding item under M4.5.5 above, which is where a third encoding mode
      would land.
- [ ] **Cut `bluepill-c8`'s 8 KiB heap carve: the board is predicted to fail `hello`'s second spawn
      by 96 bytes.** This is a **MODEL PREDICTION, not a witness** -- the board has no physical unit
      and can never be flashed. Arena 6,560, minus idle 512 and root 2,048, leaves 4,000 against the
      4,096 that two 2,048-byte stacks need. The cause is the carve rather than the part: 8 K
      `.userheap` (`arch/arm/chip/stm32f103/stm32f103.ld:27`) where `f302nucleo` now takes 2 K, plus the
      board raising ROOT/USER to 2048 over the chip default of 1024
      (`boards/bluepill-c8/include/kickos/board_config.h:31`, `:37`). Full arithmetic and its
      provenance are already in `docs/reference/boards.md`; the fix is cutting the carve. The
      prediction is worth acting on because the same model called all three `f302nucleo` silicon
      outcomes correctly -- `hello` two threads, `stress` pass, `selftest` spawns refused.
      **The boot-arena link assert cannot catch this**: `arch/common/boot_arena.ld.h` replays the
      idle and root stacks only, never the N user stacks a spawning app needs.
- [ ] **About 270 `path:N` doc citations cannot be verified by any gate, and two of two spot-checks
      had drifted -- NEEDS A CONVENTION DECISION.** `tests/check_doc_names.sh` (landed, deliberately
      not wired into CTest) says so itself at `:54-57`: it strips the `:N` and never checks it,
      because nothing in the current spelling says WHAT should be at that line. Two confirmed live
      instances, both in one document: `docs/design-m3-clock-select.md:16` cites
      `user/include/kickos/sys/abi.h:36` for the cpu-clock syscall, where line 36 is now
      `KOS_SYS_IRQ_REGISTER = 14` (the real line is 43); and `:17` cites
      `arch/include/kickos/arch/arch.h:79` for `arch_cpu_clock_hz()`, where line 79 is
      `arch_timer_arm()` (real line 84). This is the reused-identifier class the project already
      knows is expensive -- a citation that resolves to a live but unrelated thing is worse than one
      that dangles. **The decision is the spelling**: if a citation carries the expected symbol
      (`arch.h:84 arch_cpu_clock_hz`), the gate can check it in about two lines; until then `:N` is
      decoration. This item scopes that work only -- fixing the ~270 citations belongs to the doc
      audit, and the two instances above are deliberately left as found.
- [ ] **`arch_reboot` should take a MODE, and the compile knob should gate the MODE rather than the
      seam. Owner: M4.6, after 4.5.4.** Decision recorded in full in
      `docs/design-unprivileged-root.md` section 9, under `### The reboot capability`. Today
      `int arch_reboot(void)` (`arch/include/kickos/arch/arch.h:44`) takes no argument and means
      bootloader entry specifically, with two callers -- `kernel/init/console.cc:293` inside
      `bootloader_handover`, and the `KOS_SYS_REBOOT` dispatch arm at
      `kernel/syscall/syscall.cc:390` -- and the whole thing sits behind `KICKOS_ENABLE_SELFTEST`.
      Four parts: a mode argument (at least a normal system reset and bootloader entry); a per-MODE
      weak `-KOS_ENOSYS` decline instead of a per-function one; the knob narrowed to the bootloader
      mode and renamed `KICKOS_ENABLE_REBOOT_TO_BOOTLOADER`, matching the sibling
      `KICKOS_SHUTDOWN_TO_BOOTLOADER` (`CMakeLists.txt:117`) rather than spelling one destination two
      ways; and an authority bit on top of the knob for the bootloader mode alone. What it buys: no
      production in-kernel path can reset the chip today, which costs watchdog recovery, a
      fault-handler reset and a bring-up retry for no security reason, since a normal reset carries
      none of the bootloader risk. What it retires: `KICKOS_SHUTDOWN_TO_BOOTLOADER` becomes a policy
      on one seam instead of a parallel mechanism, and syscall 38 becomes a real production syscall
      taking a mode -- answering `-KOS_ENOSYS` for a mode the chip lacks and `-KOS_EPERM` without
      authority, instead of `-KOS_EINVAL` from a dispatch default arm -- which takes the
      configure-time `FATAL_ERROR` (`CMakeLists.txt:124`) and the `abi.h:62-64` compiled-out-arm
      annotation with it. The symptom that exposed the conflation:
      `arch/arm/chip/imxrt1062/chip_imxrt1062.cc:49` forward-declares `kpanic` inside a
      `KICKOS_ENABLE_SELFTEST` block, only because that chip's `arch_reboot` is a `bkpt` that must
      not resume -- a fundamental function's declaration behind a test flag.

## Found during the M4.5.3 stage-3 work (2026-07-29)

- [ ] **The console driver cannot report its own bring-up failure, by any available means.**
      `k64uart_console_start` publishes the console before spawning the driver, so the driver runs
      with `ConsoleState::USER_OWNED`, where `console_emit` is `return; // DROP`
      (`kernel/init/console.cc:133`) and RTT is compiled out on this board. `kickos::emit` is worse
      than dropped: the driver's stdout cap index 0 IS the endpoint it was spawned to serve, and
      `endpoint_send` parks on `wq_block(e->send_waiters)` when no receiver is waiting
      (`kernel/syscall/syscall_ipc.cc:137`), with no `-KOS_EPIPE` escape because `recv_holders` is
      >= 1 for its own WAIT cap. So the driver would park forever instead of exiting. Measured on
      the code, not inferred. `spawn_unprivileged` also cannot observe a failure inside the child's
      first instructions, so root reports bring-up success either way: the board goes dark with no
      evidence. This is why `kos_periph_enable`'s failure arm there is a comment rather than a
      report. The remedy is ordering, not a call-site swap -- publish only once the driver has
      proved reachability, or have root verify before publishing -- so it is a handover redesign
      owned by M4.6. Note the same drop applies to that driver's success line and to root's own
      `k64dspi_spi_start` error prints.
      **This item is what the standing "SPI-service silicon halt" blocker actually was, and the halt
      itself is refuted on both boards** (2026-07-29, `aa084a9`, captures under
      `.session/logs/m453-spihalt/`). Neither service halts: `k64dspi` reads the LAN9252 `BYTE_TEST`
      signature `0x87654321` on attempt 1 against a fitted EasyCAT shield, and `xmcssc` passes
      config, single-byte, multi-byte, null-tx and transact. The plain no-RTT builds corroborate it
      off the halted target -- DSPI0 `SR=0xC2020303` with TCF set, `U0C1_CCR=0x0000C001` (MODE=SSC),
      both cores idling in `arch_idle_wait`, neither faulted. Per-board detail is in
      `docs/reference/boards.md`. The original record is best explained as a mis-summary of
      VCOM-only captures: the paired RTT captures of those same 2026-07-25 runs already show both
      services passing, and the register state above shows the work completing in an image carrying
      no RTT at all. What remains open is therefore visibility and ordering, not a halt.

- [ ] **Consider a diagnosis preset carrying `KICKOS_CONSOLE=both`** -- no board preset sets it,
      `-st` included (only `host` and `qemu-telem` do), so a bench run has exactly one transport and
      a published console takes that one away from the app. That is how the phantom SPI halt above
      survived, and any future "the service goes quiet" diagnosis over VCOM alone will re-derive the
      same false conclusion. RTT is generic in the kernel, so `both` builds anywhere, but it is only
      readable where a probe can read target RAM -- the J-Link boards (`xmc4800-relax`, `frdmk64f`)
      are where it pays. Against it: flash, and the two 64 KiB boards have the least of it. The
      bench rule that holds regardless is recorded under *Per-board caveats* in
      `docs/reference/boards.md`.

- [ ] **Audit the whole fleet for the `-Os` clock-gate-then-configure lost write.** On K64F a
      `PIT_MCR = 0` store raced the `SIM_SCGC6` gate write and was **dropped** at `-Os`, fixed by a
      consumed read-back of the gate register (`127efb5`). The same lost-write pattern plausibly
      affects other chips' gate-then-configure sequences now the whole fleet builds `MinSizeRel`.
      Record while auditing that `(void)r32(...)` does **not** serve as a read-back and that
      `-Werror` correctly rejects it: a `(void)` cast of a volatile lvalue performs no access.
      Ranked inventory from a review, unguarded first: **`mk64f arch_pinmux_set`** -- gates a PORT
      then writes that PORT's `PCR` in the same function, write-once with no self-heal, and PORTD's
      `SCGC5` bit was never set on this chip before this milestone; **guarded since `aa084a9`**, and
      the A/B below measures the store landing either way, so treat it as closed rather than as the
      inventory's leading example; **`xmc4800 ccu4_clock_init`** --
      `CGATCLR0`/`PRCLR0` then `GIDLC`, write-once, and `GIDLC` is the monotonic clock's slice
      enable; **`xmc4800 usic.cc kernel_clock_enable`** -- it has the write/read-back/barrier idiom,
      but placed AFTER the first `KSCFG` write and protecting a different documented pipeline effect,
      so the first write into the newly ungated block is itself unprotected; **`imxrt1062
      gpt_clock_init`** -- `CCGR1` then `GPT1_CR`, partially self-healing, but its `SWR`
      software-reset step could silently no-op; the **STM32 family**
      `tim2_clock_init`/`usart2_init`/`arch_diag_led_init`/`arch_pinmux_set`; **`sam3x8e`** (unit
      retired); **`esp32c6 arch_diag_led_init`** (diag only, and that same file already uses an
      explicit `fence` for its `INTMTX` writes). Two are **SAFE for a reason, not by luck**:
      `rp2040`/`rp2350` `unreset()` polls `RESET_DONE`, a real hardware completion flag and a
      stronger pattern than a read-back; `rx72m` `MSTPCR` has substantial intervening work and uses
      the read-back idiom where its UM requires it. **Second hazard variant, unflagged so far:** a
      read-modify-write on a just-gated block can write back a **corrupted** register if the read
      returns stale data -- worse than a dropped store, which at least leaves the reset value.
      `usart2_init` and `arch_diag_led_init` do `MODER` RMWs. **The `k64dspi`/`xmcssc` dropped-mux
      lead is REFUTED -- do not re-run it.** The hypothesis was that a dropped `PCR` write on PTD1
      (SCK) left the pin at its reset mux while the service still reported "up", since nothing checks
      pin state. Measured 2026-07-29 as an A/B one commit wide across `aa084a9`'s read-back: the four
      pin-map rows read their programmed mux **in both arms** -- `PORTD_PCR1`/`PCR2`/`PCR3 = 0x200`
      (ALT2), `PORTC_PCR4 = 0x100` (ALT1) -- and `k64dspi` completes its LAN9252 `BYTE_TEST` round
      trip in both. So the naked store here is not being dropped in practice. **The barrier stays**
      and is not credited with fixing this: it closes a measured hazard for 6 bytes of flash, and the
      window it closes is the tightest instance of the class in the tree -- the disassembly shows
      exactly 1 instruction and 0 intervening bus transactions between the `SCGC5` gate store and the
      `PCR` store, tighter than the PIT failure that proved the class. **The rest of the inventory
      above is untouched** -- those sites are still unaudited; only this one hypothesis died. The
      halt it was chasing does not exist either (see the console-visibility item above).
- [ ] **`k64drv` cannot run under the flip, and is refused BY DESIGN rather than pending a seam.**
      Its PIT window is legitimately granted, but the AIPS `PACR` slot that would have to open for it
      (slot 55) spans the whole 4 KiB block, including the chained ch0+ch1 pair that carries
      `arch_clock_now`, so there is no table entry and `arch_periph_enable` answers `-KOS_EINVAL`.
      This is the opposite case from `f411spi`, which a seam did fix. `k64drv` is a diagnostic app
      (`KICKOS_ENABLE_SELFTEST` only, no CTest gate), so nothing goes red; decide whether it is
      retired, reworked onto a block whose slot is containable, or kept as a privileged-root
      diagnostic.
- [ ] **`f411spi` lost its high-speed slew configuration on `PA5`/`PA6`/`PA7`**, because the pinmux
      encoding field reaches `MODER`/`AFR`/`BSRR` but not `OSPEEDR` or `PUPDR`, so those pins run at
      the reset-default low-speed slew. `BR=/64` is ~1.3 MHz (84 MHz APB2 / 64, arithmetic from the
      tree); that the default slew carries that rate is **engineering judgement, pending a DS9716
      check** -- no line in this tree supports it, unlike the other electrical claims here. Worth
      reopening if a faster `BR` is ever wanted on that bus, which is what would need `OSPEEDR` back
      -- via the encoding, not a root MMIO write.
- [ ] **`cap_console_publish` has no owner check and no once-only guard.** It drops the kernel's
      existing stdout ref and re-points `g_stdout_target` unconditionally, so any caller that clears
      the authority gate silently steals a live console -- and `KOS_SYS_ENDPOINT_CREATE` being
      completely ungated means any thread can mint the endpoint to publish. `AUTH_CONSOLE` is the
      sole thing preventing it, and the guard is wanted independently of that bit: root itself holds
      it for the length of service bring-up.
- [ ] **The CPU/peripheral clock coupling is over-generalised, and the veto should be a notification.
      Owner: M4.6**, and a CPU governor depends on it. `cpu_clock_set` refuses outright while a
      userspace driver owns the console (`kernel/time/clock_select.cc`), because the kernel cannot
      re-derive a baud it no longer owns. That generalises from a biased sample: exactly **two** chips
      implement `arch_periph_clock_hz` and both are coupled (`chip_xmc4800.cc` fPERIPH = fCPU/2;
      `chip_mk64f.cc` `SystemCoreClock` or /BUS_DIV). A chip with an independent peripheral root has
      no backend at all, so the decoupled case has never had to be stated, and the assumption is baked
      into the seam's own contract wording ("retune the core/bus clock") -- on a chip with a dedicated
      CPU PLL there is nothing to refuse. Make the coupling a question asked of the chip, and notify
      affected services instead of vetoing. The console is not the only one: drivers size their
      divisors off `kos_periph_clock_hz` too.
- [ ] **The possession gate has no test distinguishing exact-base from containment.**
      `caller_holds_mmio_block` matches `r.base == base` exactly. Widening it to a containment test
      would pass both mutations `periph_enable_unheld` was checked against, so that regression would
      ship silently. Untestable in-env: the sim's `arch_mpu_region_encodable` is unconditionally
      false, so no DEV region can exist there. The only route is a hosted unit test that fabricates a
      `Thread` plus an `arch_mpu_region` array and calls the predicate directly, and no such harness
      exists.

## Needs hardware (bench time, not code)

- [ ] **v6-M MPU programming has zero coverage anywhere.** QEMU models no Cortex-M0+ and no
      Cortex-M23 core, so neither the emulator gates nor the silicon fleet ever exercises the
      armv6m MPU backend. Closing it needs an **RP2040 on the bench** -- there is no software
      substitute for it.
- [x] **M7 speculation class** -- already covered by validated Teensy silicon (the imxrt
      MPU-enforce hang record, `docs/design-teensy-mpu-hang.md`); recorded here so the gap list
      stays honest about what *is* already covered.
- [ ] **`arch_reboot` is witnessed on RP2350 only** (pizero2350, BOOTSEL). Two backends have never
      run: `rebootdemo` on a picopi (RP2040 -> PICOBOOT/UF2) and on a Teensy 4.1 (`bkpt #251` ->
      HalfKay). The Teensy path is the least certain: it is not vendor-documented, and on
      non-Teensy RT1062 hardware the `bkpt` faults instead.

## M3 -- landed so far (2026-07-20)
- [x] `sys_cpu_clock_hz()` read syscall; [x] per-task capability handle table (sem ABI) +
      authenticated-grant delegation; [x] priority-inheritance mutex (CAP_MUTEX). All on master,
      silicon-validated UNDER ENFORCEMENT on K64F (SYSMPU) + C6 (PMP). Design in Book ch.8.1 +
      `reference/architecture.md`.

Remaining M3 (to finish the milestone) -- gated flow (fable design review -> branch -> silicon):
- [x] **Writable user-pointer bound-check** at the syscall boundary (arch-neutral) -- landed
      ade1879 (`user_writable_ok`; clock_now retrofitted). A recv into an unchecked out-buffer was a
      privileged write oracle; the endpoint recv buf + badge-out reuse it.
- [x] **Endpoint/IPC object (CAP_ENDPOINT)** -- additive per `docs/design-m3-endpoint-stagei.md`
      (fable-reviewed): `SlotPool<Endpoint,N>` + `endpoint_refs` + `recv_holders` (struct field) +
      one `cap_resolve` case + obj_ref_inc(rights)/drop and obj_close_protocol (EPIPE-wake) arms;
      synchronous rendezvous, kernel-copied bounded payload, parks on the shared `wq_block`/
      `wq_pop_highest` primitive; send/recv/create syscalls 26/27/28 (recv gated on the writable
      check). Aliases/object-side badging DEFERRED (root-only; console needs one unbadged cap).
      Landed on master; SILICON-VALIDATED UNDER ENFORCEMENT on K64F (SYSMPU) + XMC4800 (PMSA),
      selftest 39/39 each incl. the HAVE_MPU-gated endpoint_bound + crossdomain (emulator qemu
      armv7m + qemu-riscv 37/37). Rest of the fleet build-only (only k64f/xmc on the bench).
- [x] **Console device handover** -- `ConsoleState{KERNEL_OWNED,USER_OWNED,RECLAIMED}` drop-routing,
      `console_tx_deinit` (USER_OWNED set last) + the B1 in-flight-writer drain, `kos_console_publish`
      (#29, privileged), stdout cap seated at index 0, `_write` probes `kos_send(0)` then falls back.
      Userspace polled XMC UART driver (`system/driver/xmc4800/xmcuart` + `consoledemo`). SILICON PASS on XMC:
      end-to-end app printf -> IPC -> userspace driver -> wire, under enforcement.
- [x] **Panic-path console reclaim** -- `arch_console_reclaim` per chip (XMC full in-window rewrite,
      KSCFG.MODEN-first; K64F uart0 + zero MODEM/C3/S2/IR/C7816), `kickos_isr_fault`->`kpanic_enter`
      funnel (all 6 arches audited safe), driver-death EPIPE-wake. SILICON PASS on XMC (scramble-then-
      panic: banner survives a driver-garbled UART; one intrinsic leading line-transient byte, doc'd).
      K64F reclaim built + reviewed, silicon-pending (no K64F console driver yet). Porting invariant in
      `reference/porting.md`.
- [x] **User-selectable CPU clock / low-power mode** -- `arch_cpu_clock_set` mechanism seam + syscall
      30 (privileged) + coherence tail (epoch re-anchor sole mult-writer, baud re-derive, timer re-arm,
      USER_OWNED refusal). SILICON PASS on XMC (144/48) + K64F (120/20.97): monotonic `now` across
      retune, ratio-correct timing, no fault. XMC full retune, K64F staged; other chips weak-default-0.
      Policy -> future userspace power-manager/clock-tree service (roadmap). Read side already landed.
Silicon target for the handover: the CPU-side-MPU boards (XMC/RX/C6) where per-thread peripheral
isolation is real; K64F is coarse-AIPS (documentation, not enforcement).

- [x] **Teensy 4.1 (i.MX RT1062, M7) MPU-enforce hang -- ROOT-CAUSED AND FIXED @c072712.** The
      deterministic `teensy41-st -DKICKOS_HAVE_MPU=1` hang at test 6 `rr_interleave` was never a
      switch bug. A dropped (non-pow2) whole-arena grant left the worker on the PRIVDEFENA
      background, which types the entire 1 GiB FlexSPI/SEMC aperture as Normal; the M7 -- the one
      core in the fleet that speculates -- prefetched past the populated 8 MiB into an AHB slave
      that never responds, and an in-order core cannot retire behind an access that never
      completes, so it stalled forever with NO fault to report (NXP ERR011573 / Arm 1013783-B).
      Fixed by a new shared seam, `kickos_arm_mpu_fixed` in `arch/arm/common/`: a chip declares
      thread-invariant regions that are programmed once into the LOW descriptor slots (so
      per-thread grants still override them), and imxrt wraps the unbacked apertures as
      Device + XN + no-access *before* the I-cache is enabled -- ordering is load-bearing,
      because the cache is what arms the speculation. Enforcement selftest went from a hang to a
      full pass with a clean soak; the durable teaching is Book ch.7.6. Full record:
      `docs/design-teensy-mpu-hang.md` (LANDED). Its residuals are tracked separately: D-cache
      default-on is done (below), "Option B" is the fleet-wide post-M6 item, and the
      reprogram-window / HFNMIENA bypasses are accepted in the design record.

Book + exploratory (M3-adjacent, not milestone-gating):
- [x] **Book chapter: the syscall mechanism** -- landed as ch.3.9,
      `book/the-syscall-path-trap-dispatch-return.md`. Slotted in part 3 (the trap/interrupt model)
      rather than 2.x: it needs 3.5's saved-state + deferred-switch vocabulary to say why the
      handler must not dispatch, and it is what part 4's per-ISA tour then instantiates. Chapter 0.2's
      dangling "see 3.5 for the trampoline" forward reference now points here. One correction to the
      brief: `read`/`open`/`socket` are not IPC clients, they are link-only libc bottom-edge stubs
      (only `_write` routes over IPC, to the caller's stdout cap, with the kernel console as
      fallback), so the chapter teaches the rule (a real one belongs to the server that owns the
      device) rather than claiming an implementation.
- [ ] **Exploratory spike: microkernel IPC performance** (M3 #4 -> M5). The Mach-era "IPC too slow"
      critique vs the L4/seL4 answer -- (a) fast SYNCHRONOUS IPC (direct switch to the woken
      receiver + register/bounded-copy; KickOS's sem_post already hands the token off and drives an
      immediate switch, so the fastpath shape exists) for control/RPC, and (b) shared-memory + async
      notifications (non-blocking) for throughput -- the M5 cross-core design
      (`docs/design-multicore-ipc.md`) already uses an SPSC ring + doorbell, exactly that shape.
      Survey the literature, map both to CAP_ENDPOINT (#4) + the M5 rings, recommend the
      control-plane-vs-data-plane IPC strategy + a micro-benchmark. Good deep-research candidate.

## Clock hardening (2026-07-20) -- clock off the debug-domain / narrow counters
Root cause: v7-M `arch_clock_now` used DWT_CYCCNT (core DEBUG power domain), sw-extended 32->64.
On K64F+XMC silicon DWT intermittently returns aliased garbage -> phantom 2^32 wrap -> clock
leaps ~35 s -> every timed wait strands (intermittent ~50-75%, silicon-only). Masqueraded as a
"test-5 stall" and invalidated this session's earlier single-run silicon claims. Fragility class
= narrow counter + sw wrap-extension (fails via a bad read OR a missed wrap). Fix = a wide,
reliably-readable, NON-debug free-running peripheral counter. Book ch.2.1 teaches it.
- [x] **K64F** 64-bit PIT -- SILICON 20/20 (+ mutex 10/10, under enforcement).
- [x] **XMC4800** 64-bit CCU4 (4 slices concat) -- SILICON 18/18; fixed fCCU WFI-gating (SLEEPCR).
- [~] **F411/F302** TIM2(32b), **F103** TIM2->TIM3 chained, **SAM3X** TC0 ch0(32b) -- on master,
      reviewed+fixed (f103 tear-discriminator; per-timer overflow-IRQ wrap observer; f411 APB1LPENR).
      **BUILD-ONLY, SILICON PENDING.**
- [~] **ESP32 (Xtensa)** 64-bit TIMG0 (UPDATE-latch) -- also fixes a latent CCOUNT WAITI-freeze.
      **BUILD-ONLY, SILICON PENDING.**
- RISC-V (CLINT mtime) + RX (CMTW): already sound, unchanged.

Silicon-test-later (fleet+Xtensa; `.session/*-clock*.patch` are backups):
1. idle-wrap observer: quiescent > 1 wrap period (51/67/59/102 s) -> clock still correct.
2. f103: soak across chain wraps -> no +59.6 s leap, no backward stall.
3. rate/monotonicity vs wall clock (2x error = wrong Hz); no backward step under IRQ load.
4. WFI keeps counting (f411 APB1LPENR; sam3x FSMR Sleep-not-Wait; Xtensa TIMG UPDATE-latch settle +
   DPORT ungate assumption -- the two things unverifiable build-only).
5. overflow lands in the chip clock ISR (NVIC TIM2=28/TIM3=29/TC0=27, RM-sourced).
6. debug-halt > 1 wrap period loses a wrap (DBGMCU freeze unset) -- bench artifact, not a bug.

Clock follow-ups (not blocking): arch_trace_now + KICKOS_BENCH still read raw DWT/CCOUNT (telemetry
may glitch on K64F/XMC -- tolerable, NOT the scheduler clock); ticks->ns epilogue duplicated ~7x
(hoist an arch/arm/common helper).

## M1 -- clocks (fleet audit 2026-07-09; detail in `docs/archive/M1_state.md`)

Every board's timing math is ACCURATE (no ESP32-C6-class constant bug survived the
audit). Remaining work is boards that never raise their PLL, so they run far below
capability and their benchmarks reflect a slow core. Each fix = raise PLL **and**
update `SystemCoreClock` in the same step so the ns<->tick math stays coherent.

- [x] **ESP32-WROOM: PLL bring-up 40 -> 240 MHz** -- DONE, validated on silicon 2026-07-09.
      6x confirmed by a SystemCoreClock-independent host-wall-clock spin (2203 ms @240 vs
      13020 ms @40); selftest 14/14, console clean at the recomputed 80 MHz-APB baud, 0.4 s
      beat coherent. No BBPLL lock bit on this chip -> hardened with a bounded RTC-slow-cycle
      barrier (esp-idf's mechanism) around the power-up + before the source switch.
- [x] **RP2040: PLL_SYS bring-up 12 -> 125 MHz** -- DONE, validated on FIRST SILICON
      2026-07-09 (the RP2040 port had never run on HW). selftest 14/14 at 125 MHz over
      UART0/GP0; 125 MHz confirmed by a fixed-spin interval (2573 ms/20M = 16 cyc/iter @125,
      physically impossible at 12); XIP survives the clk_sys switch (boot2 SCK=31.25 MHz
      risk resolved -- code runs from flash at 125). Watchdog `/12` tick kept on clk_ref=XOSC
      so the 1 MHz TIMER stays correct.
- [x] **SAM3X8E / Arduino Due -- port validated on silicon 2026-07-09** (selftest 14/14,
      84 MHz PLL, `-b` GPNVM1 boot-from-flash + physical-RESET flashing flow). Crystal-race
      fix (bounded `pmc_wait` + MOSCXTST margin + RC fallback) landed as part of bring-up.
      **UNIT RETIRED 2026-07-14** (removed from the available-HW list): the physical board
      developed a peripheral-I/O fault -- core + flash-controller + native USB (SAM-BA) all
      verified working, but PIO output (PB27 LED) won't toggle and the UART console is dead,
      even under a provably-correct bare-metal blink flashed via two independent paths -> HW,
      not KickOS. Likely marginal all along (the MOSCXTST margin is a documented `GUESS`).
      Port stays proven; this unit is not a reliable target. See `docs/reference/boards.md`.
- [x] **XMC4800 120 -> 144 MHz** -- DONE, validated on silicon 2026-07-09: selftest 14/14
      at 144 over the J-Link VCOM (ttyACM0); 144 confirmed by the spin ratio (1938 ms @144
      vs 2306 ms @120 = 1.19 ~ 144/120). VCO 288/K2DIV=2; flash WS=4 unchanged (already
      correct); baud recomputed for fPERIPH 72 MHz. 144 was not a hard sweet-spot after
      all: the USB PLL is separate/untouched and WS=4 already covers 144.
- [ ] *(optional perf)* STM32F411 84 -> 96/100 -- deliberate sweet-spot today; only if we
      want the true ceiling. F302 is HW-capped (Nucleo has no HSE crystal);
      C6/K64F/RX72M/F103 already at max; ESP32/RP2040/XMC now at max (silicon-validated).

## M1 -- ESP32-C6

- [x] **Diag-LED (WS2812B on GPIO8) via RMT.** DONE @d76d187 -- RMT ch0 (20 MHz tick),
      routed to GPIO8, RGB-ordered (red = 0xFF0000), blinks the panic heartbeat;
      validated on silicon. (Bit-bang was infeasible -- GPIO write latency > the bit
      high-time; `rdcycle` traps on the C6.)
- [x] **selftest 10-14 pass/fail on silicon.** DONE -- all 14 PASS on silicon. Two real
      bugs fixed: (1) console rerouted from the native USB-Serial-JTAG (never delivers
      app output -- CDC host-draining gating + reset re-enumeration) to **UART0**, exposed
      as a stable COM port by the board's **CH343P bridge** (ttyACM0); (2) the inject
      doorbell programmed enable/type/prio/thresh into the **vestigial INTC/INTPRI block
      (0x600C5000)** -- the C6's real interrupt controller is the **PLIC (0x2000_1000)**;
      moved the config there and 10-14 deliver. (INTPRI keeps only the FROM_CPU trigger.)
- [x] **PMP NAPOT verified on silicon.** DONE -- a locked, no-permission BOUNDED 4 KiB
      NAPOT region correctly took a store-access fault (mcause=7, mtval=page) on the C6.
      So the M2 RISC-V NAPOT track is safe: only the *all-ones whole-space* NAPOT special
      case is unhonored (the M1 bootstrap already avoids it via TOR). Probe was throwaway.

## M1 -- hardware validation (batch when units are connected)

- [x] **blackpill** (F411 25 MHz HSE) + **f411disco** (F411 84 MHz) + **f302nucleo** (F302 16 K) +
      **bluepill** (F103 10 K clone) -- all HW-validated on silicon 2026-07-14 (blackpill/f411disco
      14/14 + bench; f302/bluepill 13/14, test 11 = RAM-size limit). Only **bluepill-c8** (genuine
      20 K F103) stays build-only -- a linker variant of the already-validated F103. (The 10 K
      `bluepill` clone has since been retired -- see docs/reference/boards.md; use `bluepill-c8`.)
- [x] **K64F revalidated on silicon 2026-07-15** (OpenSDA/J-Link): full selftest streamed
      in-order over the buffered console ring; bench re-confirmed 77 cyc / 641 ns switch (=> 120
      MHz), 160 cyc / 1333 ns IRQ-entry; fault-dump verified (UsageFault UNDEFINSTR -> HardFault).
      Its distinguishing feature -- the **SYSMPU** -- is the M2 enforcement backend, so K64F's
      formal M2 sign-off (per-task MPU trap) still lands there. Not an M1 gate; M1 was never a hole.
- micro:bit / nRF51 -- **QEMU-only; silicon bring-up not planned.** The nRF51 is discontinued
      (no silicon obtainable), so it stays an armv6m QEMU vehicle (`-M microbit`). A real-silicon
      port would also have needed an **RTC-based timer** (the nRF51 M0 has no SysTick).
- [ ] Panic/console review HW-checklist: RP2040 PL011 `TXRIS`-at-rest with FEN=0;
      ESP32 UART FIFO DPORT-vs-AHB alias; RX72M `SCR.TIE`-while-`TDRE` fires TXI. (All
      flagged HW-unverified in-code.)

## M1 -- fleet parity (audit 2026-07-09)

Capability audit across all arch/chip. Fleet is broadly uniform (every arch has a real
console, tickless timer, fault-register dump, inject-driven IRQ path, M2 MPU no-op).
Divergences worth closing for M1, most impactful first:

- [~] **mk64f diag-LED backend ADDED build-only @b5c5665** (RED PTB22 active-low) -- code gap
      closed; HW confirm folds into the M2 K64F SYSMPU bring-up (K64F is not an M1 gate, see above).
      **esp32(lx6) DONE** -- GPIO2 (silkscreen D2), validated with `blink` on hardware.
- [x] **IRQ default-mask posture unified** -- DONE @5da8a38: riscv/xtensa/sim now init their
      mask all-MASKED (matching ARM/RX); the reset contract is documented in `arch.h` (all
      lines masked at reset; a driver unmasks/irq_register-arms before use). Validated:
      selftest 14/14 on sim/qemu/qemu-riscv, no regressions.
- [x] **`arch_console_write_sync` uniformly bounded** -- DONE @9fd9623: stm32f103/f302/f411,
      rp2040, mk64f, esp32(lx6), sam3x8e all wrapped their unbounded TX-ready poll in a
      spin-then-drop guard (ceiling ~40-140 ms; esp32 200000). A wedged UART now drops bytes
      instead of hanging the panic path (the Due's solid-LED hang). fault_dump gates confirm
      a drained console still emits the full dump. (esp32c6/rx72m were already bounded.)
- [x] **ESP32-C6 real peripheral-IRQ path + buffered (ring) console -- DONE** (@cc4b236,
      silicon-validated). The C6 was inject-doorbell only; added its first real device-interrupt
      path: UART0 TX-empty -> interrupt-matrix source (0x600100AC) -> a dedicated CPU int (30) ->
      `switch.S` `.Lextdev` -> `kickos_rv_ext_dispatch_dev` -> the console line's ISR. Level source,
      NO PLIC claim (clears by de-assert, like the doorbell). selftest 14/14 over the buffered
      console (2048-byte ring > total output => proves the ISR drains it), inject path intact.
      *(anytime coherence -- was mislabeled "M2"; it's interrupt plumbing, no MPU dependency.)*
- [ ] *(driver-era, anytime -- NOT M2)* RX `kickos_rx_default_irq` real-peripheral-IRQ demux --
      still a stub (RXv3, a different arch than the C6, so its own work; same concept). Injected
      lines pass selftest but a real peripheral IRQ drops. The C6 `.Lextdev` design is the riscv
      reference pattern. **When the 2nd real device line lands** (fable review finding 5): the
      arch IRQ mask must reach the controller for real lines -- add an `arch_rv_hw_mask` twin (or
      gate `.Lextdev` dispatch on `g_irq_masked` + disable the source), else a tier-1 driver's
      mask-until-ack and the spurious-handler mask silently fail to stop a level source (storm).
      Unreachable today: the C6 console (line 16) is permanently owned + self-gates via INT_ENA.

## M1 -- misc

- [x] RX72M `arch_irq_unmask`: replaced the `IPR index == vector` assumption with a
      vector->IPR source table (`vector_to_ipr` + `kIprMap`); IR/IER stay 1:1, only the
      shared IPR is remapped. Byte-identical for the vectors used today (SWINT/CMTW/SCI6),
      so no runtime change now; correct for driver-era device lines. RX72M re-validated on
      silicon 2026-07-09 (selftest 14/14, rfp-cli/E2 Lite flash, SCI6 console on ttyUSB0).
- [ ] *(dev ergonomics, small)* **debug-in-sleep**: set `DBGMCU` `DBG_SLEEP`/`DBG_STOP` under a
      `KICKOS_DEBUG` gate so SWD survives the idle `WFI` (no connect-under-reset dance to reflash
      a running board). A per-chip one-liner in `arch_init`.

---

## Later -- not M1

**Milestones are keyed to their THEME, not sequence** (audit 2026-07-14). **M2 = MPU /
memory-protection enforcement**, specifically. Work that merely follows M1 is not "M2" unless
it needs the MPU -- the object-pool refactor, worst-case-ISR-latency perf, `sys_cpu_clock_hz`,
and the real-peripheral-IRQ demux are orthogonal (anytime coherence / M3-substrate), tagged
below where they were previously mislabeled.

- **M2 -- MPU enforcement** fan-out: reference pair (RISC-V PMP/NAPOT + XMC v7-M PMSA) ->
  K64F SYSMPU -> RX -> tail; + the arch-independent security model (domains, per-thread
  private stacks, syscall-arg/user-pointer validation, pow2 region placement). See
  `docs/reference/architecture.md` / `docs/m2-readiness.md`.
- **Driver era -- unprivileged MMIO drivers + peripheral-isolation ceiling** (needs the M2
  grant seam; the drivers themselves are anytime coherence). Status in `docs/m2-readiness.md`
  (Driver era subsection) + the fleet peripheral-isolation matrix in
  `docs/reference/architecture.md`.
  - [x] **MMIO-grant mechanism (task #9)** -- DONE + committed 2026-07-16.
        `kos_thread_params.mmio_base/mmio_size` (grant-at-spawn), the
        `arch_mpu_region_encodable` arch seam (exact-cover, no rounding), privileged-only
        `thread_spawn` validation, `domain_for` appends MMIO as a never-shared capability.
        PLUS a Critical fix: an unprivileged `mem_base` grant is now arena-bounds-checked
        (closed a peripheral/kernel-SRAM self-grant escalation). See `docs/design-task9-mmio-driver.md`.
  - [x] **K64F first unprivileged driver (k64drv, PIT)** -- DONE on silicon 2026-07-16;
        added the weak `arch_fault_report_extra` hook (K64F decodes SYSMPU CESR/EARn/EDRn).
  - [x] **SYSMPU peripheral-gating question -- ANSWERED on silicon 2026-07-16:** SYSMPU does
        NOT gate AIPS peripheral-bridge accesses under user mode; the AIPS bridge PACR does
        (per privilege+master, per 4 KB slot, NOT per-thread). So **per-thread peripheral
        isolation is IMPOSSIBLE on K64F**; it holds on the CPU-side-MPU chips (XMC PMSA,
        RISC-V PMP, RX MPU). Hardware-ceiling docs DONE (`reference/architecture.md` matrix +
        `book/peripheral-isolation-and-the-hardware-ceiling.md`).
  - [~] **F411 canonical per-thread PMSA driver (f411spi, SPI1 loopback)** -- BUILT +
        fable-reviewed; **silicon-validation still PENDING** although the disco has been on the
        bench (2026-07-29): the loopback arm needs the PA7->PA6 jumper fitted, and under the flip
        the app faults in its bring-up shim (see the stage-2 findings section). Its PMSA claim is
        no longer the only one -- `xmcspi` proved granted-works/ungranted-faults per thread on PMSA
        silicon in 2026-07 -- so this is now the STM32-family reference rather than a fleet gap.
        `docs/design-spi-driver-stm32f411.md`.
  - [x] **K64F/DSPI driver (k64dspi, DSPI0 for the KickCAT ESC SPI PDI)** -- DONE on silicon:
        the polled-FIFO transport (~10 MHz) reached OPERATIONAL against a real LAN9252. Exported
        as the `kickos_k64dspi` lib (`<kickos/driver/k64dspi.h>`, source `system/driver/mk64f/k64dspi`)
        so an out-of-tree consumer links it. Within the K64F coarse-peripheral ceiling (window
        grant is documentation, not enforcement); microkernel invariant kept (driver in userspace).
  - [x] **C6 PMP SRAM enforcement DONE on silicon** (18/18 selftest under enforcement +
        mpu_fault cross-domain trap, 2026-07-17) -- the earlier blockers (all-SRAM image /
        gp-relative small-data / code-from-RAM) were resolved. REMAINING (peripheral side,
        follow-on -- NOT needed for SRAM enforcement): a **separate APM/PMS bus permission
        unit** defaults deny-user on peripheral targets and needs a one-time global open (not
        per-thread) on top of the PMP grant before a C6 userspace driver reaches a peripheral.
        See the C6 row in `docs/m2-readiness.md` + `docs/design-c6-driver.md`.
  - [x] **RX72M MPU DONE on silicon** (selftest 20/20 under enforcement + mpu_fault
        cross-domain trap, 2026-07-17). REMAINING: m2-review-followup #5 (RX rounds
        misaligned regions instead of skipping -- fail-closed drift, build-robustness).
        See `docs/m2-review-followups.md`.
  - [~] **MPU-commit / deferred-switch soundness race -- armv6m FIXED, fleet-wide PENDING.**
        `switch_to()` calls `arch_mpu_apply(next)` EAGERLY, but every arch with a deferred
        (PendSV/software-IRQ) switch keeps running the OUTGOING thread with `next`'s region
        set until the physical swap -> it can fault on its own stack (or, worse, on a no-MPU
        build, silently run under the wrong isolation). Found on RP2040/armv6m under
        mutex-chain churn (selftest test 14 HardFault; cur/MPU=chA while chC physically ran),
        fixed by committing the MPU in the PendSV epilogue (armv6m `kickos_armv6m_mpu_commit`,
        silicon 42/42 on the 50ms x300 repro). LATENT the same way on **v7-M / RX / RISC-V**
        (all eager-apply + deferred switch) -- unobserved there under looser timing, but a real
        soundness hole. Complete fix = move MPU-commit into EACH deferred arch's switch
        epilogue (stash-in-apply / commit-after-swap). GATE ON A FABLE REVIEW + per-arch
        silicon re-validation before it lands (core switch-path change). Pre-M4.
- **[M4] level-trigger tier-1 bindings.** The tier-1 IRQ contract is now latch-and-coalesce
  (a raise on a masked line latches one-deep, redelivered at unmask -- edge-safe, no lost
  pulse). A LEVEL source needs the opposite at rearm: after the driver clears the device, a
  still-asserted line must NOT redeliver a stale latch. The seam is already in place --
  `arch_irq_clear_pending` (added with the coalesce fix) discards the latch; the M4 work is a
  per-binding trigger-type bit in `IrqBinding` (default EDGE) that, for LEVEL sources, makes
  the `irq_wait`/`irq_ack` rearm do `arch_irq_clear_pending(line); arch_irq_unmask(line)` (a
  genuinely-asserted level source re-latches on its own; a deasserted one stays quiet). NOT
  added now: no user/test drives a level binding yet (milestone discipline -- the API bit lands
  with its first consumer). Phantom-defense for level devices lives here too.
- **[M4, lands with bulk-rearm] identity-free coalesced redelivery on the software backends.**
  Today sim/rv32imac/xtensa/rxv3-soft carry a coalesced redelivery through ONE shared cell
  (`pending_irq` / `g_inject_line`) + one physical doorbell, clearing the per-line pending bit
  as it is rung -- so AT MOST ONE `arch_irq_unmask` with a pending redelivery may fire per
  IrqLock region (a second clobbers the first and loses an event). Safe today (register/wait/ack
  each unmask exactly one line per lock section), but a future BULK-rearm path (re-arm many lines
  under one lock) would violate it. Fix when that path lands: stop clearing `g_irq_pending` at
  ring time; have the doorbell dispatcher drain `g_irq_pending & ~g_irq_masked`, looping
  `kickos_isr_irq` over the set bits. Contract stated at the `arch_irq_unmask` decl (arch.h).
- **[anytime coherence -- NOT M2] object-pool mutualisation** -- DONE (step 1). The semaphore
  pool is a generational `SlotPool<T,N>` (slotpool.h); the thread pool is grouped into a
  tailored `ThreadPool` struct (thread.h) -- deliberately **not** SlotPool: thread liveness is
  intrinsic (`state==EXITED`) and its generation bumps at *reclaim* (so a future join-by-handle
  can still resolve a just-exited thread), genuinely different from the sem pool, so forcing
  one pool would be false-DRY. Full unification (a shared handle codec across sems + the M3
  capability store) waits for that genuine second SlotPool-shaped case. (No MPU dependency --
  was mislabeled "M2 handle table"; it's the M3-caps substrate + anytime coherence.)
- **[anytime coherence -- NOT M2] general freeing allocator (M5).** `arch_ram_alloc` is a
  wholesale bump allocator (freed only at reset). Default thread stacks now reclaim via a
  single-size-class intrusive free list in `ThreadPool` (thread.h) -- the special case that needs
  no size metadata (one class == `KICKOS_USER_STACK_SIZE`, link stored in the dead block). A
  GENERAL multi-size-class freeing allocator for `arch_ram_alloc`/`kos_ram_alloc` at large is M5;
  it would subsume this free list. Until then, only default stacks are reclaimable. M5 should also
  reclaim the per-allocation ALIGNMENT RUN-UP, which is dropped on the floor today -- see the
  M4.5.1 item above (it is why boot-stack allocation order is load-bearing).
- **[anytime coherence -- NOT M2] user-pointer validation at the syscall boundary.** M2 is MPU
  *enforcement*; validating a user pointer is arch-neutral kernel logic that matters MORE at M1
  (no MPU to contain an OOB access -- see the `user-args-validated-at-boundary` invariant).
  Cheap parts DONE (fable code review): thread name copied into a bounded TCB buffer (fault path
  never derefs/`%s` a user pointer); `clock_now` out-pointer null+8-byte-alignment checked;
  `thread_spawn` stack `base+size` wrap checked; `SlotPool::resolve` rejects a dirty handle top
  byte. Remaining: copy-in the `kos_thread_params` struct via a checked read, and bound-check
  writable out-pointers (`clock_now`) + the `write()` buffer against the caller's granted region
  -- this last part wants the M1 region-ownership model pinned (privileged = whole arena,
  unprivileged = `mem_base`) so it rejects bad pointers without rejecting legit threads.
- **M3 -- capabilities + authenticated grants** (seL4-principled object model), **and
  user-selectable CPU clock / low-power mode** (needs explicit per-chip clock bring-up
  first, from the audit above).
  - [x] **Per-task capability handle table (sem ABI: global ids -> per-task caps)** -- DONE,
        silicon-validated under enforcement on ALL FOUR M2 mechanism classes: K64F SYSMPU,
        XMC4800 PMSA, RX72M RX-MPU, ESP32-C6 PMP -- each 21/21 selftest under enforcement
        (incl. domain_share / mmio_grant / confused_deputy + the close-while-parked sem test).
        `CapEntry` table embedded in the TCB (`cap.h`), single `cap_resolve` chokepoint
        (per-task cap-gen then global object-gen), rights WAIT/SIGNAL/TRANSFER each enforced at a
        real site, refcounted destroy-on-last-close, `KOS_SYS_handle_close` (renamed from
        `sem_destroy`), authenticated-grant spawn delegation (subset-only rights narrowing,
        validate-before-claim, B1 handle==index-on-a-fresh-table deterministic placement).
        Reference: `docs/reference/architecture.md` + `invariants.md`; teaching: `docs/book` ch 8.1.
  - [x] **`sys_cpu_clock_hz()` syscall** -- DONE @638620d, already on master (build+sim/qemu verified). Read-only
    `KOS_SYS_cpu_clock_hz` via the `arch_cpu_clock_hz()` seam (mirrors `clock_now`), value
    returned in-register (no out-pointer), each backend reuses its CMSIS `SystemCoreClock`;
    sim returns 0. selftest `t_cpu_clock_hz` covers both branches; all 5 ISAs + sim build,
    runtime green on sim/armv7m/rv32imac. Read-side precursor to user clock-select below.
- **[anytime perf -- NOT M2] worst-case ISR latency (shorten interrupt-masked critical
  sections).** Scheduler/switch-path timing, gated on a worst-case-latency probe -- no MPU
  dependency (was mislabeled "M2"). The uniform bench surfaced that under sustained syscall
  load the kernel spends too long masked. Ranked plan (see `docs/archive/M1_state.md` section 3.1):
  - [x] **R2** -- armv7m: skip the redundant BASEPRI raise + DSB/ISB on nested IrqLocks
        (only the outer raise needs them). Landed `5ba57fd`. Correct (ctests green) but
        **below the current bench's noise floor** -- see the measurement gap below.
  - [ ] **R1** -- thread a single `now` through switch_to->ktime_rearm->arch_timer_arm +
        arm_slice (kills the 3x arch_clock_now pileup per RR switch; on RX each is a
        nested lock + two 64-bit divides). Cross-arch signature change.
  - [ ] **R3** -- fold the min-delta clock read past arch_timer_arm's idempotency guard
        (so an unchanged-deadline re-arm reads the clock zero times). Combine with R1.
        R3b: add the idempotent-arm guard to xtensa.
  - [ ] **R6** -- xtensa: its cooperative switch runs INLINE under RSIL (masked), unlike
        the 4 other arches that defer the register save/restore to an unmasked handler.
        The one structural outlier; **high risk** (touches windowed-switch atomicity).
  - **Measurement gap (do first):** the current bench measures throughput + *best-case*
    IRQ entry (reporter injects while uncontended), NOT masked-span delay -- so R1/R2/R6
    are not demonstrable with it. Need a worst-case-ISR-latency probe (inject while a
    masked syscall span is in flight) to justify + validate these before landing R1/R6.
  - Note: the earlier **bench self-report starvation is already FIXED** by the
    reporter-as-root/woken-by-workload redesign (not a timer sleep).
- **Console device handover (driver era)** -- userspace UART/console driver takes the
  peripheral as a capability; kernel relinquishes it (`console_tx_deinit`), panic path moves
  to a kernel-retained transport. See `docs/reference/console.md` "Future".
- **[M4.x] Per-thread libc state via real TLS (local-exec).** No per-thread userspace storage
  exists today (newlib `--disable-threads`, threads share one flat image, only the kernel TCB is
  per-thread) -- so `errno` is a shared global, libc `malloc` is not thread-safe (`__malloc_lock`
  is a no-op stub; tracked as its own item in the M4.5.1 kernel-audit section above), and
  `thread_local`/`__thread` silently break. "Fully usable" needs these, so real TLS is
  the compliant mechanism (not a newlib `_REENT`-swap hack, which would still leave `thread_local`
  broken): a per-thread TLS block in the thread's data grant + a per-arch thread pointer set on the
  context switch (ARM `TPIDRURW`, RISC-V `tp`, Xtensa `THREADPTR`; RX has no TLS register -> sw-tp
  spike), local-exec model (fully static / no dlopen -> offsets fixed at link). `errno` + newlib
  reent + `thread_local` all ride on it (one mechanism). Prereq SMP (M5) needs anyway. First sibling
  of this family LANDED (M4.3): the `_write` stdout re-probe -- deleted the process-global sticky
  `g_stdout_probe` (per-invocation classify against the calling thread's own cap 0; no per-thread
  storage needed for it).
- **M5 -- multicore (AMP first on RP2040, SMP-BKL endgame on RP2350).** Design spikes
  2026-07-19: `docs/design-multicore.md` (AMP-vs-SMP feasibility on rp2040 + rp2350) and
  `docs/design-multicore-ipc.md` (the RP2040 cross-core IPC); the SMP candidate ranking + staged
  model + the SMP-is-per-chip-capability constraint are in `docs/design-m5-smp.md`. Candidate
  ranking by the real gate (inter-core atomic + arch-switch maturity): **RP2350 BEST** (M33
  LDREX/STREX enable fine-grained; also 2x Hazard3 -> prove SMP on ARM and RISC-V of one chip),
  **RP2040 big-lock-only** (armv6m has no exclusives; SIO hardware spinlocks -> single big kernel
  lock forever), **ESP32 LX6 last** (S32C1I CAS exists but windowed ABI is hardest; unblocked now
  that the fresh-thread-start bug is fixed at 700ec98, still gated on the model proven on M-profile
  first). Staged: (1) big-kernel-lock SMP first (correct on every dual-core, single-core build
  byte-identical), (2) fine-grained only where exclusives exist (RP2350), (3) LX6 after. The spike REVISED the earlier
  "SMP-only, NOT AMP" call below: ARMv6-M (M0+) has no atomics (no LDREX/STREX; the SIO bus is
  non-atomic too), so RP2040 SMP is capped at coarse Big-Kernel-Lock forever -- AMP (two
  core-private kernels + IPC) is the better FIRST step there, and fine-grained lock-free SMP is
  reachable only on RP2350 (M33 exclusives / Hazard3 A-ext). AMP + IPC and the invariant
  refactors are the near-term items; the SMP-BKL plan (one kernel image across cores) stays the
  endgame. Motivation: run the
  dual-core RP2040 (picopi) at 100% under a single KickOS. Biggest architectural axis on the
  roadmap -- it reworks the *foundation*, not a feature: the whole kernel's mutual exclusion is
  `IrqLock == arch_irq_save` ("interrupts off => exclusive"), which is a single-core-only
  guarantee (masking IRQs on one core does nothing to another). Plan:
  - **Step 1 -- Big Kernel Lock.** Redefine `IrqLock` as "disable *local* interrupts + take one
    global spinlock." Centralised, so it's a redefinition of one class, not a 200-site audit;
    every existing critical section keeps working, kernel is SMP-*correct* (coarsely). For a
    2-core MCU this likely already gives ~2x (user threads run concurrently; only syscalls
    serialise on the BKL). Per-core run-queues + finer locks come later as *optimisation*.
  - **RP2040 specifics:** M0+ has **no atomics** (no LDREX/STREX) -> use the **SIO hardware
    spinlocks** (32 in the SIO block) for the lock; launch core 1 via bootrom/SIO-FIFO
    (`chip_rp2040.cc` already notes the core-1 milestone + the single-core `TIMELR/TIMEHR`
    latch); per-core SysTick + per-core tickless state.
  - **Already seam-ready:** the `KICKOS_*_BARRIER` publish seams (console_tx / rtt) are the
    fence-injection points -- flip to real fences on the SMP build. Keep centralising `IrqLock`,
    structs-over-globals, no ad-hoc masking -> keeps this a redefinition, not a rewrite.
  - Fits the seL4 endgame (seL4 ships a big-lock SMP variant). See `roadmap.md` (M5).
  - **AMP-first on RP2040 (spike verdict, the recommended near-term step).** Two core-private
    `Kernel` instances -- the `KICKOS_MULTI_INSTANCE` per-instance seam (`instance.h:89`, built
    for the KickCAT multi-slave sim) is the ~80% substrate; re-key it on SIO CPUID instead of
    host-TLS. Each core keeps its own run queue + `IrqLock`==PRIMASK, so NO mutual-exclusion
    refactor: AMP de-risks the shared mechanics (core-1 launch, IPC, console arbitration) that
    SMP also needs, and sidesteps the no-atomics problem entirely.
  - **Cross-core IPC -- required for AMP; none exists today** (`Semaphore`/`Mutex` are intra-core
    only). Design in `docs/design-multicore-ipc.md`: a per-direction SPSC ring in a shared-SRAM
    window (one writer per index + `DMB` ordering -> no lock, no atomics needed on M0+) with the
    SIO 8x32 FIFO used only as a doorbell (write a tag, raise `SIO_IRQ_PROCn`). API = a `Channel`
    (ring + a `Semaphore` in the receiver's kernel) exposed as `KOS_SYS_chan_{open,send,recv}`;
    blocking `recv` parks on the local run queue via `sem_wait`, the peer's SIO ISR drains + wakes
    via the already-ISR-safe `sem_post`. New arch surface is small: `arch_cpu_id`, `arch_dmb`, and
    an `arch_ipc_notify`/`arch_ipc_drain` doorbell pair (so RP2350 SIO-v2 doorbells back the same
    API). The one genuinely-new isolation decision: a fixed `.shared_ipc` region (pow2 for PMSA)
    granted R|W in BOTH cores' MPU sets -- the ONLY cross-core-writable memory; everything else
    stays per-core-private, preserving the per-core-MPU isolation the AMP verdict rests on.
  - **Three single-core invariants to refactor (either path)** -- `IrqLock`==PRIMASK (local-only
    masking; -> BKL or per-core), the single global current-thread/run-queue (per-CPU), and the
    unsynchronised console + boot-on-one-core + single `arch_mpu_apply`. The arch globals
    `g_arch_current`/`g_arch_next` (+ rv32imac `g_isr_depth`/`g_clint_msip`) are the shared
    prerequisite that gates even AMP.

## Pre-M4 perf: caches / flash accelerators (fleet audit 2026-07-22)

Per-chip audit (each vs its RM; see `CONTEXT.local.md` for the local RM set): does the HW have a
software-controllable cache/accelerator, and do we use it? Binary, not "fast enough".

- [x] **RX72M: enable the 8 KB ROM cache** (pre-M4) -- DONE (5ab2575). `rom_cache_enable()` in
      `chip_rx72m.cc`: after clock-up, `ROMCIV`=1 + bounded poll, then `ROMCE.ROMCEN`=1 (not
      PRCR-gated; 16-bit access). Silicon-validated UNDER ENFORCEMENT: selftest 43/43 + soak 389,
      and the enforce bench went 46772 -> 15405 ns/sw (~3.0x, the flash-instruction-fetch win).
      Caveat carried forward: invalidate after any future flash self-program (auto-invalidated at
      reset today). RM sec 64.4.1/64.4.2/64.7.1.
- [x] **Teensy M7: enable the D-cache** (pre-M4) -- DONE. Silicon-validated (selftest 43/43 +
      a ~38 M-switch soak under enforcement, a measurable throughput win) and made the imxrt
      default (`KICKOS_IMXRT_DCACHE ON`, `arch/CMakeLists.txt`); enabled via
      `kickos_armv7m_dcache_enable()` in `chip_imxrt1062.cc` arch_init. Safe today (single-core,
      no DMA); the coherency obligation arrives with M4-era DMA (non-cacheable DMA pool or
      per-buffer clean/invalidate) -- carry this into the M4 driver work.

Fleet re-validation follow-ups (from the 2026-07-22 M3-branch gate; see `docs/archive/M3_raw_meas.md`):
- [x] **WROOM (Xtensa LX6) soak wedge -- FIXED (700ec98).** Was pre-existing (master), Xtensa-only.
      Root cause: `arch_context_init` started fresh threads via a fabricated `retw` into a trampoline
      with NO `entry` instruction (phantom window frame, garbage caller-linkage); a worker running
      entry->run->EXIT with no block walked WindowBase around the 64-AR/16-slot file until it collided
      with the phantom frame -> spill garbage -> branch into stack -> silent halt (boundary ~4 = file
      size / per-thread window use). Fix: start fresh threads via the `rfe` path with a real `entry`
      prologue (FreeRTOS/NuttX-canonical); COOP block/resume untouched. Fable-reviewed SOUND;
      HW-validated on WROOM (soak 25/25, selftest 41/41 regression, bench baseline). This also
      unblocks the ESP32 LX6 SMP path.
- [x] **RP2350 (Cortex-M33) ARMv8-M PMSAv8 MPU backend** -- DONE (e2179da). {base,size,attr} seam ->
      RBAR/RLAR base+limit + MAIR indirection; strong `kickos_arch_mpu_commit` override on the shared
      deferred-commit seam (K64F precedent); compile-time-gated so the v7-M/v6-M fleet is byte-identical.
      Fable-reviewed SOUND; silicon-validated on RP2350: selftest 43/43 under enforce, `mpu_fault` clean
      cross-domain MemManage denial, bench + soak 411+ no fault. RP2350 now enforces. (Advisories A-D
      below are the non-blocking follow-ups.)
- [ ] **[post-M4] Port the Thread-Metric benchmark suite to KickOS** -- so we can compare honestly
      against FreeRTOS / Zephyr / ThreadX / PX5 (all run Thread-Metric). Run all contenders on ONE
      board at ONE fixed clock, MPU-on-both-sides where applicable, reporting core/clock/MPU/flags +
      the exact "what is a switch" definition. Published raw-switch figures put KickOS's bracketed
      switch (~66-83 cyc M4/M7) in the ChibiOS band -- but every public number is no-MPU/monolithic,
      so only a like-for-like suite run is defensible. (Zephyr's ~468-524 cyc coop figure looks
      inflated by default-config/methodology, not the kernel -- the suite run would settle it.)
- [ ] **RP2350 v8-M backend advisories A-D (fable review, non-blocking hardening).** From the
      PMSAv8 backend review; none block first enforcement, all are build-robustness / fail-closed
      drift.
      (A) **Fail-closed on non-32-exact regions** in `arch_arm_pmsav8.cc` commit -- mirror rxv3's
          per-region `arch_mpu_region_encodable` check and SKIP (not round) an unencodable region,
          since `__kickos_appdata_start` abuts kernel `_ebss`.
      (B) **Alignment ASSERT** `ASSERT((__kickos_appdata_start & 31) == 0)` in `rp2350.ld` (and add
          the same to `mk64f.ld` -- same latent edge).
      (C) **`DREGION >= kMaxPendRegions` boot check** in `kickos_arm_pmsav8_init` (read
          `MPU_TYPE.DREGION`, do not hard-code 8; fail loud if the budget does not fit).
      (D) **Comment nit** `arch_arm_pmsav8.cc:45-46` / `regs_v8m.h:36-37` -- the PRIVDEFENA-background
          note overstates: a MATCHED region's AP also bounds privileged access.
- [ ] **Skip-if-unchanged MPU-commit optimization (post-M3, fleet-wide perf).** The per-switch
      `kickos_arch_mpu_commit` reprograms the MPU + issues DSB;ISB UNCONDITIONALLY every switch
      (measured ~2.3x throughput cost on RP2350 enforce vs mpu-off). Skip the reprogram + barriers
      when the next thread's region set is unchanged (same-domain switch / region-set generation
      compare). Helps EVERY enforce board. Note the SMP caveat already flagged in
      `docs/design-rp2350-mpu-armv8m.md`: any such cache must be per-core (or omitted) under M5, not
      a shared static.
- [ ] **ESP32-C6 enforce-bench ns-scaling** (measurement-only, not M3). `cyc` counts correct; ns
      ~8x high because `rdcycle` traps on the C6 so the bench samples an MMIO counter whose rate
      differs from `SystemCoreClock`. Also RP2350 bench `irq` reads a bogus 1 cyc (irq-probe not
      wired for the M33). Per-chip bench-instrumentation cleanup, not a kernel bug.
- **No gap (already accelerated), for the record:** STM32F411 ART (ICEN|DCEN|PRFTEN + 2WS,
  `chip_stm32f411.cc:171`); STM32F103/F302 prefetch buffer (M3/F3 have no I/D cache in HW);
  K64F FMC cache+speculation on by reset default (`PFB*CR=0x3004001F`); XMC4800 PMU buffers
  default-on + WS set (`chip_xmc4800.cc:373`); RP2040 XIP cache on by bootrom; ESP32-C6 cache
  fronts external flash only -> irrelevant to KickOS's HP-SRAM execution.
- **RP2350 (deferred M4): XIP cache on by reset + bootrom-invalidated -> NO enable needed** (unlike
  the M7). No Device anti-speculation wrap either -- the M33 isn't speculative and the QMI
  bus-ERRORS (not stalls) on unbacked reads. For the PMSAv8 backend, carry: (1) bound the RX
  region to actual code extent (RLAR arbitrary limit, no pow2 pad) -- the M7 "bounded code"
  lesson; (2) set `XIP_CTRL.NO_UNCACHED_*`/`NO_UNTRANSLATED_*` so mirror-window aliases
  bus-error (saves MPU/SAU regions); (3) MAIR NORMAL-WBWA on the flash region so the cache
  serves hits under enforcement; (4) invalidate-by-address after any future flash program.
  Fold into `docs/design-rp2350-mpu-armv8m.md`. (Also: `docs/design-rp2350.md:12` doc-drift --
  says UART0/GP0-1, actual port is UART1/GP4-5; fix when that file is next touched.)
- Common caveat for ALL the flash caches/buffers: they are NOT coherent across a flash
  program/erase -- any future in-field flash-write/OTA path must invalidate the relevant
  cache/speculation buffer. Not a live risk (KickOS is a fixed flash image today).

## Post-M6 optimizations (not scheduled)

- [ ] **RISC-V context-switch cost** (post-M6, fable-gated) -- the rv32 trap saves the full
      integer file (~60 stack words/switch vs armv7m's ~18); ~3.5x per-handoff, general to RISC-V
      (Hazard3 shares it, NOT C6-specific). Levers: (a) cooperative fast-path (callee-saved-only
      voluntary switch, ~2x, portable incl. C6); (b) optional Zcmp `cm.push`/`cm.pop` compile-gated
      path (Hazard3-only, code-size mainly). Prerequisite: fix the rv32 bench bracket (it currently
      excludes the save/restore). Full design in `docs/design-riscv-switch-cost.md`; roadmap
      "Later". Surfaced by the M3 C6 enforcement soak (C6 ~10.5k iters vs XMC ~33.9k, same window).

- [ ] **ARMv8-M TrustZone kernel-confinement backend -- opt-in, per-chip** (post-M6, fable-gated,
      needs the M4 service model + M5 SMP settled). The armv8-M-with-Security-Extension mechanism for
      kernel confinement: kernel/TCB in Secure state, apps in Non-secure. NOT per-task isolation and
      NOT an MPU replacement (MPU_NS still does all per-task work, same per-switch cost); it is the
      strongest armv8-M realization of "Option B" (confine the kernel), layered ON TOP of Option B,
      not instead of it. Buys a hardware TCB boundary (NS-privileged cannot touch Secure memory) + a
      PSA-style secure-services partition for roots-of-trust that fits the capability-gated-services
      model. Machinery: SAU/IDAU partition, secure-gateway veneers + S/NS call ABI, banked SPs, NVIC
      ITNS interrupt targeting, a separate Secure build/link. Per-chip capability -- M23/M33/M55/M85
      MAY implement it, detect + fall back to Option B alone; RP2350's M33 is a concrete target (also
      the PMSAv8 + SMP target). Security/assurance play, not perf. Design in
      `docs/design-armv8m-trustzone.md`.
- [ ] **Confine the trusted kernel with an explicit MPU map ("Option B") -- FLEET-WIDE hardening**
      (post-M6, fable-gated, per-arch). Today privileged/kernel execution runs UNCONFINED on each
      backend's permissive background; a kernel wild pointer rides it silently instead of faulting.
      Option B removes that background so even the kernel is confined and a stray kernel access
      FAULTS (defense-in-depth / debuggability -- catch our own bugs early; NOT a security boundary,
      the kernel is trusted). This is NOT a bug fix anywhere -- the M7 speculation stall is already
      closed by "Option A" (wrap the leaky external Normal bands, keep PRIVDEFENA;
      `docs/design-teensy-mpu-hang.md`); no other arch has that stall. Per-arch mechanism:
        - armv7m/armv6m PMSA (XMC/F411/RP2040/microbit): drop PRIVDEFENA + region-0 4 GiB
          Strongly-ordered/no-access/XN floor + explicit kernel regions (code RX, RAM RW, periph
          Device). M0+ is region-tight (8 descriptors).
        - K64F SYSMPU: restrict RGD0 (today supervisor-full) + explicit supervisor RGDs.
        - RISC-V PMP (C6): LOCKED PMP entries (bind M-mode too).
        - RX-MPU (RX72M): restrict the supervisor region set. Xtensa (WROOM): N/A (no MPU).
      Cost: forks the fleet-wide "privileged = background" contract every board rests on (incl. the
      armv7m non-pow2-arena-drop path) -- needs a per-arch fable pass + probe-ful bring-up.
