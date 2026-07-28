<!-- SPDX-License-Identifier: CECILL-C -->
# KickOS TODO

**M1 VALIDATION COMPLETE (2026-07-14)** -- 10 boards on silicon (5 ISAs) + 3 emulator gates
green; every board boots, has a console, runs the selftest, panics visibly, and runs at its
true (or safely-degraded) clock. Full record in `M1_state.md`. The items still open below are
either optional perf, deferred to M2, or non-gating HW-unverified notes -- none block M1.

Living checklist for **M1** (uniformity / bring-up). Check items off as they land -- this file,
not memory, is the source of truth for "where are we". M2 (MPU enforcement) and M3
(capabilities + clock-select) items are parked at the bottom so they aren't lost.

This file is the **granular, actionable** status. The milestone-level plan (the general idea
per milestone) is `roadmap.md`; validated end-state + per-board detail is `M1_state.md`; the
board/console readiness matrix is `docs/m2-readiness.md`.

## Session record -- 2026-07-27 integration (READ THIS FIRST IF RESUMING)

Three lines of work were integrated onto `M4.5.1-ci-hardening` and a day of decisions was taken
in conversation. This section exists because none of that was in the repository, and a decision
that lives only in a chat log gets re-litigated. **Everything below is maintainer-confirmed.**

### Where the branch is

`M4.5.1-ci-hardening`, **27 commits** on top of `13eab8f`
(`refs/backup/integration-base-M4.5.1-ci-hardening`) as of this commit; re-derive with
`git rev-list --count refs/backup/integration-base-M4.5.1-ci-hardening..HEAD` rather than
trusting the figure, which goes stale on every commit. Linear, no merges. The two transport
branches (`m4.5.1-book-syscall-privilege`, `m4.5.1-stage2-silicon`) were replayed and **deleted**;
their pre-replay tips survive as `refs/backup/integration-book` (`a88ddef`) and
`refs/backup/integration-silicon` (`6458d59`). Not pushed, and no upstream is configured.

**One branch only.** Worktree branches are transport: replay them, delete the branch, remove the
worktree with `git worktree remove`. The `.claude/worktrees/*` entries still listed belong to
other efforts (m4.6, spi-bug, m4.5-squash) and were left alone.

### Decisions taken (do not re-open without new information)

- **`kos_reboot` folds into `AUTH_DEVICE`**, rather than taking a capability at a reserved index.
  `KOS_CAP_RESERVED3` therefore **stays free**, which is worth more than bit granularity: spending
  the last well-known index forces the next one to raise `KICKOS_CAP_FIRST_DYNAMIC` and costs a
  dynamic slot on all four 9-handle boards. Recorded in full in the `kos_reboot` section below and
  in `docs/design-unprivileged-root.md` section 9.
- **`kos_ram_alloc` gets an explicit self-grant**, not an implicit one at alloc: `AUTH_MEMORY`-gated,
  bounded by `KICKOS_MPU_MAX_REGIONS`, failing loud with `-KOS_ENOMEM`. **LANDED** as
  `KOS_SYS_MEM_SELF_GRANT`.
- **The selftest gate asserts a named, posture-dependent expected-skip list**, not a skip budget.
  **LANDED** as `EXPECT_SKIPS`.
- **`KOS_CAP_SERVICE` is retired.** The ABI is not stable yet, so a superseded spelling is deleted
  rather than deprecated. **LANDED.**
- **clang-format is decided against** as a gate -- see the CI-hygiene section.
- **The canvas cites closing commits by SUBJECT, not by hash.** Hashes move under rebase; this
  branch proved it (the A/B witness hash `a463ab9` had to be re-resolved to `22e1c5a`).
- **The canvas is mirrored into the repo** so git carries its history. The Cursor path stays the
  live file; the repo copy is a mirror. **LANDED** at `docs/audit/` (`m4.5.1: mirror the audit
  canvas into the tree`, 675062d): the committed copy, a `docs/audit/README.md` carrying the mirror
  rule, and a pointer from `docs/README.md`. `diff` between the two copies is the staleness check,
  so a canvas edit refreshes the mirror in the same commit.
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

### The tiering measurement, taken 2026-07-27 (this is the "measure first" result)

The premise was that the fleet defaults to `Debug` with no optimisation, inflating flash ~26%. It
is **already handled**: `CMakeLists.txt:114-118` applies `-Os` across the selftest tree for exactly
`f302nucleo` and `bluepill-c8` when `KICKOS_ENABLE_SELFTEST` is on, described in-file as a holding
measure pending N16. Measured on the current tip, **with** the new `mem_self_grant` test in the
suite (`text + data`, against 64 KiB of flash):

| Board | flash used | free | headroom |
| --- | --- | --- | --- |
| `f302nucleo-st` | 49,604 B | 15,932 B | 24.3% |
| `bluepill-c8-st` | 49,436 B | 16,100 B | 24.6% |

**So size-aware presets have dissolved the problem and tiering is unnecessary for now.** Both
boards carry the fleet-uniform suite with ~16 KiB spare. Revisit only if headroom actually erodes;
the remaining N16 question is narrower than tiering -- whether to keep the two-board `-Os` block,
widen it to the fleet, or replace it with per-preset build types.

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
number: the canvas and the XMC entry under Blockers below both point at **item 5**.

1. ~~**Measure `-Os` on the tight boards**~~ -- DONE above. Outcome: **tiering not needed.** What
   remains of N16 is only the narrower question of how `-Os` should be expressed.
2. **Selftest tiering** -- **do not build it** unless headroom erodes. Kept in the queue only so
   the next reader sees it was considered and refuted by measurement, not forgotten.
3. ~~**Non-goals into `docs/reference/architecture.md`, appended to the existing `## North star`
   section.**~~ -- DONE (79b7a37). All four landed with the arithmetic that refuted them (no untyped
   memory / `Retype`; no CNodes; no derivation tree; no per-instance capabilities), under
   `### Non-goals -- seL4 machinery deliberately NOT adopted`, and the common thread is stated: the
   16-slot ceiling with 9-handle boards under it. **That section's `read`/`open`/`socket` sentence
   stays alone** -- the maintainer reads it as a design rule, not a status claim. It was not
   touched, and should not be by a later pass.
4. ~~**Record the sequencing note** (M4 driver breadth and M5 SMP behind goal 1) in `roadmap.md`.~~
   -- DONE (a5fc422), as a block quote under `## Next`.
5. **Move the XMC USIC bring-up into the granted driver thread.** The blocker is placement, not
   silicon -- see the corrected entry under Blockers below. Unblocks `xmcssc` on a flipped board.
6. **`stm32f103` `arch_mpu_min_region()` override.**
7. **Re-point `kernel_ctor_placement` at the `cxxtest` ELF** (it is vacuous where it is now; see
   the finding above -- these two are the same problem and can land together).
8. **CI hygiene set, minus clang-format** (which is decided against).
9. **Branch-wide comment sweep** -- last, because it touches everything.
10. **Commit-message reword as a SEPARATE step after the sweep**, not folded into it.
11. **Canvas subject-citation pass** -- after the canvas edits, so it runs over the final text. Now
    that the mirror exists, this pass refreshes `docs/audit/` in the same commit, or `diff` stops
    being the staleness check.

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
- [ ] **Override `arch_mpu_min_region()` to 0 in `chip_stm32f103.cc`.** STM32F1 has no MPU, but
      the chip inherits the v7-M pow2 minimum and pays its alignment tax anyway -- which is what
      took `bluepill-c8`'s free arena to zero. A 0 override drops the tax on a part that has
      nothing to enforce.
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
- [ ] **The whole fleet builds unoptimised.** Every preset sets `CMAKE_BUILD_TYPE=Debug`, whose
      default flags are `-g` with no `-O`. Measured on f302nucleo's selftest image: 65,720 bytes
      at the default against 48,848 at `-Os`, a 26% reduction with no source change. Only
      `selftest`'s OWN TUs opt into `-Os` today. This is the root cause of the 64 KiB squeeze
      (N16 above), it inflates every board's flash and every switch's I-cache footprint, and it
      makes the published bench figures a measurement of unoptimised code. Deciding the fleet
      posture is a maintainer call with real trade-offs (debuggability, and `-O` changing what a
      gate proves), which is why it is filed rather than taken.
- [ ] **`kickos_core` no longer carries the archive group.** c539d1c moved the RESCAN group onto
      the `kickos` / `kickos_cxx` leaves, because the two postures need different toolchain
      runtimes in it and CMake forbids one target's closure carrying a library in two groups. A
      consumer linking `kickos_core` DIRECTLY now gets usage requirements but no archives. The
      documented contract already says consumers link a leaf and never core, and both
      out-of-tree export gates pass -- but core is still in the export set, so the contract is
      now load-bearing where it used to be advice. Either state it in the exported package or
      make linking core alone a configure-time error.
- [ ] **The record cites commit hashes a rebase has rewritten.** Found while re-resolving the
      audit canvas: of the 56 hashes it cited, 37 named commits unreachable from `HEAD`, left
      behind by the message-trim rebase of this branch. 32 were remappable (patch-id, or a unique
      exact subject) and are fixed; the other 16 were squashed by that rebase and have no single
      successor, so they now resolve only against `backup/m4.5.1-pre-msg-trim`. This file has the
      same class of rot in six places (`5ab2575`, `638620d`, `700ec98`, `ade1879`, `c072712`,
      `e2179da`), pointing into `backup/m3-pre-squash` from the M3 squash. A hash written into a
      durable record before its branch is rewritten is a citation with a shelf life, and nothing
      warns when it expires. Either cite by subject and date, or re-resolve after any rebase
      (`git patch-id --stable` maps a reworded or rebased commit to its successor mechanically).
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
      link-only job is the whole value at none of the runtime cost. Both link today with ~16 KiB
      spare (measured in the session record above), and the job is what keeps that true.
      Note for whoever adds it: `-Os` is applied to precisely these two boards under
      `KICKOS_ENABLE_SELFTEST` (`CMakeLists.txt:114`), so the job must configure with the selftest
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
      Canvas finding **T9** closes with the same reasoning.
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

## Designed, not built -- `kos_reboot` (reboot-to-bootloader)

Fully designed, deliberately not implemented yet. Recorded at this fidelity so it can be *built*
as specified rather than redesigned:
- **No reserved number**, behind an `arch_reboot` seam with a **weak `-KOS_ENOSYS` default** -- a
  chip that cannot do it declines honestly instead of pretending. This note has now named two
  numbers that were taken before it was built: 36 went to `KOS_SYS_SHUTDOWN`, then 37 went to
  `KOS_SYS_MEM_SELF_GRANT`. `user/include/kickos/sys/abi.h` is the authority, and the rule it
  already stated is the right one -- **a number is allocated when a syscall is built, not when it
  is designed** -- so stop reserving one here. It takes the next free value at build time
  (38 as of this writing).
- **Authorized by `AUTH_DEVICE` on the existing authority cap -- DECIDED 2026-07-27**, rather than
  by a `CAP_REBOOT` at `KOS_CAP_RESERVED3`. So it needs no new cap type, no sixth rights bit (there
  is none: five bits is the whole budget), and it **leaves index 3 free** -- worth more than bit
  granularity, because spending the last well-known index would force the next one to raise
  `KICKOS_CAP_FIRST_DYNAMIC` and cost a dynamic slot on all four 9-handle boards. Gate it with
  `cap_check_authority(c, AUTH_DEVICE)`, the same call `arch_shutdown` uses.
  Recorded counter-argument: shutdown only stops execution, while reboot-to-bootloader leaves the
  board accepting new firmware over USB, so fusing them lets anything that may publish a console
  also enter flashing mode. Accepted because the feature is compiled out of production images; if
  a distinct `AUTH_REBOOT` is ever wanted, merge `AUTH_PINMUX` + `AUTH_CLOCK` to free the bit.
  See `docs/design-unprivileged-root.md` section 9.
- Compiled out entirely unless `KICKOS_ENABLE_SELFTEST`.
- **RP2040:** bootrom `UB` -> `reset_usb_boot`.
- **RP2350:** bootrom `RB` -> `reboot`, with
  `REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | NO_RETURN_ON_SUCCESS`. The bootrom lookup offset is
  **silicon-revision dependent** -- read `*(uint8_t*)0x13` and branch on it; do NOT hardcode.
- **imxrt1062:** declines with `ENOSYS`, so Teensy keeps its physical button press.

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
      than recorded here:** it is not just the five no-MPU chips (stm32f103, stm32f302, nrf51,
      sam3x8e, esp32) -- **the host sim has it too**, despite building `KICKOS_HAVE_MPU=1`, because
      its globals live in the host image rather than the mprotect'd arena. So the fix could not key
      on `KICKOS_HAVE_MPU` alone and the sim carries its own arm. Gate: selftest `writable_global`,
      confirmed failing on both broken postures beforehand. Also note the suite had already
      *worked around* this bug in `ep_recv_worker`'s comment without it being filed.

**Stage 1 -- the authority capability, root still privileged. COMPLETE** (see
`m4.5.1: gate the eight authority syscalls on a capability, not only on privilege`; +374 B flash
on frdmk64f+blink, no `.bss`).
- [x] **Added `CapType::CAP_AUTHORITY`**, seated at `KOS_CAP_AUTHORITY` (index 2, already
      reserved, and now spelled for what it holds) carrying the **five unused bits of
      `CapEntry.rights`**: `AUTH_MEMORY` (ram_alloc + MMIO grant), `AUTH_PINMUX`, `AUTH_CLOCK`,
      `AUTH_IRQ`, `AUTH_DEVICE` (console publish, shutdown, periph enable). Zero dynamic slots on
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
- [ ] **OWED: re-witness the tip on silicon.** The boards left the bench at `22e1c5a`, and four
      commits landed after it. Two touch the enforcement path and have run **only under emulation**:
      `af696e6` (the `kos_mem_self_grant` syscall) and `3c772b9` (programming a self-granted region
      before the syscall returns). The merged tip is therefore **green locally only**; do not read
      the `22e1c5a` capture as covering it. `3c772b9` first: the bug it fixes was invisible on the
      sim and present on every enforcing backend, which is the exact shape that needs a bench
      witness rather than an emulator one. Needs the XMC A/B re-run at tip plus the `frdmk64f`
      SYSMPU regression. Boundary table in `docs/reference/boards.md`.
- [ ] **Remaining boards, in this order:** f411disco, frdmk64f, pizero2350, esp32c6-wroom, rx72m.
      frdmk64f stays blocked on stage 3 (`arch_periph_enable`).
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

**Stage 3 -- the blocked bring-up bodies.**
- [ ] **Add `arch_periph_enable(base)`**, weak `-KOS_ENOSYS`, gated `AUTH_DEVICE`, covering "ungate
      the clock and drop supervisor-protect for the block at this base". Implement for K64F
      (`SIM_SCGC*` + `AIPS0_PACRN`) and ESP32-C6 (the APM/PMS one-time open recorded under Driver
      era below). Retires `k64uart` and half of `k64dspi`.

**Stage 4 -- the app story.**
- [ ] **Add `kos_cap_narrow(cap, mask)`** (rights &= mask, never widen, ~10 lines) and drop or
      narrow the authority cap in `kickos_default_init_run` (`system/init/default_init_entry.cc`)
      before `kickos_app_main`. **This is the actual app-facing win** -- without it an unprivileged
      `main` can still ask the kernel to do every privileged thing there is.
- [ ] **Declare `stress` privileged-root**, since it spawns privileged children
      (`user/apps/common/stress/main.cc:222,224,286`). **`selftest` no longer needs this** -- see
      `m4.5.1: let the selftest run under an unprivileged root`. Its two privileged spawns
      (`rr_interleave`) were incidental: nothing `rr_worker` touches needs privilege, no assertion is
      about privilege, and the flag dated to the original TAP-harness commit, before a thread's
      region set was composed from its privilege at all. Now unprivileged, verified green on all
      seven emulator gates and on both silicon boards. Worth checking whether `stress`'s three are
      the same kind of leftover before declaring it.

Carried over from the old plan, untouched by this design:
- [ ] **Move app bring-up into the service lists**, so an app is started the way a driver is.

Blockers and limits:
- **Three service bring-up bodies poke MMIO directly from root** --
  `system/driver/mk64f/k64uart/k64uart.cc:191-193` (AIPS PACR),
  `system/driver/mk64f/k64dspi/k64dspi.cc:298-327` (clock gates, pin mux, GPIO, DSPI config),
  `system/driver/xmc4800/xmcssc/xmcssc.cc:281-324` (USIC kernel clock, baud, protocol) --
  and `xmc4800-relax`, the enforcement flagship, links one. Stage 3 generalizes part of this. It
  does **not** cover the XMC, which needs USIC-specific KSCFG/FDR/BRG/CCR programming rather than
  "ungate a clock, drop supervisor-protect" -- but the reason recorded here was wrong and is worth
  correcting, because it made a software problem look like a silicon one. **The blocker is where
  the bring-up runs, not what the hardware permits.** `xmcssc` already hands the U0C1 window
  (`0x4003_0200`) to an unprivileged driver thread via `spawn_unprivileged`, so the window is
  demonstrably grantable; what fails under the flip is that `xmc_spi0_start` runs in *root*, and a
  flipped root holds no MMIO grant for it. Moving that sequence to a holder of the grant removes
  the obstacle. Separately, "FDR/BRG/CCR/INPR are PV-write-only" is an RM Table 18-20 reading
  transcribed into the driver banner, and it is **contradicted, not untested**: `consoledemo`'s
  scrambler is spawned `privileged=false` with the granted U0C0 window and writes exactly FDR, BRG,
  SCTR, TCSR, PCR, CCR and `KSCFG` from inside it, and the recorded XMC silicon PASS is that the
  panic banner survives a *driver-garbled* UART, which requires those unprivileged writes to have
  landed. So the bring-up moves into the granted driver and there is no register-level blocker at
  all. Still unread: **RM Table 18-20 itself** (the banner transcribes a reading of it), and the
  `U`/`PV`/`BE` glossary is reference-manual knowledge, not citable from this tree. **Now enforced
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
  try-index-0-then-fall-back policy (`tests/tap/tap.cc`, libc `_write`). Worth a look at whether the
  other diagnostic apps that print from a worker have the same silent-on-published-boards problem.
- **The panic-path UART reclaim clips bytes in flight.** `kpanic_enter` takes the UART back from the
  userspace driver so the report always reaches the wire, which works, but on `xmc4800-relax` it
  reproducibly garbles roughly the last 8 bytes the driver had queued (the polled TX word pending in
  `TBUF0`). Cosmetic for a terminal report, but it eats the tail of the line preceding the dump.
- **`bluepill-c8` and `f302nucleo` will likely never flip** -- both carve barely 3 KiB of arena for
  the two boot stacks, and both are 9-handle boards.
- **`Thread::privileged` survives**, with narrowed meaning: it selects the memory posture (kernel
  domain + permissive background), it is the confused-deputy bypass at `syscall_mem.cc:37`, and it
  stays the home for "may spawn a privileged child" -- which should NOT be a capability, since
  holding it is equivalent to holding everything forever. Consequence: on a root-unprivileged
  board, **no privileged thread can come into existence after boot**.
- **`idle` stays privileged and holds no capabilities** -- it runs no app code, and RXv3 `WAIT` is a
  privileged instruction while RISC-V U-mode `WFI` is optional per spec.
- **The reserved cap index range is full after this** (0 stdout, 1 clock, 2 authority, 3 promised to
  `CAP_REBOOT`), and the five rights bits are the entire budget for the life of the type.
- **Delegation packing collides with reserved names** -- spawn delegation puts cap *i* at child
  index *i+1*, so a delegated authority cap lands at index 1 (`KOS_CAP_CLOCK`). Blocks the narrowed
  hand-off to a driver manager until the deferred explicit-destination-index work lands.
- **Cap-gen is a `uint16_t`** with no object generation behind a poolless cap, so 65536
  close/re-seat cycles wrap it. Unreachable in-tree; same unbounded-counter class as the
  domain-refcount item above.

## Needs hardware (bench time, not code)

- [ ] **v6-M MPU programming has zero coverage anywhere.** QEMU models no Cortex-M0+ and no
      Cortex-M23 core, so neither the emulator gates nor the silicon fleet ever exercises the
      armv6m MPU backend. Closing it needs an **RP2040 on the bench** -- there is no software
      substitute for it.
- [x] **M7 speculation class** -- already covered by validated Teensy silicon (the imxrt
      MPU-enforce hang record, `docs/design-teensy-mpu-hang.md`); recorded here so the gap list
      stays honest about what *is* already covered.

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

## M1 -- clocks (fleet audit 2026-07-09; detail in `M1_state.md`)

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
        fable-reviewed; **silicon-validation PENDING** a bench swap to the 32F411E-DISCO. It
        first-proves granted-SPI-works AND ungranted-peripheral-faults per thread on PMSA
        silicon -- the fleet's one honest peripheral-isolation gap. `docs/design-spi-driver-stm32f411.md`.
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
  load the kernel spends too long masked. Ranked plan (see `M1_state.md` section 3.1):
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

Fleet re-validation follow-ups (from the 2026-07-22 M3-branch gate; see `M3_raw_meas.md`):
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
