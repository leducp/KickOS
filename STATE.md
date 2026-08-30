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
board.** M6.3 is CLOSED, its last step R6, and the aspace-seam VERDICT IS TAKEN, and it is NOT an
empty diff: one member added, `arch_aspace_frame_at`, thirty five records identical. The verdict
was the one piece of M6 evidence that could not be gathered early, F8's empty-diff claim being
about a FAMILY, and what settled it is the property R2 was picked for. Re-take it with
`sh tests/static/check_aspace_sigdiff.sh` from the repository root, no arguments and no build. **THE
EXPECTED OUTCOME IS EXIT 2**, printing `members 35 baseline signature record(s), 36 candidate` and
one added record, `FUNC arch_aspace_frame_at`. That is the milestone's RESULT and not a regression
you introduced: exit 0 would mean the member had gone and exit 1 means the comparison failed,
which is UNKNOWN. Do NOT regenerate `tests/static/aspace_seam_records.txt` to make it quiet, which
deletes the finding M6.3 exists to produce.

**THIS BRANCH IS M6.5, FRAME-LEVEL CAPABILITIES, AND ITS THREE STEPS ARE LANDED.** C0 froze the
capability ABI before the first object kind, C1 added a frame RUN and an ADDRESS SPACE, C2 made map
and unmap capability operations, C3 shared one run into two spaces at two addresses. The milestone's
claim is a NEGATIVE result like M6.3's and M6.4's, and it is measured: `check_cap_sigdiff.sh` is
PASS at 24 records, the only two members that ever entered being plain enumerators (60 and 61). An
address never became a FIELD, only ever an argument, which is what let C1's claim survive C2.

**FOUR THINGS ABOUT M6.5 A GREEN RUN DOES NOT SAY.** The two kinds are POSTURE-GATED, so the
thirteen region boards compile the enum values and nothing can construct one; `qemu-x86_64` runs
none of these arms at all, `CHIP_Q35` selecting no memory family, so "designed against three
backends" is a DESIGN claim and the milestone is run on two. The type field is now EXACTLY full,
values 6 and 7 being the last two, and a third kind is a repartition that spends the reply sequence
packed beside it. And the cap differ's corpus is the C-facing ABI only: `kernel/include/kickos/cap.h`
is C++ and the extractor yields twelve records of ANY name over it, so `CapType`, `CapRights`,
`CapEntry` and the resolve chokepoint are held by that header's static_asserts and by nothing else.

**BOTH OF THOSE ASSERTS WERE BLIND UNTIL C0, AND THE SHAPE IS THE LESSON.** Each was keyed on the
LAST MEMBER BY NAME, so `CAP_IRQ` and `CAP_TRANSFER` bounded themselves and nothing added past them:
a kind at 8 and a fourth rights bit both compiled clean while an independent probe over the same
tree failed on the same expression. This milestone's recurring class one level below where M6.2
through M6.4 kept meeting it -- not an instrument whose corpus can go empty, but a guard whose
SUBJECT does not grow with the thing it guards. `CAP_KIND_MAX` and `CAP_RIGHTS_ALL` are the fix, and
a third kind now refuses.

**THE M6.4 PARAGRAPHS BELOW STILL DESCRIBE THE x86_64 PORT AND ARE NOT SUPERSEDED**, that board
being unchanged by this milestone.

**THIS BRANCH WAS M6.4, THE x86_64 PORT, AND IT SITS ON TOP OF M6.3 RATHER THAN BESIDE IT.** The two
reach an external auditor as the branch pair `M6.3` and `M6.4`, and M6.4 carries fixes for M6.3 as
well as its own: one edit to `arch/include/kickos/arch/arch.h` moves what acquire and
`arch_aspace_frame_at` promise on BOTH backends, so the ten-angle pass could not be split down the
branch boundary and was not. Two things landed after M6.3's own steps ended and sit under none of
them -- that pass, and two gates that reported clean over a corpus they never read: the
completeness clause was keyed on the very knob whose absence removed the programs it was counting,
and nothing pinned the x86_64 vector census the posture flags had emptied. **The class both belong
to is the one this milestone met five times over.** An instrument whose corpus can go empty without
its report changing has witnessed nothing, and the question that finds it is not whether it passes
but what makes it go RED when it is handed nothing.

**RING 3 ON x86_64 CAN READ AND WRITE KERNEL MEMORY, AND THAT IS THE MILESTONE BOUNDARY RATHER THAN
A DEFECT.** An external auditor raised it as its top Critical. The ruling is that M6.4 is the x86_64
PORT and isolation is the next milestone, exactly as M6.1 preceded M6.2 on arm64 and as M6.3's R1.5
records the same posture on RISC-V. `arch/x86/x86_64/ring3_x86_64.cc`'s own header already states the
exposure in these words, so it is not a thing the tree hides.
*What is granted.* The image is ONE FLAT LINK, so the leaves carrying an unprivileged thread's own
text also carry kernel text, the scheduler's state, the capability table, the per-thread kernel
stacks and the arch layer's statics; and the conventional-memory grant covers every thread's stack
rather than the caller's. An unprivileged thread can read and write all of it.
*What is NOT reachable, and it is ONE clause where this file used to carry three.* The port grants
the user bit over exactly two ranges and leaves every other entry as the firmware left it, and x86
ANDs the permission down the whole walk (Intel SDM Vol 3 chapter 5), so an entry ungranted at any
level is ungranted at the leaf. Out of reach therefore: DEVICE REGISTERS, including the local APIC
window and the low legacy structures. **MEASURED rather than argued**, over a walk of the whole live
hierarchy under OVMF on `qemu-system-x86_64` q35: zero reachable leaves in the APIC band, zero in
low legacy, zero outside the image and the arena, and the same figures under `-bios` and under
`-cpu max`. Ring 3 also cannot execute a privileged instruction, touch a port, raise its own level
or write `IA32_KERNEL_GS_BASE`, which is where the entry takes its pointer from.
*The other two clauses this file used to carry are FALSE, and that is the correction rather than a
worse exposure.* A LIVE TRANSLATION TABLE is reachable and WRITABLE at CPL3, `g_kwin_table[2]`, the
port's own kernel-window level-3 table installed by `aspace_init`; and so is the PER-CORE BLOCK,
`g_cpu`, with `kernel_sp` at offset 0, a ring-3 read of it returning the live kernel-block top. Both
are `.bss` statics of the one flat link, so they sit at their identity addresses inside the 2 MiB
image leaf the grant opens, and the unit this hardware grants is a leaf. **It is NOT the
firmware-sibling mechanism an auditor proposed**: pre-grant the user bit is set NOWHERE in the
hierarchy, leaf or non-leaf, so that hazard is UNFIRED here while remaining UNESTABLISHED in
general; both halves of that sentence are the record. **And it grants nothing new.** Once kernel RAM
is writable, writing a page table or `kernel_sp` adds no power the scheduler's state and the
capability table in the same leaf did not already give. What was wrong is two bounds that made the
exposure describable, not the size of the exposure. The ruling was to leave the grant alone, correct
the record and give the instrument the sight it lacked.
*Why the old arm read clean, and this is the transferable part.* Its corpus was the THREE tables the
grant WALKED rather than every table in the live hierarchy, and it ran inside `ring3_init`, BEFORE
`aspace_init` installs the table it would have found. Either blindness alone is enough. The
whole-hierarchy census in `arch/x86/x86_64/probe4_x86_64.cc` runs after `aspace_init`, ASSERTS the
device, low-legacy and outside clauses, and PINS the two exposed pages by ROLE rather than by
address, so a third reachable table or an anchor that is not the per-core block reddens an arm and
is printed on a line of its own.
*Why one flat link makes it so.* At the granularity the adopted regime uses, the user bit follows the
ADDRESS, so separating what ring 0 may touch from what ring 3 may touch means separating the two in
the LINK. Nothing narrower is available to an in-place edit of the map firmware left.
*What the narrowing is.* Separate the halves in the LINK, which is a private half plus its own root.
That is the same work R1.5 names for RISC-V, and it is a milestone of its own because it drags the
shared-symbol duplication with it, not because it is hard to describe.

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

**The `spike/*` branches MUST NOT be merged, and there are FIVE of them, not the four this line
used to name.** `git branch --list 'spike/*'` answers the count; what it cannot answer is the rule
and what each one is for. `spike/m6-virt` is SPENT, M6 having shipped. Three carry MEASURED answers
the next milestones need, and are why several M6 freezes were already right: `spike/m63-rv64-sv39`,
`spike/m64-x86_64`, `spike/m7-smp-triarch`. The fifth, `spike/m49-unit-tests`, was never described
here and is not described here now: nothing has been read off it, so this file says only that it
exists and that the no-merge rule covers it. De-risking runs, not ports. Everything they found
that moves the contract is already folded into `arch/include/kickos/arch/arch.h`,
`docs/design-m6-mmu.md` (F8, T2's four RV64 corrections, T9, section 3) and
`docs/design-m7-smp.md`. Read those, never the branches.

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

- **THE INSTRUMENT A FLEET VERDICT COMES FROM HAS BLIND SPOTS OF ITS OWN, AND THEY BOUND EVERY
  BULLET BELOW.** That verdict comes from `tools/sweep_host_gates.sh` and
  `tools/sweep_image_gates.sh`, and a green run of both leaves each of these standing. The emulator
  is not the chip: a gate green under qemu says the image boots under qemu. A gate that fails only
  under load passes here; the image half serialises to remove the instrument's own noise, which is
  not evidence that a gate is load-independent, and the host half batches its set by design. A gate
  that fails intermittently reads exactly like one that is broken, nothing being re-run. Nothing
  snapshots the tree: each preset reads the source afresh, so a tree edited while a sweep runs
  yields verdicts belonging to different tree states, with no record of which preset saw which. And
  both tools take the service list each preset defaults to, so a provider that lives only in
  `tests/static/service_lists.txt` is compiled by neither.
- **NOTHING WITNESSES THAT THE x86_64 DECODE IS FED THE LIVE ATTRIBUTE TABLE**, and the arm that
  used to is gone on purpose. It proved the feed by REPROGRAMMING `IA32_PAT`, which SDM 14.12.4
  makes the operating system's job to sequence and which this port has no reason to spend a cache
  flush on; an external re-review called it out and it was replaced by synthetic tables the decode
  is checked against. So the DECODE is witnessed over four layouts no firmware here provides, and
  the FEED is witnessed nowhere. The two arms that would separate them are vacuous on this bench
  because OVMF leaves the register at its power-up value, and only a machine whose live table
  DIFFERS could tell them apart. A backend handed a constant instead of the register passes
  everything.

- **NOTHING WITNESSES THE M6.5 KINDS OUTSIDE A TEST-ONLY MINT.** There is no user-facing way to
  create a frame-run or address-space capability: both arrive through `KOS_ASPACE_OP_CAP_SEED` and
  `KOS_ASPACE_OP_CAP_SELF_SPACE`, which are selftest scaffolding. So the OBJECTS, the map pair and
  the sharing are exercised, and the question of who may mint one is not answered anywhere. A
  milestone that gives them a real mint decides it.
- **THE FRAME RUN'S REFCOUNT AND `Domain::borrowed_from` ARE TWO OWNERSHIPS, NOT ONE.** The step
  plan predicted C3 would replace the donor edge and it does not: F10's handoff takes its frames
  from the donor's RESERVATION, which its range list owns, so that path still needs the edge, while
  C3's frames belong to the run object and its mapping space owns nothing. Anything reasoning about
  frame lifetime has to ask which path put the mapping there.
- **READING A SHARED PAGE DOES NOT TEST ITS LIFETIME, and the first version of `cap_share` got that
  wrong.** A freed frame stays readable through a leaf nobody tore down, so an early free slipped
  the read entirely. The POOL is the instrument: the arm asserts the run is still out at the moment
  the borrower's task has died and the donor still maps it. Freeing at the first drop reddens
  exactly that assertion and nothing else.
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
- **The x86_64 board IS in this tree, and what it does not witness is the aspace family.** This
  bullet used to say the opposite, that x86_64 was a sibling branch and not a merge, which the
  paragraph twenty lines above already contradicted: `boards/qemu-x86_64/` is tracked, M6.4 sits ON
  M6.3, and both halves ship out of one tree. So there is ONE seam measurement now rather than two
  to be unioned, and it is the one the top of this file records. What the board still does not run
  is the family itself: the chip selects no memory family, ships no map editor and declares no frame
  pool, so its `arch_aspace_*` claims all come from X5's ad-hoc link, which is a `ninja` target and
  not a registered arm. A green `ctest --preset qemu-x86_64` says nothing about them.
- **THE RV64 MODEL LINE READS `32 PA bits` SINCE THE 2026-08-29 RE-REVIEW, NOT 56, AND NO COUNT
  MOVED WITH IT.** The 56 was the PTE's PPN field, which is the architecture's output width for
  every RV64 mode; the physical extent is a PLATFORM figure and RISC-V publishes no register for it,
  so it comes from the chip (`KICKOS_RV64_PHYS_ADDR_BITS`) and the arch keeps none beside it. Read a
  `56 PA bits` in any dated record as what that step measured. **AND THE 32 IS NARROWER THAN THIS
  MACHINE**, which also backs the 16 GiB PCIe window: measured in machine mode, `2^32`, `2^35`,
  `2^40` and `2^55` each access-fault while 16 GiB reads. The board describes nothing above 4 GiB,
  and the port parses no device tree, so the described extent is what it refuses against.
- **THE BOOT GRANT MEASURES ITS OWN EFFECT NOW, and a zero PMP readback is DENIAL on this hart.**
  Every PMP field is WARL, so `pmpcfg0` and `pmpaddr0` both reading zero is a hart with no entry
  (everything permitted) and a hart with every entry OFF (every supervisor access denied), one
  readback either way. Measured: writing zero to both reproduces that readback and an
  `mstatus.MPRV`/`MPP=S` load faults, so the case the prologue used to carry on through is the
  denied one here. What no probe on this bench reaches is a hart that produces it WITHOUT being
  written zero, which is the WARL hardwiring the specification permits and no emulator model offers.
  **AND THE THREE THINGS THE PROLOGUE NOW ESTABLISHES BEFORE IT MEASURES ARE ALL UNFIRED ON THIS
  MACHINE, which is exactly why they are worth a line here.** `satp` reads zero out of reset here, so
  the Bare write changes nothing; `mstatus.MPV` will not set at all (`csrs mstatus, 1 << 39` leaves
  `mstatus` at `0x0000000a00000000`) although `misa` bit 7 says the hypervisor extension is present,
  so the clear changes nothing; and the `SFENCE.VMA` the PMP write owes changes the probe's answer in
  NEITHER direction, measured both ways over a revoked grant, QEMU synchronising its own PMP writes.
  Each of the three is required by the specification and none of them has a witness here. The
  misalignment probe is a fourth of the same kind: this core resolves misaligned accesses in
  hardware, the probe's accumulator reads `0x0`, and the delegation it would add is never asked for.
  **What DOES have a witness is the ENTRY CENSUS**, because a hart with entry 0 reading OFF and a
  higher entry granting only the probed word is buildable here, and without the census that hart
  boots into a SILENT HANG: no console, no finisher word, the emulator killed by the timeout.
- **`qemu-riscv64` HAS NO SILICON, so every rv64 claim is emulator-grade.** `docs/reference/boards.md`
  names no RV64 part and no `tools/flash*.sh` names this arch, so the backend has only ever run under
  emulation and there is no path to a run. **And F8's named silicon witness does not boot this image
  at all**: `-cpu thead-c906` on the shipped `hello` produces NO output, one `zfa` privilege-spec
  warning and a timeout kill, with the default core printing the banner as the control. So the c906
  figures in the M6.3 record are a standalone probe's and never the suite's.
- **THE MAP EDITOR'S TLB MAINTENANCE IS WITNESSED IN NO PLACE OF SIX, and this line claimed one
  until it was re-taken.** The one it claimed rested on `qemu_riscv64_aspace_fault`, an arm retired
  on 2026-08-29 for reading the page from the privileged side, where a supervisor read faults on an
  unprivileged leaf whether the leaf stands or not. Re-taken against its replacement,
  `qemu_riscv64_aspace_ufault`, as two isolated single-site mutations on a wiped build directory:
  removing `unmap`'s per-page invalidate ALONE leaves every one of the board's arms green, image
  arms included, so that site joins the unwitnessed five; making `arch_aspace_unmap` a no-op that
  still reports success turns `qemu_riscv64_aspace_ufault` red on "the unmapped page did not fault"
  and `qemu_riscv64_selftest` red beside it. **So what has a witness here is that unmap CLEARS the
  leaf, and the invalidate beside it has none.** HELD BY A COUNTER AND NOT BY AN ACCESS: the
  fresh-map per-leaf invalidate, caught only by `map_tlbi_elided`'s floor. NOT WITNESSED AT ALL:
  `unmap`'s per-page invalidate, break-before-make, destroy's sweep ahead of `free_subtree`, and
  `arch_aspace_activate`'s whole-hart fence. And the fresh NON-LEAF fence's ISSUED path never executes
  in this suite, proved by multiplying that bump by 100 and seeing every figure stand still, so only
  its ELIDED leg runs (2 of the seed's 47).
- **A ROOT WRITE APPEARS TO FLUSH THE WHOLE TLB ON THIS EMULATOR, WHICH IS WHY ACTIVATE'S FENCE IS
  DEAD-EFFECT.** Deleting it left all 132 arms green on a suite that switches between live per-space
  low halves. **That 132 is DATED to when the mutation was taken and the suite is longer now**; the
  fence mutation has not been re-taken at the higher count, unlike the unmap pair above, which was.
  Read the current plan line off a run rather than off this file, which has already carried a stale
  successor to that 132.
  The reading itself stands: a stale low-half translation WOULD be consulted there, so a green run is
  only consistent with the emulator dropping translations on the `satp` write itself. That is an inference
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
- **A MISPAIRED WINDOW RELEASE IS NOW DETECTED BY A TEST RATHER THAN REFUSED IN PRODUCTION.**
  `arch_aspace_release` on rv64 used to `kpanic` on a release that named no hold and whose frame
  lay outside the kernel window; it counts instead, in the low byte of
  `arch_aspace_tlbi_counts`, and `map_tlbi_elided` is the only thing that reads it. So an image
  built without the self-test records the defect and reports it nowhere, and no board refuses
  one at run time any more. What bought that: the member sits on the fault reporter's descent
  (`kaccess_to_user` reaches it through `access_copy`), so the panic fired inside the record it
  was writing; and rv64 is the only backend that does the work at all. **The arm is NOT vacuous
  and that was measured three ways**: inverting the classifier turns it red, a genuine double
  release inside `op_acquire_dup` turns it red, and a genuine double release of every page
  `access_copy` touches leaves it GREEN, because an offset-route hold spends no slot and a
  repeat surrenders nothing. That last case was invisible to the panic too, so the change costs
  the refusal and not the coverage.
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
- **`trap_redzone` IS REGISTERED ON NO EMULATOR BOARD, and M6.4 is what that costs.** The six
  presets a milestone actually measures on -- `qemu-arm64`, both RV64 postures, `qemu-x86_64`,
  `sim` and `sim-telem` -- are exactly the six that declare no pair in `trap_redzone_roots.txt`,
  so a green pass on them is silent about every trap-stack figure in the fleet. One
  `KICKOS_ASSERT` added on the console route at `fdf58b3d` put `kpanic` on the fault-exit descent
  and took 32 presets red, and nothing this milestone measured could see it: it surfaces as a red
  gate on boards nobody named, never as a build error. **A change on the syscall or the console
  path needs the full 50-preset sweep, not a three-board sample and not the emulators.**
- **`console_reach` IS THE HALF OF THAT GAP THAT COULD BE CLOSED WITHOUT A TRAP-STACK FIGURE, and
  it is registered on the four TRANSLATING presets rather than on the six.** It asks one
  reachability question over the same `-fcallgraph-info` graph `trap_redzone` builds: no `kpanic`
  and no `kpanic_at` reachable from the fault-record console route. It is on `qemu-arm64`, both
  RV64 postures and `qemu-x86_64`; `sim` and `sim-telem` are still uncovered by either gate.
  **It is registered where the doors EXIST, and that is the whole reason for the preset set**: on
  the region-model boards `access_copy` is an unconditional `kmemcpy` gcc proves cannot fail, so
  the `cap_console_deliver` error family folds away and there is nothing for a reachability clause
  to find. **It found four doors and all four are closed**: the reachable panics were
  `reent_seat` and `reent_prime` (`kernel/thread/reent.cc`), `ep_copy`
  (`kernel/syscall/syscall_mem.cc`) and, on RV64 only, `arch_aspace_release`
  (`arch/riscv/rv64imac/aspace_rv64imac.cc`); the first two now slay the incoming
  thread, the third refuses and the fourth counts. **The gate was RED on three of the four
  between `517449e5` and `58b43d62`/`d4977780`, and two Reference records went on saying so
  afterwards** -- `docs/reference/boards.md` and `docs/design-m6-mmu.md`, corrected 2026-08-29.
  That is worth the line because it is this milestone's own class: a record that was TRUE when it
  was written and stopped being true when the defect it described was fixed, with nothing tying
  the sentence to the fix. When a door closes, the file that named it open is part of the fix. Its declaration is
  `tests/static/console_reach_roots.txt` and it states in its own header what it would fail to
  catch; do not widen that file to quiet it.
- **`bluepill-c8-st` AND `f302nucleo-st` LINK AGAIN, AND WHAT FIXED THEM WAS A FLEET-WIDE COST
  RATHER THAN THOSE TWO IMAGES.** They contributed no trap-stack figure between `480767f1` and
  this fix, `trap_redzone` dying at the same link inside its own scratch tree: a build error
  wearing a depth gate's name, on a preset that IS declared in `trap_redzone_roots.txt`, so a
  reader scanning for a missing declaration finds nothing. What the fix removed is the per-app
  build stamp's runtime reformat, which every image on every board compiled, so what looked like
  two boards' problem was a fleet-wide charge to flash and `.data` on every app of every preset.
  **The thin part was the SPLIT, not the fleet, and the split is now sized against the image**: the
  two `#undef TAP_ADD` boundaries had never been measured, so part 1 sat on a few dozen free bytes
  of its 64 KiB while parts 2 and 3 sat on kilobytes. Region 1 now ends after `call_timeout_reply`
  and region 2 after `cap_reply_slot_reuse`. Same arms, same order, proved by the arm-name union of
  `microbit`'s three parts being identical across the move -- which is the part no command answers
  and the reason this bullet exists. Every byte figure is a link away: the per-image sizes are at
  `docs/reference/boards.md` and the per-app attribution at `TODO.md`.
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
  run on this bench can tell whether it is there. Re-measured at T8b on ARMV8A, and the sibling
  claim that stood here, that removing `unmap`'s per-page invalidate fails `aspacefault`, is a
  T8b measurement on `qemu-arm64` and is NOT a statement about rv64. The rv64 half was re-taken on
  2026-08-29 and came out the other way; the map-editor bullet above carries it.
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
  **AND THE SIX COUNTS SOMETHING ELSE ON RV64 SINCE THE 2026-08-29 AUDIT.** That backend now
  REFERENCE COUNTS a window slot per (space, page), which `arch/include/kickos/arch/arch.h` admits
  in the same breath as the per-call rule, so its capacity is six DISTINCT pages per core and not
  six calls. The floor is neither tightened nor loosened by it: the scenario the six came off needs
  four distinct pages, `ep_copy`'s two ends naming pairs two of the outer holds already name. What
  moved is what a reader should take the constant to MEAN on that port.
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
