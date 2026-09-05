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
| ESP32 LX6 | yes, in internal SRAM | S32C1I, MEASURED | matrix, sources 24-27 | PRID, MEASURED | yes | matrix | shared kernel |
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

**THE TWO COLUMNS A CHIP MANUAL COULD NOT ANSWER ARE MEASURED NOW, AND BOTH ANSWER YES.** The
atomic and the per-core identity are core-architectural; the ESP32 TRM has no instruction-set
chapter and lists no architecture reference to defer to, and the ISA defines both as configurable
OPTIONS whose realisation only the part can state. So neither was settled by reading, and a probe
on silicon settled both.

What it measured, on an ESP32-D0WD-V3 revision v3.1 at 240 MHz, both cores incrementing one word
in internal SRAM a hundred thousand times each: the counter finished EXACTLY at two hundred
thousand while the primary recorded a hundred thousand retries, so the loops genuinely overlapped
and no update was lost. Losing one lowers the total and nothing can raise it, so an exact total
under witnessed contention is exclusion rather than arithmetic -- and the probe refuses its own
verdict when NEITHER core retries, which is the reading that would have been a coincidence. The
two identity registers answered `0xcdcd` and `0xabab`, so the option is realised and the
integrator wired the cores apart.

**AND `ATOMCTL` READ BACK ALL-RCW, WHICH IS THE MECHANISM AND NOT ONLY THE OUTCOME.** The ISA
leaves an inter-processor conditional store to the part precisely because it may execute locally in
DataRAM, raise a load-store error, or issue a read-conditional-write bus transaction; Special
Register 99 is what selects between them, per memory class (ISA summary 4.3.13.4 and Table 52,
p.121). The probe read `0x15`, all three fields selecting the bus transaction, which is the arm
that makes the instruction exclude between two CPUs over shared internal memory.

**AND THAT READING WAS A DRAW, WHICH IS THE HALF WORTH WRITING DOWN.** The probe never WROTE the
register, and `0x15` is not the architectural reset value: Table 190 (p.313) gives that as `0x28`,
which selects the core-local arm for both cacheable classes and an exception for bypass, so not one
class reaches the bus. The exclusion the probe witnessed is therefore an observation and not a
guarantee -- it held because something outside the image had left the register usable, and nothing
in the image asked. What makes the LX6 row's exclusion column a fact about an IMAGE rather than
about one boot of one die is that `kickos_lx6_init` now SEATS `0x15` on every core and reads it
back, REFUSING by name before that core can reach `arch_kernel_lock`. The refusal is what the
declaration rests on; the measurement is what made the declaration worth attempting.

**THE FAILURE THIS CLOSES IS SILENT AND IT IS THE ONLY KIND HERE THAT IS.** A part selecting the
core-local arm executes every instruction of `arch_kernel_lock` and excludes a core from itself and
from nobody else: the lock appears to work while excluding nothing, and no fault is raised
anywhere. `arch/xtensa/lx6/smp.cmake` states that hazard in its `exclusion` predicate and could not
check it, a predicate being a configure-time declaration; `tests/static/check_lx6_atomctl.sh` reads
the seat and the read-back out of the linked image, and the die's answer stays the board's.

**Both properties are INTEGRATION OPTIONS, so this is one die's answer**, and a record of it names
the die.

**TWO CONDITIONS RIDE WITH THIS ROW, AND EACH IS DISCHARGED WHERE IT CAN BE REFUSED RATHER THAN
DESCRIBED.** Kernel state must be PLACED in internal SRAM, which is a link-time property and never
a runtime one, so the chip owes a link that REFUSES the placement it cannot support: kernel data, a
boot stack or kernel text leaving internal SRAM each fail the link by name in
`arch/xtensa/chip/esp32/esp32.ld`. Two things about those refusals are the decision rather than the
detail. They read the SoC's own bounds and not the script's own regions, because an assert
comparing a region against itself passes wherever the region is repointed, which is the one edit it
exists to catch. And they are unconditional rather than gated on the core count, because a rule
that runs only once the backend needing it exists is a rule nothing has ever run.

The second condition is requirement 5, and it is declared at the granularity the scheduler asks it
at. The cores are interchangeable for anything a thread does and they are not identical: one 8 KB
memory answers to one core alone, and a per-CPU peripheral answers differently to each core at one
address. So the chip declares symmetry for SCHEDULING, and a caller reaching either of those two
places is outside what the declaration covers.

**THE RP PARTS CAN RUN A SHARED KERNEL AND ARE DECLINED ONE, and the ground is FIT rather than
cost.** FreeRTOS ships a dual-core RP2040 port building two kernel locks from two SIO spinlocks, so
a future reader meeting it will otherwise reopen this. What requirement 6's absence costs is not
effort: it is that the MODEL WOULD MISREPRESENT THE HARDWARE. A line "granted" to one core is
granted only because the other masks it, which the isolation principle reads as the wrong shape, a
grant that is a mask being neither the narrowest unit nor a refusal. Shipping that on a part whose
hardware makes AMP the fitting model hands a user a tool whose grants are masks in disguise, and
refusing rather than silently masking is the principle's own rule -- the same rule N10's
configure-time refusal enforces.

**The cost corroborates and does not decide.** Requirement 6's absence also buys per-core mask
reconciliation and a second lock, in SHARED kernel source that every preset compiles and every
single-core board must keep correct, for a bound of 1.31x on two boards; the byte-identical
invariant protects the image, not the source. Worth recording, because it is the half a reader can
measure -- but a fleet-wide charge is a reason to dislike the work, where misrepresenting the
hardware is a reason not to ship it.

**AND NONE OF IT TRANSFERS TO AMP, which is why these parts get one.** N6 states that the section
1 predicate does not apply to an AMP node: each node's kernel is its own, so there is no per-core
mask to reconcile and nothing stands in for absent hardware. This decline is a ruling about ONE
model on these parts and about nothing else.

**That argument covered three parts and was only ever valid for two, which is how the LX6 row's
error survived.** It turns on requirement 6's ABSENCE, and the LX6 satisfies requirement 6. ESP-IDF
shipping dual-core FreeRTOS on the ESP32 was read here as a part running a shared kernel it should
be declined; it is better read as the part meeting the predicate.

---

## 2. The freezes

A freeze is a decision this contract may not reopen without saying so; the deliberately open
questions are section 9. Per the MMU contract's rule, a freeze rests on another decision and never on
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

**A SERVER MAY BE REQUIRED TO SIT WHERE ITS LINE IS, AND A PASSER-BY MAY NOT BE MOVED.** N3 buys one
lock with a pin, so the words a logical line is gated by are image-wide, and they record ONE core's
view soundly and two cores' views not at all. Two kinds of caller reach them and the rule is not the
same for both, which is what keeps this consistent with section 8's placement being an ASK AND NEVER
A YANK.

A thread holding `CAP_WAIT` on a line is ASKING to serve it, so `irq_wait`, `irq_ack` and
`irq_discard` require it to be on the routed core: the request is admitted through
`sched_admit_mask` as a SUBSET, so a task whose grant cannot reach that core is REFUSED and never
clamped. That is an ask with a refusal, made by the thread that asked, and it needs no exception to
section 8. `CAP_SIGNAL` is deliberately outside it: `irq_notify` posts on the binding and reaches no
controller register, so its holder is not a server.

A caller that merely PASSES a line on its way to something else, such as the console handover
masking the kernel's own TX line inside `kos_console_publish`, asked for none of that. Its affinity
is its own and narrowing-only, and re-placing it as a side effect of an unrelated syscall would be a
yank. So the discipline for a passer-by is not a pin: the TOUCH is routed to the owning core rather
than the thread being routed to the line. Measured on esp32-wroom, where the debug guard named line
0x1e as routed to core 0 and touched from core 1, from `console_tx_deinit`.

**AND THE PIN COVERS A ROUTED LINE ONLY, WHICH IS MOST OF WHAT N3 BUYS AND NOT ALL OF IT.** A
logical line with no device route has no owning core: `arch_irq_line_core` answers -1,
`irq_line_op` performs the op on whatever core called, `pin_to_line_core` places nothing, and the
debug guard's refusal is silent because its subject is the routed core. On `esp32-wroom` exactly
one line is routed, the console's TX; every software-injected line, which is every selftest line,
is unrouted. So the backend's image-wide gating cells are reached from more than one core there,
and what carries them is NOT the pin.

**WHAT CARRIES THEM, PER CELL, BECAUSE THE TWO ANSWERS ARE DIFFERENT.** The pending latch is
serialised by the kernel lock: every one of its accesses is reached with that lock held, or is
performed by a doorbell service body on behalf of a core that holds it, and no ISR-context path
touches it at all, the two callers of `irq_line_op_local` both asking for a mask. The mask cell is
not: the ISR's mask is the one unlocked writer in the image. It is therefore an ATOMIC CELL, which
buys the two things a plain byte does not, one indivisible access of a stated width and a compiler
that may no longer cache the load across a branch nor sink or elide the store. Its ordering against
the waiter's unmask is carried by the kernel lock's own causal chain: the ISR masks and then calls
`sem_post`, which takes the lock, and `S32C1I` "plays the role of both acquire and release",
requiring every ordinary store to have performed before its atomic pair performs (Xtensa ISA
summary 4.3.13.5, p.122); the release is `S32RI`; the waiter takes the same lock before it
unmasks.

**WHY THE LX6 DOES NOT NEED THE RV64 BACKEND'S ATOMIC READ-MODIFY-WRITE, stated because the two
are now visibly different and the difference reads like drift.** rv64imac keeps its gating state as
a BITMASK WORD, so setting one line's bit is a read-modify-write of a word carrying every other
line's, and a peer mutating it concurrently clobbers the update; that is what `amoor.w` and
`amoand.w` are there for, and its own declaration says so. **One cell per line removes that
read-modify-write outright**: a mask is a store of 0 and an unmask a store of 1, whole values, so
there is no shared word to clobber and nothing for an atomic RMW to protect. Read out of the linked
image, every write to either cell is a `movi` followed by an `s8i`, and not one of them derives its
value from a value read. rv64's other half, the store-load fence pair between its unmask and its
inject, answers a Dekker race between those two bodies; here both of those bodies hold the kernel
lock, so that race does not arise, and importing one fence without the exactly-once take that
settles which side delivers would assert a completeness this backend does not have. Freeze N9 is
the licence rather than an excuse: the cross-core mechanism is a per-arch seam whose cheapest
correct mechanism differs per part. `tests/static/check_lx6_irq_cells.sh` asserts the no-RMW half
out of the image, because it is exactly the property an innocent edit removes with no local
symptom.

**AND ONE THING IS NOT CLOSED BY ANY OF IT, so it is recorded rather than implied.** Two stores to
one cell from two cores have no order that a primitive can give: an atomic access is indivisible
and still unordered against a peer's, and rv64's AMOs do not order its equivalent pair either. For
an unrouted line the mask-versus-unmask order therefore rests entirely on the causal chain above,
with no pin behind it. No reachable inversion of that chain was constructed, and none is claimed
impossible.

`kernel/irq/irq_route.cc` is the one place that decides, and
`tests/static/check_irq_line_op_sole.sh` refuses a kernel-layer call to `arch_irq_mask`,
`arch_irq_unmask` or `arch_irq_clear_pending` anywhere else. **The gate ships with the routing and
not after it**, because N3's discipline was correct and unenforced for the whole of this backend's
life, and that is precisely how a wrong-core touch reached an audit and stranded a board with no
message. A rule enforced by nothing is a rule someone has to remember.

**THE PRICE OF ROUTING RATHER THAN PLACING, AND THE HISTORY OF GETTING IT WRONG TWICE.** The first
estimate was new machinery: a cross-core action queue. That was wrong, and wrong because the
far-side hook was invisible until someone looked. All three shared-kernel backends already call a
kernel-layer function from their doorbell service bodies (`kickos_amp_node_service`), so a second
one is one line in each. The second estimate was that line plus a helper. That was also wrong, and
it was wrong FOR THE VERY REASON THE DESIGN IS CORRECT.

The drain must live in the doorbell SERVICE BODY, because the poll inside `arch_kernel_lock`'s
acquire loop is the only thing that answers a peer spinning for the lock an initiator holds, and an
action anywhere else deadlocks exactly as N2 warns. `klock_resched_ask`'s consumer looks like the
natural home and is not: it is drained by the dispatch ENTERING THE SCHEDULER, which the acquire
poll never does. But the service body is reachable from every chain that can spin on the kernel
lock, so every one of them now charges the deepest line-gating operation, whether or not an ask is
pending, because **A STATIC CALLGRAPH BUDGET CHARGES WHAT IS REACHABLE, NOT WHAT IS TAKEN** and a
runtime "nothing owed" test is invisible to the reader. On lx6 that is 128 bytes measured:
`KICKOS_LX6_TRAP_DEPTH` moved 496 to 608 and the smp board's idle stack 768 to 896, both refused by
a gate until they did. `arch/xtensa/lx6/include/kickos/arch/lx6_trap_stack.h` carries the resulting
32-byte margin as a constraint on future work, with the exits named. arm64 and rv64 absorbed the
same hook with no knob moved.

That is the transferable lesson rather than a local figure: anyone adding a cross-core action
anywhere near a lock path pays it, and the way to find out is to let the budget gate answer.

**One claim in this area had no writer in the other direction and was false.** The routing's own
comment said the rendezvous is never reached from interrupt context and that no such call exists;
the callgraph reader proved it reachable from the first-level dispatch through `irq_event_isr`. The
fix is two entry points rather than a reworded comment, `irq_line_op` for a caller that may need
routing and `irq_line_op_local` for one that is the routed core by construction, so it is a fact
about the graph instead of a promise about the code.

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
the RP2350 sends the SAME six words (datasheet 5.3), the ESP32 writes the entry to the APP_CPU
boot address register and clears its reset bit, and the i.MX8MP writes the companion's initial
vector table into an IOMUXC general-purpose register and sets the enable bit in the reset
controller.

**THE RP2350 CLAUSE SAID "the same plus an arch marker" AND THAT WAS FALSE.** Section 5.3 carries
the identical six-word sequence with no seventh word, and one `cmd_sequence` in the vendor SDK
serves both parts. Which architecture an RP2350 core comes up in is a boot-time property of the
part rather than a word in this handshake. The freeze's ruling is untouched and only its citation
moved; it is corrected here because the wrong reading costs a porter a word that does not exist.

**AND THE ESP32 CLAUSE NAMES TWO WRITES WHERE THE PART TAKES SIX**, established by a probe that
started the second core rather than by reading. Beyond the boot address and the reset bit, the
core is held by TWO separate software stall fields in different registers of the always-on
domain, and by a CLOCK GATE that **resets closed**. The gate is the one a porter loses a day to:
a core released from reset with its clock ungated does nothing whatever, which looks exactly like
a core that never started. The shape the freeze states is still right -- write an entry point,
release a reset -- and what it understates is how many holds a part may keep the core under.
Read the count off the port, never off this sentence.

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

### N6b. AN AMP NODE'S IMAGE IS ITS OWN, and the one-image node is the special case

N6 above admits two shapes and reads as though the one-image shape were the ordinary one. It is
not, and the parts decide it rather than taste. **The default is one image per node.** A node
sharing an image with its peers is a narrower case, available only where the cores run the same
instruction set and want the same kernel configuration.

**Two named parts settle it, and neither is reachable by a shared image.** The i.MX8MP's companion
is a Cortex-M7 beside an A53 cluster: a different instruction set, so one image is not a
preference it declines but a thing it cannot execute. The CV1800B's second C906 has no MMU where
its first has one, so even at one instruction set the two kernels are different builds, a region
model beside a translating one, with different pools and different stack sizes. A model that
cannot express either of the two parts this work names is not the default.

**FOUR THINGS FOLLOW, AND THREE OF THEM DELETE MACHINERY RATHER THAN ADDING IT.**

- **The node's identity is a BUILD CONSTANT.** An own-image node reads no core register to learn
  which node it is; its image is built as that node. This is not only simpler, it is the only
  shape that works on the CV1800B, where `mhartid` is hardwired to zero in the openc906 release
  and customised per instance by the integrator, so a node keying on its core identity has no
  register to stand on.
- **`KICKOS_MULTI_INSTANCE` leaves the AMP path.** Instance keying exists so several kernels can
  share one address space; own-image nodes share none. With it goes the homogeneous holder, whose
  cost is that `InstanceLocal<T>` gives every node a `T` of the same size: a node provisioned for
  work it does not do, in `.bss` that the part's other node pays for.
- **USERSPACE NEEDS NO NEW MECHANISM AT ALL, and that is the strongest evidence for this shape.**
  A second kernel needs a second root, and under its own image that root is the ordinary
  `KICKOS_INIT_PROVIDER`, `KICKOS_SERVICE_LIST` and application entry every image already selects.
  Under one image the same requirement is a per-instance selection of all three, invented only to
  keep the binary count at one.
- **What it costs is a PLACEMENT CONTRACT.** The shared window was one image's `.bss` object; two
  images must agree on a region at a fixed address, outside either one's own allocations. That is
  the AMP partition layout section 9 leaves open, and meeting it deliberately is better than
  inheriting it from a linker that was never asked.
- **AND A DOORBELL MAP, WHICH THE TREE DOES NOT HAVE, SO THE POSTURE IS REFUSED AT CONFIGURE.**
  `amp::send` rings its peer with `arch_ipi_send(1u << to)`, spending a NODE index as a hardware
  CORE mask. That holds only under the shared image, where a node's identity IS the core register;
  under one image per node the two are unrelated, and nothing in the tree maps a node onto the core
  carrying it, so the doorbell would ring the wrong core and answer OK.
  `KICKOS_AMP_POSTURE_OWN_IMAGE` is therefore offered by Kconfig and REFUSED by `CMakeLists.txt`,
  the refusal naming the missing map rather than the broken line. This sits beside the placement
  contract above and not under it: both are the partition layout section 9 leaves open, and both
  are what an own-image part owes before its first boot. No board in the tree selects the posture,
  so the refusal costs nothing today.

**THE CONTRACT COVERS EVERY CELL TWO NODES BOTH WRITE, WHICH IS MORE THAN THE WINDOW, and getting
this wrong presents as a HANG rather than as a build error.** A doorbell backend keeps its
rendezvous cells -- the per-pair request and answer counters and the served count -- as ordinary
statics indexed by the writing core. Under one image that is one array both cores address. Under
one image per node it is two private copies: each core bumps and reads its own, the initiator's
wait never observes an answer, and the spin runs to its bound and terminates the image. Nothing in
either build can see it, because each side compiles and links perfectly. So the region two link
scripts agree on holds the window AND the rendezvous cells, and a backend that leaves either
behind is broken in the one way no gate on a single image can report.

**DEPLOYMENT IS A MERGE AND NOT A SECOND FLASH.** Two images are built and combined into one
programmable artefact. The launch freeze already permits it: N5's handshake hands the secondary an
arbitrary vector table, stack pointer and entry, so the started core may enter a wholly separate
image resident in the same flash. On a part whose companion runs from tightly-coupled memory the
LOAD half of N5 is what places it.

**THE SHAPE WAS CHECKED AGAINST A WORKLOAD RATHER THAN DERIVED FROM THE PARTS ALONE**, and the
workload is recorded because it is what every clause above was tested on. Fieldbus communication
owns one core with its controller and its bus; motor control owns another with its converter and
its own bus; a logging store on a third device is owned by the first node and written by both.
Each node's peripherals are granted by its own image, which is the partitioning falling out of the
model rather than being imposed on it -- and on a part failing requirement 6 it is also why the
missing per-line targeting stops mattering, each core masking what it does not own. The shared
store is reached as a SERVICE per N6d: the owning node publishes a port, the other holds a far
endpoint. The logging path is a SEND under N6e, because a control loop may not wait on the other
node's scheduler, and the record's size against the ring's depth is what decides how many ticks a
transfer spans.

### N6c. THE DERIVATION OF `KICKOS_AMP_NODE` CONTRADICTS N6b AND IS A DEFECT

Measured against the tree rather than argued: `KICKOS_AMP_NODE` is derived from
`KICKOS_NUM_CORES > 1` together with the model, and the model choice is itself unavailable at one
core. So an own-image AMP node -- whose image drives exactly one core, which is N6's own
description of it -- resolves the macro to 0, compiles the shared window to nothing, and cannot
join the ring at all. **The mechanism is one-image-only by construction, and the parts it excludes
are precisely the two the work targets.**

What it must become is a posture a board STATES, carrying the node's identity with it, rather than
an inference from a core count. The count then says what it says everywhere else, how many cores
this image drives, and an own-image node says one.

### N6d. A NAME CROSSES BETWEEN KERNELS, NEVER A CAPABILITY

A capability entry names an object in the kernel that issued it, by pool slot or by generational
handle. Another kernel's pools are its own, so the bit pattern carries no meaning across the
boundary, and the delegation right is about handing a capability into a CHILD table in one kernel
rather than about a wire format. **There is no representation of a capability that a second kernel
could accept, and the absence is the boundary working.**

What crosses is a NAME the far kernel can interpret: a node and a port, which are configuration on
both sides rather than a pointer on either. The receiving kernel then mints a capability of its
own for that name, under its own policy. **The far kernel MINTS; the near one may only ask**,
because authority comes from the table's owner and no table spans two kernels.

Two consequences. Only globally nameable things can cross, so an endpoint can and a frame run or an
address space cannot, those naming memory inside one node's own partition. And a service SHARED
across nodes -- one node's driver serving another's threads -- is exactly this and needs nothing
further: the serving node publishes a port, the calling node holds a far endpoint for it, and the
receiver is an ordinary thread parked in its own kernel.

### N6e. PRIORITY DOES NOT CROSS, AND THE TWO SCALES ARE NOT COMPARABLE

Donation across nodes has no meaning: there is no thread on the far side to raise and no seam by
which one scheduler reaches another. Stated rather than solved.

**The sharper half is that the two priority scales are unrelated number lines.** Under one kernel a
priority orders every thread; under two kernels a number on one node and the same number on the
other order nothing between them. So making a cross-node service meet a deadline is a SYSTEM
INTEGRATION act performed across two configurations, and neither kernel can check the result --
which puts it in the same column as every other claim no green run makes.

The design rule that follows, for a caller with a deadline: a cross-node CALL blocks at the far
node's scheduling discretion, so work a real-time loop must not wait on is published with a send
and drained by the far node at its own priority.

**WHAT THE CALLER DOES WITH A REFUSAL IS THE APPLICATION'S AND NOT THIS CONTRACT'S, which is the
microkernel posture applied to back-pressure rather than a preference.** Retry, drop and buffer
are POLICY: how stale a dropped record may be, and how much may be held back for a later tick,
are properties of the workload and of nothing else. So the kernel supplies the mechanism -- an
immediate refusal, named and counted, spending no time -- and supplies no queue, no retry and no
deadline of its own behind it. A loop can then treat a publication as a budgeted attempt. **A
kernel that buffered on the caller's behalf would be choosing the staleness policy for every
workload at once**, which is the same error as a kernel-side driver, one layer out.

**RING DEPTH IS THEREFORE A PARTITION-SIZING INPUT rather than an internal constant.** One ring
per ordered pair holds a fixed number of slots at the local message bound, so a producer offering
more than that per drain is refused by construction and must chunk across ticks. A workload that
moves kilobytes at low priority and one that exchanges single records have different right
answers, and a partition that cannot state its own depth forces both to live at one.

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

**THE FASTPATH SENTENCE DESCRIBED A GUARD THE TREE DID NOT HAVE, and it does now.** It read that
the register fastpath "bails out on a remote partner", present tense, when nothing in
`kernel/syscall/syscall_ipc_fast.cc` tested locality at all: what it had was a `recv_holders == 0`
test, which is a DEAD-ENDPOINT test and refuses a far endpoint only incidentally, because one
carries no local receiver. Incidental is not a guard, and a freeze asserting one it does not have
cannot be checked against the tree. The ruling is untouched and only its standing moves.

What the fastpath now carries is a locality test of its own: a far endpoint's partner holds no
register of this core's, and a reply arrives from a doorbell rather than from a thread the path
could hand off to. It is a FALL-THROUGH and never an errno, in that file's own terms, so
`endpoint_call` produces the answer and the guard stays invisible above the syscall. It folds to
nothing wherever the posture makes `endpoint_is_far` constant, which is every part that links the
fastpath today, so section 7's "the fastpath ruling is unexercised" stays true and this adds no
run that would make it false.

**AND IT SITS AHEAD OF THE DEAD-ENDPOINT TEST, WHICH IS WHAT MAKES IT A TEST AT ALL.** Placed
after, it is unreachable BY CONSTRUCTION rather than merely unexercised: a far endpoint is minted
with no local receiver and only the wait right raises that count, which a signal-only mint can
never hold, so the dead-endpoint refusal answers first in every case forever. **This sentence
described such a guard when it was first written, which is the SECOND time this freeze has
misdescribed the fastpath in one milestone** -- the first was claiming a guard the tree did not
have at all. The lesson is the one the review that caught it names: a refusal that happens to be
reached by another clause is not the same object as a refusal that states its own reason, and only
the ordering tells them apart.

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

- **No silicon, and the three shared-kernel backends do not share one standing.** The A53 is an
  emulator only and the RV64 board has no part at all, so those two are emulator-grade until the
  i.MX8MP arrives.
- **THE LX6 IS STILL THE ONLY BACKEND HERE THAT NO MODEL RUNS, AND IT IS NOW THE ONLY ONE A
  MACHINE HAS RUN.** Upstream QEMU models no ESP32, so this image is executed by nothing in CI and
  by nothing on this bench without a board on the bus: the other two backends are witnessed by a
  model of the architecture and this one by no model at all. What changed is the other side of it.
  The bring-up check is no longer compiled into every build and executed by none of them: it has
  been executed on silicon, answering all 0x40 rounds, on an ESP32-WROOM-32 module. **A green CI
  run stands behind none of that** -- what stands behind it is a board, and a serial capture is
  dated evidence rather than a standing fact.
- **AND THE PART THAT CAN CARRY A SHARED-KERNEL SILICON WITNESS IS THAT SAME LX6, so the two
  standings meet on one board. THAT WITNESS NOW EXISTS.** Its two open columns were measured on
  silicon and both answer yes, so the part satisfies all six on evidence rather than four, and a
  backend rests on them. It remains a BENCH OPERATION AND NOT A GATE: no run in this tree produces
  it and no green run stands in for it. **What the shared kernel now has behind it is more than
  the primitive**: two cores in one scheduler completing 125 selftest arms with a userspace UART
  driver owning the console, the doorbell and the kernel lock answering their bring-up check on the
  die, and a device line's gating routed to its owning core. What it does not have is a SECOND
  die, or any of the specific non-witnesses below.
- **The lock has no silicon witness specifically.** An AMP port on a real dual-core part would still
  exercise the doorbell, the ring and the ordering claims; the shared kernel is what loses its
  hardware witness.
- **THE SECOND CPU'S RELEASE HAS A DOCUMENTED HALF AND AN INFERRED HALF, and the inferred half is
  the one that makes it run.** Six writes start the LX6's second CPU. The manual states the value
  that STALLS it, split across two RTC_CNTL fields, and states nowhere the value that releases it;
  clearing both fields is taken from their reset value and from a probe that started the core. So
  that half of the sequence rests on a measurement precisely where the page is silent, and a
  reader looking for its authority will not find one. What the image adds beside it is a refusal
  rather than a claim: the primary waits a bounded interval for the core to publish its own
  arrival and terminates by name if it does not, because released is not arrived.
- **AN UNROUTED LINE'S MASK ORDERING IS ARGUED AND NOT WITNESSED, and no run can witness it.**
  N3 records that the pin covers a routed line only, that the gating cells are atomic, and that
  the order between the ISR's mask and the waiter's unmask rests on the kernel lock's
  `S32C1I`/`S32RI` chain. Two stores to one cell from two cores admit no primitive that orders
  them, so what stands behind that order is the chain and the ISA's wording, not an arm. **The
  archived boots exercise those cells through a real driver and through the widened park window,
  which is evidence that the cells WORK and not evidence about the ordering**: the interleaving
  that would expose it needs the ISR's mask to land after an unmask that is logically later, and
  nothing in this tree can schedule that on purpose. Same standing as the lost-edge argument
  above, and the same reason: a probabilistic soak here would read exactly like a broken one.

- **THE DOORBELL'S LOST-EDGE ARGUMENT IS WRITTEN AND NOT EXERCISED.** No hardware clears an
  inter-CPU trigger on this part, so a sender's set can be erased by a receiver's clear. What
  bounds that at one spurious entry rather than a lost request is an ordering discipline on both
  sides plus the input being LEVEL, so a set landing after a clear re-asserts and the receiver
  enters again. No arm reaches that interleaving and none could be built from an honest sender, so
  this is a proof about the mechanism and not an observation of it.
- **THE SINGLE-LOCK RULING IS CHECKED IN A DEBUG BUILD, AND A DEBUG BUILD HAS NOW RUN IT.** N3 buys one
  lock with interrupt affinity, and this backend makes the pin structural rather than
  conventional: binding a device interrupt takes the core that will take it, with no default, so a
  route cannot silently mean the primary. **The debug build ran, and the discipline did NOT hold**:
  the guard fired on the first board, naming line 0x1e as routed to core 0 and touched from core 1,
  and the caller was the kernel's own console handover inside `kos_console_publish`. That is what
  the archive's boots 2, 3 and 9 through 12 record, and it is why the other direction is no longer
  a caller's discipline at all: `kernel/irq/irq_route.cc` routes the touch to the owning core, its
  per-line cells give each line a single writer, and two gates hold both
  (`check_irq_line_op_sole.sh`, `check_route_service_order.sh`). N3 in section 2 carries the
  server-versus-passer-by split that resulted.
  **What no run still says** is whether that holds under a caller nobody has written yet: the
  routing is a shape rather than a rule, but `arch_irq_inject` remains a cross-core writer of one
  line's cell, gated to selftest and refused by name in a debug build rather than made impossible.
  A release image still compiles the guard to nothing, which stays the deliberate half of the
  trade.
- **THIS BACKEND'S RENDEZVOUS COUNTER IS STRUCTURALLY ZERO AND IS NOT AN ABSENCE OF SENDS.** There
  is no translation to invalidate and no cache over the memory both cores fetch from, so nothing
  pairs a send with a wait here at all. The counter reads zero on a correct image, which means a
  reader comparing it against a backend that does pair them is comparing two different questions.
- **The fastpath ruling is unexercised.** Neither target links the fastpath, so N10's refusal is what
  carries it and no run demonstrates it. **The locality guard N7 asserts now EXISTS in that file
  and this bullet is unmoved by it**: the guard folds to nothing on every part that links the
  fastpath, so what a green run there witnesses is that it compiles away, not that it fires.
- **A FAR CALL IS WITNESSED END TO END, AND THE FAR SIDE IS NOT A KERNEL.** On `qemu-arm64-amp` a
  call on a far endpoint crosses to node 1, is echoed by that node's service body carrying the
  token it was handed, and comes back through node 0's own doorbell service to wake the parked
  caller. That is a real witness of the token, the ring, the five validation clauses and the
  wake. What it is NOT is a far side that is a receiving THREAD in another kernel: the peers here
  run no scheduler, so the receiving half of N6d's shared service is still unwitnessed and needs
  the two-image posture and a part that has one.
- **THE FAR-ENDPOINT SYSCALL HAS NO USER-SIDE CALLER ON ANY BOARD IN THE TREE.**
  `KOS_SYS_AMP_ENDPOINT_CREATE` is privileged per N8, and root is unprivileged from its first
  instruction, so what a run witnesses of it is its REFUSAL and nothing else. The arms that
  exercise a far endpoint reach the same mint body through the probe scaffolding instead. **That
  scaffolding is gated to root's TASK**, so the other tasks of a selftest image reach neither the
  mint nor the reply forge; what that narrows is who can cross a node on such an image, and it
  does not answer who SHOULD be able to mint one. Who may mint one is section 9's open question,
  and this is what that question costs today.
- **THE MASKED WINDOW A FAR PUBLISH HOLDS IS UNMEASURED, AND THERE IS NO FIGURE TO REPORT.** A far
  send copies TWICE with this core's interrupts masked: user memory into a `KOS_EP_MSG_MAX` kernel
  stage on the syscall stack, and that stage into the peer's ring slot, so up to 512 bytes move
  inside one `IrqLock`. What it costs is not known, and the reason is not that nobody looked.
  `bench_cyccnt` has a source for RISC-V, RX, Xtensa and armv7m and returns 0 on every other arch,
  so on the ONE board carrying this posture, `qemu-arm64-amp`, every phase accumulator and
  `bench_irq_masked_once` alike read zero; a wall-clock figure taken around the syscall on a
  host-scheduled vCPU would measure the emulator. An A64 source is arch work rather than an
  instrumentation switch: `CNTVCT_EL0` counts system-counter ticks and not cycles, and
  `PMCCNTR_EL0` needs a PMU no image here programs. The order is not negotiable either way:
  instrument before replacing, because a reserve/commit API is safe only if a failed user copy
  cannot leave the ring a half-written slot, and that is a harder contract than the copy it
  removes. So this is a recorded debt with an explicit absence of a number, not an accepted cost.
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
  still get no port, and whether they are worth one is still section 9's open question.

---

## 8. Thread placement: the grant, the mask, and the default core set

### The scheduling grant

A task carries a SCHEDULING GRANT: a priority ceiling and a core set, both members of `struct Task`
(`kernel/include/kickos/task.h`). It is made at task creation and it only ever narrows.
`task_create_call` seats both halves from the CREATOR's own task, inherited whole, and
`kos_task_sched_grant` narrows them afterwards; a request wider than the grant it is narrowing is
refused with `-KOS_EPERM` and never clamped. The narrowing is checked against the CALLER's grant as
well as the target's, because **a creator cannot give what it does not hold**, and a task that
defaulted to the whole machine would make creating a group a way to widen a grant.

**Inside its grant a task plays freely with its own scheduling and consults no capability.** There
is no capability for pinning, no authority bit for choosing a priority, and no per-call lookup: the
grant IS the authority, and it was checked at the moment it was given. That is why the grant is a
pair of scalars beside the task's domain pointer rather than an object in a table. A capability
consulted on every placement call would pay, on the frequent side, for a question already settled
on the rare one.

Both halves are read through accessors that are TOTAL over an absent or zero-initialised task: a
stored 0 reads back as the machine's full width. That is what lets idle, root and every implicit
task answer without an initialiser, and it is what keeps every `Task` member zero-initialised, which
is a hard requirement of its own (a non-zero initialiser anywhere in `Kernel` moves the whole
constinit instance out of `.bss`).

A grant may not narrow under a live member: `kos_task_sched_grant` answers `-KOS_EBUSY` once the
task has one. `Thread::affinity` is a subset of the task's core set and nothing re-derives it, so a
narrowing under a member would strand that member where it already runs. Refusing while the group is
empty is what makes the thread-side invariant hold with no re-scan and no fixup pass.

### One mask, no flag

A thread carries one core mask, `Thread::affinity`, and its default is its task's core set
less the isolated cores. **A
single-bit mask is a pin and a multi-bit mask is affinity, so there is deliberately no boolean
beside it saying which.** A flag would be a second truth about the same fact: two fields to write,
two to read, and a disagreement between them that no code could interpret.

`kos::thread::pin` and `kos::thread::unpin` (`user/include/kickos/kos.h`) are SPELLINGS of
`kos_thread_set_affinity`, not operations of their own. Every NONZERO set is
`requested & task_core_set(task)`, and that intersection is what makes the operation total: it
cannot produce a set outside the grant. **Unpin is `set_affinity(0)`**, and a zero mask is resolved
to `task_default_cores` at the syscall boundary before that intersection runs, which is the same
resolution the spawn boundary makes of a zero `kos_thread_params::core_mask`. There is exactly one
authority for the default set and both paths call it.

Unpin is `set_affinity(0)` and not `set_affinity(~0u)` because the two differ on an image that
isolates a core: all ones is an ordinary request for the WHOLE grant, isolated cores included, so
spelling unpin with it would let a thread that had never named an isolated core drift onto one at
its first unpin. Userspace still restores the default without learning what it is; the word it
passes is 0 rather than all ones.

The invariant on `affinity` is that it is never empty and always a subset of the task's core set.
It is seated at create from `task_default_cores`, restored to it by a zero mask, and admitted
against the task's set on every later write. **A NONZERO `ThreadAttr::core_mask` HAS ALREADY BEEN
ADMITTED WHEN `thread_create` SEES IT**: the spawn boundary ran it through `sched_admit_mask`
against this same task's core set and put the admitted set into the attr, so the store at create
is verbatim by contract rather than unchecked, and a `KICKOS_DEBUG` build re-asserts the subset
where the store happens. A stored 0 is therefore not a thread that may run nowhere; it is a slot
no thread occupies.

A thread may place any thread of its OWN TASK unprivileged, itself included where it holds its own
handle. **A TASK IS THE SCHEDULING DOMAIN.** Its threads share one grant, one priority ceiling and
one core set, and placing each other inside that grant is what a task is for rather than an
escalation within it: no such placement can raise the ceiling, widen the core set, or reach a
thread of another task. Direction therefore decides nothing, and a child placing its parent is as
ordinary as the reverse.

Reaching into ANOTHER task is privileged today and is the one remnant this work leaves to the
capability layer, listed in section 9; answering it here would decide the general case through a
special case, exactly as N8 says of the crossing.

### The pick rule

`sched_placeable_on` (`kernel/include/kickos/sched.h`) is the placement rule and the only one:

```c
    inline bool sched_placeable_on(Thread const* t, uint32_t core)
    {
        return (t->affinity & (1u << core)) != 0;
    }
```

It reads `t->affinity` alone and never re-reads the task's core set, because the mask already
carries every narrowing that was ever applied to the thread. **It is the mask test and nothing
else**, which is what the next two subsections are about: what stops an unpinned thread reaching an
isolated core is the mask it was given, not a clause here.

The rule has exactly two consumers, and both call it rather than re-deriving it: `available_to` in
`kernel/sched/policy_fifo_rr.cc`, which is what `pick_next` scans the ready lists with, and
`poke_peers_below` in `kernel/sched/sched.cc`, which narrows a cross-core ask by it, so a core the
thread may not run on is never woken to pick the thread it is already running.

**THIS IS ANTI-WORK-CONSERVING BY CONSTRUCTION: a runnable thread waits while a core it may not run
on idles.** That is the feature and not a price paid for it. A latency-critical thread pinned alone
onto an isolated core is worth having exactly because nothing ARRIVES there by default and only a
mask that names the core reaches it, and a policy that borrowed the core whenever it looked free
would be selling that guarantee back. What the guarantee does NOT say is that nothing else will
ever run there: an explicit mask naming the isolated core beside an ordinary one puts the thread in
that core's picker like any other, which is the opt-in and not a leak.

### Migration rides N4's mechanism and grows no second one

A thread EXECUTING on a core its new mask excludes is not yanked off it. It is made INELIGIBLE
there, and that core is asked to reschedule; that core's own scheduler pass is the only thing that
moves it. `sched::set_affinity` writes the mask and then splits three ways:

- a READY thread is already eligible elsewhere at the next pick, so the ask exists only so a core
  that could take it now LOOKS, instead of waiting for its next natural switch. A BLOCKED,
  INACTIVE or EXITED thread is on no ready list any core reads, so it gets no ask: the cores that
  could one day take it are woken by whatever makes it READY;
- a RUNNING thread the new mask still admits KEEPS the core it is on, placement saying where a
  thread MAY run and never where it runs best;
- a RUNNING thread the new mask excludes gets `kickos::klock_resched_ask` against the core running
  it.

That last path is N4 unchanged: **the state is published against the target and then the raise
goes out**, never a raise carrying scheduling meaning of its own. Two details the code carries are
worth naming, because neither follows from the rule.

**`klock_resched_ask` strips the caller's own bit from what it PUBLISHES** (`kernel/sync/klock.cc`),
so a core can never owe itself a reschedule. Self-migration therefore cannot travel through the ask
at all, and `set_affinity` takes its own scheduler pass directly, calling `reschedule()` when the
running thread it is re-masking is on the calling core.

**`switch_book` is where a re-masked thread becomes available to its new cores.** Until the store
that moves it out of RUNNING it IS running, and every peer's `pick_next` refuses a thread another
core is running, so a poke sent any earlier is consumed against a thread nobody could have taken.
`switch_book` therefore tests placement once on the outgoing thread and, when that test fails,
issues the poke on the far side of the store that makes the thread takeable. One mask test on the
switch path, and the call it guards is reached only by an actual migration.

`available_to` is the other half of the same seam: for the thread a core is CURRENTLY running it
returns the placement test rather than a bare true. That is what stops a core re-picking the thread
it must give up, and it is what makes `reschedule()` the thing that moves a thread rather than any
yank.

### Placement rests on every kernel core taking its own scheduler passes

A re-placement moves nothing by itself. The mask is written, the core running the thread is ASKED,
and that core's own scheduler pass is what moves it. So the whole of placement is alive only while
every kernel core actually takes scheduler passes, which on a shared kernel means every core arming
its own comparator and preempting on it. **A backend whose secondary never arms one satisfies every
line of this section and still strands what runs there**: the call returns 0, the mask is stored,
every invariant holds, and a thread RUNNING on that core stays on it for the life of the image.
The failure is silent by construction, because a placement is an ask and never a yank.

That is a property of the MACHINE and not of this section, so an image states it rather than
assuming it. `KOS_SCHED_OP_PREEMPTED` answers one bit per core whose own slice timer has taken a
thread off it. It is the one scheduling probe that is machine-wide rather than a read of the
caller's own state, and it has to be: what a fresh backend leaves open is whether a SECONDARY
preempts at all, and nothing a thread can read of itself on the boot core says that.

**A set bit is a conjunction and the exclusions are the content.** It says that core's comparator
fired AND the scheduler put a different thread on it. A cross-core reschedule, a device wake, and a
slice expiry that re-picked the same thread each set nothing, because none of the three is evidence
that this core can take a thread off itself on its own clock. The answer is monotonic and
machine-wide, so a caller reads a FLOOR of what has ever happened and never a sample of what is
happening.

**It is one cell per core and never one shared mask**, which is the same rule N9 states one level
down. A shared mask is a read-modify-write from several cores, and a lost update there erases a bit
that may never be set again: a core preempting once and its only evidence going with it. One writer
per cell has no such failure.

### Two kernel cores is the narrow case, and the boot core is a destination there

On a part with two kernel cores the boot core is the only core a thread on the other one can be
moved ONTO. Nothing in the pick rule or in admission distinguishes it, and nothing should: it is an
ordinary scheduling core, and the single thing that treats it apart is the configuration refusal
that keeps it out of the isolated set, which exists so the default core set is never empty. So a
placement claim on such a part is a claim about the boot core carrying an ordinary thread, and a
part that could not do that would be a part on which the default core set means nothing.

**Observing that costs an observer that cannot be root, and the reason is the ABI rather than the
scheduler.** A thread above the observer's priority spinning on the observer's own core never gives
it back, so the observer must be somewhere else, and no call answers "which thread am I": a
thread's handles are the ones it was handed, and nothing hands a thread its own. Root therefore
cannot place root. What closes it needs no new authority, because a task is one scheduling domain:
a spawner places its child, so an observer pinned off the core under test is an ordinary child of
the thread that wants the observation, and the placement is one the grant already permits.

### Isolation shapes the default, not the admission

`KICKOS_ISOLATED_CORES` is subtracted from the DEFAULT core set and from nothing else.
`task_default_cores` (`kernel/task/task.cc`) is the task's grant less the isolated cores, and it is
what a thread takes when it names no core: at `thread_create` with an empty `ThreadAttr::core_mask`,
at the spawn boundary where `kos_thread_params::core_mask` is zero, and at
`kos_thread_set_affinity` where the requested mask is zero, which is how `unpin` is spelled. Those
three are ONE authority called from three places, not three computations of the same set.

**THE GUARANTEE, EXACTLY: nothing arrives on an isolated core by default, and only an explicit mask
reaches one.** A thread that never names a core never runs on an isolated core, whatever sequence
of unpins it goes through. It is NOT the stronger claim that nothing else will ever be placed
there. A mask naming an isolated core beside ordinary ones is admitted, and that core's picker then
takes the thread exactly as an ordinary core's would; a caller that asks for that has asked to
share the core.

That distinction is where the Linux analogy stops, and it is worth naming because it was got wrong
once. Under `isolcpus` a task may name an isolated CPU in its affinity mask freely and still not be
migrated onto it, because `isolcpus` removes the CPU from the load balancer's domains and the mask
is not what moves a task there. **KickOS has no balancer.** Each core's picker takes any runnable
thread whose mask includes that core, so naming IS being picked and the two Linux behaviours
collapse into one. The default set is therefore the ONLY thing between an ordinary thread and an
isolated core, which is why `unpin` restores the default and not the grant.

**An explicit mask names isolated cores freely** -- one, several, or only isolated ones. A grant of
{2,3} on a four-core image is a reserved pool, which is the normal way to hand a real-time workload
more than one core, and `kos_thread_set_affinity` over the same pair is placeable on both.

A grant naming nothing BUT isolated cores answers itself: `task_default_cores` subtracts and finds
nothing left, so it returns the grant. Its unpinned members are pinned by construction, which is
what the operator asked for by granting that set.

### One admission

`sched_admit_mask` (`kernel/include/kickos/sched.h`) is the single gate every core mask passes
through before it is stored anywhere: a spawn's `kos_thread_params::core_mask`, a
`kos_thread_set_affinity`, and both halves of `kos_task_sched_grant` (the caller's grant in
`syscall_thread.cc`, the task's own in `task_sched_narrow`). It applies two clauses in order.

It takes a REAL mask, empty being malformed, and a zero word means something different at each ABI
that carries one, so each entry resolves its own before calling: the spawn boundary and
`kos_thread_set_affinity` both resolve 0 to `task_default_cores`, and `kos_task_sched_grant` reads
it as "leave this half alone".

**The machine, then the grant.** A request is intersected with
`KICKOS_CORE_SET_ALL` first, so a bit naming a core this kernel does not schedule is DROPPED rather
than refused; that is what keeps all ones an ordinary request. A NONZERO mask naming no core this
kernel schedules at all is `-KOS_EINVAL`, a request no grant could satisfy. Then the grant bounds
it, in one of two disciplines the caller states:

- a THREAD's affinity is a set of ACCEPTABLE cores, so the grant INTERSECTS it and an empty
  intersection is `-KOS_EPERM`;
- a TASK's grant is an AUTHORITY, so it narrows only: a request reaching past it is refused and
  never clamped.

**There is no third clause, and none is reachable.** What a mask surviving both is worth asking is
whether the pick rule can seat a thread on it, and it always can: the survivor is non-empty and is a
subset of `KICKOS_CORE_SET_ALL`, whose bits are exactly the cores `pick_next` scans, so some core in
the scan takes it. A clause here would guard a case the two above cannot produce.

### The configuration refusal beside it

**Core 0 may never be isolated.** `kickos_isolated_cores_check` (`cmake/isolated_cores.cmake`)
refuses bit 0. Core 0 is the boot core, and it is what guarantees the kernel, root and every task
holding a default grant have somewhere to run; an image that isolates it boots and then schedules
nothing. On a two-core part this leaves core 1 as the only isolatable core, which is the intended
shape: everything unpinned runs on the boot core and a latency-critical thread is pinned alone onto
the isolated one. The same function refuses an isolation mask naming a core this kernel does not
schedule.

**It is the one refusal isolation needs, and it follows from the default rather than standing
beside it.** The default core set is the machine less the isolated cores; holding bit 0 out of the
mask is what keeps that set non-empty at any core count, so a board can never be configured into an
image whose unpinned threads have nowhere to go. It turns a silent hang into a NAMED REFUSAL at the
moment the mistake is committed, which is when the image is configured. A board that boots and does
nothing is the most expensive shape a configuration error can take.

The core-0 check lives in a CMake function rather than inline in the root lists file so that
`tests/static/check_isolated_cores.sh` can drive the same authority the build drives, over synthetic
values, with a control beside every refusal. An inline `FATAL_ERROR` is reachable only by
configuring a whole tree that actually fails, which is one arm and no controls.

### The errno split

`-KOS_EPERM` and `-KOS_EINVAL` answer different questions here, and collapsing them would lose the
answer.

- **`-KOS_EPERM` means the caller's GRANT is too narrow.** The mask names real cores and the request
  is expressible; this caller may not have it. That is a CONFIGURATION answer: widen the grant, or
  ask from a thread that holds a wider one.
- **`-KOS_EINVAL` means the request could never be satisfied by ANY grant.** A NONZERO mask that
  meets `KICKOS_CORE_SET_ALL` nowhere names no core this kernel schedules, and no grant on any
  board could contain it. A mask of zero is not this: it is the ask for the default set.

A mask is a SET OF ACCEPTABLE CORES, so it is intersected with the machine before it is weighed
against the grant, and a bit naming no core costs nothing beside one that does. **That is what
keeps all ones an ordinary request rather than a magic value**, so a caller wanting the whole
grant, isolated cores included, can spell it. A mask refused for merely CONTAINING an unschedulable
bit would refuse that request on every board.

Under one code a too-narrow grant becomes indistinguishable from a typo in a mask literal, and the
reader debugging it widens the wrong thing: adds a core to a grant to fix a shifted constant, or
edits a constant to fix an access decision. The split costs one comparison at the syscall boundary,
and it is made at every entry that takes a mask: the spawn boundary, `kos_thread_set_affinity` and
`kos_task_sched_grant` all range-check before they authority-check.

### What folds and what does not

The placement half is entirely behind `#if KICKOS_KERNEL_CORES > 1` and contributes NOTHING to a
single-core image. `Task::core_set`, `Thread::affinity`, `sched_placeable_on`, `sched::set_affinity`,
`sched::add_idle`, `available_to`, `poke_peers_below` and `switch_book`'s mask test are all absent
from such a build, and `kos_thread_set_affinity` is `-KOS_ENOSYS`.

**The PRIORITY CEILING does not fold, and it is not meant to.** It is unconditional where the core
set is conditional, because the hole it closes is not an SMP hole: unbounded priority let any
unprivileged thread take the top of the run queue and starve the system, on every board including
every single-core one. The ceiling closes a liability that predates multicore and it closes it for
every image. Reading it as an SMP feature would be reading it as optional.

One thing the ceiling deliberately does NOT bound is a priority INHERITANCE boost. `sched::set_prio`
is the sole writer of an effective priority and is not ceiling-checked, because every caller of it
is the kernel's own, inheritance and the console-publish temporary, and not a task asking for
priority. The ceiling is enforced where a task ASKS, at the spawn boundary.

### Idle needs no exemption

An isolated core still needs something to run when nothing is pinned to it, and the obvious way to
give it one is an exemption clause in the placement rule for idle threads. There is none, and two
separate things are why.

What GUARANTEES the core has an idle thread is `policy_pick_next`'s last line: a scan that admits
nothing falls through to `kernel().idle[core]` unconditionally, and that predates this work. What
`sched::add_idle` adds is the mask, EXACTLY that core's own bit, which is what keeps idle inside the
`affinity` invariant every reader of the field relies on and what stops a peer's scan taking it.
`sched::add` seats the boot core's idle by the same rule, so the two paths agree rather than one of
them being the exception.

**This is what kept the placement rule TOTAL instead of special-cased**, and it is recorded as a
decision rather than left to look like a coincidence. The alternative, a rule reading "placeable, or
idle", would have to be repeated at every one of the rule's call sites, and it would be the seam
through which the second exemption arrived: once the rule admits one class of thread on grounds
other than its mask, nothing in it argues against the next.

---

## 9. Deliberately NOT frozen

- **Per-core run queues and any finer locking.** The spike's stage 2, and the lock-hold shortening
  that moves the bound belongs to the IPC optimisation work.
- **The MCU dual-core parts' MODEL is settled and their port was never in question.** This entry
  used to ask whether they were "worth an AMP port at all", which was badly worded and invited
  the wrong reading: the only question was ever SMP versus AMP, and requirement 6 answers it.
  They get AMP. Nothing in this contract declines support for a part on the ground that the work
  is not worth doing -- section 1 gates on HARDWARE, never on whether a port earns its keep.
- **What the ISA DID settle needs no further source and bears on every backend's ordering.** Memory
  ordering is core architecture on this family rather than an option, so it is present on any part;
  ordinary loads and stores carry NO inter-processor ordering at all, the model being an explicitly
  weak release consistency, and the acquire and release pair rather than the blanket memory-wait is
  the intended cross-processor tool. A rendezvous written against the blanket instruction alone would
  be reading past the specification.
- **Who may mint a cross-node endpoint, start a core, or PLACE A THREAD OF ANOTHER TASK.** Static
  in kernel init for the first AMP work; the general question is the capability layer's and is
  answered with it. The third is the placement work's one remnant: a thread places itself and its
  own siblings unprivileged, a task being one scheduling domain whose single grant bounds every
  placement made inside it. Reaching into ANOTHER task is the case no existing authority answers,
  and answering it here would decide the general case through a special case, exactly as N8 says
  of the crossing.
- **The AMP partition layout, NARROWED BY N6b RATHER THAN STILL OPEN AS WRITTEN.** The image
  boundary is decided: one image per node, so instance-keyed storage is not what an AMP partition
  is made of and the contiguous array's protectability is a question about the SIM's posture
  alone. What stays open is the memory partition itself -- the region each node's image owns, the
  fixed address the shared window sits at, and whether a part's protection unit is asked to
  enforce the boundary or merely to describe it. Until something enforces it, the window's header
  rule stands: a compromised peer kernel reaches every other node's memory, and validation defends
  against a malformed peer and not a hostile one. **The LX6 names two of those shapes concretely
  and owes neither today.** Its cross-core rendezvous cells are ordinary statics indexed by the
  writing core, which is the placement N6b requires two link scripts to agree on once each node
  carries its own image; and its doorbell partition is target-keyed, one trigger per core, which is
  a node index spent as a hardware core mask. Both are sound under one image spanning the cores and
  both are exactly what an own-image node on this part would have to redo.
- **Whether a shared kernel and an AMP node can coexist on one part.** The i.MX8MP is both at once
  and nothing here decides how the two halves see each other.
