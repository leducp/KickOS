<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# Multicore: a shared kernel where the hardware earns it, AMP where it does not

> **Status: ACTIVE** -- the design contract for multicore, written to be audited before code.
> Section 1 is the hardware predicate that decides which parts get a shared kernel at all,
> section 2 is what this contract FREEZES, section 6 is the step plan with the expected result
> of each step. An auditor is invited to add steps, split them, reorder them and correct the
> expected results; the step identifiers exist so a finding can name one.
>
> **This document names no milestone, and that is deliberate.** `roadmap.md` owns the schedule
> and is the sole entry point mapping a milestone to the documents it draws on and the steps it
> adopts. A design that carries its own milestone number inverts that: the mechanism starts
> depending on the plan, and every resequencing rots a document that describes something which
> did not change. Step identifiers here are local to this file.

`docs/design-m7-smp.md` is the exploration and stays one: it carries the cross-core IPC transport,
the atomics residue, the hardware mechanics of the RP parts and the corrected TLB bullet, and all of
that is still the input.

**What it does NOT decide is the target, and its candidate ranking is not this plan.** That ranking
was written when multicore was expected to precede the MMU. It sorts RP2350 above RP2040 above the
LX6, it never mentions an A53, and its gate is an MCU gate: exclusives, SIO spinlocks, the E2
erratum, the windowed ABI. Translation landed first, so the machine multicore arrives on is the A53
the MMU port already runs on, and every clause of that ranking is satisfied trivially by it. Read the
spike for its transport design and its hardware mechanics; do not read its ranking as a plan.

The silicon target is the quad-core i.MX8MP. `qemu-arm64` is the development vehicle, which is why
section 2 freezes bring-up and interrupt decisions against that part rather than against the emulator.

---

## 1. When a shared kernel is reachable

`docs/design-m7-smp.md` states the gate as two coupled facts, the inter-core atomic and switch-path
maturity. That is not enough: it names the primitive the lock needs and says nothing about the two
properties whose absence turns a shared kernel into software standing in for missing hardware.

**The predicate is a set of hardware properties, never a family name.** An architecture family is a
proxy, and a proxy dates the first time a part violates it. A future part qualifies by meeting the
predicate, with no list to edit.

### 1.1 Mandatory: a shared kernel is not implementable without these

1. **Coherent shared memory with no software maintenance.** Run queues, thread control blocks, the
   capability table and the lock word are read and written by every core. Satisfied EITHER by
   hardware coherency (an A53 cluster in an inner-shareable domain) OR by there being no per-core
   cache over that memory at all. The second clause is why this is stated as a property rather than
   as "has cache coherency": the RP2040's M0+ has no data cache, so it is coherent by absence.
2. **An inter-core exclusion primitive**: an architectural atomic (load-exclusive/store-exclusive,
   compare-and-swap, a large-system atomic) or a hardware lock block. Which one a part has caps the
   lock ALGORITHM, not the granularity.
3. **An inter-core interrupt.** Cross-core reschedule, shootdown where the architecture does not
   broadcast, and the reclamation stall all need one core to reach another.
4. **A cheap per-core identity**, which is what `arch_cpu_id` becomes above one core.
5. **Symmetric cores.** One kernel image scheduling a thread onto either core requires the cores to
   be interchangeable. This is the AMP boundary by definition rather than a quality judgement.

### 1.2 Required for a sane kernel: implementable without, and declined

6. **Per-line interrupt targeting**, a controller that can direct a line to a core. Without it every
   line lands on every core, and three things follow. Which core services a line becomes software
   state kept coherent across cores. The interrupt-versus-kernel-lock asymmetry cannot be dodged by
   pinning, which forces a second lock. And the grant model stops mapping to hardware: a line
   "granted" to one core is granted only because the other masks it, which the isolation principle
   reads as the wrong shape, a grant that is a mask being neither the narrowest unit nor a refusal.

### 1.3 The MMU is not on the list

A shared kernel needs coherent memory and exclusion. It does not need translation, and a shipping
counterexample settles it rather than an argument: FreeRTOS's dual-core RP2040 port is SMP with no
MMU at all. What translation buys is isolation between processes, which is exactly as valuable on
four cores as on one and no more.

One consequence belongs in the value section rather than here: without translation the cores share
one flat physical space, so isolation between their workloads is whatever a per-core protection unit
gives. That changes what a shared kernel is WORTH on such a part. It does not change whether one
works.

### 1.4 Efficiency is a property of the workload, and no number is frozen here

The predicate answers "implementable and sane". It says nothing about "worth it", which is a
different quantity with a different owner.

Under one lock, speedup is Amdahl over the lock-hold fraction `f`: `S(n) = 1 / (f + (1 - f) / n)`.
`docs/design-m5-ipc-fastpath.md` section 3.0.4 measures `f` at 0.53 on a call and reply round trip,
with a leaf floor of 0.43, bounding two cores at 1.31x and four at 1.55x.

**`f` is a property of the workload.** That measurement is the most kernel-dense traffic this system
has; a compute-bound pair has `f` near zero and scales nearly linearly. So a shared kernel pays when
cores spend their time OUTSIDE the kernel, and this system's characteristic traffic, IPC-mediated
driver calls, is close to the worst case for it. What raises the payoff structurally is per-core run
queues so cores stop contending on scheduling, which is the spike's stage 2 and belongs to the IPC
optimisation work rather than here.

**No speedup figure is a freeze.** Both bounds are re-derived after the lock-hold shortening rather
than frozen now, and `roadmap.md` already requires that.

### 1.5 How the fleet sorts, with every exclusion named

| part | coherent | atomic | inter-core IRQ | identity | symmetric | targeting | verdict |
|---|---|---|---|---|---|---|---|
| A53 cluster (`qemu-arm64`, i.MX8MP) | yes | yes | SGI | MPIDR | yes | GIC | shared kernel |
| RV64 multi-hart (`qemu-riscv64`) | yes | A extension | MSIP | mhartid | yes | PLIC | shared kernel |
| RP2040 | by absence | SIO lock | FIFO | CPUID | yes | **NO** | AMP |
| RP2350 | by absence | exclusives | doorbell | CPUID | yes | **NO** | AMP |
| ESP32 LX6 | yes, in internal SRAM | **not established** | matrix, sources 24-27 | **not established** | yes | matrix | shared-kernel CANDIDATE, gated |
| ESP32-C6 | | | | | **NO** | | AMP |
| i.MX8MP Cortex-M7 companion | | | | | **NO** | | AMP |

Three rows want their evidence stated, because each is the opposite of the obvious reading.

**The RP parts fail on targeting and nothing else.** The RP2040 datasheet section 2.4.1.3 says it
in the vendor's own words: "Each M0+ core has its own interrupt controller which can individually
mask out interrupt sources as required. The same interrupts are routed to both M0+ cores." There is
no distributor. They pass every mandatory requirement, and they are excluded by 1.2 rather than 1.1.

**THE LX6 ROW SAID IT FAILS COHERENCY AND THAT WAS WRONG, so it is not excluded and its verdict is
now a gate rather than a ruling.** The old evidence was that the two cores' caches are not coherent
over external flash and PSRAM, which is TRUE and is not the question: requirement 1 asks about the
memory holding run queues, control blocks, the capability table and the lock word. The ESP32's two
32 KB caches sit ONLY on the external path -- their backing pools are carved OUT of internal SRAM0
rather than covering it, both cores reach embedded memory directly, and the manual gives SRAM as a
single-cycle access. No cache, no enable and no maintenance operation exists over internal memory
anywhere in it. So kernel state in internal SRAM satisfies requirement 1 by its second clause, and
the constraint that buys it is a PLACEMENT rule rather than an impossibility.

**Its targeting is a real distributor, and that is the opposite of the RP reading above.** Every one
of the 71 sources has a map register in EACH CPU's own bank, and pointing one at an unconnected
input number disables the source for that core: routed into a sink, never raised and masked. The
four sources that are not routable to either core are hard-BOUND to one core each rather than
unroutable, and GPIO targets per PIN per CPU underneath. The inter-core interrupt is four sources
of that same matrix, so the doorbell and the targeting are one mechanism.

**What blocks it is two columns a chip manual cannot answer, and they are the same kind.** The
atomic and the per-core identity are core-architectural, and the ESP32 TRM has no instruction-set
chapter, mentions its own core three times, and lists no architecture reference to defer to. So
both are unsourced rather than absent, one document settles both, and until it does this part
cannot DECLARE the predicate -- which is what `smp.cmake` exists to make a part do.

Two conditions ride with the row even if that document satisfies both. Kernel state must be PLACED
in internal SRAM, so a port owes a linker rule rather than a runtime check. And the cores are
interchangeable for anything a thread does, while not being identical: one 8 KB memory is reachable
by one core only, and a per-CPU peripheral answers differently at one address, so requirement 5
holds for scheduling and not as a general statement about the part.

**THE RP PARTS CAN RUN A SHARED KERNEL AND ARE DECLINED ONE, and the record says so deliberately.**
FreeRTOS ships a dual-core RP2040 port building two kernel locks from two SIO spinlocks. A future
reader meeting it will otherwise reopen this. The exclusion is a judgement about value, and it is
recorded here so that judgement is what gets argued with: requirement 6's absence buys per-core mask
reconciliation, a second lock, and a grant model that no longer matches the hardware, and it buys
them in SHARED kernel source that every preset compiles and every single-core board must keep
correct. The byte-identical invariant protects the image, not the source. That is a fleet-wide
charge for a bound of 1.31x on two boards.

**That argument covered three parts and was only ever valid for two, which is how the LX6 row's
error survived.** It turns on requirement 6's ABSENCE, and the LX6 satisfies requirement 6. ESP-IDF
shipping dual-core FreeRTOS on the ESP32 was read here as a part running a shared kernel it should
be declined; it is better read as the part meeting the predicate.

---

## 2. The freezes

A freeze is a decision this contract may not reopen without saying so; the deliberately open
questions are section 8. Per the MMU contract's rule, a freeze rests on another decision and never on
an observation of the tree: where one needs a fact about the code, it says whether that fact is a
decision or a measurement.

### N1. The deliverable is a second core, not a lock

`roadmap.md` frames this work as a Big Kernel Lock first. The lock is the smallest and least uncertain
piece of it, and framing the deliverable around it would ship an empty corpus.

Two reasons, and the second is the one that matters. The lock's span was already frozen by F4 of
`docs/design-m6-mmu.md`, "one lock spans capability resolve-to-use", carried forward from the spike
and honoured by the frame-mapping capability pair, so this work does not choose it. And a big lock is
UNFALSIFIABLE at one core by construction: the invariant is that a single-core build stays
byte-identical BY THE LOCK COMPILING TO NOTHING, so at one core a correct lock and an absent lock are
the same image. That is this project's recurring class, an instrument whose corpus can go empty
without its report changing.

The deliverable is therefore a second core executing the kernel, with the lock as what makes it safe.

### N2. One lock, its span at F4, and the lock and the doorbell are one design

F4 is inherited unchanged and does not widen. Section 4 is why it does not need to: what a lock
substituted for the current exclusion primitive leaves open is mostly places that never had a lock,
not spans too narrow.

The coupling is already frozen and `arch/include/kickos/arch/arch.h` carries it in prose: the far
side's handler takes no kernel lock, and the lock's acquire loop services a pending doorbell.
Otherwise an initiator holding the lock waits on a core spinning to acquire it. **These are one
design and may not be built as two**, and the risk is asymmetric: that deadlock is unavailable on A64
and available on the other two, so it cannot be discovered by the first backend.

### N3. One lock and not two, bought by interrupt affinity

The reference kernel this lineage cites splits into a task lock and an interrupt lock precisely
because both cores of an MCU take every interrupt and neither can be told not to. Requirement 6
says we do not ship a shared kernel on such a part. On a GIC, affinity is ours to set, so device
lines are pinned and one lock suffices.

**The asymmetry is the reason, so it is recorded rather than the conclusion alone.** A future part
meeting requirements 1 to 5 and failing 6 would reopen the second lock, and requirement 6 exists to
keep that part out.

### N4. The doorbell serves both models

`arch_ipi_send` and `arch_ipi_wait` are the same seam and the same hardware for a shared kernel's
cross-core interrupt and for an AMP node's inter-node doorbell. Only the callers differ: under a
shared kernel the far side is another core of one kernel and N2's rule binds it; under AMP the far
side is a different kernel and there is no shared lock to reason about.

**Narrowing the doorbell's contract to shared-kernel semantics is a defect, not a simplification.**
It is the obvious reading of "the MCU class is AMP" and it would break AMP before AMP is written.

**AND THE RESCHEDULE IS EXACTLY SUCH A NARROWING, so the raise carries none of it.** A cross-core
reschedule cannot be the raise itself: the raise is an edge, and the kernel lock's acquire loop
absorbs it by POLLING, which acknowledges the edge and enters no scheduler. It is therefore state,
published against the target before the raise that wakes it and consumed by the one dispatch that
enters the scheduler. **Where that state is published is what decides whether the doorbell is
generic.** Published inside the send, every raise carries a reschedule, and an instruction-side
rendezvous or a peer-start wake then costs each target a self-raise, an exception entry, and a
contended kernel lock for a switch nobody asked for. Published by the caller that wants the switch,
`kickos::klock_resched_ask`, the send is a doorbell and the rendezvous callers cost their targets
nothing. The kernel is the only side that can name the publisher, which is what makes this
structural rather than conventional.

### N5. Bring-up is an entry point and a reset release, and a load is a separate question

Every part's start button has one shape: write an entry point, release a reset. The RP2040 sends
`{0, 0, 1, VTOR, SP, PC}` over the inter-processor FIFO to a bootrom wait loop (datasheet 2.8.2),
the RP2350 does the same plus an arch marker (datasheet 5.3), the ESP32 writes the entry to the
APP_CPU boot address register and clears its reset bit, and the i.MX8MP writes the companion's
initial vector table into an IOMUXC general-purpose register and sets the enable bit in the reset
controller.

What differs is whether a LOAD precedes it. On the one-image parts the secondary's code is already
resident and the primary supplies only an entry point. On the i.MX8MP the companion runs from
tightly-coupled memory and something must place code there first.

**Two consequences.** The launch is chip business below the seam, never a kernel-level concept. And
because the i.MX8MP companion is startable by two register writes from the A53 side, "the bootloader
owns it" is a convention rather than a constraint: the seam may not ASSUME it, because owning the
launch is what makes a hung companion restartable, and a companion that cannot be restarted sits
badly against a kernel whose posture is that a dying driver is contained and the system continues.

### N6. An AMP node is a single-core kernel

Each node runs its own kernel over its own memory. From inside, the core count is one and every
single-core invariant holds unchanged: no big lock, no cross-core wait queues, no per-core mask
reconciliation. That is the whole reason AMP is less code than a shared kernel rather than more, and
it is why the predicate in section 1 does not apply to an AMP node.

The mechanism exists. `KICKOS_MULTI_INSTANCE` keys instance state through one accessor, and
`docs/design-m7-state-inventory.md` section 6 already records that a different keying is a
substitution of one macro.

**THE COUNT IS WHAT THE IMAGE DRIVES AND THE MODEL IS HOW IT DRIVES IT, and separating the two is
what makes an AMP port writable.** The running thread is reached as
`per_instance[kickos_instance_index()].current[arch_cpu_id()]`, two keyings composed. A shared
kernel is one instance over many cores, so the instance index folds and the core identity does the
work. An AMP node whose image is its own is one instance over one core, so both fold and the node
correctly believes itself alone. An AMP node sharing ONE IMAGE with its peers is neither: its
instance index has to come from its core identity, so it needs a count above one to have an identity
at all, while needing none of section 1's properties, a core whose kernel is its own excluding
nobody.

So the count states how many cores the image drives and a separate model states whether one kernel
spans them, and **N10's refusal is the MODEL's rather than the count's.** Keying it on the count
locked the parts the predicate sends to AMP out of the count AMP needs, which made the first line of
an AMP port unwritable and is corrected here rather than left for that port to discover.

What an AMP image on one image still owes, and it is not a freeze because no such port exists: the
instance index is a simulator's host-thread word today, so a chip's has to come from its core
identity.

### N7. Cross-node IPC is ONE mechanism, and locality never reaches the API

A caller holds an endpoint capability and calls it. Whether the receiver executes on this core, on
another core of this kernel, or in another kernel is resolved below the seam.

Discovery and invocation are separate questions and only discovery needs naming. In a capability
system a caller does not find a service by name, it is handed a capability, so the locality
knowledge a caller might need already arrived with the delegation.

Two API surfaces would be a second truth, two answers to "how do I call a service", with every
consumer branching on something it has no business knowing. The driver framework and the KickCAT
reality check are exactly the consumers that would carry that branch.

**One measurement makes it affordable and is recorded so the decision can be rechecked:**
`KOS_EP_MSG_MAX` is 256, so a ring slot at full message size is 256 bytes and a message that fits
locally fits remotely. Had the slot been forced smaller, a uniform contract would have been a lie
and this freeze would not stand.

The register fastpath has no shared registers across cores and bails out on a remote partner, which
is a kernel-side guard and is invisible above the syscall.

### N8. No capability authorises the crossing; privilege sits at configuration

**The inter-core interrupt is not a user-facing operation.** Userspace never sends one; it calls an
endpoint it holds a capability for, and the kernel decides what that means. There is nothing to
authorise.

A crossing capability would also be wrong on its own terms. The endpoint capability IS the
authority, so requiring a second one to use the first is two answers to "may I invoke this". And it
would be a COARSER grant than the one already held, authorising any remote endpoint where the
endpoint capability authorises one, which inverts the isolation principle's rule to grant the
narrowest unit.

Authority is owed at two configuration acts instead: minting a cross-node endpoint, and starting a
core. Both are privileged, both are kernel init from a static partition in the first AMP work, and
both belong to the same undecided question as the frame-run and address-space capability kinds.
Answering it here for cores would decide the general case through a special case.

**The security work of cross-node IPC is validation, not authorisation.** The shared window is
memory a peer node writes, so every index and length read from it is untrusted input and the
receiving kernel validates all of it, trusting nothing. That is the AMP analogue of the per-page
validation and access split in `docs/design-m6-mmu.md` section 3.3: the far side is untrusted exactly
as userspace is. Stated because "we have capabilities" invites the assumption that the crossing is
already safe.

### N9. No read-modify-write surface is added above the seam

`docs/reference/style.md` carries the rule and `tests/static/check_atomic_rmw.sh` enforces it
tree-wide. The cross-core lock is a per-arch seam whose cheapest correct mechanism differs per part;
what it must not do is expose an RMW above itself.

### N10. The predicate is enforced at configure time, not documented

An architecture declares the properties it satisfies, and a SHARED-KERNEL selection without that
declaration is a configure refusal that names the missing properties and points at AMP. The refusal
matters more than the ruling: the spike already records that on a dual-core part with no cross-core
primitive, bracketing with the local interrupt mask is a CORRECTNESS bug rather than a slow choice,
and that the seam should refuse rather than trust the selection.

**It is keyed on the MODEL and not on the count**, per N6: an AMP image raises the count to have a
core identity at all and satisfies none of the six, so a refusal reading the count alone refuses the
model that is the predicate's own answer for those parts.

---

## 3. What the MMU work already cut, so this does not

The seams compile to nothing at one core. Stated so the plan is not re-made around work that exists.

- The per-core identity and the doorbell are declared, folded at one core, and frozen as records.
  The doorbell is TWO calls from its first line because a fire-and-forget one cannot express a
  rendezvous.
- `struct armv8a_percpu` is arrayed by the core count with its first field's displacement asserted,
  and `arch/arm64/armv8a/vectors.S` names the multi-core edit in its own comment.
- The per-core translation-root cache is already keyed and already loops over every core.
- The address-space family is frozen and coherence-complete, with no flush call to schedule and no
  address-space identifier above the seam.
- The active-core set is DECIDED and not built: never a parameter, because a parameter edits every
  backend at once. T9 named the alternative a readable FIELD on the opaque space, and that form is
  unimplementable: on all three translating backends the opaque `struct arch_aspace*` IS the root
  page-table frame, cast, and four things depend on that identity -- the value written to the
  translation base, the comparison that answers whether this core has the space installed, the
  boot space's reconstruction from the register, and the subtree walks. A header word or a stolen
  descriptor slot breaks one of them or hands the hardware walker a non-descriptor. **The set is
  therefore DERIVED**, from what the backend last installed per core, which honours every reason
  T9 gave -- opaque type, no signature fan-out, backend-local, free to add -- better than the
  field would.
- Cross-core maintenance is file-local bodies inside the armv8a backend. Nothing named a flush
  crosses the seam, so what changes is those bodies and not a signature. *There are THREE, not the
  two this said:* the by-address invalidate, the range sweep, and the root change, which open-codes
  a sweep of its own instead of calling the range one. The third stays LOCAL when the other two go
  broadcast, a root change concerning the PE whose register changed.
- The data-cache clean and invalidate seam is landed with an armv8a backend and no caller.

---

## 4. What a lock substituted for the current primitive leaves open

An audit of `docs/design-capability-table.md` section 8's catalogue against the tree found most of it
ANSWERED, because `kernel/syscall/cap.cc` states the precondition the substitution needs: every entry
point's caller holds the lock, and a resolved object pointer is used under the same continuous lock.
The frame-mapping capability pair honours F4 as written.

Six survive, and the first three are not span problems at all.

1. **The register fastpath holds no lock object**, and says so in its own comment. It reproduces five
   of the catalogue's hazards unguarded on a peer's thread control block, including the width bound
   that keeps a slot lookup inside its own run. **It is unreachable on both targets of this work**:
   the opt-in exists only for armv6m, armv7m, rv32imac and rxv3, and the symbol is in neither image.
   So this contract RULES on it and does not fix it, and N10's refusal is what carries the ruling.
2. **The interrupt entry takes no lock** and dereferences a binding a concurrent teardown is freeing.
   The teardown's own claim to safety is a masked window, and a mask on one core is not exclusion on
   another. N3 answers it by affinity.
3. **The exited-slot reclaim window opens where the lock closes** -- and the answer is that the
   lock no longer closes there. `kickos_switch_unlock` moved the release inside the swap, to the
   point where the outgoing frame is parked and the core stands on the incoming one, so the
   bracket that publishes EXITED does not release before the park and the window has zero width.
   **No second mechanism, and deliberately none:** an epoch or a quiescence flag would be a second
   answer to "is this thread off-CPU", and the release point is already that answer. What the gap
   owed and now has is the invariant made checkable rather than argued across three files, which
   `tests/unit/exitquiesce/` does at two cores over both halves of the swap. The reclaim key stays
   exactly EXITED: widening it admits a thread still running its own teardown.
4. **Cross-core translation invalidation** after unmap and release. The armv8a maintenance is
   NON-BROADCAST as written: the barriers around it are already `dsb ish`, and what applies to
   the executing PE alone is the TLBI, which carries no shareability component at all
   (`tlbi vaae1`, not `vaae1is`). *Non-shareable* is the wrong word for it -- that is a named
   architectural domain with its own `dsb nsh` form, and no code here is in it. The second half
   of this entry, "there is no inter-core interrupt in the tree", was true when written and
   stopped being true at S3.
5. **A peer core's installed translation root** at release: every core's cell is cleared, the boot
   root is reinstalled only locally, and the tables are freed regardless. The other half of the
   same hole is the switch path, which returns without touching the translation base when the
   incoming thread's task holds no space, so a core that ran a thread of some task and then took
   the idle thread keeps that task's root while its tables go back to the pool.
6. **The switch cells are file-scope scalars.** On armv8a the register switch is inline and under the
   lock for a voluntary switch, so the catalogue's pending-backend hazard does not apply in the form
   it was stated; what does apply is that two cores would share one cell naming the context on the
   CPU. Both cells move into the per-core block.

**Every one of these verdicts is conditional on the current thread pointer becoming per-core**, which
is one global read without the lock at every syscall entry today. That is S1's first item.

One entry was refuted and the refutation is worth keeping: the authority word is read without the
lock and is safe, not because it is a single byte but because every one of its readers passes the
CURRENT thread and no path reads a peer's. What it owes is a publication edge at spawn, which is a
property of the lock this work writes rather than of that code.

---

## 5. What this work must produce as evidence

- Four cores running threads on `qemu-arm64`, with the whole existing fleet green and the
  single-core images unmoved except where a recorded change explains it.
- A gate that reddens when a secondary core does not arrive.
- The SMP-seam signature verdict from a baseline frozen before either backend, which is the method
  the capability ABI differ used: `tests/static/smp_seam_records.txt` and its differ exist at S0.
- A configure refusal on a part that fails the predicate, with a positive control showing it fires.
- For every gap in section 4 that is fixed, a mutation that reddens exactly the arm claiming it.

---

## 6. The plan: steps and expected results

Step identifiers are local to this document. `roadmap.md` maps them to milestones.

**S0 -- the contract and the instrument.** No runtime code. This document; the SMP seam differ with
its baseline frozen now and its four members MOVED out of the entry family, so this work never
reddens the entry-and-boot-path verdict the x86_64 port closed; the configure-time refusal of N10.
*Expected:* the entry differ still passes, the SMP differ passes against its own baseline, the fleet
is unmoved.

**S1 -- per-core state, at one core.** Everything that must be keyed, landed while the count is one
so it folds to nothing and every preset proves it: the current thread pointer, the fault record, and
the two armv8a switch cells into the per-core block. Not the sleep queue and not the ready queues,
which belong to per-core scheduling and stay global under one lock. *Expected:* fleet green;
`microbit`'s arena base moves by the documented adjacency and a build catches it.

**S2 -- the second core boots and idles.** The shape the unicore A53 port used: prove the axis with
no shared-state work. PSCI on four cores from the start so that "the peer" never becomes a synonym
for one core, secondary stack and exception level and vector base, the identity read from the
hardware register, the per-core interrupt CPU interface, and the secondaries parked. *Expected:* four
cores up, core zero runs the selftest unchanged. Measured baseline: raising the core count alone is
inert today, so any change is ours.

**S3 -- the lock, the doorbell, and threads on every core.** N2 as one design; gaps 2 and 3 of
section 4; threads scheduled on any core with cross-core wake through the doorbell. *Expected:* the
selftest runs with threads distributed across four cores. This is the first step at which the lock
witnesses anything.

Four facts the GIC architecture settles, so the send is written against the specification rather
than against the emulator, and so a reader does not re-derive them:

- **The core-index mask is the right currency and it survives both controllers.** A subset of cores
  is expressible on either, so `arch_ipi_send` is written once. What may NOT appear in a shared
  interface is anything below it: a target list plus a filter is GICv2's register in disguise and a
  GICv3 backend would have to synthesise affinity fields, while affinity plus a routing mode is
  GICv3's and a GICv2 backend would have to invent an affinity space its hardware has no notion of.
- **One call is not one register write.** GICv2 reaches any mask in one `GICD_SGIR` write, its
  8-bit target list also capping that backend at eight cores architecturally. GICv3 needs one
  `ICC_SGI1R_EL1` write per affinity group and range-selector window present in the mask, so the
  send loops and how often is a property of the machine's topology.
- **The GICv2 target bit is NOT the core identity, and treating them as equal is a bet.** A GICv2
  target list names CPU INTERFACE numbers, and the architecture relates them to no processor
  identity register at all -- the specification's only discovery is that a core reading its own
  banked target register gets its own number back. So each core publishes its bit at bring-up. The
  existing hard-target of interface zero is that same bet, made where one core made it safe.
- **A group mismatch drops the interrupt silently on both**, which makes it the likeliest first-boot
  failure: the send must be the group the target is configured for, and the acknowledge and
  end-of-interrupt registers must be that group's too.

**S4 -- translation across cores.** Gaps 4 and 5; the broadcast maintenance and the instruction-side
poke that A64 genuinely owes, its instruction barrier not being broadcast; the active-core set built
as it was decided. *Expected:* processes on multiple cores, the address-space arms green at four
cores.

*Landed:* the broadcast maintenance, gap 5's invariant, the active-core set in its DERIVED form
with the elision it re-enables, and the instruction-side doorbell poke.

Three things the work settled that the plan did not anticipate.
- **The elision survives multiple cores.** It was compiled out above one core because a peer's
  translation base is not readable; with the set derived, a space no core has installed elides its
  whole seeding again, so the four-core image seeds a new space at zero maintenance exactly as the
  one-core image does.
- **An elided invalidate still owes a barrier.** The elision drops the DSBs with the TLBI, which is
  sound only where no walker exists. One `dsb ishst` per CALL is what makes the descriptors
  visible before any later activation reads them (DDI 0487 M.b, D8.17.1), and per call rather than
  per page is what keeps the elision's measurement intact.
- **The two removal paths gate the poke differently, and that is a ruling.** `arch_aspace_unmap`
  reads the execute permission out of each leaf BEFORE clearing it, because the alternative is a
  rendezvous on every data unmap and that would be the largest cost in the step. `arch_aspace_destroy`
  does not read permissions at all and gates on the peer mask alone: a space a peer still holds is
  losing its whole image, the cost is once per teardown rather than once per page, and the mask is
  empty in every case gap 5's invariant permits. Reading permissions there would mean trusting a
  walk of a tree already being dismantled, where missing one leaf silently loses the rendezvous.
- **The single-core fold needed a constant-false predicate, not a dead branch.** The execute-permission
  read has to disappear from a one-core image, and it does not do so on its own: the recursion in
  `free_subtree` defeated an earlier shape that returned the flag up the tree, because proving a
  recursive function's result constant is beyond what the compiler will do. The predicate itself is
  what folds.
- **The fault reporter needs nothing.** It puts the boot root back without telling the kernel's
  per-core cache, and a cache that OVER-reports is NOT harmless: the switch path skips the
  activate when the cell already names the incoming space, so a cell naming a space the register no
  longer holds would run a thread against the boot root. It is unreachable rather than harmless --
  that reporter never returns to a scheduler, so no switch reads the cell again. The backend's own
  record needs no special case at all, being written where the register is.

**S5 -- the RV64 backend and the SMP-seam verdict.** The hart park first, since with no firmware
every hart currently enters the reset path. Then the lock, the doorbell over the machine software
interrupt, and the genuine software rendezvous that A64 receives from hardware. *Expected:* the
differ's verdict, whatever it is, IS the step's finding, exactly as the address-space seam's was.

**S6 -- the GICv3 posture, matching the silicon target.** Off the critical path deliberately: the
doorbell's semantics are settled at S3 against one lowering, and the banked-register question the
mask triad carries is answered there too, so this step implements a settled contract rather than
carrying an open one twice. *Expected:* a second interrupt posture on `qemu-arm64`, the way the
RV64 board ships two translation postures.

**S6b -- `imx8mp-evk` as a chip port.** A second arm64 part, and the first whose interrupt
controller is a real one rather than an emulator option: the machine wires a GIC-500 and offers no
`gic-version` choice, so this step cannot precede S6. What it is FOR is the predicate: GIC version,
routing and cluster topology are properties of a part while `smp.cmake` declares them per arch, so
a second part is what moves that declaration to where the facts live. *Expected:* the predicate
declared per part with the arch keeping what is genuinely architectural, and a board that boots
four cores under the GICv3 posture.

**S7 -- AMP.** An AMP node is a single-core kernel per N6, so this step spends the doorbell and the
shared window rather than the lock. It comes after S5 so the doorbell has two backends' witness
before a second model is built on it, and after S6b only because that step settles what a part
declares. *Expected:* a node whose kernel believes itself alone, reached over the same doorbell seam
the shared kernel uses, with every index and length read from the shared window validated as another
node's writing. NOT expected: the heterogeneous case, which has no emulated vehicle -- QEMU's
`imx8mp-evk` models the A53 cluster alone and ships no Cortex-M7 companion.

**The GICv2 code is already out of the chip file, and GICv3 lands beside it rather than inside it.**
Everything that code holds except its base addresses and its timer identifier is architected rather
than per part: the register displacements, the boundary below which a distributor register is the
calling core's own bank, the acknowledge and end-of-interrupt protocol, and the identifier that
means no interrupt is pending. The chip declares which controller it has and where, because that is
where the GIC version, the routing and the cluster topology live: an ARCH may not certify them for a
part it has never seen.

**A shared interface with one consumer is shaped by that consumer, and this one is deliberately
cheap to reshape.** It is internal to `arch/arm64`, so no backend outside the family satisfies it and
nothing above the seam names it, which is what separates it from the frozen families F8 of
`design-m6-mmu.md` reasons about: those are contracts every port must meet, and this is a file
boundary. Whether the two controllers end up sharing one interface or keeping two is decided when
the second one exists, by which shape carries less duplication.

---

## 7. What this work will not witness

- **No silicon.** The A53 is an emulator only and the RV64 board has no part at all, so every claim
  here is emulator-grade until the i.MX8MP arrives.
- **AND THE ONE PART THAT COULD CARRY A SHARED-KERNEL SILICON WITNESS IS THE LX6, which this work
  does not target.** This bullet used to say the RP parts are the fleet's only real multicore
  hardware and are all in the AMP column, which stopped being true when the LX6 row was corrected.
  The LX6 satisfies four of the six properties on sourced evidence and its two open columns need one
  document, so it is the only candidate on this bench for taking the shared kernel off emulator-grade
  evidence. Nothing here schedules that: it is a whole backend, the part has no emulator, and its
  atomic is exactly the property a shared kernel rests on. Recorded so the gap has a named way out
  rather than only a date.
- **The lock has no silicon witness specifically.** An AMP port on a real dual-core part would still
  exercise the doorbell, the ring and the ordering claims; the shared kernel is what loses its
  hardware witness.
- **The fastpath ruling is unexercised.** Neither target links the fastpath, so N10's refusal is what
  carries it and no run demonstrates it.
- **The rendezvous is A64-free on the DATA side ONLY, and the acknowledgement side is software on
  every backend.** A64's coherency supplies the data half. What no interrupt controller supplies is
  the other half: neither GIC version reports to a SENDER that a target has serviced a
  software-generated interrupt. GICv2's per-source pending registers are banked to the ACCESSING
  core, so an initiator cannot read a target's state at all; GICv3's are addressable in the target's
  own redistributor frame but carry no source identity, and a cleared bit does not distinguish
  serviced from never delivered. So `arch_ipi_wait` spins on per-core acknowledgement in shared
  memory on BOTH postures, and no backend reads a controller register in it. This bullet previously
  read that the blocking path is unexercised until S5, which conflated the two halves: what defers
  is the first CALLER that needs a rendezvous rather than the mechanism, a cross-core wake needing
  no wait at all.
- **The instruction-side maintenance has a MECHANISM and a PARTIAL arm, and its architectural
  effect has none.** This bullet used to say nothing in the tree changes an executable mapping
  across cores, which had been false since S3: the app's text is mapped read-execute in every
  space, it is unmapped when the space is released, and threads run on peer cores. What the tree
  now carries is the doorbell service body's `ISB` and a send-and-wait from `arch_aspace_unmap`
  and `arch_aspace_destroy` over the peers holding the space. Three claims, three standings.
  - **That the poke is sent and answered by the right cores IS witnessed**, by a per-core count of
    doorbell services and of rendezvous initiated, asserted across a task kill, and cross-checked
    against QEMU's GIC acknowledge trace. For the poke's own arms that cross-check is
    one-directional: a peer already spinning in the acquire loop answers by POLLING and
    acknowledges nothing, so the trace can confirm an interrupt path that ran and never refute one
    that did not. The bring-up check is the exception, and it is one by construction: its first
    phase runs the peers with interrupts open while the initiator holds the lock, so the vector is
    the only thing that can answer and an acknowledgement per peer per round IS asserted. Its
    second phase asserts the other side, the peers observed spinning before the raise, where only
    the poll can answer.
  - **That the `ISB` precedes the answer IS witnessed, structurally**, by a disassembly gate over
    the service body rather than by any run.
  - **That the poke carries NO scheduling meaning is witnessed on both sides.** Structurally, by a
    second disassembly gate: the send and the rendezvous branch to neither the reschedule
    publisher nor the scheduler, the dispatch reaches the scheduler only behind the take that
    consumes the cell, and the publisher has a caller that is not a backend body. Dynamically, by
    the raise count QEMU's own GIC model reports over the bring-up check, whose sixty-four rounds
    are rendezvous and nothing else: with the publish inside the send each of the thirty-two poll
    rounds made every peer re-raise at itself, and the boot's `GICD_SGIR` writes fall from about
    180 to about 84 once it moves out. That is a count of raises rather than of scheduler entries,
    which is the honest reading: the emulator reports what the controller was asked to do, and a
    peer answering by poll acknowledges nothing.
  - **That the `ISB` has its architectural effect is NOT witnessed and cannot be here.** QEMU's TCG
    models no prefetch queue and invalidates translated blocks on a flush, so an arm shaped "the
    peer stopped executing the revoked text" passes on an image carrying no `ISB` at all. This
    rests on the specification -- DDI 0487 M.b section B2.7.4.2 for the absence of any bound on
    re-executing already-fetched instructions, the Glossary's "Context Synchronization event" for
    the fact that every way of taking one is self-executed, and section B2.2.5 step 3 for the
    requirement that each PE executing changed code run its own -- exactly as the data-cache
    seam's non-witness does, and not on a green run.
- **The AMP column stays a ruling even though S7 builds AMP.** What S7 ports is an AMP node on
  `qemu-arm64`, a part the predicate sends to the SHARED kernel. So the parts section 1 excludes
  still get no port, and whether they are worth one is still section 8's open question.

---

## 8. Deliberately NOT frozen

- **Per-core run queues and any finer locking.** The spike's stage 2, and the lock-hold shortening
  that moves the bound belongs to the IPC optimisation work.
- **Whether the MCU dual-core parts are worth an AMP port at all**, which the spike lists as open and
  which is a question about value rather than mechanism. Section 1 rules only that they do not get a
  shared kernel.
- **Whether the LX6 gets a shared kernel.** Section 1.5 makes it a candidate rather than a ruling.
  **The gate is NOT an architecture reference, which is what reading one established.** The ISA
  defines both missing primitives -- a compare-and-swap against a dedicated compare register whose
  stated primary purpose is exclusion between processors, and a privileged single-instruction
  processor identity -- and defines BOTH as configurable OPTIONS, the identity's value being wired by
  the integrator. So the ISA closes what a shared kernel would USE and cannot say whether this part
  has it: that is a per-core data book, and neither document on hand carries one. Which way it goes
  changes what the AMP silicon paths ARE, the RP parts being the only others and both excluded on
  targeting rather than on anything a document could settle.
- **What the ISA DID settle needs no further source and bears on every backend's ordering.** Memory
  ordering is core architecture on this family rather than an option, so it is present on any part;
  ordinary loads and stores carry NO inter-processor ordering at all, the model being an explicitly
  weak release consistency, and the acquire and release pair rather than the blanket memory-wait is
  the intended cross-processor tool. A rendezvous written against the blanket instruction alone would
  be reading past the specification.
- **Who may mint a cross-node endpoint or start a core.** Static in kernel init for the first AMP
  work; the general question is the capability layer's and is answered with it.
- **The AMP partition layout**, including whether the instance-keyed storage can be made separately
  protectable, which the current contiguous array does not give for free.
- **Whether a shared kernel and an AMP node can coexist on one part.** The i.MX8MP is both at once
  and nothing here decides how the two halves see each other.
