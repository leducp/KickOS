<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS -- current state

One screen, and the only file that changes every milestone: read it to re-ground, then go
straight to the record you need. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

**AND NOTHING BELOW MAY BE A FIGURE A COMMAND ANSWERS.** This file carried thirty wrong
claims into 2026-08-25 -- a stale master hash, six stale test counts, a preset count off by
half, and two "called by NOTHING" findings that were false -- because it quoted what
`git log`, `ctest` and a grep already answer. Every one of them cost the reading session
time. If you can derive it, derive it. What is written here is what a green run does NOT
say.

## Where we are

**M6.2 is the live track. T1 to T5 and T6a are landed; T5b.1 is next and it needs a DECISION before
code.** Two steps the contract did not have were found by trying to run it, and both are written into
section 5 rather than worked around. The blocking one: `memcpy`, `memset` and `strlen` are called from
the kernel AND from the app, so an app/kernel link split cannot give one symbol two addresses, and
every option costs something the freezes forbid. Read T5b before touching T6. The step plan is `docs/design-m6-mmu.md` section 5:
M6.1 was S1..S9 and is merged, M6.2 is T1..T9 plus T8b, and T1 and T2 are landed and pushed.
`git log master..M6.2` and `ctest` are the authorities for anything countable.

**Four spike branches exist and MUST NOT be merged**: `spike/m6-virt`, `spike/m63-rv64-sv39`,
`spike/m64-x86_64`, `spike/m7-smp-triarch`. De-risking runs, not ports. Everything they found
that moves the contract is already folded into `arch/include/kickos/arch/arch.h`,
`docs/design-m6-mmu.md` (F8, T9, section 3) and `docs/design-m7-smp.md`. Read those, never the
branches, and do not re-litigate the T2 freeze from spike code: it has already absorbed four
RV64 corrections and three from the tri-arch run.

**The one thing in the aspace family still deliberately unfrozen** is where the active-core set
a TLB rendezvous needs comes from: an `unmap` parameter, or a readable field on the opaque
space. T9 owns it. Nothing in M6 needs the answer and the field is free later where the
parameter would not be, which is why it is named rather than decided.

**T6 and the per-core pointer are ONE edit, and neither is M7 work.** `TPIDR_EL1` is already
spent as the EL0 entry scratch by `ENTER_FROM_EL0`, and it is needed there only because
`SP_EL1` cannot be trusted on entry, and that is only because `thread_create` seats
`ctx.kernel_sp` after `arch_context_init` returns. Seating it inside `arch_context_init` frees
the register and is the same change T6 owes to get a thread's privileged return state off its
own stack. `kickos_armv8a_kernel_sp` is a single global and becomes per-core at the same time.

## What the fleet does NOT witness

The whole point of this file. A green fleet pass says none of the following.

- **arm64 is QEMU `virt` only.** No A-profile silicon on this bench, so every armv8a claim is
  emulator-grade. Its selftest declares one PARTIAL, `periph_reg_write_unheld`, which is a real
  coverage gap -- and a PARTIAL reports `ok`, so no count reconciliation can ever see it.
- **There is no riscv64 and no x86_64 board.** RV64 Sv39 and x86_64 are spikes. A doc saying M6
  "ships THREE backends" is a plan, not a tree.
- **RX and LX6 have no emulator and no CI gate.** `rx72m` and `esp32-wroom` silicon is the only
  check either arch ever gets; a green fleet pass says nothing whatever about them.
- **An RX `pspguard` is OWED.** `pspguard` is armv7m/armv6m only, and `.Lsvc_nokstack` is
  structurally unreachable on RX, so nothing there can reach the REFUSE side of the
  trusted-stack guard. Green on RX means "accepts what it must", never "refuses what it must".
- **No poisoned-user-stack witness for the death-path move.** It must use the SLAY path, not the
  fault path: a fault stacks its own frame on the dying thread's stack by construction, so no
  fault can carry an intact-stack claim. The band must be poisoned ABOVE the parked sp too. An
  unlanded attempt sits at `/var/tmp/kos-agent-faultsurvive.patch`.
- **`f302nucleo`, `f302nucleo-st`, `due`, `due-st` rest a blocking syscall's continuation on the
  USER stack.** The four presets where `KICKOS_KERNEL_STACKS` resolves 0, and it is deliberate.
  `roadmap.md`'s "either a lower thread ceiling or continuation-style blocking" is a false
  dichotomy: a third option shipped, both entry designs under one knob.
- **Zero slack is the CONVENTION in `trap_redzone_roots.txt`, not a warning.** An enforced depth
  IS the fleet maximum for its class, so a class at its setter preset always reads `n <= n`. Do
  not read those as near-misses. Only a BLOCK or FLOOR margin is one.
- **`qemu-riscv` under enforcement is the only posture reporting zero partials** where every ARM
  enforcing posture reports one. An encoded per-arch difference, not a defect.
- **`errnoprobe`'s arm C does NOT witness the IPC fastpath on arm64.** There is no
  `ipc_fastpath.cmake` for armv8a, so the arm exercises the generic path there while its name says
  otherwise. Its assertions still hold; only its name over-promises on that board.
- **The invalidate a FRESH map owes is unwitnessed.** Architectures cache negative translations, so
  a leaf installed where the slot was empty needs one; QEMU does not model that, and removing the
  invalidate leaves every arm green. It is in the code because the architecture requires it, and no
  run on this bench can tell whether it is there.

## Debts and declines a command cannot re-derive

- **`errno` is not thread-local, and the reason is not in our code.** `_REENT_THREAD_LOCAL` is
  off on all three pinned toolchains; 239 `libc.a` members reference `_impure_ptr` and NONE
  calls `__errno()`, so overriding `__errno` reaches nothing. `sizeof(struct _reent)` is
  512/284/288 across the three. RX has no thread-pointer register at all and falls back to a
  single-threaded `emutls.o` with no diagnostic.
- **A kernel-mediated `brk` would NOT make multithreaded `malloc` safe.** It closes one race,
  and `__malloc_lock` is a no-op, so shipping a serialized `_sbrk` reads as a safety it does not
  provide. Declined on that ground, not on cost.
- **The reclaim-window invariant is ONE-WAY.** The window must COVER what the reclaim WRITES; it
  need not match the service-list grant. `dev_window_free` tests OVERLAP. So this is not a
  coupling wanting enforcement across `arch/` and `system/init/`.
- **`_appdata_fits` had never fired on any board.** GNU ld completes layout before evaluating
  assertions, so an app-data overflow died on `cannot move location counter backwards` and the
  actionable message was unreachable in exactly its own case. `. = MAX(., ...)` fixes it. Do not
  re-derive this by reading the scripts: the condition was always correct, the assert was simply
  never reached.
- **A capture's CRLF names the transport ONLY on a polled driver.** `uart_service.h`'s
  `cook_crlf` voids the tell on `_uartirq` boards. It has already been read once as "the console
  was never published".
- **`EXPECT_SKIPS`/`EXPECT_PARTIALS` catch a LOSS of arena slack automatically and a GAIN
  never.** So any change that moves `microbit`'s `.bss` needs its skip set diffed by eye.
- **Two review heuristics worth keeping.** *A figure charged twice*: one figure covering two
  postures wants a posture-dependent macro, not a bigger number. *A preset nobody measured*:
  closed structurally now, the ctest ladder deriving the name and asking
  `trap_redzone_roots.txt`.
- **The fleet shipped `-O0` until M4.5.2, at roughly 2x footprint**, so every silicon witness
  taken before it is invalid. On the K64F, `-Os` then dropped a PIT clock-gate-race write that
  `-O0` had masked. `build: optimise the fleet (MinSizeRel)` is the commit to revert when
  bisecting a footprint or timing regression.

## Gates: what is not gating, and the one that is deliberate

- **`check_c_headers.sh` compiles with no `-D` at all**, so a C-facing header's other `#if
  KICKOS_<knob>` arm is compiled by nothing and the gate still reports PASS. NOT fixed on
  purpose: widening wants measuring first.
- **The `docs/`-out-of-the-oracle fix OUTLIVES the `.html` that motivated it.** `doc_names`
  reads tracked markdown only, validates a path and an identifier and never a line number, so
  the next non-markdown file committed under `docs/` reopens the hole. Widening was measured at
  about 3% precision and REFUSED.
- **Four gates were once not gating**, and the mechanism matters more than the fix: an
  `IFS=$'\t'` dash bashism made `check_seam_defaults.sh` leg 1 vacuous and `dash -n` does not
  catch it; `panic.ere` never matched the RX or LX6 reporters; `extern "C"` overrides an
  anonymous namespace.
- **The `KCAP_`/`CAP_` left-word-boundary defect**: 26 names were valid only as substrings.
- **A line-number citation is what the doc gate cannot check**, and this file used to carry one
  (`porting.md:1464`) that had drifted onto unrelated text. Cite a path and an identifier.

## Board caveats a matrix does not carry

- **`microbit` has no arena slack, structurally.** `.userheap` is `default 0 if CHIP_NRF51`, so
  `__kickos_ram_start` lands exactly at the 32-byte-aligned `_ebss`.
- **The `_ebss`-to-arena gap is NOT slack.** The `.userheap` carve slides up with `_ebss`. The
  shape that really is pinned is an enforcement window.
- **`usbcdcwit` is built by no default configuration of any board** (gated on
  `KICKOS_SERVICE_LIST MATCHES "_usbcdc$"`).
- **`picopi` owes a slay capture** -- the fleet's only armv6m enforcement unit.
- **The bench chain refuses BY NAME on an absent rig value**, and `bench-fleet.sh` ending
  `INCOMPLETE` with a non-zero exit is the EXPECTED result for a fleet pass (frdmk64f is out by
  ruling).

## Open, and verified still open

- **`kos_print` does not survive a published console.** `emit.h` exists and there are three
  publish-aware writers, so silence from a `kos_print`-only app is not evidence of a dead driver.
- **USB CDC: bulk OUT is never exercised and `Shared::configured` never clears on unplug.**
  Detail at `TODO.md`.
- **No emulated gate for a buffered-ring panic flush**; the sim's ring is provably empty at
  panic time. Detail at `TODO.md`.
- **Four apps grant a DEV window a live driver holds**: `xmcspi`, `xmccshold`, `pvprobe`,
  `inprstorm`. **`KICKOS_APP_AUTHORITY` surfaces only at runtime.**
- **`kstack_high_water` has no caller.** Its sibling `kstack_canary_intact` DOES, at
  `kernel/sched/sched.cc`. This file previously claimed both were uncalled and concluded the
  gate described a mechanism that cannot fire; that conclusion was false.
- **`M8` is unnumbered in one place and contradicted in another.** `roadmap.md` has M8 as
  IPC/IRQ optimisation with the list running to M9 and M10, and names an ABI-freeze milestone
  without assigning it a number. This wants a ruling, not a carry-forward.

## Machine-local traps live in CONTEXT.local.md, not here

Two that were in this file and belong there instead, and are now recorded there:
`/var/tmp/kickos-imagesweep` still holds a stale run whose summary reads as current (check its
`finished` timestamp), and `genconfig.py` warns "set more than once" whenever a `-D` knob is
re-passed unchanged, which is noise in a channel documented to mean a declaration is wrong.

## History that must not be garbage-collected

**M4.9.1's SILICON WITNESSES BANNER COMMITS ITS OWN SQUASH DESTROYED**, and
`backup/m491-presquash-20260816` is what keeps them resolvable -- local and unpushed, like every
branch named in this section. `usbcdcwit` on `pizero2350` stamps `e0ab9cf9` and the `teensy41`
selftest `a4a3d8dc`; the `picopi` run carries no banner at all, its console being the device, so its
tree is named only here. **The squash changed NO CONTENT** -- the four-commit tip is byte-identical
to that backup outside this file -- so those captures do describe the code that shipped, and the
backup is what lets a reader CHECK that rather than take it.

**`c296feb` is reachable only from the local unpushed branch `m4.2-presquash`.** It holds
`git show c296feb:docs/design-m4-rx-irq-demux.md`, which `docs/design-m4.6-irq-driver.md` section 6
cites rather than reproduces for the RX routing-class taxonomy, the group-register table and the
level-versus-edge semantics. One `git branch -D m4.2-presquash` destroys an M4.6.1 prerequisite.

Captures and records across `TODO.md` and `docs/` stamp pre-squash tips (`c5d9b0d`, `270b6fa`,
`124b68c`, `989af16`, `16e4af0`, `788b1d8`) that folded into `dde73ca` and reach no branch. The
stamps stay as written.

**ELEVEN HASHES CITED ACROSS THIS FILE AND `TODO.md` SURVIVE ON THIS BOX ONLY.** Re-derived
2026-08-16, and the earlier wording -- that squashes "destroyed" them -- was wrong in the direction
that matters: every one still resolves HERE, each held by a local backup branch, and NONE of those
branches is pushed (`git ls-remote --heads origin` carries 43 heads and only the two
`backup/m4.5.2-*` among them). So a fresh clone loses all eleven, and so does one `git branch -D`.
The squashed commits carry only their final tree; every hash below is an INTERMEDIATE tree that no
current commit reproduces.

| hash | what it stamps | reachable only from |
| --- | --- | --- |
| `b77a3ef4`, `a2695e08` | M4.8.2's six-board pass, and its review | `backup-m4.8.2-presquash` |
| `f8cc32bd` | the `m483` fleet pass | `backup-preland-final` |
| `58e7174e` | the `m484` capture banners | `backup/presquash-m483-m484` |
| `e21167b6`, `1c250bad`, `a1220233`, `367497c2`, `7bdf1067`, `aa38390a` | the M4.8.1 driver-class measurements | `backup/m481-presquash-20260811` |
| `182e0dd2` | scratch console-reclaim instrumentation | `wip/console-reclaim-window-precondition` |

**Two kinds of citation, and only one of them should ever be rewritten.** A hash naming a TREE you
might check out is a reference, and it gets converted to the milestone it belongs to. A hash quoted
as a BANNER is a fact about a string an image printed, and rewriting it would falsify the capture --
those stay verbatim, and this table is what makes them resolvable. `dde73ca` above is the same
situation with a happier ending: it is on `master`.

What makes the `58e7174e` witnesses still good is not reachability but the diff: the two commits
after it touched docs plus one redundant cast, with the armv7m object byte-identical.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `roadmap.md` -- the milestone plan, and the sub-milestone ledger: the only place a number is
  ASSIGNED. This file carries the locked ORDER and cites those numbers.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
