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

**M6.2 is CLOSED, every T-step landed, and `qemu-arm64` is the fleet's first ISOLATING translating
board.** M6.3 is CLOSED at R5 and the aspace-seam VERDICT IS TAKEN, and it is NOT an empty diff: one
member added, `arch_aspace_frame_at`, thirty five records identical. The verdict was the one piece of
M6 evidence that could not be gathered early, F8's empty-diff claim being about a FAMILY, and what
settled it is the property R2 was picked for. Re-take it with
`sh tests/static/check_aspace_sigdiff.sh`, whose exit 2 IS the result; do not move the baseline.

**A CLOSING ASSIGNMENT SWEEP over `docs/design-m6-mmu.md` found 8 obligations of 41 not discharged,
and five of them were closeable by running them.** S2's model confirmation, F10's cross-task refusal
and teardown release, T2's acquire-depth floor and T6's flags-match rule are witnesses now; two are
recorded as debts (T7's latency figure and the driver gate on this board) because the rig, not the
step, blocks them; four records that contradicted the tree were corrected in place. **The general
finding is the one already at section 5's opening note:** every one of the five was implemented and
believed, and what was missing was the arm -- so an obligation that reads as satisfied because the
CODE exists is the class to sweep for, and reading the document again is not what finds it.

**AN EXTERNAL AUDIT OF THE BRANCH (2026-08-26) RAISED TEN FINDINGS AND ALL TEN ARE DISCHARGED**,
recorded in `TODO.md` under `## External audit of the M6.2 branch, 2026-08-26`, whose checkbox set is
now empty. Two of them are worth carrying forward as reasoning rather than as closed tickets:

- **The HIGH was real and the counter could never have caught it.** A donor's teardown freed frames a
  borrower still mapped. T4's rule covered borrower-dies-first ONLY, and the refused-free counter is
  STRUCTURALLY BLIND to the case: every frame is freed exactly once, so nothing is refused and the
  counter reads clean while a borrower reads another task's memory. A counter that counts refusals
  cannot witness a lifetime bug that never double-frees. The fix takes `domain_ref` on the donor
  where the handoff succeeds and surrenders it in one `drop_space()`.
- **THE AUDIT'S PRESCRIBED FIX FOR THE DATA TEMPLATE WAS WRONG AS STATED**, and running it is what
  showed that. A frozen post-constructor snapshot breaks `irq_driver`: root writes `g_mmio` AFTER its
  constructors, so a child seeded from the frozen image gets a null pointer and faults. Root stays
  the LIVE template while root lives; the snapshot is taken at root's release. An external reviewer's
  direction is evidence about the defect, not about the remedy.

**THE FIRST WORD-WISE FRAME SCAN WAS A REGRESSION ON THE PATH THAT NEVER SCANS**, and the shape is
the lesson: `release` parks the hint on the frame it freed, so the common case tests one bit, and
routing it through the two-pass word loop cost 2.1x to 2.4x. A hint test ahead of the loop restores
parity while the scan-heavy case keeps its win. Both paths exit through one `take_one`, so a single
place still turns an index into an address. Allocation ORDER is unchanged and that was measured, not
argued: old and new return the same offset from base in every shape, size and optimisation level.

**THE CONTRACT NOW MATCHES THE TREE, AND FOR MOST OF THIS MILESTONE IT DID NOT.** An audit
(`docs: the contract stops asserting what the tree stopped doing`) corrected ten present-tense claims
the M6 contract made that were FALSE against the code, each with a freeze, a step obligation or a
witness resting on it: F1, F2, F5, F6, F7, F10 and sections 2, 3.1, 3.3, 3.4b and S9. T6 took three
attempts that stopped short, and T5b is an entire STEP the contract did not have, found by trying to
run T6 rather than by reading it. So the doc reads as a contract NOW; it did not for most of M6.2,
and a reader assuming it always could will misattribute the next finding. **Read section 5's opening
note before adding a present-tense sentence there** -- the audit did not retire the habit, four more
of the class were corrected in the steps after it, the last at T8b.

**Four spike branches exist and MUST NOT be merged.** `spike/m6-virt` is SPENT, M6 having shipped.
The other three carry MEASURED answers the next milestones need, and are why several M6 freezes were
already right: `spike/m63-rv64-sv39`, `spike/m64-x86_64`, `spike/m7-smp-triarch`. De-risking runs,
not ports. Everything they found that moves the contract is already folded into
`arch/include/kickos/arch/arch.h`, `docs/design-m6-mmu.md` (F8, T2's four RV64 corrections, T9,
section 3) and `docs/design-m7-smp.md`. Read those, never the branches.

**The active-core set a TLB rendezvous needs is DECIDED at T9 and NOT BUILT**: a readable field on
the opaque space, never an `unmap` parameter, because `arch_aspace` being opaque makes the field free
to add later where the parameter's signature fan-out would not be. Nothing in M6 reads one, so
neither is in the tree. `docs/design-m6-mmu.md` T9 carries the reasoning; M7 implements it rather
than re-litigating it.

**`TPIDR_EL1` is written by NOTHING on armv8a, and that is deliberate.** T6a took the EL0 entry
scratch off it by seating the kernel block BEFORE `arch_context_init`; T9 then made the per-core cell
the first field of `struct armv8a_percpu`, whose address is a link-time constant at one core. The
register is held for the multi-core arm that reads the block out of it, so a port that spends it
takes M7's only free one.

## What the fleet does NOT witness

The whole point of this file. A green fleet pass says none of the following.

- **arm64 is QEMU `virt` only.** No A-profile silicon on this bench, so every armv8a claim is
  emulator-grade. Its selftest declares one PARTIAL, `periph_reg_write_unheld`, which is a real
  coverage gap -- and a PARTIAL reports `ok`, so no count reconciliation can ever see it. Minting an
  MMIO window is what retires it.
- **The model's OWN report now agrees with the manuals, and that is all it says.** `aspace_model`
  reads `ID_AA64MMFR0_EL1` and gets the A53's reset value: 4 KiB and 64 KiB supported, **16 KiB
  not**, 16 identifier bits, a 40-bit physical range. So S2's clause is discharged and there is no
  divergence to record. What it still does not say: nothing tags a translation, so `TCR_EL1.AS`
  stays at an 8-bit identifier and the 16 the machine offers is a figure nobody spends; and a
  granule arm that re-reads `arch_aspace_granule`'s own constant is still beside it, which is why
  that arm was never the confirmation.
- **F10's REAL CONSUMER never runs on the translating board.** F10 makes the driver framework the
  gate for the allocation ABI, in terms saying no selftest arm substitutes for it -- but `qemu-arm64`
  declares no service list, so `drv::bring_up` runs only on region boards and against host fakes.
  What discharged the readback there is `task_handoff_readback`, which is that substitution. It now
  covers the flags-match rule too, a block of its own going through a self-grant and a task create
  at `KOS_MEM_NOCACHE` with both sides reporting the type recorded -- but a grant-carrying SPAWN has
  no memory-type field in its ABI at all, so that consumer maps Normal whatever the donor holds and
  no caller can obey the rule through it. Detail at `TODO.md`.
- **A non-cacheable mapping is witnessed as a RECORD and never as a cache behaviour.** QEMU models
  no data cache, so what the flags-match leg asserts is that both mappings carry the same memory
  type, not that either is actually uncached. The type reaching the descriptor at all is unwitnessed
  on this bench, and the first bus master is where that stops being true.
- **There is no x86_64 board in this tree.** x86_64 is a sibling branch, not a merge. A doc saying M6
  "ships THREE backends" is a plan until both halves land in one tree; measured separately, the aspace
  differ reads 36 against 35 here and 35 against 35 there, and the union is what a merge produces.
- **`qemu-riscv64` HAS NO SILICON, so every rv64 claim is emulator-grade.** `docs/reference/boards.md`
  names no RV64 part and no `tools/flash*.sh` names this arch, so the backend has only ever run under
  emulation and there is no path to a run. **And F8's named silicon witness does not boot this image
  at all**: `-cpu thead-c906` on the shipped `hello` produces NO output, one `zfa` privilege-spec
  warning and a timeout kill, with the default core printing the banner as the control. So the c906
  figures in the M6.3 record are a standalone probe's and never the suite's.
- **THE MAP EDITOR'S MAINTENANCE IS WITNESSED IN ONE PLACE OF SIX**, each measured as an isolated
  single-site mutation. WITNESSED: `unmap`'s per-page invalidate, whose deletion turns
  `qemu_riscv64_aspace_fault` red on "the unmapped page did not fault". HELD BY A COUNTER AND NOT BY
  AN ACCESS: the fresh-map per-leaf invalidate, caught only by `map_tlbi_elided`'s floor. NOT
  WITNESSED AT ALL: break-before-make, destroy's sweep ahead of `free_subtree`, and
  `arch_aspace_activate`'s whole-hart fence. And the fresh NON-LEAF fence's ISSUED path never executes
  in this suite, proved by multiplying that bump by 100 and seeing every figure stand still, so only
  its ELIDED leg runs (2 of the seed's 47).
- **A ROOT WRITE APPEARS TO FLUSH THE WHOLE TLB ON THIS EMULATOR, WHICH IS WHY ACTIVATE'S FENCE IS
  DEAD-EFFECT.** Deleting it leaves all 132 arms green on a suite that switches between live per-space
  low halves, and a stale low-half translation WOULD be consulted there, so the green run is only
  consistent with the emulator dropping translations on the `satp` write itself. That is an inference
  about QEMU and not a measurement of it: what the bench cannot separate is that explanation from "at
  one core a root that was never installed has no cached translation to drop". Either way only silicon
  witnesses the fence's necessity. The fresh non-leaf fence rests on the specification for a second
  reason, QEMU modelling no caching of invalid PTEs where RISC-V Privileged 12.2.1 permits it.
- **THE IDENTIFIER'S WIDTH-ZERO CASE IS A CODE PROPERTY HERE AND NEVER A MACHINE ONE.** Every core
  model on this bench reports a contiguous 16-bit field, the suite's own model line reading `16 ASID
  bits` on both postures, and NO emulator property narrows it: `asid-bits=off`, `asid_bits=off` and
  `asidlen=off` are each refused as `Property 'rv64-riscv-cpu.<name>' not found` while `sv48=off` on
  the same command line is accepted, which is the positive control that makes the absence worth
  stating. What stands in for a zero-width hart is a mutation of the port's own probe. A NON-CONTIGUOUS
  field has no machine here either, so the width-against-popcount distinction is held by graded
  controls alone.
- **`aspace_frame_token`'s CONVERSION to the seam member is unwitnessed while the member's ANSWER is
  witnessed.** Reverting that caller to the two-acquire-pointer arithmetic is green, because every
  frame it is asked about sits inside the kernel window where `arch_aspace_acquire` is an addition and
  the subtraction is accidentally right. The defect is latent, not absent, and no arm in this tree puts
  a frame outside that span. What IS witnessed is the member: seven arms go red when it answers one
  constant frame and eight when it answers zero.
- **THE KERNEL'S WRITE-EXECUTE SPLIT IS ENFORCED BY HARDWARE AND NO SHIPPED ARM SAYS SO.** What holds
  the runtime half is a pair of privileged probes with a negative control on the parent commit, not a
  ctest entry, because a privileged fault is a panic here rather than a contained kill. The static arm
  `riscv_kernel_wx` reads the linked image's own leaves and is the only shipped witness.
- **THE gp GATE'S HAZARD HAS NO RUNTIME ARM.** Nothing in the tree sets a hostile global pointer, so
  `riscv_kernel_gp` and `riscv_kernel_apphalf` are held by their own positive controls (a cross-half
  word reverted to a direct reference, an allowlist entry removed) and never by a thread exploiting
  one. And `virt_rv64.ld`'s comment claims its displacement assert "turns a missed cross-half
  reference into a link error", which R2.2's audit falsified for both the gp-relaxed and the
  medlow-absolute encodings; the comment is still there.
- **Sv57 IS ONE CHOICE ENTRY AWAY AND IS NOT OFFERED**, the emulated core accepting it, because a mode
  the board does not run is a claim with no arm behind it. Two postures ship, Sv39 and Sv48.
- **Neither RX nor LX6 has an emulator, and only RX has no CI gate either.** `ci.yml` runs a
  dedicated `xtensa` job that builds `esp32-wroom` and `-st` and runs `ctest -L host`; `rx72m`
  appears in no job at all, so `rx72m` silicon is the only check that arch ever gets.
- **An RX `pspguard` is OWED.** `pspguard` is armv7m/armv6m only, and `.Lsvc_nokstack` is
  structurally unreachable on RX, so nothing there can reach the REFUSE side of the
  trusted-stack guard. Green on RX means "accepts what it must", never "refuses what it must".
- **No poisoned-user-stack witness for the death-path move.** It must use the SLAY path, not the
  fault path: a fault stacks its own frame on the dying thread's stack by construction, so no
  fault can carry an intact-stack claim. The band must be poisoned ABOVE the parked sp too. An
  unlanded attempt sits at `/var/tmp/kos-agent-faultsurvive.patch`. T6a's `parked_frame_hostile` is
  NOT this arm -- it corrupts a SIBLING's parked frame, and says nothing about the death path.
- **SIX armv7m presets rest a blocking syscall's continuation on the USER stack**, not the four
  this file used to name: `f302nucleo`, `f302nucleo-st`, `due`, `due-st` and ALSO `bluepill-c8` and
  `bluepill-c8-st`, `CHIP_STM32F103` selecting no MPU exactly as the other two chips do. Those are
  the presets where `KICKOS_KERNEL_STACKS` resolves 0, and it is deliberate.
  `roadmap.md`'s "either a lower thread ceiling or continuation-style blocking" is a false
  dichotomy: a third option shipped, both entry designs under one knob.
- **Zero slack is the CONVENTION in `trap_redzone_roots.txt`, not a warning.** An enforced depth
  IS the fleet maximum for its class, so a class at its setter preset always reads `n <= n`. Do
  not read those as near-misses. Only a BLOCK or FLOOR margin is one.
- **And armv8a has NO record in that file at all**, so the depth the fault-exit stub descends on a
  4 KiB kernel block is UNGATED. The stub starts at the block top with the whole block under it and
  the fault frame already popped, the most favourable position any backend gives it -- but no figure
  is enforced. Declaring the arch is a step of its own.
- **`qemu-riscv` under enforcement is the only posture reporting zero partials** where every ARM
  enforcing posture reports one. An encoded per-arch difference, not a defect.
- **NO BOARD IN THE FLEET WITNESSES THE IPC FASTPATH'S OWNER ARGUMENTS.** `armv8a` has no
  `ipc_fastpath.cmake`, and no arch that HAS one (`armv7m`, `armv6m`, `rv32imac`, `rxv3`) has an
  `aspace.cmake` -- so T7's two owner arguments are a compile-time null everywhere, and `errnoprobe`'s
  arm C exercises the generic path on arm64 while its name says otherwise. `call_reg_fastpath`
  witnesses the site compiling and behaving and nothing more. The owner needs a fastpath on a
  translating arch, which is M6.3's backend.
- **No arm asserts console CONTENT.** T7 funnelled the console site, but only root writes to the
  console here, so a misdirected read has nothing to distinguish it. Truncating the funnelled helper
  visibly splices the TAP stream, which is the MECHANISM witness; the content witness wants the
  published-console route, where root reads back what the kernel actually streamed.
- **The invalidate a FRESH map owes is unwitnessed.** Architectures cache negative translations, so
  a leaf installed where the slot was empty needs one; QEMU does not model that, and removing the
  invalidate leaves every arm green. It is in the code because the architecture requires it, and no
  run on this bench can tell whether it is there. Re-measured at T8b, and its SIBLING is not the
  same case: removing `unmap`'s per-page invalidate DOES fail `aspacefault`, so the two halves of
  the map editor's maintenance have opposite witness status.
- **Neither of destroy's two orderings is witnessed either**, measured the same way. Removing
  `arch_aspace_destroy`'s whole-TLB sweep from ahead of `free_subtree` leaves every arm green, and so
  does moving `aspace_release`'s restore of the BOOT space to after the destroy that frees the dying
  root. Both are held by source order and review. The second one's window is not interrupt-masked,
  so a preemption inside it would return to EL0 on a freed root, and nothing on this bench can
  produce that. Identifier reuse is VACUOUS rather than witnessed, nothing allocating one.
- **The forced-failure sweep reaches FIVE injection points, not six**, and the EQUALITY of the two
  sweeps is what says so. `domain_for`'s own inner unwind -- a handoff that MAPPED and then failed to
  record -- is unreachable by frame injection here: the donor block sits under a level-3 table the
  image already built, so every refusal lands in `claim_slot` ahead of `aspace_handoff`. Reaching it
  wants a second injector, into `VirtualRanges::reserve`, and it is not built.
- **One of F10's three ABI rules is still IMPLEMENTED AND UNTESTED, and it is the third one.** The
  CROSS-TASK self-grant refusal and the teardown release are arms now (`self_grant_cross_task`,
  `reservation_teardown`). What is left is T6.2's: the handoff destination's COLLISION refusal, a
  target space that cannot take the range at the donor's address. It is untestable rather than
  untested here -- this backend's reservation namespace is globally unique, an address being a
  frame-pool output address, so no correct caller can collide.
- **The data-cache flush and invalidate seam has no caller and no witness.** T9 landed
  `arch_dcache_flush`/`arch_dcache_invalidate` with an armv8a backend; QEMU models no data cache, so
  an arm exercising it would pass with the loop bounds wrong. The member is compiled and never
  extracted, so no image carries it and no gate can see it. Section 7 owns the consumers.
- **`appdata_no_kernel` does not run on the one board that splits its image.** It is registered under
  `KICKOS_HAVE_MPU` and keys on `__kickos_appdata_start`/`_end`, which `virt_arm64.ld` does not
  define -- so the guard against a kernel archive landing in the app's low window is absent exactly
  where that window is now the only memory EL0 can reach. Detail at `TODO.md`.

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
- **The acquire pair's floor is `ARCH_ASPACE_ACQUIRE_MIN` = SIX, and NO BACKEND EXERCISES IT.** The
  figure was measured off the page-split scenario (four pages across two spaces, plus one end each
  inside `ep_copy`) and is asserted by the armv8a backend against a capacity of its own, which is
  unbounded -- acquire there is an addition. So the constant and its assert are a shape a WINDOWED
  port fills in, and nothing on this bench can fail them. The measurement also found `SPAN` walking
  600 pages while holding every one, which is now released page by page: a caller's defect, not the
  seam's, and the kind only counting finds.
- **The app-data-fits assert had never fired on any board.** GNU ld completes layout before
  evaluating assertions, so an app-data overflow died on `cannot move location counter backwards`
  and the actionable message was unreachable in exactly its own case. `. = MAX(., ...)` fixes it,
  and it is in all eleven enforcing scripts -- not in `virt_arm64.ld`, which carves no MPU window.
  Do not re-derive this by reading them: the condition was always correct, the assert was simply
  never reached. It no longer has a name of its own; do not go looking for `_appdata_fits`.
- **A capture's CRLF names the transport ONLY on a polled driver.** `uart_service.h`'s
  `cook_crlf` voids the tell on `_uartirq` boards. It has already been read once as "the console
  was never published".
- **`EXPECT_SKIPS`/`EXPECT_PARTIALS` catch a LOSS of arena slack automatically and a GAIN
  never.** So any change that moves `microbit`'s `.bss` needs its skip set diffed by eye.
- **Two review heuristics worth keeping.** *A figure charged twice*: one figure covering two
  postures wants a posture-dependent macro, not a bigger number. *A preset nobody measured*:
  closed structurally now, the ctest ladder deriving the name and asking
  `trap_redzone_roots.txt`.
- **An added ARGUMENT is caller stack, and the fix is a posture word rather than a bigger figure.**
  F10's handoff grew `task_for` by 8 bytes on the deepest chain `syscall_dispatch` has, which took
  the armv7m SVC depth past its red zone on the four `KICKOS_KERNEL_STACKS=0` presets -- region
  boards paying for a translating backend's argument. Two caller booleans became one posture word and
  the measurement came back exactly. A control tree with the argument dropped is what ATTRIBUTED it
  before the fix.
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
- **Turning CONTAINMENT on flips what a gate may assert.** `kernelhalf` and `stackguard` were
  dying-image gates reading the panic dump; once armv8a joined `KICKOS_FAULT_ISOLATION` the same
  encodings had to be read off the thread-kill record instead, and `check_tap_stream.sh`'s blanket
  refusal of ANY thread-fault record became a by-name permission set. `aspacefault` STAYS a
  dying-image gate and that is the scenario, its read being the kernel's own at the current-EL
  vector where the kill rule declines it.

## Board caveats a matrix does not carry

- **`microbit` has no arena slack, structurally.** `.userheap` is `default 0 if CHIP_NRF51`, so
  `__kickos_ram_start` lands exactly at the 32-byte-aligned `_ebss`.
- **The `_ebss`-to-arena gap is NOT slack.** The `.userheap` carve slides up with `_ebss`. The
  shape that really is pinned is an enforcement window.
- **`usbcdcwit` is built by no default configuration of any board** (gated on
  `KICKOS_SERVICE_LIST MATCHES "_usbcdc$"`).
- **`picopi`'s slay capture EXISTS and is not valid for this tree.** The five slay arms passed on
  it in a 2026-08-16 session log, which is why "owed" was the wrong word -- but that log attests the
  M4.9.1 tip, and M6.2 has since changed `arch/arm/armv6m/arch_armv6m.cc` at T6a. A witness is valid
  for a TREE. It is still the fleet's only armv6m enforcement unit, so nothing else can stand in.
- **The bench chain refuses BY NAME on an absent rig value**, and `bench-fleet.sh` ending
  `INCOMPLETE` with a non-zero exit is the EXPECTED result for a fleet pass, frdmk64f being out by
  ruling. **That ruling exists NOWHERE but this line** -- `bench-fleet.sh` still lists and probes the
  board, and an absent one records `ABSENT` without failing, leaving its service lists uncovered.

## Open, and verified still open

- **`kos_print` does not survive a published console.** `emit.h` exists and there are three
  publish-aware writers, so silence from a `kos_print`-only app is not evidence of a dead driver.
- **USB CDC: bulk OUT is never exercised and `Shared::configured` never clears on unplug.**
  Detail at `TODO.md`.
- **No emulated gate for a buffered-ring panic flush**; the sim's ring is provably empty at
  panic time. Detail at `TODO.md`.
- **Four app SOURCES grant a DEV window a live driver holds**: `xmcspi`, `xmccshold`, `pvprobe`,
  `inprstorm` -- six targets, `inprstorm` now building three ELFs from one source.
  **`KICKOS_APP_AUTHORITY` surfaces only at runtime**, one consumer at boot and no build file
  reading it.
- **T7's OWED LATENCY MEASUREMENT WAS NEVER TAKEN, and there is no instrument to take it with.**
  The doc makes a compact-SVC-frame decision wait on the number; `qemu-arm64` has only a `base`
  preset, `bench-fleet.sh` does not list the board, and no aarch64 round-trip figure exists
  anywhere. Recorded as a debt at T7 and in `TODO.md` since 2026-08-26, so this line is no longer
  the only thing that says so.
- **The boot identity root still grants EL0 read-write over all of low DRAM**, the kernel's own
  `.data` and `.bss` at their LOAD addresses included. No unprivileged thread runs under it today,
  and revoking EL0 there was MEASURED green -- but that root is what the fault reporter and
  `aspace_release` install and what `arch_aspace_boot` hands out, so revoking changes what the space
  MEANS. It wants a decision, not a patch.
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
branches is pushed. So a fresh clone loses all eleven, and so does one `git branch -D`. The squashed
commits carry only their final tree; every hash below is an INTERMEDIATE tree that no current commit
reproduces.

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
- `docs/design-m6-mmu.md` -- the M6 contract; section 5 is the step plan M6.3 continues.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
