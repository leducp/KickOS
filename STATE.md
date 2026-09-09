<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# KickOS -- current state

The only file that changes every milestone, and a REFERENCE rather than a briefing: it is read
by section, and the headings are its index. Open the ones your work touches to re-ground, then
go straight to the record you need. What belongs here is a cause, a measurement, a trap or a
decline that no command re-derives. No history and no task lists -- granular items live in
`TODO.md`, the docs map in `docs/README.md`, every silicon wire value in
`docs/reference/boards.md`.

**AND NOTHING BELOW MAY BE A FIGURE A COMMAND ANSWERS.** Every wrong claim this file has
ever carried into a reading session was one it quoted where `git log`, `ctest` or a grep
already answered. If you can derive it, derive it. What is written here is what a green run
does NOT say.

## Where we are

M7 is the multicore milestone and `docs/design-multicore.md` is its contract; every ruling and every
freeze lives there, `roadmap.md` assigns the numbers, and `git log` carries the order things landed
in. **What follows is only the causes, measurements, traps and declines that a green run does not
say and no command re-derives.** The two M7.11 and three M7.12 sections at the bottom of this file
carry the AMP window and the RP2350 port.

**EVERY M7 SUB-MILESTONE HAS MERGED AND M8 IS OPEN, BUT M7'S REVIEW HANDED DEFECTS FORWARD RATHER
THAN CLOSING CLEAN**, which is why M8 is cut to run fixes first, then de-duplication, then
optimisation, across eleven sub-milestones. `roadmap.md`'s `### M8` section is the ledger and the
only place those numbers are assigned; the M8 section at the bottom of this file carries what
opening it established and no command answers.

**THE PREDICATE OPENS ON HARDWARE PROPERTIES RATHER THAN AN ARCHITECTURE FAMILY, because a family
name is a proxy that dates the first time a part violates it.** The MMU is NOT one of the six and
per-line interrupt targeting IS, which is what puts the RP parts in the AMP column: the RP2040
datasheet says the same interrupts reach both cores' own controllers, so a line "granted" to one
core is granted only because the other masks it. **The record says in as many words that the RP
parts CAN run a shared kernel**, FreeRTOS shipping a dual-core RP2040 port, so their exclusion is
argued as a value judgement and never re-derived from a link. And **coherency sitting on the ARCH
side carries an OBLIGATION rather than a guarantee**: that declaration is true only while every chip
of the arch programs shared-attribute descriptors over kernel state, and a chip that did not would
break it silently.

**THE LX6 DOES NOT LAND THERE, AND WHAT SAYS SO WAS MEASURED OUT OF THE ESP32 MANUAL.** The part's
two caches sit only on the external flash and PSRAM path, their pools carved OUT of internal SRAM
rather than covering it, so kernel state placed in internal SRAM satisfies the coherency requirement
by its second clause. Its interrupt matrix is a real distributor besides, per-CPU map register banks
with an unconnected input serving as a routing sink, so it passes targeting. **Its verdict is a GATE
rather than a ruling, and what gates it is a per-core data book**: the Xtensa ISA defines both
remaining primitives, the compare-and-swap and the processor identity, and defines both as
configurable OPTIONS whose realisation no document on this bench states.

**ONE IPC MECHANISM AND NOT TWO WAS AFFORDABLE BECAUSE OF ONE FIGURE**: at `KOS_EP_MSG_MAX` 256 a
ring slot at full message size costs about 1.5 percent of an RP2040's memory, so locality never
reaches the API.

**`qemu-system-aarch64 -M imx8mp-evk` MODELS NO WAY TO RELEASE A SECONDARY CORE AT ALL, measured
three independent ways that agree.** Every CPU object reports `psci-conduit` 0, so there is no PSCI
at any conduit and an `SMC` is an exception to our own vector; cores 1 to 3 carry
`start-powered-off` true with nothing in the model able to clear it; and the reset controller and
the mux registers that would do it on silicon are both `unimplemented-device`, confirmed by writing
a pattern and reading back zero while the model logged the discard. So the board is refused above
one core at COMPILE time rather than left to present as a boot that stops. **The release was
deliberately NOT written**, and that is the decision to argue with: the silicon sequence is
documented down to the register offsets, but it is an entry-point pair split across two registers
with an ordering the manual does not state, a released core arrives at EL3 owing the same handover
the primary gets, and no preset could compile or run it. Unrunnable code carrying a correctness
claim was judged worse than a recorded mechanism. **It is a raise, not a ruling.**

**AND THE HETEROGENEOUS CASE HAS NO VEHICLE ON THIS BENCH AT ALL.** That part is both an SMP part
and an AMP part at once, its Cortex-M7 companion having its own NVIC and its own instruction set and
being startable from the A53 side by two register writes. The machine models the A53 cluster alone,
ships no companion, and decodes its tightly-coupled memory as unimplemented. **This is not a gap in
the AMP work, it is the absence of any machine that could carry the case.**

**WHAT THAT BOARD'S GREEN RUN DOES NOT WITNESS.** The EL3 handover is exercised on every run and is
the one piece a run does witness. Nothing exercises the EL1-entry path, no vehicle here producing
it, and the EL2 refusal beside it has never fired. The UART enable is correct for silicon and
unwitnessable: the model emits on the data-register write whatever the enables hold, so an image
that never enabled the transmitter prints the same, and no baud rate is programmed at all. And the
system counter's own enable is never written, the block being unmodelled here; **on silicon out of
reset a stopped counter would read a constant with a plausible frequency beside it, which is the
shape that HANGS a bounded wait rather than reddening it.**

**NO BOARD IN THE TREE FAILS THE PART'S HALF OF THE PREDICATE WHILE PASSING THE ARCH'S**, every chip
that ships a declaration declaring all three, so the predicate is one authority driven twice: the
build calls it and `tests/static/check_smp_predicate.sh` calls the same function over SYNTHETIC
declaration trees, each case against a control differing in one clause and **each checked by
MUTATION rather than by passing.**

**RELEASED IS NOT ARRIVED, and that distinction is the only reason the bring-up has a witness.**
PSCI answering SUCCESS says a core was STARTED; a core handed a bad entry, a bad stack or a
translation it cannot walk is started and never reaches its own code. **And a write nobody reads is
not a witness**: the primary published its own arrival into cell 0 while the wait loop started at 1,
so commenting that write out changed nothing. The arrival check asks the same question at every
index now.

**THIS FILE PREVIOUSLY RECORDED THE ARRIVAL WITNESS AS MUTATION-PROVEN, AND THAT CLAIM WAS FALSE.**
The mutation had been a hand-run image, not a gate, and turning `release_secondaries()` into an
early `return;` left `ctest` fully green. So the bring-up had no witness in the tree at all while a
record here said it did. `tests/integration/check_smp_arrival.sh` is the gate now.

**ASSERTING THE BANNER'S CORE COUNT IS A TAUTOLOGY, AND THAT IS WHY THE GATE RUNS A SHORT MACHINE.**
Mutating both release loops to stop one short releases one secondary fewer and the banner still
reads the declared count. The second arm runs the image on `-smp N-1`, where PSCI answers
`INVALID_PARAMETERS` only for a loop that actually reaches the last index. **That is an oracle the
image does not supply, and it is what binds the loop bound.**

**FOUR LATENT DEFECTS SURFACED AT S1 AND S2 AND NONE WAS CAUSED BY THAT WORK.** The multi-core arm
of `armv8a_percpu` had NEVER COMPILED, a function sharing a struct's name hiding that struct's
constructor under `-Wshadow` where this tree is `-Werror`. `cpu_id_fold` had no `SKIP_RETURN_CODE`,
so the exit 77 it has always been able to return read as a FAILURE the first time a preset returned
it, this being the first test in the tree that ever skips. `map_tlbi_elided` asserted a single-core
figure unconditionally. And **the fold gate's own scanner could DIE and still report PASS**, found
by writing `and` for `&&` in its awk and watching it go green over a corpus it never read. **The
class is the one this project keeps meeting: an arm nothing exercises says nothing, and the first
thing to exercise it finds whatever was wrong with it.**

**THE CACHE CLEAN THE HANDOVER OWES HAS NO WITNESS HERE.** The primary publishes a boot record that
secondaries read with their caches off, and `arch_dcache_flush` to the point of coherency is what
makes that legal. QEMU models no data cache, so deleting that flush passes every run on this bench.

**`arch_irq_mask`, `arch_irq_unmask` AND `arch_irq_clear_pending` WRITE BANKED GIC REGISTERS for a
line below 32**, so they act on whichever core calls them. Correct at one core, and a semantics
question the moment threads run on more than one.

**AN `ESR=0x2000000` AFTER THE PLAN LINE WAS REPORTED AT FOUR CORES AND DOES NOT REPRODUCE.**
Recorded as UNREPRODUCED rather than dismissed, because EC 0 is `unknown reason` and four clean runs
is not proof of absence. **What it does illustrate is real: a fault after the plan line is something
a TAP-reading gate can miss**, which is the same blindness the arrival gate exists to close.

**A DIAGNOSTIC THAT NAMES A DOCUMENT IS WORSE THAN A COMMENT THAT DOES, because the reader is
already stuck.** One audit action is refused on that ground: it asked that the configure predicate
keep an explicit link to its contract document, and the refusal enumerates the six required
properties in its own message instead. **And code no longer cites an in-repo document at all, the
direction being the ungated one**: `check_doc_names.sh` validates markdown naming code and never
code naming markdown, so a comment citing a nonexistent document passed every gate. EXTERNAL manual
citations stay, being immutable and outside the repo. **No gate enforces the new direction yet**, so
it will regrow.

**`tests/static/trap_redzone_indirect.txt` BINDS CALL SITES BY EXACT `file:line:column`, SO A
COMMENT EDIT IN ONE OF THE SIX FILES IT NAMES COSTS A RE-PIN**: `sched.cc`, `syscall_ipc_fast.cc`,
`irq.cc`, `console_tx.cc` and the two rp2 chip files. `trap_redzone_decls` does not catch a stale
line; of its readers only `check_trap_redzone.sh` and `check_console_reach.sh` reject one, both PER
SCOPE, **so a re-pin is only witnessed by a preset that compiles that scope** and `irq.cc` is not
pinned in armv8a at all. Stranded behind that binding are every surviving in-repo doc PATH in code,
three step designators, and the DEFINITIONS of the `INVARIANT H1..H8`, `D1..D9`, `B1..B3` and `RULE
L1/U2/U4` families. **The re-pin is mechanical rather than manual**, so the cost is smaller than the
count suggests.

**THE LOCK SPANS THE CONTEXT SWITCH, AND THAT IS WHAT CLOSED GAP 3 WITHOUT AN EPOCH.** Until the
swap parks it, an outgoing thread's saved frame still names an earlier run, so the lock is handed
THROUGH the switch and released in `switch.S` where the outgoing frame is parked. **An epoch was
deliberately not built**: it would be a second answer to one question, and the obvious cell to build
it on is pre-seated BEFORE the park, so a guard on it would answer "already off-CPU" in exactly the
window it was guarding. A consequence worth keeping: the lock is never held with interrupts
unmasked, so an interrupt always enters at depth zero. **Three of the six windows section 4 of the
contract records never had a lock to substitute for**, and two audit claims were refuted there:
`cap_teardown`'s deliberate mid-chunk lock drop is FINE under a shared kernel, and the authority
word's unlocked read is safe because every reader passes the CURRENT thread and no path reads a
peer's.

**THE DOORBELL'S WAIT IS SOFTWARE ON EVERY BACKEND.** No GIC version reports to a SENDER that a
target serviced a software-generated interrupt: v2's per-source pending registers are banked to the
ACCESSING core, and v3's carry no source identity. Coherency supplies the DATA half of the
rendezvous; nothing supplies the acknowledgement half.

**DELETING THE PARKED CORE'S INTERRUPT UNMASK LEFT THE IMAGE'S OWN CHANNEL FULLY GREEN.** WFI wakes
on a pending SGI and the software poll then answered everything, so the count, the postconditions
and the raise total all held. Only QEMU's own GIC trace caught it. **That is why the doorbell gate's
oracle is the emulator's and not the image's. AND THAT ORACLE CANNOT SEPARATE TWO THINGS, WHICH IS
WHY THE GATE IS SERIAL**: a core already spinning in the lock's acquire loop answers the doorbell by
POLLING and acknowledges nothing, so "the interrupt path never ran" and "the host starved that core"
read identically. Under a parallel `ctest` a correct image went red once in three runs.

**THE BOOT SELFCHECK USED TO KILL A WORKING MACHINE, AND THE LESSON IS ABOUT INSTRUMENTS RATHER THAN
ABOUT THE LOCK.** Its contention arm asserted that every peer had taken the lock inside a fixed
round count, which a peer can only do in the gap between the primary's release and its next acquire,
and losing every gap made the IMAGE call `kfault_terminate`. Saturating the box reproduces it 3 of
3; idle it passes 5 of 5, which is why it read as weather for a while. **A gate that fails only
under load is a claim about the host, not about the kernel.** The fix waits, bounded, for the state
each arm needs. **AND THAT CHECK'S TWO ARMS WANT OPPOSITE PEER STATES**: the emulator arm needs
QEMU's GIC to report each secondary ACKNOWLEDGING the doorbell, which a core polling in the acquire
loop never does, while the poll arm needs the peers masked and spinning. Hence two phases, and
anyone tightening this must keep both or one arm goes vacuous silently.

**THE CROSS-CORE POKE'S FIRING IS SCHEDULING-DEPENDENT, AND THAT IS CORRECT RATHER THAN A DEFECT.**
A peer that has already switched off a dying space took its Context synchronization event in that
switch, so the rendezvous is owed only while a peer is still on the space; over six runs of the
four-core selftest the run-total came out 0, 4, 4, 6, 6 and 8. **So NO arm may assert that a task
kill produced a poke**, and `doorbell_xpoke` asserts a per-core service floor and a pairing
invariant instead. **There is no route from an arm to force one**: `KOS_MEM_FLAGS_ALL` is NOCACHE
alone, so unprivileged code cannot create the executable page whose removal would owe the poke.
Making one forceable is a decision, not a fix.

**`hello` CAN NEVER WITNESS THREADS ON EVERY CORE, AND NO SAMPLING BOUND FIXES IT.** It holds one
runnable thread: ping and pong alternate through semaphores with a 400 ms sleep each, so which core
picks up the single sleeper is the host's draw, and sampled to 180 s under load the vCPU set
plateaus at two of four. The gate boots the `stress` soak for that reason. **THAT MAKES THE SOAK'S
SIZE LOAD-BEARING, AND ITS BUDGET PROBE CAN SHRINK IT**: `stress` sizes itself down against the
board's thread and semaphore pools independently of `MAX_PAIRS`, and a soak at or below the core
count returns the gate to a lottery. The app reports the size it realized and the gate refuses
anything at or below the core count; without that line the gate would pass while proving nothing.

**THE AMP POSTURE IS REACHABLE BY CONFIGURATION ALONE**, `KICKOS_MULTICORE_AMP` in `Kconfig`, which
sets one kernel core while the image still drives four. It links `kickos_arm64_doorbell_service`, so
**gates keyed on the kernel-core count silently skip a body that exists**, which is why the ISB gate
is keyed on `KICKOS_NUM_CORES`.

**THE ACTIVE-CORE SET WAS BUILT DERIVED AND THAT WAS A RULING.** Its frozen form, a readable field
on the opaque space, is unimplementable: on all three backends the opaque handle IS the root
page-table frame, so there is no object to hold a field. A per-core installed-root array written
only where the register is written answers the same question and changes no signature.

**`TPIDR_EL1` IS WRITTEN BY NOTHING ON armv8a, AND THAT IS DELIBERATE.** T6a took the EL0 entry
scratch off it by seating the kernel block BEFORE `arch_context_init`; T9 then made the per-core
cell the first field of `struct armv8a_percpu`, whose address is a link-time constant at one core.
**The register is HELD for the multi-core arm that reads the block out of it, so a port that spends
it takes M7's only free one.**

**THE PER-CORE BLOCK IS PADDED TO A CACHE LINE, WHICH IS A COMPILE-TIME BET THE MAINTENANCE CODE
BESIDE IT REFUSES TO MAKE**, that code reading the line size from a register at run time. The bet is
taken anyway because this family ALREADY gives its per-core rows a line each with named constants,
and its size is a literal the secondary entry's assembly spells, so the alignment and that literal
move together. **What is still unpadded is the ISR, fault-report and dispatch DEPTHS**, compact
arrays on the hottest path.

**FIVE ARMS REPORT PARTIAL ABOVE ONE KERNEL CORE FOR TWO DIFFERENT REASONS, and collapsing them into
one loses the distinction.** `irq_spurious`, `irq_mask_coalesce`, `irq_discard` and
`irq_stale_register` each carry a half that is a NON-EVENT, unwitnessable wherever the observer runs
concurrently with the subject; the one-core fleet still checks those halves in full.
`thread_slay_window` is the other reason: its control leg needs the victim to reach a park before
the kill lands, an UNESTABLISHED PRECONDITION that the machine grants or does not, so a spent
restage budget is a partial and never a red. **A red that is a claim about the host rather than
about the tree is the failure mode this project pays most for.**

**THE SKIP SET'S LABEL IS A MISDIAGNOSIS, AND THE LABEL COST MORE THAN THE SKIPS DID.** It reads "a
cross-thread progress order is not a property of N kernel cores". For most of the tier-1 IRQ arms
the progress order was only how the precondition got MANUFACTURED; the real blocker is an ABSENCE
CLAIM, a service, a redelivery or a wake that must not happen, and a non-event raises nothing for a
later read to be ordered after. **THE PROOF is one mutation answering two questions: deleting the
masked window the coalescing property is about reddens `irq_mask_coalesce` and `irq_discard` at one
core and PASSES at four.** **Ask each arm left on the set whether it asserts that something did NOT
happen, not whether it assumes an order.** The class is skipped whole rather than one at a time for
a reason that has not changed: a failing arm bails without releasing its pooled object, so one flaky
arm becomes dozens of red arms in a single run.

**`errnoprobe` CANNOT PASS ABOVE ONE KERNEL CORE AND NO PER-CORE KEYING FIXES IT.** newlib's
reentrancy state is reached through ONE word in the process's memory, and it is read at EL0 where a
thread cannot ask which core it is on, so two threads of one process on two cores share an errno.
The seat belongs in thread-local storage. **Declined VISIBLY as a skip carrying the mechanism.** And
that decline is REPRODUCED on two arches rather than reasoned: it had never been TESTED until a
four-core RV64 board existed to run it, and it reports the seat's own failure, a preempted thread
coming back on the peer's errno, with the four-core arm64 preset reporting the identical one.

**THE SLAY-GUARD PREDICATE IS RIGHT AND HAS NO RUNTIME ARM.** Idle control blocks sit outside the
pool, so no user handle can name one and the branch is unreachable from userspace. It becomes
load-bearing the moment an idle block is poolable, and mutating it today reddens nothing.

**THE HOST UNIT LAYER IS OFF WITHOUT A GTest, AND THAT HID A SUITE THAT DID NOT BUILD.** **A FIXTURE
defect from the same cause is worse than the suite**: an `arch_start` stub that RETURNS lets a
deliberately-abandoned bracket unwind and underflow the lock depth, after which every acquire in the
process is silently skipped. Provision GTest before believing a unit verdict. **And a unit fixture
that defines `arch_cpu_id` REDDENS EVERY SINGLE-CORE PRESET IN THE FLEET**: that gate reads tracked
SOURCE and skips only a block opened by the literal `#if KICKOS_NUM_CORES > 1`, so a host fixture
stubbing the seam is a finding on boards the fixture never runs on. It was found by running one
single-core preset, never by the preset the fixture belongs to.

**THE GICv3 GROUP MISMATCH IS SILENT IN HARDWARE AND LOUD IN THIS FLEET, AND NOTHING IN THE TREE
OBSERVES A GROUP DIRECTLY.** `ICC_SGI1R_EL1` generates Group 1 alone, the controller drops a
mismatched interrupt with no fault and no log, and the loudness is bought entirely by the timer PPI
riding the same group decision: put it in Group 0 while the SGI stays Group 1 and six gates fail.
**The protection is therefore INCIDENTAL.** A future backend that grouped the doorbell and the timer
separately would boot, answer doorbells and hang, and no arm would name the reason.

**AND A WRONG `GICR_BASE` TERMINATES RATHER THAN HANGING**, printing `gicv3: no redistributor frame
carries this core's affinity`, which is the whole point of discovering the frame by `GICR_TYPER`
instead of indexing it by core number. A GIC version neither backend implements refuses at CONFIGURE
with the value in the message, not at link with an absent triad. And a wrong acknowledge-event name
fails the doorbell gate's timer PARSE CONTROL rather than reporting a vacuous absence of
acknowledgements.

**THE GICv3 DISABLE PATHS RETURNED BEFORE THE DISABLE TOOK EFFECT, AND EVERY GATE WAS GREEN OVER
IT.** Found by external audit after the step was committed. GICv3 applies a cleared enable
asynchronously and RWP reports the completion; the backend read `GICD_CTLR.RWP` at the three control
writes and read the REDISTRIBUTOR's RWP nowhere at all, so all five `ICENABLER` writes returned
early. The operational one is `arch_irq_mask`, the driver teardown path, where it is a mask that is
not exclusion. **Nothing on this bench can show it and nothing on this bench ever will**: QEMU
completes these writes synchronously, and a probe that refused the instant RWP read set booted clean
through the whole selftest, so the poll is never once entered here. A GIC-500 need not behave that
way. **The lesson is the shape, not the register: the file had the mechanism written down in its own
words for one register and applied it to that one only, which is the kind of gap a green fleet is
structurally unable to report.** QEMU's GICv3 model is not a GIC-500 either, so "the posture matches
the silicon target" is a claim about the ARCHITECTURE version and not about the i.MX8MP's
implementation of it.

**VERIFY A RISK YOU WROTE DOWN BEFORE ACTING ON IT; THE FIX MAY NOT BE WHERE THE NOTE SAYS.** I
flagged the GICv3 trace parse as assuming core numbers below ten and it converts the radix
explicitly. What WAS wrong sat one line away: the timer INTID was hand-spelled twice with nothing
checking the two agreed, so a changed INTID would have left the planted control proving the parse
against a line the emulator never emits. Both suffixes are PRINTED from one constant now.

**THE INSTRUMENT THAT FOUND THE S5 LOCK DEFECT IS WORTH MORE THAN THE FIX: bisect by MODEL.** The
same image passes at four harts under AMP and hangs under the shared kernel, which puts the fault in
the lock and cross-core scheduling in one step and touches no debugger. The emulator monitor then
named it outright: the lock word held, an owner cell naming core 0, and core 0's PC inside the idle
wait.

**THE SEND HAS NO MACHINE-MODE LEG AND THE RULING BOUGHT A SMALLER TRAMPOLINE THAN EXPECTED.**
`mideleg` bit 3 is not writable here, measured, so a peer's raise arrives as a machine software
interrupt that must be lowered; but a SUPERVISOR store to a peer's CLINT `msip` word is permitted,
also measured, so only the RECEIVE side runs in machine mode.

**`invalidate_all()` MEANS DIFFERENT THINGS ON THE TWO BACKENDS, AND COPYING armv8a's SEQUENCE
COPIES AN ASSUMPTION ABOUT BROADCAST THAT DOES NOT HOLD HERE.** Its `tlbi vmalle1is` clears every
core before it returns, so ordering a free after it is safe there; `sfence.vma` reaches the
executing hart alone, so the same sequence frees frames while peers still hold cached translations
into them. The two operations are therefore ONE UNIT here, `invalidate_all_everywhere`, and the
comment naming the trap sits at the call a porter would otherwise reach for. **The next backend
written against armv8a's shape inherits this: check what the model's invalidate actually REACHES
before reusing an ordering built on it.**

**THE CROSS-CORE SHOOTDOWN WAS WIRED ONLY TO ELIDE, WHICH IS THE HALF THAT COSTS NOTHING.** The
derived active-core set answered "no core holds this space" and skipped maintenance, correctly; the
case it exists to detect, a space a PEER holds, had no send at all, so unmap and destroy invalidated
the calling hart and left every peer on a revoked translation. **This backend owes a send everywhere
armv8a gets peer reach from hardware**: `tlbi vmalle1is` and `vaae1is` are inner-shareable and
`sfence.vma` has no broadcast form at all, so map, unmap and destroy each rendezvous once per call.
The execute-permission gate armv8a uses is NOT copied: that is its instruction-side optimisation,
and gating on it here would leave every DATA removal unsent.

**AND THE FAR SIDE FENCED IN THE WRONG ORDER, which the send made reachable and an audit caught.**
The service body executed `SFENCE.VMA` and only then loaded the request, so a peer could fence, an
initiator could then write tables and raise, and the peer could answer a request its fence never
saw. The answer was truthful about the fence and silent about which writes preceded it. **The order
is three-part now and stated as such: observe the request, THEN fence, THEN answer**, with the
acquire on the load pairing against the initiator's release of the raise.

**A FOUR-CORE IMAGE NEEDS REPETITION BEFORE "GREEN" MEANS ANYTHING, and two passes in a row is not
repetition.** Four of S5's defects announced themselves as a HANG rather than as a red arm, which is
the shape a lost wake takes. **Every claim about a four-core image in this file rests on a run
COUNT, and a single-figure count is not a claim**; a count taken while anything else uses the box is
not one either. 120 clean runs at or after the interrupt-controller repair bound the lost-wake rate
under about 2.5 percent rather than proving it zero.

**AND THE INSTRUMENT BUILT TO NAME THAT CAUSE NEVER CAUGHT ONE, so its discrimination is
unexercised.** `kickos/smptrace.h` separates a park no waker searched for, a search that came back
empty, and a wake whose switch never took. **It was validated on a PASSING run**, where it correctly
reports that no thread parked and stayed parked, and on 40 instrumented runs it had nothing to
decode. It is kept, off by default and compiling to nothing, because the next lost wake on any
backend is what it is for. **Nothing here says it works on a real stall.**

**S7's PEERS ARE NODES FOR THE WINDOW AND NOT FOR A SCHEDULER, AND THAT BOUNDARY IS THE PARTITION
LAYOUT THE CONTRACT LEAVES OPEN.** A peer core answers the doorbell, drains its inbox, validates it
and publishes a reply, all under its own node identity and touching nothing the kernel built. It
runs no scheduler, and the reason is not effort: **the arena is ONE linker region with a link-time
assert modelling its exact allocation order**, so giving each node an arena of its own IS the
unfrozen question and building one would have answered it by accident.

**NOTHING WITNESSES THAT THE INSTANCE HOLDER RESOLVES A PEER'S OWN KERNEL.** What a board witnesses
is a peer core running the shared service body and reading its own core identity: its counters move
and node 0's do not. **No run on hardware says a peer resolved a Kernel of its own, and provisioning
four is what makes the claim cheap to believe rather than what checks it.**

**THE DEFECT S7 FOUND WAS AMP-EXPOSED AND NOT AMP-CAUSED, and the difference is checkable rather
than a claim.** `VirtualRanges` was not a total record of a space's mappings: the user stack was
installed behind its back and recorded in no range, **so a frame capability could be mapped ON TOP
of a live thread's stack with no overlap refusal**, the exit path then zeroed that leaf, and the
release path re-derived the run's identity by reading the leaf back and dropped nothing. Raising the
instance provisioning only moved the app half far enough for one arm's chosen address to land on a
child's stack base, and changing a stack size alone reproduces and un-reproduces it with the keying
untouched.

**AND THE MUTATION THAT WITNESSES THAT FIX DOES NOT REDDEN AN ARM, IT KILLS THE RUN.** With the
stack's range record removed, the framecap map onto root's own stack page is accepted, the frame
under root's locals is replaced, and root faults at once with the whole stream truncated. So the arm
asserts the refusal and **the CRASH is what demonstrates the defect**; the arm's own red is
unreachable, because the same acceptance that would fail it also kills the thread that would report
it.

**A TOTAL RANGE LIST MADE A LIVE STACK FINDABLE, AND THE ADMISSION PATHS HAD NEVER BEEN TAUGHT ABOUT
IT.** Before it, `find()` answered null over a stack and the refusal for an address the space never
reserved covered it BY ACCIDENT; after it, a stack is a reservation like any other and each of the
three caller-controlled paths was filtering on the image flag alone. **The lesson is the shape and
not the bug: widening what a record NAMES silently widens what every reader of that record admits.**
And the stack's own self-grant never reaches admission at all: a thread carries its stack as one R|W
region, so the syscall's already-reachable short circuit answers a plain R|W request 0 without
consulting the range list. What reaches the list is a request naming a memory type the mapping does
not carry, or the GUARD, which no region covers.

**RECORDING THE STACK COST THE RANGE BUDGET AND THE COMPILE-TIME FLOOR DID NOT CATCH IT.** The floor
refuses a configuration that cannot seat its own threads' stacks, which is a structural claim; what
actually bit was an APP's demand. A floor sized for the selftest would force every translating board
to provision for an app it does not run, so the two figures stay separate and only the DEFAULT
moved. **AND THAT LOSS PRESENTED AS SKIPS AND NOT AS A RED, which is the failing-declaration shape
rather than a failing arm**: the self-grant arm reads the free-slot count live and takes one more,
so at zero free slots it seats nothing and skips by its own guard, and two of the three said only
"thread pool too small", **which is the spawn refusal's message and names the wrong resource.**

**A SCAFFOLDING OP WAS HANDING OUT AN ADDRESS INSIDE THE FRAME POOL'S OWN WINDOW.** The
seeded-address probe returned the run's physical base, on the stated ground that nothing in the
space named it. Every thread stack is mapped at its own output address, so that window is exactly
where a future stack lands: **the promise held for the caller's space at that instant and never for
a child's.**

**FOUR THINGS ABOUT THE WINDOW THAT NO ARM ON THIS BENCH REACHES.** *The service bounds*, by
construction: one call drains a ring's worth per sender and every peer's across the call, which is
exactly what a static ring set holds, so no arm built out of publications can exceed either, and the
ceiling's subject is a peer refilling while the receiver drains, which needs a hostile far side no
in-tree node can play. **A green run says the bounds do not truncate the honest path, never that
they hold against one that is not.** *The peer's leftover*, which rests on the doorbell being
LATCHED: every controller on this bench latches, so no run here distinguishes a part that drops a
raise against a masked target. *The SEND side's validation*, unreachable from a receive-side test,
the cell the producer reads being the far node's. *And every bound at all*, reachable only by a
FORGED publication, no peer being willing to malform its own writing; one arm is honestly weaker
than the rest, **a zero-length message being a message and not an empty ring is a DISCRIMINATION,
and the mutation that reddens it is an ADDITION rather than a deletion** where every other bound
was reddened by deleting it.

**THE REENT DESCRIPTOR'S BOOT CHECK HAS NO WITNESS.** The in-tree app provisions correctly, so no
run reaches that panic: raising the required count by one is what shows it is live, **and it
truncates every image test on the board rather than reddening an arm.**

**M6.3's ASPACE-SEAM VERDICT AND M6.5's CAPABILITY-SEAM VERDICT ARE RECORDED RESULTS AND NEITHER IS
RE-TAKEABLE, the differs being gone from the tree.** **An address never became a FIELD, only ever an
argument**, which is what let the capability seam survive its own map-and-unmap step. And the
cap differ's corpus was the C-facing ABI only: `kernel/include/kickos/cap.h` is C++ and the
extractor yields records of ANY name over it, so `CapType`, `CapRights`, `CapEntry` and the resolve
chokepoint are held by that header's static_asserts and by nothing else.

**THE THIRTEEN REGION BOARDS COMPILE M6.5's TWO MEMORY KINDS AND NOTHING THERE CAN CONSTRUCT ONE**,
so their green is a compile and never a witness.

**AN INSTRUMENT WHOSE CORPUS CAN GO EMPTY WITHOUT ITS REPORT CHANGING HAS WITNESSED NOTHING, and
M6.4 met the class five times over.** **The question that finds one is not whether it passes but
what makes it go RED when it is handed nothing.** Beside it, **an obligation that reads as satisfied
because the CODE exists is the class to sweep for, and reading the document again is not what finds
it**: a closing sweep over `docs/design-m6-mmu.md` found 8 obligations of 41 not discharged, five
closeable by RUNNING them, every one implemented and believed with only the arm missing.

**AN EXTERNAL REVIEWER'S DIRECTION IS EVIDENCE ABOUT THE DEFECT, NOT ABOUT THE REMEDY, and running
the prescription is what shows it.** M6.2's audit HIGH was real, a donor's teardown freeing frames a
borrower still mapped, and `docs/design-m6-mmu.md` carries the defect and the landed fix. The fix
that audit PRESCRIBED was wrong as stated: a frozen post-constructor snapshot breaks `irq_driver`,
root writing `g_mmio` AFTER its constructors, so a child seeded from the frozen image gets a null
pointer and faults.

**THE `spike/*` BRANCHES MUST NOT BE MERGED.** `git branch --list 'spike/*'` answers the count. They
are de-risking runs and not ports, and everything they found that moves a contract is already folded
into `arch/include/kickos/arch/arch.h`, `docs/design-m6-mmu.md` and `docs/design-m7-smp.md`. Read
the folded findings, never the branches.

## What the fleet does NOT witness

The whole point of this file. A green fleet pass says none of the following.

- **A DRIVER INSIDE THE KERNEL IS A LIABILITY TO A MICROKERNEL, AND THE CONSOLE IS THE ONE THIS
  PROJECT ACCEPTS.** It is a transgression admitted for the sake of reality rather than a
  component earning its place: something must print before any userspace driver exists, and
  during a panic when none can be trusted. It is accepted because it is genuinely needed, not
  because it is adequate, and it is the only such transgression accepted.
- **THE OWNERSHIP, PUBLISH AND RECLAIM MACHINERY EXISTS TO END ITS REACH**, as early as a
  userspace driver can take over. It is not a workaround for a weak console. It is how the
  project bounds the blast radius of a deliberate violation of its own model, and reading it as
  an unrelated feature inverts what it is for.
- **SO IMPROVING THIS CONSOLE WOULD BE INVESTING IN A LIABILITY, and that is what settles
  serialisation.** Making it ordered, or fast, entrenches the thing the architecture wants to
  minimise and makes it likelier that something else grows to depend on it. **The interleave is
  therefore not a defect, not a limitation awaiting a fix, and not primarily a cost trade: it is
  a property of something accepted under duress and deliberately left unimproved.**
- **WHICH ROUTE INTERLEAVES, MEASURED PER ROUTE.** Stated this way because a reader who takes
  the widest form will distrust a published capture that is fine, or hunt for a lock that should
  not exist.
  - **The kernel's DIAGNOSTIC route, the chip UART, interleaves at BYTE granularity.** That is
    what the banner, the status lines and the fault reporter use. `console_emit` brackets only
    the ownership-count read with `IrqLock` and leaves the device write outside it deliberately;
    on this board `arch_console_write` is an unlocked byte loop.
  - **The PUBLISHED route does not interleave kernel records with each other**, and what makes
    that true is the KERNEL LOCK rather than the driver: `cap_console_deliver` copies the whole
    record into one parked receiver under `IrqLock`, so a record arrives as one datagram. **What
    it does NOT guarantee is a single writer.** The kernel pops `wq_pop_highest(recv_waiters)`,
    so a driver parking more than one thread gets its records spread across them with no
    ordering enforced between their device writes. A future multi-writer console driver breaks
    this property without touching the kernel.
  - **AND THE PUBLISHED ROUTE IS STILL EXPOSED THROUGH THE KERNEL'S FALLBACK.** With no receiver
    parked, `kvprintf_route` falls back to the chip route with `force_sync`, whose own comment
    accepts "interleaving with the driver's in-flight bytes". So a published capture can carry
    interleaved bytes: not two kernel records racing, but a kernel fallback racing the driver.
    Deliberate and documented rather than a defect, and a gate reading that route inherits it.
  - **RTT does not interleave**: the whole write runs under `IrqLock`.
- **THE MECHANICAL COSTS, WHICH BOUND THE IMPLEMENTATION RATHER THAN DECIDE THE QUESTION.**
  `console.cc` carries the sharpest: the chip transport "must NEVER be held under IrqLock across
  a whole transmission: a 256 B write at 115200 would mask interrupts for ~22 ms". Beside it, a
  lock there serialises every core behind the slowest UART poll, and `kprintf_fault` runs in
  panic and fault context where a lock may already be held, putting the deadlock in the one path
  that must always work. **A faster transport would not change the answer**: these say only that
  the lock cannot be taken cheaply, where the ruling above says it must not be taken at all.
- **WHAT A GATE READING EACH ROUTE MUST DO.** Reading the diagnostic route: tolerate a split and
  SAY when it did, which is what `check_qemu_panicgate.sh` now does, strict match first and a
  split recovered only by deleting the KNOWN kernel status lines. Reading a published console:
  sound for whole records while the driver keeps one writer, and exposed to the fallback above.
  Reproduced once by an external run and not by two authoritative ones, so it is infrequent
  rather than rare.
- **THE RV64 DOORBELL'S INSTRUCTION-SIDE HALF HAS NO OPERATION AT THIS BOARD'S ISA BASELINE, so it
  is not witnessed and cannot be.** The service body carries the TRANSLATION-side fence, which is
  `SFENCE.VMA` and which the ISA gives no way for one hart to perform on behalf of another. The
  instruction half is what `FENCE.I` exists for, and Zifencei is not in
  `arch/riscv/chip/virt_rv64/cpu.cmake`'s march string, so the instruction does not assemble.
  **Writing `SFENCE.VMA` into that comment as though it covered both would be a false statement of
  contract, which is worse than the gap**, so the body says what it does and no more. No caller
  exists yet: the rv64 instruction-side rendezvous is unwired and `--gc-sections` drops it, which is
  also why `check_doorbell_generic.sh` asserts no rendezvous body on this arch and why
  `check_doorbell_isb.sh` is not registered for it at all. Raising the baseline is a real option and
  not a free one, the toolchain's multilibs being named for exact march strings, so it is measured
  against them rather than assumed when a caller appears.

- **THE RV64 IRQ HANDSHAKE'S STORE-TO-LOAD FENCE IS ARCHITECTURE-MANDATED, HAS NO WITNESS HERE, AND
  NOTHING ON THIS BENCH WILL EVER GIVE IT ONE.** `arch_irq_unmask` and `arch_irq_inject` are
  DEKKER-SHAPED against each other, each writing its OWN word and then reading the PEER's, and
  RVWMO preserves no order between a store and a later load to another address (RISC-V
  Unprivileged ISA 18.1.3) while an AMO with `.aq` and `.rl` both clear adds none (13.1). With no
  fence on BOTH sides both writes may sit behind both reads at once and NEITHER side raises
  `sip.SSIP`: the bit pending, the line unmasked, the driver asleep for good. **The take-back
  settles the LOGICAL race and never the visibility one**, which is what that code's own comment
  claimed for it until this hotfix, and a false statement of contract is worse than a gap. QEMU's
  TCG orders more strongly than RVWMO, so an image carrying no fence at all passes every arm in
  this tree and the defect is indistinguishable from the fix under emulation; a randomised soak was
  DECLINED for the standing reason, an arm that fails intermittently reading exactly like a broken
  one. So it is held by the SPECIFICATION and by a DISASSEMBLY gate, `rv64_irq_fence`, which
  resolves each access to the word it names out of objdump's own symbol annotation. **FENCE.TSO is
  as much what that gate exists for as a deleted fence**: it is spelled like a fence, assembles at
  this board's baseline, and omits exactly this edge. What the gate does NOT say is which word
  `arch_irq_inject` reads after its fence: the re-read goes through a register the unmasked branch
  rebinds, so the walk drops that binding at the join and that side is asserted on its publish
  alone. **AND THE THIRD WORD OWES NO FENCE FOR A REASON THAT IS NOT ATOMICITY.** `g_irq_raised`'s
  accesses are one instruction each, which buys no visibility; what does is that `raise_line` sets
  `sip.SSIP` on the CALLING hart, so the consumer that must not miss a bit is the hart that set it
  and the load value axiom orders it (Appendix A.3.2). A peer's doorbell poll may read that word
  stale in either direction at no cost. **FOUR OTHER BACKENDS CARRY THE SAME THREE WORDS AND OWE
  NOTHING**, none of them being SMP-capable: at one hart the local interrupt mask those bodies
  already take IS exclusion, and a fence written there would be cargo.

- **THREE ORDERINGS ON THE arm64 ENTRY AND TIMER PATHS REST ON THE ARCHITECTURE AND ON A DISASSEMBLY
  GATE, AND NO MACHINE ON THIS BENCH CAN WITNESS ANY OF THEM.** `msr SPSel, #1` ahead of the first
  write to SP, and an ISB after each `CNTP_CTL_EL0` disable ahead of the Device write it protects,
  in `arch_timer_disarm` and in `kickos_armv8a_percore_init`. Every emulator here enters at reset
  with `PSTATE.SP` already 1, so the SP_EL0-versus-SP_ELx confusion is reachable only from a
  firmware handover that left `SPSel` 0, which no machine here produces; and the timer models
  deassert on the register write with no pipeline to drain, so an image carrying neither barrier
  boots green on all four arm64 presets. Held by `arm64_entry_order`, which reads the three bodies
  out of the linked image and asserts ORDINALS, and by nothing else. **What that gate does NOT say
  is that the barrier is SUFFICIENT**: it asserts one ISB stands in the window, not that a Context
  synchronization event is all the part owes between a system-register write and a Device store.
- **THE `extern_c_linkage` SCANNER WAS VACUOUS OVER A WHOLE FILE OF THIS BRANCH AND REPORTED CLEAN,
  which is the failure mode the whole character-level design has.** It skipped `#` directive lines
  but not their LINE CONTINUATIONS, and this branch's chip file is the tree's only one whose
  continued `#error` leaves an apostrophe unpaired. The state machine then sat inside a literal from
  line 40 to the end of the file, counted ZERO braces, and passed at depth 0. Fixed, and the arm
  that would have caught it is a planted file rather than anything in the corpus: a mis-parse does
  not report the wrong line, it stops reporting, so no real file can distinguish a working scanner
  from a stalled one. Every other continued directive in the tree happens to carry even quote
  parity, which is luck and not a property.
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
  `tests/static/service_lists.txt` is compiled by neither. **And the TREE STAMP each tool prints is
  a check on none of this**: it is taken from `$ROOT` by `git -C` and agrees with itself whatever
  sources cmake actually read, which is how a sweep invoked from another checkout once reported 59
  of 60 presets against a tree it had never compiled.
- **THE SLEEP PATH WAS MISSED ON THE FIRST PASS, AND THE ENUMERATION IS WHY.** The death point's
  class is every site that writes `ThreadState::BLOCKED`, and there are exactly three: `wq_block`,
  `park_queueless` and `ktime_sleep_until`. Enumerating from the two park funnels being refactored
  finds the first two and misses the third, which reaches neither. **A sleep self-wakes on its
  deadline, so the ordinary miss is a latency bug**, a cancelled thread sleeping out its remaining
  delay before dying; `ktime_sleep_ns` saturating to `UINT64_MAX` on overflow is what makes the
  unbounded case real, and that one is the defect the death point exists to close. The check belongs
  in the prologue for a reason sharper than the empty unwind: **a check at the park would exit with
  the thread already on the sleep queue and the one-shot armed for it, and `sched::exit_current`
  does not sweep the sleep queue**, so the list would keep a pointer into a slot the pool re-hands.
  Witnessed at the unit layer, deterministically. **The on-target suite cannot witness it at all: no
  arm anywhere cancels a SLEEPING thread.** Every cancel-facing worker in the selftest is written to
  park on a semaphore nothing posts, so 64 sleep call sites and every kill, slay and group-kill site
  between them never produce the interleaving. Recorded rather than closed with a probabilistic arm.
  **`park_death_point` is now a closed class enforced by a gate**, which counts the
  `ThreadState::BLOCKED` writes against a declared set: it cannot check that each park ASKS, the ask
  being in the caller for two of the three sites, but it can refuse a park nobody declared, which is
  the failure that actually happened.
- **THE SINGLE-CORE DEFECT M7.5's DEATH POINT EXISTS TO CLOSE HAS NO WITNESS IN THIS SUITE.**
  Forcing the predicate to answer false reddens NOTHING at one kernel core: the window needs a
  preemption between `syscall_dispatch`'s entry read and the lock acquisition, and no arm can drive
  that from userspace. The fix is correct BY CONSTRUCTION, the predicate being read under the same
  lock every writer of `cancel_kind` holds, and it IS witnessed at four cores, where the same
  forced-off predicate parks a group member forever and reddens `task_group_kill` 3 of 3. It is NOT
  witnessed through the slay-window arm, which stayed green in all three runs: root must return
  from a syscall, run the wrapper and enter the kill syscall while the worker needs two or three
  user instructions to reach its park, so that staging race is not close. **A randomised soak would
  reach the one-core window eventually and was DECLINED**: an arm that fails intermittently reads
  exactly like one that is broken.

- **THE GICv3 BACKEND'S ONE-CORE FOLDS ARE WITNESSED NOW, AND THE IMPRECISION THAT DELAYED IT IS
  THE PART WORTH KEEPING.** `cpu_id_fold` reads the count of cores the image DRIVES and skips as a
  class above one of those. It is NOT the kernel-core count, and the difference decides which
  preset can ever close the arm: an AMP image sets one kernel core while DRIVING four, so the
  gate skips there too. What closes it is a GICv3 board that drives ONE core, which
  `imx8mp-evk` is for an unrelated reason, the emulator modelling no secondary release. **A gate's
  skip condition is a claim about which figure it reads, and naming the wrong figure moved an
  obligation onto the wrong milestone for two whole steps.** The multi-core folds above one driven
  core remain unwitnessed and `cpu_id_fold` structurally cannot reach them.
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
  gate for the allocation ABI, in terms saying no selftest arm substitutes for it -- but
  `qemu-arm64` declares no service list, so `drv::bring_up` runs only on region boards and against
  host fakes. What discharged the readback there is `task_handoff_readback`, which is that
  substitution. It now covers the flags-match rule too, a block of its own going through a
  self-grant and a task create at `KOS_MEM_NOCACHE` with both sides reporting the type recorded --
  but a grant-carrying SPAWN has no memory-type field in its ABI at all, so that consumer maps
  Normal whatever the donor holds and no caller can obey the rule through it. Detail at `TODO.md`.
- **A non-cacheable mapping is witnessed as a RECORD and never as a cache behaviour.** QEMU models
  no data cache, so what the flags-match leg asserts is that both mappings carry the same memory
  type, not that either is actually uncached. The type reaching the descriptor at all is unwitnessed
  on this bench, and the first bus master is where that stops being true.
- **The x86_64 board IS in this tree, and what it does not witness is the aspace family.**
  `boards/qemu-x86_64/` is tracked and M6.4 sits ON M6.3, so there is ONE seam measurement rather
  than two to be unioned. What the board does not run is the family itself: the chip selects no
  memory family, ships no map editor and declares no frame pool, so its `arch_aspace_*` claims all
  come from X5's ad-hoc link, which is a `ninja` target and not a registered arm. A green `ctest
  --preset qemu-x86_64` says nothing about them.
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
  MACHINE, which is exactly why they are worth a line here.** `satp` reads zero out of reset here,
  so the Bare write changes nothing; `mstatus.MPV` will not set at all (`csrs mstatus, 1 << 39`
  leaves `mstatus` at `0x0000000a00000000`) although `misa` bit 7 says the hypervisor extension is
  present, so the clear changes nothing; and the `SFENCE.VMA` the PMP write owes changes the probe's
  answer in NEITHER direction, measured both ways over a revoked grant, QEMU synchronising its own
  PMP writes. Each of the three is required by the specification and none of them has a witness
  here. The misalignment probe is a fourth of the same kind: this core resolves misaligned accesses
  in hardware, the probe's accumulator reads `0x0`, and the delegation it would add is never asked
  for. **What DOES have a witness is the ENTRY CENSUS**, because a hart with entry 0 reading OFF and
  a higher entry granting only the probed word is buildable here, and without the census that hart
  boots into a SILENT HANG: no console, no finisher word, the emulator killed by the timeout.
- **`qemu-riscv64` HAS NO SILICON, so every rv64 claim is emulator-grade.**
  `docs/reference/boards.md` names no RV64 part and no `tools/flash*.sh` names this arch, so the
  backend has only ever run under emulation and there is no path to a run. **And F8's named silicon
  witness does not boot this image at all**: `-cpu thead-c906` on the shipped `hello` produces NO
  output, one `zfa` privilege-spec warning and a timeout kill, with the default core printing the
  banner as the control. So the c906 figures in the M6.3 record are a standalone probe's and never
  the suite's.
- **THE MAP EDITOR'S TLB MAINTENANCE IS WITNESSED IN NO PLACE OF SIX.** Taken as two isolated
  single-site mutations on a wiped build directory: removing `unmap`'s per-page invalidate ALONE
  leaves every one of the board's arms green, image arms included, while making `arch_aspace_unmap`
  a no-op that still reports success turns `qemu_riscv64_aspace_ufault` red on "the unmapped page
  did not fault". **So what has a witness here is that unmap CLEARS the leaf, and the invalidate
  beside it has none.** An arm reading the page from the PRIVILEGED side cannot say either, a
  supervisor read faulting on an unprivileged leaf whether the leaf stands or not. HELD BY A COUNTER
  AND NOT BY AN ACCESS: the fresh-map per-leaf invalidate, caught only by `map_tlbi_elided`'s floor.
  NOT WITNESSED AT ALL: `unmap`'s per-page invalidate, break-before-make, destroy's sweep ahead of
  `free_subtree`, and `arch_aspace_activate`'s whole-hart fence. And the fresh NON-LEAF fence's
  ISSUED path never executes in this suite, proved by multiplying that bump by 100 and seeing every
  figure stand still, so only its ELIDED leg runs (2 of the seed's 47).
- **A ROOT WRITE APPEARS TO FLUSH THE WHOLE TLB ON THIS EMULATOR, WHICH IS WHY ACTIVATE'S FENCE IS
  DEAD-EFFECT.** Deleting it left every arm green on a suite that switches between live per-space
  low halves. **That mutation has NOT been re-taken since the suite grew**, unlike the unmap pair
  above, which was. The reading itself stands: a stale low-half translation WOULD be consulted
  there, so a green run is only consistent with the emulator dropping translations on the `satp`
  write itself. That is an inference about QEMU and not a measurement of it: what the bench cannot
  separate is that explanation from "at one core a root that was never installed has no cached
  translation to drop". Either way only silicon witnesses the fence's necessity. The fresh non-leaf
  fence rests on the specification for a second reason, QEMU modelling no caching of invalid PTEs
  where RISC-V Privileged 12.2.1 permits it.
- **THE IDENTIFIER'S WIDTH-ZERO CASE IS A CODE PROPERTY HERE AND NEVER A MACHINE ONE.** Every core
  model on this bench reports a contiguous 16-bit field, the suite's own model line reading `16 ASID
  bits` on both postures, and NO emulator property narrows it: `asid-bits=off`, `asid_bits=off` and
  `asidlen=off` are each refused as `Property 'rv64-riscv-cpu.<name>' not found` while `sv48=off` on
  the same command line is accepted, which is the positive control that makes the absence worth
  stating. What stands in for a zero-width hart is a mutation of the port's own probe. A
  NON-CONTIGUOUS field has no machine here either, so the width-against-popcount distinction is held
  by graded controls alone.
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
  frame it is asked about sits inside the kernel window where `arch_aspace_acquire` is an addition
  and the subtraction is accidentally right. The defect is latent, not absent, and no arm in this
  tree puts a frame outside that span. What IS witnessed is the member: seven arms go red when it
  answers one constant frame and eight when it answers zero.
- **THE KERNEL'S WRITE-EXECUTE SPLIT IS ENFORCED BY HARDWARE AND NO SHIPPED ARM SAYS SO.** What
  holds the runtime half is a pair of privileged probes with a negative control on the parent
  commit, not a ctest entry, because a privileged fault is a panic here rather than a contained
  kill. The static arm `riscv_kernel_wx` reads the linked image's own leaves and is the only shipped
  witness.
- **THE gp GATE'S HAZARD HAS NO RUNTIME ARM.** Nothing in the tree sets a hostile global pointer, so
  `riscv_kernel_gp` and `riscv_kernel_apphalf` are held by their own positive controls (a cross-half
  word reverted to a direct reference, an allowlist entry removed) and never by a thread exploiting
  one. And `virt_rv64.ld`'s comment claims its displacement assert "turns a missed cross-half
  reference into a link error", which R2.2's audit falsified for both the gp-relaxed and the
  medlow-absolute encodings; the comment is still there.
- **Sv57 IS ONE CHOICE ENTRY AWAY AND IS NOT OFFERED**, the emulated core accepting it, because a
  mode the board does not run is a claim with no arm behind it. Two postures ship, Sv39 and Sv48.
- **Neither RX nor LX6 has an emulator, and only RX has no CI gate either.** `ci.yml` runs a
  dedicated `xtensa` job that builds `esp32-wroom` and `-st` and runs `ctest -L host`; `rx72m`
  appears in no job at all, so `rx72m` silicon is the only check that arch ever gets.
- **An RX `pspguard` is OWED.** `pspguard` is armv7m/armv6m only, and `.Lsvc_nokstack` is
  structurally unreachable on RX, so nothing there can reach the REFUSE side of the
  trusted-stack guard. Green on RX means "accepts what it must", never "refuses what it must".
- **No poisoned-user-stack witness for the death-path move.** It must use the SLAY path, not the
  fault path: a fault stacks its own frame on the dying thread's stack by construction, so no
  fault can carry an intact-stack claim. The band must be poisoned ABOVE the parked sp too.
  T6a's `parked_frame_hostile` is NOT this arm: it corrupts a SIBLING's parked frame and says
  nothing about the death path.
- **SIX armv7m presets rest a blocking syscall's continuation on the USER stack**: `f302nucleo`,
  `f302nucleo-st`, `due`, `due-st`, `bluepill-c8` and `bluepill-c8-st`, `CHIP_STM32F103` selecting
  no MPU exactly as the other two chips do. Those are
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
  `KICKOS_ASSERT` added on the console route put `kpanic` on the fault-exit descent
  and took 32 presets red, and nothing this milestone measured could see it: it surfaces as a red
  gate on boards nobody named, never as a build error. **A change on the syscall or the console
  path needs the full 50-preset sweep, not a three-board sample and not the emulators.**
- **`console_reach` IS THE HALF OF THAT GAP THAT COULD BE CLOSED WITHOUT A TRAP-STACK FIGURE, and it
  is registered on the four TRANSLATING presets rather than on the six.** It asks one reachability
  question over the same `-fcallgraph-info` graph `trap_redzone` builds: no `kpanic` and no
  `kpanic_at` reachable from the fault-record console route. It is on `qemu-arm64`, both RV64
  postures and `qemu-x86_64`; `sim` and `sim-telem` are still uncovered by either gate. **It is
  registered where the doors EXIST, and that is the whole reason for the preset set**: on the
  region-model boards `access_copy` is an unconditional `kmemcpy` gcc proves cannot fail, so the
  `cap_console_deliver` error family folds away and there is nothing for a reachability clause to
  find. **It found four doors and all four are closed**: the reachable panics were `reent_seat` and
  `reent_prime` (`kernel/thread/reent.cc`), `ep_copy` (`kernel/syscall/syscall_mem.cc`) and, on RV64
  only, `arch_aspace_release` (`arch/riscv/rv64imac/aspace_rv64imac.cc`); the first two now slay the
  incoming thread, the third refuses and the fourth counts. **The gate was RED on three of the four
  for part of M6.4, and two Reference records went on saying so afterwards** --
  `docs/reference/boards.md` and `docs/design-m6-mmu.md`, corrected 2026-08-29. That is worth the
  line because it is this milestone's own class: a record that was TRUE when it was written and
  stopped being true when the defect it described was fixed, with nothing tying the sentence to the
  fix. When a door closes, the file that named it open is part of the fix. Its declaration is
  `tests/static/console_reach_roots.txt` and it states in its own header what it would fail to
  catch; do not widen that file to quiet it.
- **`bluepill-c8-st` AND `f302nucleo-st` LINK AGAIN, AND WHAT FIXED THEM WAS A FLEET-WIDE COST
  RATHER THAN THOSE TWO IMAGES.** They contributed no trap-stack figure from the commit that broke
  their link until this fix, `trap_redzone` dying at the same link inside its own scratch tree: a
  build error wearing a depth gate's name, on a preset that IS declared in `trap_redzone_roots.txt`,
  so a reader scanning for a missing declaration finds nothing. What the fix removed is the per-app
  build stamp's runtime reformat, which every image on every board compiled, so what looked like two
  boards' problem was a fleet-wide charge to flash and `.data` on every app of every preset. **The
  thin part was the SPLIT, not the fleet, and the split is now sized against the image**: the two
  `#undef TAP_ADD` boundaries had never been measured, so part 1 sat on a few dozen free bytes of
  its 64 KiB while parts 2 and 3 sat on kilobytes. Region 1 now ends after `call_timeout_reply` and
  region 2 after `cap_reply_slot_reuse`. Same arms, same order, proved by the arm-name union of
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
  `aspace.cmake` -- so T7's two owner arguments are a compile-time null everywhere, and
  `errnoprobe`'s arm C exercises the generic path on arm64 while its name says otherwise.
  `call_reg_fastpath` witnesses the site compiling and behaving and nothing more. The owner needs a
  fastpath on a translating arch, which is M6.3's backend.
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
  `arch_aspace_destroy`'s whole-TLB sweep from ahead of `free_subtree` leaves every arm green, and
  so does moving `aspace_release`'s restore of the BOOT space to after the destroy that frees the
  dying root. Both are held by source order and review. The second one's window is not
  interrupt-masked, so a preemption inside it would return to EL0 on a freed root, and nothing on
  this bench can produce that. Identifier reuse is VACUOUS rather than witnessed, nothing allocating
  one.
- **The forced-failure sweep reaches FIVE injection points, not six**, and the EQUALITY of the two
  sweeps is what says so. `domain_for`'s own inner unwind -- a handoff that MAPPED and then failed
  to record -- is unreachable by frame injection here: the donor block sits under a level-3 table
  the image already built, so every refusal lands in `claim_slot` ahead of `aspace_handoff`.
  Reaching it wants a second injector, into `VirtualRanges::reserve`, and it is not built.
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
- **`appdata_no_kernel` does not run on the one board that splits its image.** It is registered
  under `KICKOS_HAVE_MPU` and keys on `__kickos_appdata_start`/`_end`, which `virt_arm64.ld` does
  not define -- so the guard against a kernel archive landing in the app's low window is absent
  exactly where that window is now the only memory EL0 can reach. Detail at `TODO.md`.

## Debts and declines a command cannot re-derive

- **A PARK THAT IS NEVER WOKEN HANGS THE SUITE INSTEAD OF REDDENING IT, and no arm can bound it.**
  There is no timed semaphore wait in the ABI, so every counted wait in the selftest is untimed: a
  dropped latch or a stranded park stops the run with no verdict, which is worse than a failure
  because a timeout reports nothing about which claim broke. Tree-wide and long-standing; M7.5's
  gated IRQ arms add counted waits and so add surface, and one of them found it the hard way as a
  30 second timeout during development. Closing it is an ABI change and belongs to whoever adds the
  timed wait.
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
- **An added ARGUMENT is caller stack, and the fix is a posture word rather than a bigger figure.**
  F10's handoff grew `task_for` by 8 bytes on the deepest chain `syscall_dispatch` has, which took
  the armv7m SVC depth past its red zone on the four `KICKOS_KERNEL_STACKS=0` presets -- region
  boards paying for a translating backend's argument. Two caller booleans became one posture word
  and the measurement came back exactly. A control tree with the argument dropped is what ATTRIBUTED
  it before the fix.
- **THE ABI-FREEZE MILESTONE IS DELIBERATELY UNNUMBERED, and eight sites had invented a number.**
  It fires when the ABI is ready, which is a state and not a position, so a number would assert a
  readiness nobody has. `roadmap.md` states that and owns it. What had drifted: four design
  documents and `TODO.md` said "the ABI-freeze milestone (M8, the last one)" -- wrong twice over,
  M8 being IPC/IRQ optimisation and the list running well past it -- and `docs/README.md` listed
  the freeze as M8's second half while omitting the last numbered milestone entirely. The number is stripped everywhere. Do
  not re-add one, and do not read the absence as an omission to fix.
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
- **A line-number citation is what the doc gate cannot check**, and one carried here had drifted
  onto unrelated text. Cite a path and an identifier.
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
  ruling. **That ruling exists NOWHERE but this line** -- `bench-fleet.sh` still lists and probes
  the board, and an absent one records `ABSENT` without failing, leaving its service lists
  uncovered.

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

## A commit hash is never a record

**RULED 2026-09-01: NO HASH GOES IN THIS FILE.** The workflow is fleet-and-measure, then squash,
then merge, so a hash written down during a milestone names a commit the squash destroys. Branches
are not an archive either: a ref nobody pushes keeps nothing, and neither origin, the reflog nor
the object store held the hashes a local branch inventory was said to keep alive.

**SO A CITATION MUST CARRY ITS OWN CONTENT.** A document citing another reproduces what it needs. A
capture that matters states its own numbers. A witness names the MILESTONE it measured and what it
measured, never the commit it sat on: the tree is what a witness is valid for, and after a squash
the tree survives while the hash does not.

## Rebase and merge invariants

**A DIFF OF DIFFS PROVES NO LINE WAS LOST AND SAYS NOTHING ABOUT AN INVARIANT STATED OVER A WHOLE
FILE.** Check the invariant, per file, after every rebase. Two deltas that never touch the same
lines merge into a lossless union in which one block still fails a rule the file states over all of
them, and a cross-reference to a section another branch renumbered merges cleanly and wrongly the
same way.

**M7.9 (thread placement) AND M7.10 (the LX6 SHARED KERNEL) HAVE NO SECTION IN THIS FILE**, on any
branch. Their silence here is an absence of record, not an absence of findings.

## M7.11: the two-image AMP vehicle, and what its green runs do not say

`docs/design-multicore.md` N6b through N6h is the contract and carries every ruling. Both merge
conditions were met. What follows is only what a green run and that contract do not say.

**THE DEFECT IS THE HEADLINE, because it says what the vehicle is FOR.** The GICv3 SGI raise bounded
its target sweep on `KICKOS_NUM_CORES`, which is how many cores THIS IMAGE drives and is 1 under the
own-image posture, while the mask it sweeps names the PARTITION's cores. Node 0 raising at node 1
set the lead bit at index 1, the sweep `for (index = 1; index < 1; ...)` never ran, nothing cleared
the bit, and the enclosing `while (pending != 0)` spun that core forever with its interrupts masked.
**Not a dropped raise: a HANG.** Every place it could have been caught earlier is blind to it: under
the shared image both counts are the core count so the bound is right, and an own-image node running
ALONE skips the raise before that loop, its peer never being seated. It needed a seated peer in
another image, which needed the merged artefact. The lead bit now leaves `pending` before the sweep
runs, so a future bound that stops agreeing with the mask drops a raise, which is visible, instead
of hanging a core, which is not.

**THE INSTRUMENT IS TWO HALVES THAT FAIL ON OPPOSITE SIDES, AND THAT IS N6c's WHOLE POINT: a node-1
build is not a duplicate of a node-0 build.** Executed rather than argued, on four keyings. A
doorbell sweep bounded by the cores THIS IMAGE drives, and a matrix row taken as zero instead of as
this node's, each redden the node 1 build ALONE, node 0 and the shared image passing because their
bound and their row genuinely agree. A total accessor answering an out-of-range row with node 0's
row reddens node 0 AND the shared image alone, that row being a peer's and quiescent in a node
booted alone. A role keyed on node ZERO instead of on this node leaves the shared image and node 0
entirely green and kills node 1 at boot. And reverting the port table's cleared-is-unbound bias
reddens the shared image and leaves node 0 entirely green.

**A FIELD'S DOCUMENTATION IS NOT EVIDENCE OF A WRITER, and the grep for its writer is one command.**
Found twice in this pass. The region every node writes was stated as cleared by the partition
primary in two documents and was cleared by nothing: no C, no assembly and no tool referenced its
bounds, and the section is NOBITS so nothing loads it either. **A bench whose RAM starts zeroed
cannot see that**, which is the whole reason it survived, and the same is true of a warm start on
any board here. The doorbell probe's ABI carried the shape one layer in: a documented bit 0 meaning
the peer took the message with no raise, never written, and not implementable synchronously at all,
since the probe cannot observe a peer draining at its own pace. Both consumers read past it.

**AND NOTHING HERE WITNESSES THAT THE REGION'S CLEAR IS NEEDED.** The primary writing ones rather
than zeros reddens four arms, which proves the initial content is load-bearing and the call site
reached; moving the clear after `arch_init` kills the run, which proves the PLACEMENT is
load-bearing rather than decorative. Neither says the clear is NEEDED: that claim rests on a part
whose RAM does not start zeroed, and this bench has none.

**A NON-VACUITY GUARD RESTING ON A COUNTER THE PATH UNDER TEST DOES NOT INCREMENT PROVES NOTHING.**
The window's forges write their slots directly rather than through the send path, so the publication
counters do not move across them and a guard built on one reddens while saying nothing about the
claim beside it. **Read WHICH LINE failed, never only which arm.**

**AND A REFUSAL YOU HAVE NEVER SEEN PRINT IS NOT A REFUSAL YOU HAVE WITNESSED**, which is a live
hazard wherever a console is reclaimed before a fatal path speaks. The launch-handshake refusal was
seen on the wire on this board, which is why the configure refusal quotes it verbatim.

**AN ARM THAT ASSERTS BEFORE SPENDING ITS REPLY CAPABILITY REDDENS THE ARM AFTER IT.** `TAP_CHECK`
returns on failure, so such an arm abandons a live reply capability; at a reply ceiling of 1 the
NEXT arm's far caller is then refused one and reddens with it, and two arms redden as one. Any
future arm holding a reply capability owes the reply BEFORE every check.

**A GATE THAT SHELLS OUT TO THE BUILD SYSTEM MAY NOT RUN CONCURRENTLY WITH ANOTHER THAT DOES.** Two
`ninja` invocations on one build directory race on the intermediates they share, proven directly by
starting two of these gates' builds together and finding a kernel archive truncated mid-file with
one process dead in `ranlib`. **The dangerous half is that the same race can leave a WRONG archive
rather than an error.** Under a parallel test runner it is a DRAW and not a certainty, which is why
it resists re-provoking; **the tell is a gate failing far too fast to have built or booted anything,
so read the DURATION and not just the name.** Fixed in-tree by a serial property on the three gates
that build artefacts, and the rule outlives the fix for anything copied from them.

**`/var/tmp/kickos-trap-redzone-<preset>` IS KEPT PER PRESET AND REUSED, AND IT DOES NOT RE-DERIVE
Kconfig.** Without removing it between two readings the second reading IS the first. That is how a
red was confirmed to be the same red across a rebase rather than a new one wearing its name: with
the directory removed, taking two milestones underneath moved the reachable node count and the
unenforced SVC reading, while the eleven bound sites and every enforced class were byte-identical.

**AND THAT RED WAS A BINDING JOB AND NOT A DEPTH ONE, which is the reading it costs most to get
wrong.** `pizero2350-amp`'s `trap_redzone` was unbound indirect sites, nine in `console_tx.cc` and
the two `chip_rp2350.cc` bootrom pointers. Every ENFORCED depth class was inside budget throughout;
the two classes that read OVER are the ones this image does not enforce, the entry design they
describe not being compiled here. **Reading that red as "the trap path got too deep" sends the next
session at the wrong thing entirely.** The set was taken from what the gate REPORTED rather than
copied from a sibling preset, this gate hard-failing an over-declaration as loudly as an
under-declaration, which is what says the set is exact.

**AN AUDIT'S SEVERITY CAN STAND ON A WRONG MECHANISM, AND THE NEXT READER RE-DIAGNOSES IT.** Two of
this branch's audit claims were misattributed.

- **The gawk-only conversion does NOT fail silently.** Measured end to end on the real two-ELF
  partition: `mawk` exits 2 and busybox `awk` exits 1, both BEFORE a single record, so the script
  produces nothing and its existing emptiness check refuses. The severity stands and the failure is
  a **REFUSAL NAMING THE WRONG THING**, `has no loadable segment`, which accuses the ELF instead of
  the awk. Nothing in the tree could see it because `awk` on a Debian developer box IS gawk.
  `tests/static/check_awk_portable.sh` makes that non-silent, and it found a second instance nobody
  named, in `tests/static/check_amp_elf_agree.sh`, where the same call sat inside a gate's span
  arithmetic and would have had that gate report its own clause instead of the awk.
- **The 2^53 awk-precision warning does NOT apply to a `PT_LOAD` address**, so inheriting it would
  have bought a fix for a defect that is not present. Checked against real kernel-half addresses,
  which round-trip through a double EXACTLY: a `p_paddr` is at least page-aligned and the spacing of
  doubles at that magnitude is 2 KiB. The warning is about a SUBTRACTION that materialises a small
  delta out of two large addresses. The merge reads addresses as hex STRINGS and never converts one;
  the span arithmetic that genuinely adds is the shell's, which refuses a `p_paddr` at or above 2^63
  by name rather than wrapping.

**THE RESYNCHRONISATION OWNS ITS RECORDS' DEATH, AND THAT IS WHY A RECORD CARRIES A GENERATION.**
N6f rules that a call ring slot is reclaimed when the reply is sent, and the record IS that slot, so
whatever destroys the slot destroys the record. The depth reset is the one path that destroys a slot
with NO reply to spend it, and unlike the reply path it frees a record whose CAPABILITY IS STILL
LIVE. Left standing, such a record refused a seat to every later call landing on its masked slot, so
that far caller reached a service with no reply capability and waited out its own deadline; and its
holder's release landed on the same masked index ONE WRAP LATER, a different call whose reply was
still owed. **The arm reproduces that aliasing BY CONSTRUCTION and not by chosen numbers**: the
forge jumps the far head by a whole multiple of `RING_SLOTS`, which is a power of two, so the tail
the reset adopts masks back onto the abandoned record's slot at ANY ring width. Anything but a whole
multiple would be an arm that starts testing nothing the first time someone resizes the ring.

**WHAT THE ARMS STILL DO NOT REACH.** Every forge names the lowest node that is not this one, so the
inbox, the inbound records and the strike count are exercised at ONE sender index and at no other;
those records are keyed per ordered pair and are correct, and it is the arms' reach into them that
stops at one row. Widening the forges is a milestone of its own and was deliberately not attempted.
The send-side tail forge's residual is a LOST UPDATE with no reachable red, no lock spanning two
kernels. And the deferred-delivery clause is witnessed while its TIMING is not: the real bring-up
window, between node 0 releasing its peer and that peer seating, is a race node 0's userspace cannot
reliably enter.

**AN EXPECTED DROPPED-REPLY COUNT IS THE PARTITION'S WIDTH AND NOT A CONSTANT.**
`t_amp_far_reply_guard` needs a THIRD RING for the wrong-ring forge, and a partition of two has
none. Beside it, an arm that counts dropped replies must have nothing of its own in flight: a far
SEND to a port a peer THREAD serves is answered by that thread, that answer names no caller, and it
lands as a dropped reply at a moment the sender does not control. A peer that is a WINDOW LAYER
never produced that stray, which is why the arm was sound for as long as it was held. **And a far
call finding nothing parked is refused on the spot (N6f)**, so an arm that sends before it calls is
calling the peer while it is still replying to the send: harmless against a window layer, wrong
against a thread, and the order is load-bearing.

**THE SHARED IMAGE'S PEERS CONSTRAIN WHAT THE PARTITION MAY NAME THERE.** They run a service body
and no kernel, so they bind nothing: a call to a partition port at one is dropped and only the
window layer's echo comes back. On the own-image pair both nodes run kernels and both entries reach
threads. That is why that board's crossing list reads as an odd list until the reason is stated.

**A SHAPE DIFFERENCE BETWEEN THE TWO DOORBELL BACKENDS IS A PORT THAT IS OWED, not a line nobody
wrote.** The armv8a backend builds its doorbell half whenever something rings it, which above one
core is a shared kernel's peers and at one core is an AMP node's peer nodes; the rv64imac twin is
guarded on the core count alone, so an own-image RV64 node gets no doorbell backend and fails to
LINK, loudly, which is the shape the seam intends. Matching it is a port to a backend no board
selects.

**THE ENDPOINT POOL IS A PARTITION COST AND THE FLEET DEFAULT DID NOT COVER IT.** Every listed
crossing claims an endpoint slot for the life of the image, and the default of 4 left the AMP
boards' apps with one, **which presented as `kos_endpoint_create` failing in twenty unrelated arms
rather than as anything about AMP.** A partition the pool cannot seat is refused at configure now.

**TWO CLAIMS HAVE NO REACHABLE RED, and saying so is worth more than claiming them.** The positional
derivation of the partition's port capabilities is defended by a BOOT PANIC and not by an arm: a
dynamic install into root ahead of the partition's shifts every constant by one and the seating's
own handle check refuses to boot past it, so the arm witnesses the ROLE half and the panic the
POSITION half. And the ambient-handout mutation trips the capability table's own already-live assert
and kills the run, so that column stands as a positive statement about what a non-root task sees and
not as a mutated claim.

**A TWO-KERNEL GREEN RUN WITNESSES LESS THAN A ONE-KERNEL ONE**, the console interleaving being a
fresh draw each time, and this vehicle's standard is ten runs. **A gate may not read the quieter
node's console line**: both kernels write one console with no lock between them, so the peer's
single banner is regularly cut in half by node 0's TAP traffic, and a gate that greps for it is
measuring the console. The peer is witnessed through node 0's own reading of the peer's counters
instead. CI runs the gates ONCE as a regression pin, and that job **may not join the `--repeat
until-pass` set**: retrying an interleaving-sensitive gate masks the exact class this vehicle exists
to find. N6f and N6h carry both as rules. And the ping-pong is NOT a witness that two kernels are
isolated: they share one machine and one console.

**AND THE CI JOB'S POSTURE PIN IS WHAT KEEPS IT FROM PASSING VACUOUSLY.** Every AMP gate is
registered by a CMake clause keyed on the posture, so a preset that LOST the posture does not FAIL
those gates, it stops registering them, and ctest then passes on a run that covered plain arm64
under another name. `.github/scripts/amp-pin.sh` reads the partition's own generated description and
compares it against LITERALS the job states, so nothing in the expectation derives from the knob
being checked. Exercised in both directions: a plain arm64 build is refused for stating no crossing,
a node 0 build used where node 1 is needed is refused by name as N6c's collapse, an own-image build
pinned as shared is refused, and the two correct pairings pass.

**THE GEOMETRY REFUSALS BELONG TO THE TABLE AND NOT TO THE PARTITION.** The three geometries that
used to link cleanly and fault at the first touch of unmapped RAM are refused in
`arch/arm64/chip/virt_arm64/startup.S`, each clause naming its own field, because every derivation
they guard is a truncating division or an index fixed by which level-1 entry that table hangs off.
They are NOT beside the geometry in `CMakeLists.txt`: that site is arch-agnostic and also serves a
part whose whole share is smaller than one 2 MiB block of this one.

## M7.12: three nodes wide, and two kernels conversing on RP2350 silicon

`docs/design-multicore.md` N6h states the two-ELF obligation over the partition and
`docs/design-rp2350.md` carries the port's machine facts. What follows is only what a green run and
those documents do not say.

**THREE NODES CONFIGURE, LINK, MERGE, AGREE ON THEIR WINDOW AND PASS THEIR OWN SUITE.** One
node-agnostic app serves both peers. **What that pass does NOT say is that the crossing has been
exercised under concurrency at this width**: the two interleaving-sensitive vehicle gates are still
not run at width three, deliberately, the width's own claim being link geometry, and the three-node
CI job configures, builds and runs the comparator alone for that reason. Each node's suite is its
own image's, so three green runs are three single-kernel runs beside a live partition and not one
three-kernel run.

**THE CAP TABLE IS WHAT WIDTH SPENDS, AND IT IS OWED BY WHOEVER TAKES WIDTH PAST THREE.** Every
entry of `CONFIG_KICKOS_AMP_PORTS` claims an endpoint slot for the life of the image, so the supply
`CONFIG_KICKOS_MAX_ENDPOINTS` states has to cover those plus whatever the app holds beside them, and
width three leaves ONE spare where amp2 leaves two. **Running short does not present as a width
problem and names no node count**: it is refusals out of `kos_endpoint_create`, `kos_irq_claim` and
`kos_sem_create`, with generation and bit-pattern assertions failing downstream of them on objects
that were never created. A suite that passes says the arms fit the table as it stands, and nothing
about the margin left.

**AND READING THAT FAILURE COUNT IS ITSELF A TRAP.** `ctest --output-on-failure` reprints the whole
failure list in its recap, so a `grep -c 'not ok'` over the output returns DOUBLE the real figure.
**Read the suite's own `# N test(s) failed` tally, never a grep over the runner's output.**

**A NODE THAT ANSWERS NOTHING IS A DOORBELL QUESTION BEFORE IT IS A WINDOW QUESTION.**
`KICKOS_RP2350_SIO_IRQ_BELL` is CORE-LOCAL, so every node opens its own line in `arch_init`. An
image that leaves it masked still reaches `kickos_rp2350_doorbell_service`, but only through the
`doorbell_poll()` inside its OWN `arch_ipi_wait`, which is to say only while it is itself initiating
a rendezvous: a peer parked in `kos_recv` is never woken, every far call waits out its deadline, and
the AMP arms fail exactly as they fail with the peer PARKED. **The two are told apart by whether the
peer's own counters move**, which the shared region carries and the console does not.

**THE SILICON RUN THAT SAYS THE PEER SERVES IS `1..125`, 125 ok, 1 skipped, 0 partial, 0 failed.**
RE-MEASURED 2026-09-06 after the app-alive cell landed, on the merged two-node artefact
(`amp_partition_selftest` off `pizero2350-amp2-n0`, selftest as node 0 and `ampecho` as node 1)
flashed over BOOTSEL and captured on GP4 / UART1. The banner stamps `7192ddfd` with NO `-dirty`, so
this capture is attributable to a committed tree, which is the only identity that survives a squash.
The plan comes off a runtime counter (`tests/tap/tap.cc`), so that figure is a property of what ran
on the board and is derivable from no build here; what a build DOES state is `KICKOS_TAP_MAX_TESTS`,
which read 125 for this posture, so the registry's size and the plan the board printed agree. The
arm that moved it from 124 is `amp_app_alive`. **And it is node 0's
suite run beside a live peer, never the peer's own**: the peer has no console of its own, so what
witnesses it is node 0 reading the shared region.

**THE PEER EVIDENCE IN THIS CAPTURE IS THAT TWO ARMS DID NOT SKIP.** `amp_far_call` and
`amp_far_reply_guard` are the arms that decide at RUNTIME off the peer's own serviced count, and the
posture DECLARES them as permitted skips because the same image run standalone finds nobody. Both
reported `ok` here, so the hand validation prints them as expected-skips that never fired. That is
worth more than the peer's banner, which arrives on the same wire and cannot be relied on.

**THE BOARD'S ONE SKIP IS `amp_deferred_doorbell` AND IT IS NOW DECLARED**, so the shared verdict
script accepts this capture. It skips on this chip by construction: its probe returns negative where
a doorbell raise names its peer with one register write and keeps no seat to clear.
`KICKOS_EXPECT_SKIPS` names it where `KICKOS_CHIP` is `rp2350` and nowhere else, since a part whose
doorbell does keep a seat must run the arm.
**WHAT STILL READS NOTHING IS THE BOARD'S CAPTURE ITSELF**: `pizero2350` registers no arm that boots
the artefact, so the verdict is a HAND run and no gate would catch the next declaration that rots.
The hand command, and the two figures it needs that no ctest supplies here:

    EXPECT_SKIPS="amp_far_call,amp_far_reply_guard,amp_deferred_doorbell" \
    EXPECT_PARTIALS="grant_reserved" \
    sh tests/integration/check_tap_stream.sh <label> 125 < <log>

Both lists are DERIVED from `user/apps/common/selftest/CMakeLists.txt` against this posture's
resolved config (one kernel core, MPU, own-image AMP, chip rp2350); nothing in a `pizero2350` build
tree prints them, because no test here consumes them.

**THE PEER'S OWN BANNER INTERLEAVED WITH NODE 0's TAP STREAM BY WHOLE LINES, not by bytes.** Both
banners are in the one log, alternating line for line with the `ok` lines around them, and
`ampecho: node 1 echoing on port 3` arrived intact. Two banners in a capture is normally the tell
that two RUNS landed in one log; here it is the two kernels, and the build stamps are what separate
the readings.

**A PORT WRITTEN BLIND BECAME TWO KERNELS CONVERSING ON SILICON, and what made it tractable is worth
more than any of the defects.** The peer has no console of its own, so every failure looked
identical: a hang, a truncated line, silence. Three runs were spent trying to READ the peer from
outside through its debug window and none produced a believable value. **What worked was making the
peer report on ITSELF** into the region both nodes already share by design, which the primary then
reads and prints. That is DIRECT evidence the peer ran, where an external register read could at
best have said what state it was in. **Anyone porting a part whose peer has no console should reach
for the shared region first and the debug window second.**

**THE HANDSHAKE COMPLETES AND THE PEER EXECUTES.** The primary's own report reads `seq=6`, so all
six launch words were echoed, and the peer then wrote its third progress marker into the shared
region, meaning it reached its reset handler, ran its C-runtime init and got PAST its own
`arch_init`. **The reading is reconstructed and says so**: the marker's line arrives interleaved, so
its digits were read through corruption, and the visible characters match the third marker and
cannot be the seed value or either earlier marker, which differ in every digit.

**AND INTERLEAVED CONSOLE OUTPUT IS NOT EVIDENCE THAT THE PEER IS EXECUTING.** The primary ALONE has
two writers, the buffered ring drained by an interrupt and the synchronous writer going straight to
the UART FIFO, and those two interleave by themselves. That reading was taken as peer evidence once,
and it was the appealing conclusion rather than the supported one.

**WHICH NODE OWNS A PERIPHERAL IS A PROPERTY OF THE PARTITION, AND EVERY IMAGE RUNNING ONE
`arch_init` TOOK IT FROM A PROPERTY OF THE IMAGE. That is the most transferable thing here.**
`arch_init` on this chip releases peripheral resets, runs `clocks_init`, resets UART1 and
re-initialises it. A peer doing that re-enters the clock-tree setup while its primary is executing
off those PLLs, and drives the console UART through a reset while the primary is mid-transmission;
stopping `clk_sys` is documented as an unrecoverable chip lock-up. Both present as the PRIMARY
wedging. It is fixed, and validated by a run in which the peer got past `arch_init` while the
primary was still printing. **It is invisible to every single-image build and to both emulated
postures**, because on `qemu-arm64` no image owns a clock tree, so **any future own-image port on a
part with global bring-up meets this on its first two-kernel boot.**

**AND "THE CONSOLE TRUNCATES MID-LINE" MEANS CORE 0 DIED, NOT THE PEER.** That is the reframing the
whole diagnosis turned on. The console is a ring drained by an interrupt, so visible text LAGS
execution and a truncation means execution went PAST the last printed line and then stopped hard. A
peer's fault does not stop the primary; only a handful of documented mechanisms let the peer take
the primary down with it. **So the question is not "why did the peer fault", it is "what did the
peer do to the primary".**

**THE MISSING `SEV` WAS THE FIRST REAL FIND, AND A CODE LISTING THAT CALLS A HELPER HIDES WHAT THE
HELPER DOES.** The datasheet's own listing pushes through `multicore_fifo_push_blocking()`, an SDK
helper that signals the event itself, so the only SEV the listing SHOWS is the extra one before a
zero. The peer parks in WFE between reads, so a push carrying no event leaves it asleep. **And an
SEV is NOT architecturally inert on this part**, the event flag being sticky and cross-wired between
the cores, which corrects a reading taken from the generic architecture.

**FOUR CANDIDATE CAUSES WERE RULED OUT BY READING THE SILICON RATHER THAN THE DATASHEET.**
`ACCESSCTRL.FORCE_CORE_NS` is 0, so the peer is not forced non-secure onto the other SIO bank;
`ACCESSCTRL.ROM` is `0xff`, so it may still fetch its wait loop from ROM; `PSM.FRCE_OFF` is 0 and
`PSM.DONE` carries PROC1, so it is powered and released. The tree writes none of those registers, so
each sits at its reset value, and that was CONFIRMED rather than assumed.

**AND THE RCP SALT WAS THE WRONG SUSPECT, WHICH IS WORTH RECORDING BECAUSE ACTING ON IT WOULD HAVE
DESTROYED BOTH CORES.** The core-1 wait does begin by waiting on the salt, but the bootrom seeds
BOTH cores' salts on core 0's own path, unconditionally, before any flash image is entered. Writing
an already-valid salt raises an RCP fault, and a fault on one core immediately faults the other
until a warm reset. **A hypothesis about a register that looks unset is not a licence to write it.**

**THE MPU LOCKDOWN IS NOT SETTLED BY THE DATASHEET AND IS NOT A LEADING SUSPECT.** The bootrom does
lock core 1's MPU execute permissions to a small ROM window, stated once and nowhere else, with no
region, register, bank or PRIVDEFENA detail and no statement of what happens to it at launch. But
`MPU_CTRL.HFNMIENA` resets to 0, so the MPU does not filter a handler at priority below zero: a
confined peer should land in ITS OWN HardFault handler rather than lock up. **The right treatment is
the ATRANS method**: have the peer program its own MPU as its first act rather than inherit
anything, which makes the question moot instead of answered.

**THE WORDS HANDED TO THE PEER ARE VERIFIED, AND STATICALLY, so they are off the suspect list.**
Node 1's vector table at its own text base reads `word[0]` as its `_estack`, exactly what the link
assert bounds, and `word[1]` as its `Reset_Handler` with the Thumb bit set, which the datasheet's
own listing warns about. **A `PC=0` in a fault frame is therefore NOT the entry this node hands
over.**

**TWO FIGURES THE BOARD PAID FOR.** The SIO FIFO depth is FOUR, measured by filling it, which
settles the datasheet contradicting itself at four and eight. And a capture opens with UART framing
noise at reset, a short garbage run before the banner, **which would trip a gate anchored at the
head of the stream.**

**THE BOARD IS UNRECOVERABLE WITHOUT A HAND ON IT once an image hangs**: no `2e8a` device,
`picotool` cannot see it, `picotool reboot -u` needs a device already listening, and there is no
full-reset option. **`KICKOS_SHUTDOWN_TO_BOOTLOADER` is what makes iteration possible at all**, and
what it buys is exact: the board returns to BOOTSEL on a CLEAN EXIT, so a completed run, a refusal
and a fault that reaches shutdown are all self-recovering. **It does nothing for a hard hang.** Its
value is that a wedge becomes the only outcome costing a hand. It defaults to n and depends on the
selftest, so the own-image AMP defconfig states it rather than inheriting it. **And flash a non-AMP
image FIRST, every time**, so a later silence can be attributed: a board that boots and cannot speak
is indistinguishable from one that did not boot.

## M7.12: the premises, the provisioning, and the defect, now attributed

**THE ATRANS PREMISE IS NOT AN INFERENCE.** The whole two-image layout needs flash mapped 1:1,
because node 0 reads its peer's vector table out of flash to launch it. The reset values ARE an
identity map and the bootrom is documented to reprogram them only "when the booted image is inside
of a flash partition", and this partition uses none, but the document never says in one sentence
that the no-partition-table path leaves them alone. So the image calls `flash_reset_address_trans`
itself and the premise stopped being a premise. **A `ROLLING_WINDOW_DELTA` item in node 0's
IMAGE_DEF would reopen it**, which is why its absence is a requirement rather than an accident. The
restore running ahead of the banner without faulting or wedging XIP is NOT a witness that the
mapping IS identity, which needs the four ATRANS words read back.

**AND THE ROM CODE PACKING ORDER IS NOT IN THE DATASHEET AT ALL.** It gives the packing function
with parameters `c1` and `c2` and lists each function's code as a character pair, and never maps
"printed first" onto `c1`; no packed constant appears anywhere to cross-check, and there is no
documented return for an unknown code, so trying both orders is not a fallback either. The spelling
used rests on `arch_reboot`'s `'R','B'`, which has run on this bench. **That is a witness standing
in for a specification, and it is the only thing holding two new ROM calls.**

**THE PINNING REFUSAL IS A PROPERTY OF THE POSTURE AND NOT OF THE PART**, and a single-image kernel
on this chip may pin freely. It is a gate rather than a Kconfig refusal because nothing in the tree
pins, so there was no knob to refuse. **The datasheet does offer a partial mitigation** (clean does
not unpin, invalidate-by-address confines the damage, and pinning outside the QMI range avoids flash
entirely) **and it does not lift the refusal**: the whole-cache flush unpins everything wherever it
lives, and the mitigation is a discipline no gate in this tree can hold.

**AN EMULATOR BOOTS A BARE ELF AND THIS PART'S FLASHING TOOL VALIDATES IT.** `picotool` refused the
merged artefact with `failed validation - Unrecognized ABI`. Cause measured by comparing headers: a
node image carries `OS/ABI UNIX - System V` and a hard-float EABI flags word, and the merged
artefact carried `OS/ABI ARM` and a zero flags word, because the binary-wrapped node blobs carry no
EABI attributes for `ld` to propagate. So N6h's "the artefact is an ELF" holds for QEMU and not for
the RP. Fixed by taking the identification byte and the flags word out of node 0's image as every
other fact there already is; the early bring-up runs were obtained by flashing the two node images
SEPARATELY, which is an instrument and not the deployment N6b rules for.

**A NODE'S SHARE ON THIS PART IS 240 KiB AND IT CANNOT HOLD BOTH BUDGETS AT THE FLEET'S DEFAULTS.**
The arena floor is the thread count times `KICKOS_USER_STACK_SIZE`, which at the default count is
136 KiB; that leaves about 64 KiB for appdata once kernel data is paid, a heap of about 52 KiB, and
the shared selftest wants more. **So an AMP node here serves a SMALLER THREAD POOL by arithmetic and
not by choice**, `KICKOS_MAX_THREADS` being narrowed in the posture's defconfig. **The POSTURE is
what narrows rather than the suite**, because the suite is the fleet's shared instrument and a
reduced app would have answered whether a small app fits rather than whether this posture works; the
precedent is `f302nucleo-st`, which states a smaller pool for the same reason one layer in.

**APPDATA AND THE ARENA CONSUME ONE SPAN FROM OPPOSITE ENDS, so neither figure can be read without
the other**, and every slot dropped from the thread pool frees a stack from the floor plus its
kernel stack. **A future change to `KICKOS_MAX_THREADS` or `KICKOS_USER_STACK_SIZE` on this posture
moves a wall that is already touching**: the arena assert defends only the arena's floor for DEFAULT
stacks, so an app asking a larger stack, or one more thread, has to find the room inside the margin.

**AND THE APPDATA FIGURE LIVES IN THE PRESETS AND NOT IN THE DEFCONFIG, because it cannot live
there:** `KICKOS_APPDATA_SIZE` is a CMake cache variable read by the link script and is not a
Kconfig symbol, so a defconfig cannot state it. Both node presets carry it, and the peer receives it
through cache inheritance rather than by being told twice.

**THE MEASURED HEAP REQUIREMENT CORRECTS AN INFERENCE, AND THE INFERENCE IS THE INSTRUCTIVE HALF.**
The suite was believed to want 116 KiB of heap because that is what the NON-AMP build has, and **a
build HAVING an amount is not the same as needing it.** The suite completes on 105 KiB, measured.

**AND AN INVERTED LADDER IS BLIND PAST THE POINT THE SUITE STOPS.** Walking DOWN for the smallest
appdata that passes, rather than up for the largest the link permits, is the right search; but while
the suite still stops early, two adjacent values can reach the SAME arm and the smaller one is not
thereby the winner, since an arm past the stopping point could need heap the smaller value cannot
give and no run can currently see it. **Choosing the smaller figure there would be provisioning
against an unobservable.** Re-walk the ladder once the fault below is fixed.

**16 KiB WAS NEVER A CONSIDERED FIGURE.** It was the value that linked when the chip default of 128
KiB was refused by the arena assert, chosen with no middle tried. The link was the authority both
times. **And the squeeze was never the window**: two nodes plus the shared region end precisely at
the striped SRAM end, so the partition's TOTAL span has no margin at all and is guarded by a link
assert that names it, while inside each node's share there is real headroom and the shared region
uses under a third of what it reserves.

**THE MILESTONE'S CENTRAL DEFECT IS `SCB->VTOR`, AND IT IS ATTRIBUTED AND FIXED.**
`chip_rp2350.cc` set VTOR from a hard literal equal to node 0's image base. Node 1 links elsewhere,
`arch_amp_release_peers` had already handed core 1 the correct vector address over the SIO FIFO,
node 1's own `Reset_Handler` threw it away, and nothing else on this chip writes VTOR. So every
exception core 1 took vectored into node 0's image and ran node 0's handlers against node 0's SRAM:
`g_arch_current`, `g_arch_next`, the `Kernel`, the kernel stacks and every node-0 `Thread`. The
context switch then wrote core 1's PSP into a node-0 thread's saved SP, both cores wrote
`g_arch_current` with no lock between them, and the kernel-core index folds to a constant under one
kernel core. The fix takes the address from the vector symbol, as the other Arm chip in the fleet
already did. `docs/design-rp2350.md` carries the mechanism.

**AND IT IS INERT IN EVERY POSTURE THAT IS NOT THIS ONE, WHICH IS WHAT LET IT LIVE.** A parked peer
never reaches `Reset_Handler` at all, and on node 0 the literal was correct. A wrong vector table
costs nothing until the core holding it takes an exception, so single-image RP work will not meet
it.

**HOW IT WAS FOUND, IN ONE LINE, BECAUSE THE METHOD TRANSFERS AND THE STORY DOES NOT:** a ladder of
peer images between a parked peer and a full one, one change per rung, node 0 byte-identical
throughout.

**WHAT "PARKED" MEANT CONCRETELY, because a peer must be PARKED rather than ABSENT.** A standalone
image at the peer's own text base holding nothing but a vector table and three instructions: word 0
the peer's own stack top, word 1 its entry with the Thumb bit set, and an entry that masks
interrupts and sleeps forever. No kernel, no chip init, no console, no scheduler. An ABSENT peer
leaves erased flash where the vector table should be, so the launch is handed a stack pointer and an
entry of all-ones and the run tests a jump into garbage rather than the absence of a peer.

**`0xf0000000` IS A CONSEQUENCE AND NOT A CLUE.** The bus fault that address named is producible by
no arithmetic anywhere in the tree: it is simply whatever the other core last left in the saved SP,
and it is unmapped on this part, which is why the read faults instead of returning rubbish.
**Hunting for arithmetic that produces it is wasted effort.** `docs/design-rp2350.md` states that as
a reading rule.

**BUS CONTENTION ON THIS PART IS MEASURABLE AND WAS NOT THE DEFECT.** A spinning parked peer
collapses node 0's XIP access rate by 81% and takes its misses from 3 to 28,359. That cost is real
and it is not what stopped the suite.

**THREE THINGS THIS FILE CARRIED ARE CORRECTED HERE.**

- **On this part the byte-level console shredding was at least largely the defect, and not two
  kernels sharing one UART.** With the fix in, the merged two-node run shows both banners intact and
  readable, and the shredding appears in both failing runs and in none of the passing ones. What
  still holds is narrower: the primary ALONE has two writers and those interleave by themselves, so
  an interleaved capture is still not evidence that the peer executed.
- **`rr quantum ... advanced N during the burn` is node-local and was never a peer witness.** Both
  of that arm's workers are threads of THIS node, so the figure has no cross-node content at all and
  reads the same beside a peer that provably cannot write a byte. The arm names the other RR thread
  now.
- **The node vector table is NOT compiled out of the posture that needs it, and that claim must not
  be repeated.** The guard is correct: `pizero2350-amp` compiles the table and byte-checks it, and
  installing it on the own-image posture would park that peer's own SysTick, SVC and PendSV, which
  is a second bug and not a fix. What IS true is that nothing installs it and nothing starts a
  second core on that posture, so it is provisional scaffolding, and whoever writes that launch owes
  the VTOR write from the symbol.

**THE PEER BOOTS AND STILL SERVES NOTHING, AND THAT IS OWED.** Ten AMP arms fail with the fix in,
the same ten that fail with the peer parked, so a live peer buys the suite nothing yet. **The lead
is UNMEASURED in those words**: the SIO doorbell IRQ is never unmasked on either core and nothing
installs the doorbell ISR, which leaves the service body reachable only from a node's own IPI wait.
It was read, not measured. `TODO.md` carries it with the ten arm names and the instrument to take it
with.

**NOTHING SKIPPED UNDER A PARKED PEER, WHICH ANSWERS ONE QUESTION AND NARROWS A CLAIM.** The AMP
arms FAIL rather than skip there, because they decide at RUNTIME off the peer's own serviced count
and a parked peer reads exactly like a live peer that answers nothing. **So the derived-skip
machinery is AVAILABLE AND UNTESTED on this posture** and must not be read as "the skips are
derived": no run has reached one. Narrowing the pool cost nothing in coverage either, every arm
running and none skipping.

## M7.12: the instrument rules, and what is owed

**AN INSTRUMENT REPORTS NOTHING UNTIL IT HAS PROVEN ITSELF ON A KNOWN VALUE.** The self-hosted debug
reader writes two known words to SRAM itself and requires both to read back through the peer's
access port before it prints anything about the peer. It never got that far, so no peer register was
ever reported. Two earlier runs without that acceptance test produced peer readings that were PURE
ARTEFACT and would have been written down as findings, a pre-push `DHCSR` showing the peer awake and
a post-hang one showing it not locked up. **A PLAUSIBLE reading is the trap, not an implausible
one**: an arbitrary word inside a debug block already looked like a valid identification register.

**AND EVERY DIFFERENT TARGET READING ONE CONSTANT MEANS NO TRANSACTION WAS EVER SET UP**, rather
than a locked or absent component. That diagnosis sent three runs at power, locks and enables when
the cause was a register map addressed at the wrong offsets.

**THE OFFSETS WERE THE WHOLE PROBLEM, AND THIS PART'S MAP IS ADIv6.** The datasheet names ADIv6
normatively and deliberately does not reproduce it, and an ADIv6 memory-mapped access port addresses
its own CSW, TAR, DRW and IDR near the TOP of its block rather than at its base. The offsets first
used were ADIv5 DP-scanned addresses, so every read landed on an arbitrary word. **The datasheet's
own witness is the one such table it does print, whose standard CoreSight ID register sits at
`0xdfc` and not `0xfc`.** Two explanations are now EXCLUDED rather than outstanding: the CoreSight
lock-access register was written and changed nothing, and no lock-access mechanism is documented for
these ports at all.

**AND THERE IS NO POWER OR ENABLE STEP TO FIND: the path is SUPPORTED and this is a
documentation-by-reference gap, which must not be recorded as an unsupported instrument.** The debug
power request lives in the SW-DP with no memory-mapped equivalent; the only overrides are registers
of the one access port that cannot be reached from the system bus; the rest of the debug hardware
shares the power domain of the running software; and ACCESSCTRL permits a Secure privileged core by
default and would FAULT rather than return values if it did not.

**AT THE RIGHT OFFSETS THE PORT READS SANE AND IS ENABLED, AND THE TRANSFER STILL FAILS.** Its
identification and control words read plausibly and DIFFERENTLY, the control word's size field reads
WORD, and both flags the datasheet names for "will this port perform bus accesses" are set, DeviceEn
and SDeviceEn. The first address-register write followed by a data-register read then ends the run.
**So the port is present, correctly addressed and permitted, and the remaining fault is in the
TRANSFER itself.** A fourth attempt starts there rather than from scratch: the untried leads are the
in-progress flag between setting the address and reading the data, and the documented rule that a
transfer which stalls returns a bus fault after 65,535 cycles.

**THE FALLBACK RULE, because it decides when that work matters.** A SILENT self-report is where the
external read becomes necessary, since silence cannot separate "never executed" from "executed and
wrote nothing", and only an external read of the peer's lockup bit separates those. The runs were
not silent, so this work is deferred rather than wasted.

**ONE WRITER FOR THE WHOLE DIAGNOSTIC PATH.** A buffered console drained by an interrupt and a
synchronous writer going straight to the peripheral shred each other byte by byte. A settle loop in
front of a mixed path fixes the FIRST block only, which is exactly why it looks fixed, so the
durable form is one writer and not a delay in front of it. Use the SYNCHRONOUS writer for anything
diagnosing a two-kernel bring-up: the buffered one is drained by an interrupt, so a peer that wedges
the console destroys exactly the report that would explain it. **And attribute every capture by
BUILD STAMP**, since several images share a commit and a `-dirty` stamp is the only tell that an
instrument was in the tree.

**WHAT IS OWED, NONE OF WHICH NEEDS THE BOARD.**

- **The own-image presets are declared to `trap_redzone` and measure GREEN. Wipe
  `/var/tmp/kickos-trap-redzone-<preset>` before re-measuring**, that gate reusing its tree and not
  re-deriving Kconfig. Declaring them measured the posture for the FIRST TIME and opened two
  reachable cycles, both now broken in the code. What a re-run does NOT re-derive:

  - **NEITHER CYCLE COULD HAVE BEEN DECLARED AWAY, AND NO DEPTH FIGURE COULD HAVE ABSORBED THEM.**
    The tool reports the cycles of the BARE walk beside the excluded one, so the `exclude` set never
    reached them; and SVCK measured 1268 bytes against a red zone of 1492, on a
    `KICKOS_KERNEL_STACK_SIZE` of 1008 with 1004 usable above the canary, on a posture whose whole
    node share is 240 KiB. **A gate finding that no declaration and no number can close is a CODE
    finding.**
  - **THE DEPTH OVERRUNS WERE THE CYCLE**, not a second finding, and the fix proved it rather than
    arguing it: the walk charged the cycle prefix 380 bytes, EXITK measured 956 with the cycle and
    576 without, and 956 - 380 = 576. After the fix both own-image nodes read the sibling
    single-image preset's figures to the byte.
  - **THE DOORBELL CYCLE'S CLOSURE WAS NEVER REACHABLE, AND THE FIX IS STRUCTURAL ANYWAY.**
    `arch_doorbell_core()` is the literal `KICKOS_AMP_SELF_CORE` on this posture
    (`arch/include/kickos/arch/doorbell_cells.h`), `amp::ring` has exactly one caller, `amp::send`
    refuses `to == me` before it, and `CMakeLists.txt` refuses at configure any node-to-core map
    putting two nodes on one core, so the self bit could not be set from `amp::ring` at all. **It
    was still a real edge in the compiled graph, which is what a callgraph bound is about**, so the
    self raise goes to this core's own doorbell input set now and the service is carried by its own
    doorbell interrupt. The shared-image branch keeps the inline poll.
  - **The bootrom call site in the reset path needs NO record**, against what this list expected.
    The `kickos_rp2350_xip_identity` sites ARE in both own-image graphs and NONE of them is
    reachable from a trap root, so the gate asks for nothing there. What the two own-image scopes
    did need is the `console_tx.cc` sites and the two bootrom sites of `arch_reboot`.
  - **`arch_reboot`'s failure tail no longer panics, AND THE SAME SHAPE IS LATENT IN
    `chip_rp2040.cc` AND `chip_imxrt1062.cc`.** It reported through `kickos::kpanic`, which ends in
    `kfault_terminate`, which is what called it through `kickos_bootloader_handover`; the once-flag
    there bounded that at runtime and **no walk can follow a flag.** It reports through the polled
    writer and calls `arch_shutdown` now. Those two other chips' tails still panic, and it closes no
    cycle there ONLY because `amp2` is the one variant in the tree setting
    `CONFIG_KICKOS_SHUTDOWN_TO_BOOTLOADER`, so `kickos_bootloader_handover` compiles to an empty
    body on every other preset. **Fixing them shifts line-bound bindings in `chip_rp2040.cc` and
    needs its own measurement pass.**
- **A KNOB STATED IN A PRESET DOES NOT REACH A PEER NODE.** Peers were configured from board,
  variant and node index alone, so anything in node 0's preset was dropped for every other node, and
  **the two-ELF comparator cannot see it**: such a knob changes what sits INSIDE a node's share,
  which no geometry clause compares. It was found only because that knob's link REFUSES; one that
  merely changed behaviour would have shipped.
- **A GREP PROVES A CALL EXISTS; ONLY THE EMITTED CODE PROVES A WRITER DOES.** A shared-region clear
  was claimed by a comment and called by nothing. The fix was verified by DISASSEMBLING the reset
  path and finding the byte-store loop with the region's bounds as literals: `nm` reports no symbol
  because the function inlines, and a call-site grep would have said "called" for either state.
- **A GATE THAT READS A FIXED FIELD OF A TOOL'S OUTPUT IS PINNED TO THAT OUTPUT'S WIDTH.** `readelf`
  right-aligns a section index in two characters, so a single-digit section shifts every later field
  by one. The pair-shaped comparator had been correct only because every image it had ever read
  numbered that section in two digits.
- **A GATE REGISTRATION KEYED ON THE WRONG PREDICATE FAILS IN WHATEVER WAY ITS PREDICATE DOES, and
  the two ways look nothing alike**: one silent skip and one hard configure failure, same class, in
  one milestone. `docs/design-rp2350.md` carries the pair.
- **BOTH OBLIGATIONS THIS MILESTONE OPENED ARE DISCHARGED, AND THE VEHICLE HALF IS WORTH MORE THAN
  ITS GREEN.** The twelve-preset regression is taken, every preset measured from a scratch tree
  wiped first. `amp_peer_arms` and `amp_partition` each ran ten of ten on `qemu-arm64-amp2-n0`,
  individually and never under `--repeat until-pass`, and what the ten draws bought is the NEGATIVE
  a single run cannot give: no flakiness of any shape, duration flat at runs 1, 5 and 10, and **not
  one draw showed the quieter-node console symptom the ten-run standard exists to catch.**
- **`qemu_arm64_selftest` FAILED ONCE ON `qemu-arm64-amp3-n1` AND ONCE ON `-n2` IN ONE PASS, AND
  THAT IS NOT ROOT CAUSED.** Nothing was changed for it and nothing here says it is fixed. It has
  not reproduced since, over 40 isolated draws of that arm on each of the three nodes, 30 rounds
  with the three nodes' arms running at once against their own build trees, and three whole-preset
  passes per node. The standing suspicion is the three-kernels-on-one-console interleave this
  posture already draws on, and no draw since has shown it. **A concurrency arm that pointed two
  runners at ONE build tree is not evidence either way**: ninja raced itself there and the arm never
  ran, so every draw of it measured the harness.

## M8 opened: what the cut established and a green run will not say

- **THE AUDIT M8 WAS CUT FROM IS NOT IN THE TREE, AND THAT IS WHY `TODO.md` NOW CARRIES ITS
  FINDINGS.** The verified audit of M7.10 through M7.12 was an external canvas, outside the
  repository and outside git (its location is in `CONTEXT.local.md`, this file shipping no personal
  path), so a citation to it carries no content and a session without that file cannot act on a
  reference to `SM-2` or `AMP-1`. Every finding was itemised into `TODO.md` with its own evidence
  for exactly that reason. **Do not go looking for the canvas, and do not read its absence as work
  having been lost**: the tree is the record now, and the canvas is a superseded input rather than
  a missing one.
- **EVERY STANDING SPEEDUP ESTIMATE FOR THIS MILESTONE RESTS ON A TERM THAT SHRANK BY 4.7x, SO NONE
  OF THEM IS A PLANNING INPUT.** `docs/design-m5-ipc-fastpath.md` section 3.0.4 measured
  `MPU_APPLY` at 443 cycles a switch, 886 of a 3651-cycle locked round trip, and priced removing
  protection at `f = 0.457` and 1.37x. Section 8.5 of the same page measures the same board after
  the PMP precompute at 19 for `MPU_APPLY` plus 75 for `MPU_COMMIT`, **94 a switch and 188 a round
  trip**: the 443 was the pre-split phase carrying the commit work, and the phase NAME does not say
  so, which is how the stale figure kept being quoted as current. M7's own Amdahl figures (two
  cores at 1.31x, four at 1.55x) rest on the same 53 percent hold. `roadmap.md` is corrected;
  `docs/design-m7-smp.md` 215-217 still carries the old pair and M8.4 owns it. **No replacement
  multiplier has been computed on purpose** -- P0 owns it, and the whole phase table shares the
  defect, every figure predating trusted stacks, the MMU, the big kernel lock and word-wise
  `memcpy`, with the A53, rv64 and x86 carrying no cycle figure at all.
- **A MILESTONE TAG IN `TODO.md` CAN PREDATE A RENUMBERING, AND ONE CLASS OF THEM DID.** Three items
  there carried an `M8` tag from the era when four design documents and `TODO.md` had settled on
  "M8, the last one", which `roadmap.md` had already ruled against. Only one of the three, the
  RISC-V context-switch cost, belongs to IPC and IRQ optimisation on its merits; the other two are a
  kernel-confinement ladder and stay in `Later`. Retagged this pass. **The trap generalises**: a
  bare `M8` in a document is not evidence of an assignment, and `roadmap.md` is the only file that
  makes one.
- **A CI JOB CAN BE NAMED FOR A PRESET IT DOES NOT BUILD, AND ONE IS.** The job at
  `.github/workflows/ci.yml` 487 is called `pizero2350-amp` and its body configures
  `pizero2350-amp2-n$node`, the TWO-image posture. The shared-image `pizero2350-amp` preset is
  therefore uncovered behind a job bearing its own name, and **checking the matrix by job name
  concludes the opposite of the truth**. `gicv3` and `riscv64` are the honest half of the same gap:
  each appears zero times in that file, CI's `qemu-riscv` being rv32, so rv64 has five dedicated
  gates, a QEMU machine, a toolchain CI already fetches and no job at all. Derive coverage by
  grepping the preset name in `ci.yml`, never by reading the job list.

- **THE FIX PHASE WILL NOT BE WITNESSABLE ON HARDWARE FOR THE TWO DEFECTS THAT MATTER MOST.** SM-2
  is a kernel crash reachable only with two members of one task dying on two harts, which wants
  `qemu-riscv64` with four harts and has no board; AMP-1 needs a node with more callers than
  `RING_SLOTS` and a peer that has not drained its reply ring, which is a unit arm rather than a
  silicon draw. Both are emulator or unit work by nature, not a bench booking, and neither absence
  is a reason to defer them.

## M8.1: what the four lifecycle fixes established, and what they do NOT witness

- **`klock_leave` CANNOT FIRE THE SELF-RAISE AFTER A BOOKED SWAP, AND READING THAT ARM ALONE SAYS
  THE OPPOSITE.** Its guard is `r.depth == 0 and r.owed == 0`, and `klock_detach` sets `owed` when
  a swap is merely BOOKED, which is what armv8a and rv64imac do from an interrupt: the swap lands
  at the exception exit, `klock_attach` skips the re-acquire while `owed` stands, and the
  `klock_leave` that follows the booking therefore releases nothing and raises nothing. The release
  that ends that span is `kickos_switch_unlock`, and that is where a resched cell owed to THIS core
  is now raised. **This was got wrong in direction during M8.1** -- the fix was briefed as "let the
  existing depth-0 arm carry it", which is unreachable on exactly the backends the change targets,
  and the symptom was a cross-core slay timing out at `-KOS_ETIMEDOUT` with the victim keeping its
  core, indistinguishable from the unfixed tree. `klock_resched_ask` still strips self, so the cell
  remains the only publisher and `switch_book` sends nothing.
- **SM-4'S WIRING IS WITNESSED ON THE HOST, AND THE CLAIM THAT IT COULD NOT BE WAS WRONG.** It
  first landed unwitnessed on the reasoning that `arch/arm64/armv8a/aspace_armv8a.cc` is not
  host-buildable and that the only live-fire arm needs a caller mapping `R|X` over one VA from two
  cores, which the map editor refuses. **Half of that was cost dressed as impossibility**: the
  file's system operations now live one-per-instruction in `sysops_armv8a.h`, and
  `tests/unit/mapexec` compiles the editor itself at two cores against a seam, the same technique
  `tests/unit/deathspace` already used for `KICKOS_HAVE_ASPACE` code the host has not got. What was
  genuinely refusable stays refused: no arm fabricates an `R|X` caller, every pre-state is built by
  calling `arch_aspace_map`, and the architectural EFFECT (a PE re-executing already-fetched
  instructions) is unobservable under QEMU, so this reads the wiring and not the hardware.
  **Do not test `removal_owes_rendezvous` alone** -- it pre-existed the fix with one correct
  caller in `arch_aspace_unmap`, so a predicate arm covers the half that was never broken; the
  defect was that `map_into` never consulted it.
- **A NEW SYSTEM OPERATION IN THE ARM64 MAP EDITOR GOES IN `sysops_armv8a.h`, AND ITS
  SHAREABILITY IS THE CALLER'S TO PICK.** `check_tlbi_shareability.sh` decodes CRm out of the
  LINKED IMAGE rather than reading the mnemonic, so the header composes no form and each call site
  spends the one its posture owes. An `__asm` added back into `aspace_armv8a.cc` is invisible to
  the host gate that now compiles that file, and a maintenance op named without its shareability
  is a gate failure that reads as a codegen change.
- **`taskleave` IS A CRASH DETECTOR FIRST, AND WHAT IT IS BLIND TO IS NARROWER THAN ONE MUTATION
  SUGGESTS.** Reverting the `exit_current` CALL SITE alone leaves it printing `release peer hits: 0`
  and PASSING: the race wants the last member's release to land inside a peer's `cap_teardown`, an
  image cannot observe when a peer is inside its own sweep, so that shape is biased but not forced
  from userspace. **But no-opping `aspace_install_boot` ITSELF does redden it** (`release peer
  hits: 8` on `qemu-riscv64-smp`), so it is not blind to every revert of the mechanism, and an
  earlier note in this file that said so without qualification was too absolute. It also now
  carries `space destroys: N` against `ROUNDS`, so `hits == 0` can no longer be satisfied by a run
  where no destroy happened at all. The deterministic ordering control remains the host arm, which
  samples inside the sweep through the fixture's chunk gap.
- **M8.10 CAN SILENTLY UNDO SM-2, AND IT IS THE ONLY PLACE IN M8 THAT CAN.** Installing the boot
  root before the last member's release elides TLB maintenance for anything that edits the space
  afterwards, because a space installed nowhere makes `invalidate_page_if` skip. That is sound only
  while `arch_aspace_activate` is a full non-tagged flush on both translating backends, which it is
  today. **ASIDs make it false**, and ASIDs are M8.10's. Re-read SM-2's argument then rather than
  trusting that it once held.

## M8.2: the AMP window and far IPC, and what these green runs do NOT say

- **C5 AND AMP-1 ARE ONE FIX IN TWO HALVES, AND EACH HALF IS GREEN ALONE WHILE THE PAIR IS NOT.**
  The refusal answer publishes through `send`, which can answer FULL, so answering every post-take
  refusal only closes the untimed leak once reply-ring space is the admission for TAKING a call;
  and the admission only holds the ring's arithmetic once every taken call answers exactly once,
  refusals included. **What no single-fix run says**: each half passed its own presets, and the
  interaction surfaced only as two selftest arms failing on a merged tree
  (`amp_far_undisclosed`, `amp_far_infoless` on `qemu-arm64-amp2-n0`). The consequence that neither
  half's argument contains: **the reserve wall stands at RING_SLOTS ANSWERS, not at RING_SLOTS
  SERVED CALLS**, so a node whose every call is refused reaches it exactly as fast as one whose
  every call is served.
- **RESERVING A REPLY SLOT TURNED A LOST REPLY INTO A STRANDED CALL, AND THE ARM THAT SHOULD HAVE
  CAUGHT IT SUPPLIED THE MISSING PASS ITSELF.** A call declined for want of a reply slot is retried
  only from a service pass, and a service pass comes only from a doorbell: the peer's drain frees
  the slot but rang nobody, so a well-formed caller past the wall parked forever under
  `KOS_TIMEOUT_NONE` -- the very leak C5 said must not survive this sub-milestone, reintroduced by
  its own remedy. **The unit control called the service body by hand after the drain**, so it
  asserted the right positive outcome over a stimulus production has no source for. **THE
  GENERALISATION**: an arm for a mechanism reachable only by a raise must take the passes a real
  raise produces and no others, and a harness that can synthesise a pass will hide exactly this
  class. An external audit found it; two green fleet sweeps and 512 passing tests did not.
- **AND THE FIX BREAKS A FREEZE THAT READS AS UNIVERSAL, WHICH IS WHY IT IS WRITTEN DOWN RATHER THAN
  JUST DONE.** "A skipped raise costs latency and never a message" is true of a PUBLICATION, which
  sits in a ring the peer rescans before waiting. A credit return is not one: it changes a far-owned
  tail that only a reservation reads. **The obvious cheap gate is unsound and was nearly shipped**:
  raising only when the ring goes full to not-full misses the real shape, because the admission test
  is `used + owed` and `owed` is invisible to the peer that drains -- two answers published with two
  more owed refuses at four slots while `used` is only two. The credit-request alternative was
  refused on a stronger ground than cost: one cell has two writers and a clear can land on a set,
  which is a lost update and a permanent park, and the RMW-free form needs two cells and a full
  barrier on both sides because a credit request has no monotonic counterpart to the seated flag.
- **A SHIPPED ARM WAS STANDING ON THE C5 LEAK, WHICH IS THE STRONGEST EVIDENCE IT WAS REACHABLE.**
  `amp_far_reply_guard` parked on a port the peer's kernel BINDS and its app never receives on.
  Before the fix that shape was answered with silence, and the arm was built on the silence; the
  fix made it answer, and the arm broke. It now withholds the peer's doorbell seat deliberately to
  construct the old state. **The general form, and it is the third instance the invariants page
  records**: an arm written against a weaker environment is not validated by passing there, and
  what the stronger environment changes is not only WHETHER an answer comes but WHEN it comes and
  WHOM it names.
- **A NODE BOOTED ALONE IS A DIFFERENT ENVIRONMENT FROM A PARTITION, AND ONE ARM CANNOT WITNESS
  C5 THERE AT ALL.** Nothing drains the me-to-peer reply ring on a node with no peer, so a suite
  of forging arms fills it at `RING_SLOTS` and the admission then correctly refuses every further
  take. Arms therefore own their own reply-ring precondition rather than relying on registration
  order, which was a latent trap the milestone removed rather than re-sorted around. And
  `amp_far_reply_empty` is **C5-insensitive on a node booted alone by construction**: standalone it
  exercises the forge wake and not the publication, so the merged partition is the only place that
  arm witnesses C5.
- **THE VERDICT REACHING USERSPACE IS NOT THE SAME THING AS THE VERDICT BEING WITNESSED.** Giving a
  refusal its own code is what M8.2 did for both `CLASS` and `RESERVE`, and the reason is that a
  verdict folded into `EMPTY` cannot have a hardware arm: an arm asserting `EMPTY` passes whether
  the mechanism fired or the ring was genuinely empty. A code with no forge selector behind it is
  the same defect one step later, so the code and the arm land together or the code is decoration.
- **`RESERVE` IS THE FIRST VERDICT THAT NAMES THIS NODE'S OWN STATE** rather than which field a
  peer malformed, which is why it could not fold into `EMPTY`: a ring holding unread calls is not
  an empty ring, and reporting one as the other reports a peer's waiting traffic as absent.
- **THE RP2350 DOORBELL DESCENT IS UNMEASURED AND THAT IS NOW STATED RATHER THAN IMPLIED, and
  rooting it was tried and refused on evidence.** The body runs in handler mode on SP_main and no
  armv7m class is rooted there. Rooting it under `PENDSV` reports **524** bytes where the descent
  measures **740**, because a thread-stack class applies the panic-tail exclusions: the class
  choice alone moves the figure, so that root would compare a number against the wrong budget. What
  would bound it is a root set over every handler-mode entry measured against `_kernel_stack_size`,
  and the blocker is that the figure is a per-chip LINKER constant the gate cannot scrape.
- **A THREAD-MODE CALLER OF THE SAME DESCENT IS ALREADY REFUSED, BY THE CALLGRAPH AND NOT BY A
  CONFIG PREDICATE.** `arch_ipi_wait` reaches the node service through `doorbell_poll`, and the
  red-zone gate walks the callgraph from its declared roots, so a caller reachable from
  `syscall_dispatch` fails the build: a probe takes SVCK to **1276** against the reserved 1216. **A
  `static_assert` on the config pair was refused as a second truth and a weaker one** -- it refuses
  a posture, so a thread-mode caller arriving from AMP code at one kernel core walks past it, where
  the gate catches any caller from any root.
- **`pizero2350-amp2-n0` AND `-n1` SIT AT SVCK EXACTLY 1224 OF 1224, so the enforced figure has
  ZERO slack**, and the block has 8 bytes above the 1448 it needs. **This milestone already spent
  that headroom once**: the far-call work grew the deliver chain by 8 bytes and took the figure from
  1216 to 1224, which no per-preset check caught -- the red-zone pass ran before the far-call work
  existed, and the far-call pass verified on qemu presets, which do not enforce this class. Only the
  host fleet sweep crosses that boundary, and it is the reason to run it rather than trust the parts.
  The block did not have to grow, the posture's Kconfig default already standing at 1456, but the
  next 8 bytes on that chain do force it.
- **`pizero2350-amp` SELECTS NO AMP POSTURE, SO IT IS NOT A NODE**, whatever its name says. Its own
  defconfig claimed the posture derives from the core count, which the option's help text refuses
  as an N6c defect. Reading that preset as the shared-image node it is named for concludes the
  opposite of the truth, exactly as the CI job named `pizero2350-amp` does for the two-image one.
- **THE `PORT_REPLY` RULE IS NOW TOTAL ON BOTH SIDES AND UNREACHABLE BY CONSTRUCTION, so neither
  half can be witnessed without lifting a build gate.** The mint and the bind both refuse it, which
  is the asymmetry M8.2 closed, but `CMakeLists.txt` refuses port 1 for EVERY node at configure
  time, so no buildable partition reaches either. Witnessing it took lifting that CMake guard,
  naming port 1 in a partition and removing the mint's refusal so only the bind was in play: with
  the bind's refusal the node dies at boot on the port-seating panic, without it the node silently
  binds the reply port and runs. **`amp_port_bind_local` has no syscall route at all**, kernel init
  only, so no selftest arm can reach it the way `KOS_AMP_OP_MINT` reaches the mint.
- **AND THE HELP TEXT FOR THAT CONFIG CLAIMED A REFUSAL THE BUILD DOES NOT MAKE.** It said ports 0
  and 1 are refused; only port 1 is, and an in-tree partition names port 0 for a peer deliberately,
  that being the echo the window layer answers with no thread. A reader trusting the help would have
  concluded an existing config could not exist.
- **AMP-2 HAS NO RUNTIME WITNESS AND ONE COULD NOT BE MANUFACTURED HONESTLY.** The window between
  the validating load and the copy contains no call the seam can interpose, and `kmemcpy` folds to
  `memcpy` with its length already evaluated, so a racing mutator would redden only
  probabilistically and would be a flake by construction. The instrument is a STATIC gate,
  `check_amp_slot_snapshot.sh`, which self-tests planted twins before reading the tree; the runtime
  half asserts only that a producer rewriting a held slot's header cannot change what the taker
  already took. **No production-side test hook was added, and that was the constraint that made the
  static gate the answer.**

## M8.3: isolation and syscall robustness, and what these green runs do NOT say

**THE TEN-ANGLE GATE RETURNED FOUR CRITICALS AND EVERY ONE WAS OUTSIDE THE PRESET SAMPLE THE WORK
HAD BEEN VERIFIED ON.** That is the finding this section exists for, and it outranks everything
below it. Five reviewers over ten angles found: a selftest gate RED on a multicore preset, an
`esp32c6-wroom-st` link that no longer completed, five presets failing a red zone they passed at
the merge base, and an AMP protocol state a live node could never leave. The work had been checked
on a hand-picked set that covered none of them, while the instrument that answers the whole fleet
was being held for the end. **The rule this confirms was already written down** -- a three-board
sample is not a sufficient sample, and anything on the syscall path needs the full sweep -- so the
lesson is not the rule but that having it is not enough: the sweep has to be the acceptance test of
the change, not the ceremony after it.

- **THE AUDIT PRICED THE COPY-SITE ITEM ON A PREMISE THAT WAS TRUE AND IRRELEVANT.** It was filed
  Medium and "latent in production while `CAP_FRAME` is minted only by the selftest probe", which is
  a correct statement about the unmap route and says nothing about the other one: `ep_copy` refuses
  a copy whose two ends are the same memory under one owner, and on a region backend both owners are
  null, so ANY overlapping sender and receiver satisfies it. Two threads of one task naming one
  static array as both ends of a rendezvous reached the assert with no capability, no `AUTH_MEMORY`,
  no MMU and no selftest build. **That fact is also what settled the item's two open directions**:
  refusing the unmap while a thread is parked with its buffer inside the range closes the route the
  audit described and leaves this one standing. The generalisation: a severity resting on which
  capability is mintable is a severity resting on one route, and the cheap check is to ask what the
  same predicate answers where the enforcement is absent.
- **THE FRAME BUDGET WAS MEASURED ON armv7m AND armv6m ONLY, AND THE TWO ARCHES NOBODY MEASURED BOTH
  BROKE.** The zero-delta claim holds where it was taken -- the `KICKOS_KERNEL_STACKS=0` presets and
  the RP2350 AMP nodes are unmoved, and armv6m improved -- and rxv3 and rv32imac were never in the
  sample. Both regressed, and **neither regressed at the site the first attribution named**: on
  rv32imac a funnel helper gained a `kpanic` EDGE it had not had before, so the whole console tail
  interposed into its frame; on rxv3 a slot accessor gained a null test worth two instructions and
  `-Os` stopped inlining it, which `objdump` settled by showing an inlined divide at the base and a
  call at the tip. **An attribution handed down is a hypothesis**, and the disassembly is what
  decides it.
- **THE PANIC TAIL IS MOST OF WHAT A RED-ZONE FIGURE MEASURES ON THOSE TWO ARCHES**, so a new assert
  anywhere on a dispatch chain drags the whole console with it. Eight figures across rxv3 and
  rv32imac now sit at exactly their reserve, which means the next one fails a gate on a board nobody
  named. `TODO.md` carries it as owed with a direction; it has no milestone home.
- **A GREEN RED-ZONE GATE IS ONLY A WITNESS IF ITS DURATION IS READ.** That gate keeps one scratch
  tree per preset and reuses it, and a reused tree does not re-derive generated Kconfig state, so it
  fails OPEN. A pass that returns in a fraction of a second measured nothing; a real run takes
  seconds. Same family as the reuse traps `CONTEXT.local.md` records, and the only member whose
  false outcome is a PASS.
- **THE PRODUCER'S BOUND DID NOT COVER THE STATE IT WAS BUILT FOR, AND THE REVERSAL IT JUSTIFIED WAS
  THEREFORE WORSE THAN WHAT IT REPLACED.** The reply-tail bound believed any outstanding count the
  ring can legally hold, and ZEROED its strike count on that path, while a publication into an
  exactly-full ring is refused as FULL. So in the one state that needs a recovery, no strike ever
  accumulated: the node could not move its own head and the only other index was the peer's. The
  earlier rule released the slot there and stayed clean; holding it pending wedged the crossing for
  the life of the image. **The fix bounds the OBLIGATION and not the index**, because striking the
  tail would resynchronise a legitimately full ring and destroy a slow peer's unread answers -- a
  stalled peer and a dead one are indistinguishable without a timeout. Expiry gives the slot back
  and abandons one answer whose bytes were already counted lost. What it costs a slow peer is that
  one refusal; what it costs above two nodes is that any doorbell spends the budget, so an
  unrelated peer's traffic can expire an obligation early.
- **AND THE ARM THAT PROVES THAT FIX HAD TO REFUSE THE PLAUSIBLE FIX TOO.** Striking the tail in the
  full state is the obvious answer and it is wrong; the negative control applies it and reddens two
  arms. An arm that only distinguishes broken from fixed would have accepted it.
- **A COMMENT ASSERTING TOTALITY WAS HIDING A DEFECT, AND IT WAS FOUND BY DISBELIEVING THE COMMENT.**
  The discharge's release was documented as total over a slot whose run a resynchronisation had
  abandoned -- "this releases nothing". It masks its index against the run standing NOW, so once
  later calls have been taken it moves the tail over one of THEM, whose reply is still owed. The
  record carries the fact now, because a masked slot cannot tell an abandoned record from a later
  wrap's call: they share one.
- **THE OWNERSHIP RECORD IS A WHOLE-IMAGE LIFETIME BUDGET AND ITS EXHAUSTION HAS NO BOARD WITNESS.**
  Nothing frees a record, as the bump allocator never takes a block back. A full table answers NULL
  and spends no arena doing it, which is indistinguishable at the ABI from an exhausted arena BY
  CONSTRUCTION -- that call returns a pointer and has no errno route. Only the host arm drives the
  table dry, and its first version could not tell its two subjects apart either, since an
  arena-exhausted refusal answers NULL and spends no arena as well.
- **THE OWNERSHIP PROMISE IS TRUE WHEREVER PROTECTION EXISTS AND NOWHERE ELSE.** A board with no
  region descriptors and no translation admits every in-arena range to every caller, deliberately,
  in the same posture the grant module already takes. The region and translating backends answer ONE
  question through two records, and the region side wraps the arena seam rather than joining it,
  because two callers of that seam are not user allocations at all and a seam-level owner argument
  would have had to invent an owner for them.
- **THE OBJECT BUDGET IS CHARGED TO THE POOL SLOT AND NOT COUNTED ON THE TASK, AND THE REASON IS A
  BUG A COUNTER WOULD HAVE.** The task that releases an object is often not the one that created it,
  and an exiting thread's task pointer is cleared before the capability sweep runs, so a credit
  could land on a RECYCLED task slot and grant it capacity it never earned. Deriving the count by
  scanning removes that class rather than guarding it.
- **WHAT THE BUDGET BOUNDS IS CREATES, NOT HOLDS, AND THAT IS WEAKER THAN THE DENIAL THE ITEM
  DESCRIBES.** A task at its ceiling can delegate its objects into a second task and die; the
  disown that stops a recycled slot inheriting a charge also clears the charge from an object that
  is still live on somebody else's capability, so one live task can end up holding a whole pool
  while its own charge reads one. Two tasks at their ceilings empty a pool between them anyway.
  Charging on delegation, or counting holds, is the direction; both are past what this milestone's
  decided shape covers, and `TODO.md` carries it.
- **NOTHING WRITES THE BUDGET AFTER THE SEED, AND THAT IS THE WHOLE OF THIS MILESTONE'S SHARE.**
  There is no route to lower it and no authority for one. It holds no count, so no release can lift
  a ceiling something has lowered, which is what makes a zero stay zero once that route exists.
- **THE SELFTEST GATES COULD NOT BE PRIVILEGE GATES, AND READING "ROOT-GATE BOTH" AS ONE WOULD HAVE
  REFUSED ROOT.** Root is unprivileged from its first instruction, so a privilege test on the
  capability mint refuses the only thread that calls it and breaks four already-shipped arms. The
  mint asks an AUTHORITY; the injection is gated on the LINE rather than the caller, because a line
  the kernel dispatches itself is not a device a caller could be standing in for, and an authority
  gate there would have hung two existing injectors that hold none.
- **THAT ITEM'S SCOPE WAS UNDERSTATED AND A GREP SAYS SO.** Every AMP node variant enables the
  selftest knob in its own defconfig, so "selftest images only" is "every AMP node this tree can
  build".
- **AN ARM THAT REPORTS PLAIN `ok` WHERE ITS SUBJECT IS ABSENT IS INVISIBLE TO BOTH BOOKKEEPING
  MECHANISMS.** The injection refusal is witnessable only where a controller reserves a line, and
  its arm took an else-branch that fell through to a control -- no skip, no partial, so neither the
  skip permissions nor the partial list could see that the claim went unexercised. It reports a
  partial now. The general form: a permission set catches an unlisted skip, and nothing catches an
  arm that never says it could not run.
- **AND A GUARD CAN BE DEAD BECAUSE OF A CAST.** The same arm tested a probe's refusal for absence,
  but widening a 32-bit negative errno into a 64-bit signed value makes it POSITIVE, so the guard
  was dead on exactly the 32-bit region boards it existed for. Read a refusal at pointer width.
- **THERE IS NO GAP-FREE USERSPACE READING OF A PARKED RECEIVER.** Every reading this kernel offers
  comes out of a COMPLETED rendezvous and describes the sender's side. So an arm needing a receiver
  parked before a peer acts cannot poll for it; the two that needed it pin both parties to the one
  core no image may isolate and rank them by priority, which makes the order a property of the
  scheduler rather than of a sleep. **The skip that had excused them from multicore was itself the
  defect**: both assert a POSITIVE outcome, so neither was ever in the non-event class that skip is
  for, and excusing them cost the copy-refusal route its only witness above one core.
- **A PRIORITY RACE CANNOT BE REDDENED ON THIS BENCH, SO THE MUTATION THAT PROVES THAT PINNING IS
  NOT THE ONE IT LOOKS LIKE.** Unpinning the two parties passed five runs out of five, the emulator
  dispatching the high-priority thread to its park before the low one's first instruction. What
  proves the arm refuses a wrong order is SWAPPING the priorities, not removing the pinning.
- **THE SYSCALL TABLE'S DOCUMENTED CODE LISTS ARE READ BY NO GATE**, which is why a refusal code
  landed in the API header and in none of the four creator entries beside it. Proven by stripping
  three codes from one entry and watching every static gate pass. A gate over that surface is
  `TODO.md`'s and belongs to the milestone that owns the gates.
- **THE ITEM ON THE GENERATOR NAMED TWO CHARACTERS AND THE REAL SET IS FOUR, ONE OF WHICH IS NOT A
  SYNTAX BREAK.** A quote ends the argument and a semicolon splits it into a list, as filed; a
  backslash must be escaped first or it consumes the others; and a DOLLAR is EXPANDED inside a
  quoted assignment, so a knob value could pull in an unrelated variable that happened to be in
  scope where the fragment is included. That one is a substitution, not a broken line.
- **THE MERGE HAZARD THAT BIT WAS THE SELFTEST ARM COUNT, AND A CLEAN THREE-WAY MERGE GOT IT WRONG
  BY ONE.** Two items edited one arm-count clause in opposite directions -- one added an arm to it,
  the other moved an arm out -- and neither hunk conflicted, so the union is neither side's number.
  Nothing but the image's own plan check catches it, and it catches it as a failed run. Re-derive
  such an invariant from the registrations after a merge, never from arithmetic on the two diffs.
- **A BOARD THAT LOOKS LIKE THE TIGHT ONE IS NOT ALWAYS THE TIGHT ONE, AND "TIGHTEST" WAS NOT
  ESTABLISHED.** The board this file's caveats name as having no arena slack has room and an
  alignment run-up that absorbs movement; another board's fattest image has far less. Only three of
  the fleet's presets were priced, and on raw headroom the ranking differs from the one on
  survivable growth, so the honest statement is per-board and not superlative. Related: an arena
  cost quoted for one image can be wrong for the image the link assert actually BINDS, which is the
  fattest one, and a claimed hard-failure threshold was off because the assert is an inequality
  rather than a strict one.
- **AN ARENA CLIFF CAN COST 32 KiB WITHOUT FAILING A LINK.** On the esp32c6 split-image posture the
  app window is aligned, so kernel `.bss` crossing its boundary does not fail anything -- the arena
  simply starts a window later. One preset is 68 bytes from it and one has already fallen off, which
  is why that one reaches zero pool headroom under a wider code window. No assert reads that
  distance. `TODO.md` carries it.
- **AND WIDENING THE CODE WINDOW WOULD HAVE BEEN THE WRONG ANSWER TO THE ONE IMAGE THAT OVERFLOWED
  IT.** It costs the whole reserve on every enforcing preset of that chip, drives one to exactly
  zero pool headroom, and halves the thread ceiling that board ships. Splitting the selftest image
  the way every other tight board is split leaves the fattest part with room to spare.

## M8.4: the gates, the CI matrix and the instrument, and what these green runs do NOT say

**THE MILESTONE'S SHARPEST FINDING CAME OUT OF THE INSTRUMENT WORK AND NOT OUT OF THE GATES, AND IT
IS A PRIVILEGE ESCALATION THAT DEFEATED AN EXISTING DEFENCE.** On rv32imac `gp` is U-mode writable
and deliberately not saved in the trap frame, and `trap_entry` computed `&g_arch_current` THROUGH it
before re-anchoring. `trap_entry`'s own comment documents this threat for `sp` and answers it with
the swap onto the trusted trap stack; the bounds test that backs that swap reads `stack_hi` and
`stack_lo` out of a ctx located through `gp`, so a forged ctx passes it and the `kernel_sp` beside it
aims two M-mode frame stores past PMP. **The hazard was seen for one register and solved, and the
identical shape one register over was not.** `mtvec` is vectored over 32 slots that all jump to that
one entry, so the anchor there covers every route in.

**AND WHETHER IT REPRODUCES DEPENDS ON THE IMAGE, WHICH IS WHAT MAKES IT THE WORST KIND OF LATENT
DEFECT.** The access is gp-relative only where the linker's relaxation finds that global inside gp's
psABI window of plus or minus 2048 bytes, and the distance is a property of one IMAGE's data layout
rather than of the board: on the bench image `g_arch_current` sat 1976 bytes from
`__global_pointer$`, inside, so the pair relaxed and three gp-relative reads stood inside
`trap_entry`; on another image the same global lands 2840 bytes out, no relaxation happens, and the
entry path is clean. **So reading one image proves nothing about another**, reading the source
proves nothing at all, and the defect appears and disappears as unrelated data moves. It was live on
a shipped preset's image, so the anchor is a fix rather than a guard against drift, and the gate's
order clause is worth having on the images where it currently passes trivially.

**THE GATE THAT NAMES THAT THREAT RAN ON NO rv32 PRESET, AND EXTENDING IT WAS THE WRONG ANSWER.**
`check_riscv_kernel_gp.sh` states it in its own header and bans gp-relative kernel access
image-wide, which is true of the rv64 SPLIT image and false of the flat rv32 one: rv32 has no
address space, so one `.text` carries kernel and app with about 69 legitimate gp accesses under a
single anchor. So a whole-image ban would have reddened the fleet while asserting something untrue.
What the threat needed was a narrow gate over the trap vectors, and the relaxable-anchor case is the
member a source reading misses: the `la` looks right and only the LINKED image shows the linker
turned it into `addi gp,gp,off`, which reads the gp under attack.

**A DISMISSAL NEEDS A POSITIVE CONTROL AS MUCH AS A FINDING DOES.** That escalation was first
measured as NOT reproducing, by an `awk` scan spelled `/\bgp\b/`, where `\b` is not a word
boundary at all: it matched nothing over a corpus full of hits, and the zero agreed with the
expectation that produced it. An instrument that refutes a claim is exactly as capable of being
vacuous as one that reports it, and neither says so in its output.

**THE SEVENTEEN-GATE ITEM WAS SIXTEEN PLUS EIGHT, AND WRITING THE CONTROLS IS WHAT FOUND THE REST.**
One of the seventeen was retired rather than controlled. The other sixteen surfaced FIVE gates that
passed over their whole corpus when a reader died, three of them outside the masked-scanner item's
own list, and FIVE real gate defects: a scanner truncating its own findings file so only the last
offending file in the tree could ever be reported, a relocation listing never checked for being the
wide one so half the aarch64 GOT types went unseen, a gate that could not tell a refused column
width from a dead scanner, a vacuous pre-filter, and a reporter file's death invisible to the leg
that read it. **A gate with no control is not merely unproven; it is where the defects are.**

**THE PLAUSIBLE WRONG FIX FOR A MASKED SCANNER IS INERT, AND THAT WAS MEASURED RATHER THAN
ARGUED.** `|| fail` at the END of a pipeline reads the last command's status and never the reader's:
under a killed reader its message appeared ZERO times in three of the gates, and each run went red
only on a planted in-script control reporting the wrong cause. Over a corpus with no control it
would have been silent. `pipefail` is not available either, every gate being `#!/bin/sh` over dash.

**A NUMERIC FLOOR PER ROOT DOES NOT CLOSE A DROPPED-ROOT HOLE, because dropping the root removes its
floor with it.** The banner gate stayed green at 347 files with two roots deleted. What closes it is
a TOTAL classification of every top-level directory that tracks source, so an unknown directory is a
named failure rather than a silence: the same move as making an authority total instead of guarding
its absence.

**THE INSTRUMENT'S COMPOSITE CORRECTION WAS WRONG, AND THE SHAPE OF THE ERROR NEEDS NO COUNTER.**
`PH_NEST` is the empty bracket sited INSIDE an enclosing span, so `NEST = NULL + one nested
bracket`, and subtracting `k*NEST` removes the composite's single `NULL` residue k times: exact only
at k=1, nothing at k=0, over-subtracting above. Measured over a synthetic depth sweep whose every
composite is empty and so reads 0 on a perfect instrument, the documented rule gave 240, -140, -400,
-680, -1300 at k = 0 to 4. **k belongs to a SAMPLE and not to a row**, and nothing prints it.

**THE rv32 SWITCH FIGURES MOVED AND THAT IS NOT A CYCLE COST.** The window now spans the register
save and restore every other arch's does, so before and after measure two different brackets. `n`
drops by exactly one per report, which is the deferred bank showing itself: the `mret` leaves no
register for a call, so the halves are banked at the next switch, and the sample dropped before each
print is the switch INTO the reporting thread, a preemption rather than a ping-pong handoff. **No
runtime cross-arch bracket comparison exists on this box** to check it against, mps2's DWT being
dead under QEMU and the bus empty, so the comparison is static, by what each window encloses.

**PERCENTILES LAND ON THE SWITCH ACCUMULATOR AND DELIBERATELY NOT ON THE PHASE TABLE.** A
log-linear histogram costs 672 bytes of `.bss` for SWITCH; forty phase rows would cost 26.9 KiB, a
fifth of the XMC4800's RAM, and worse, a phase histogram runs inside the accumulator call whose cost
every enclosing composite carries k times, so it would degrade the table to buy a percentile of a
mixture. p50 and p99 are bucket LOW EDGES at an eighth of an octave, so both are floors, and the
histogram was never validated against a known distribution. **The board that keeps MIN is the
tightest board**, so the histogram compiles out exactly where the RAM is scarce.

**MIN IS NOW A CHIP FACT RATHER THAN A FLEET RULE**, declared where facts of the part belong and
knobs do not, and witnessed at byte level: the XMC bench image carries the string `min/avg/max`
while the non-XMC armv7m image carries `p50/p99/max`.

**`cap_share` WAS THE INSTRUMENT AND THE KERNEL IS SOUND, AND THE CONTRACT SETTLES IT RATHER THAN
THE COUNT.** The arm asserted a synchronous property of an explicitly asynchronous operation: a kill
returning 0 means the request was accepted, never that the thread is gone, which the tree states in
three places. Instrumented, the shortfall converges to exactly the expected value, the victim's own
frames still out at the instant the call returned. **No frame-conservation invariant exists anywhere
in `docs/reference/`.** Load is the trigger and not the mechanism: it reddens SERIALLY on a
contended box, so `ctest -j` was never the cause, and `RUN_SERIAL TRUE` was refused with that
measurement rather than applied as a calming patch. Had the answer been a kernel race, that fix
would have hidden it permanently.

**A SLAY RETURNING 0 ANSWERS FOR THE GROUP AND ITS SLOT AND NOT FOR THE ADDRESS SPACE.** Measured
while trying it as that gate's fix: the first site went exact and the closing read still failed,
the space settling only after hundreds of probe spins.

**GICv3 WAS ALREADY WITNESSED AND GICv3 UNDER A SHARED KERNEL WAS NOT**, the AMP variants that set
it holding `KICKOS_KERNEL_CORES` at 1. And the RP2350 shared-image preset registers the tree's ONLY
`rp_node_vectors` case while the job bearing its name configured the two-image posture, so
**checking that matrix by job name concluded the opposite of the truth**. Deriving coverage by
grepping the preset NAME is lossy in both directions: that name matched with no build behind it,
while presets reached through a shell variable match nothing at all.

**AN x86_64 CI JOB WOULD HAVE BEEN A SILENT GREEN.** `tests/lib/gate.sh` SKIPs every image gate when
it finds no UEFI firmware and again when `mtools` is absent, and ctest reports success on a skip, so
the first draft of that job would have booted nothing on every run and passed. The firmware pair is
RESOLVED rather than named, shortest match winning because every qualifier lengthens the name and a
`.secboot` or `.ms` firmware refuses an unsigned image; this box ships five code files, so the
multi-spelling case is not hypothetical.

**TWO AMP MINTERS SPEND AN ENDPOINT SLOT WITHOUT CHARGING IT**, so the object budget's four charged
creators are not every route into that pool. Neither is reachable from an unprivileged task and
repeating one crossing is not refused, so what bounds them is the caller and never a count;
`invariants.md` says so now rather than promising more than the code does.

**THE FILE:LINE:COLUMN INDIRECT-CALL PIN IS AS FRAGILE AS `TODO.md` SAYS.** A seven-line insertion
moved four of its records with ZERO codegen change, and the red-zone gate correctly reddened on two
presets first.

**WHAT THE BY-RULE CONFIGURE CHANGE DOES NOT WITNESS.** A `-D` naming no Kconfig symbol is still
DROPPED rather than refused, CMake's own non-fatal notice being the only report: refusing that class
needs the road where the build decides no knob at all, which is not assigned. A `-D` naming a
PROMPTLESS symbol is now refused BY NAME where it used to do nothing at all. And the reach gate
gates the OUTCOME rather than the mechanism, so a hand list that happened to name every symbol
declared today would still pass; what it catches is the next symbol added without the list edit.

**THE AUDIT HELD TWICE, BOTH TIMES ON THE ACCEPTANCE SURFACE AND NEITHER TIME ON THE
IMPLEMENTATION.** No critical or high finding, no open kernel correctness or security defect, and
every blocker a claim that outran what backed it. First pass: a code-synced coverage table still
saying rv64imac and x86_64 had no CI job, months after one landed and hours after the other; the
new terse-diagnostic arm riding a broad selection with nothing naming it, so losing its
registration would have shrunk the job silently; and a design record telling readers the corrected
instrument was invalid. **Second pass caught the remediation's own overclaim**, which is the one
worth keeping: the sentence added to say a missing emulator can never green a job was itself
unbacked, three prechecks existing where it promised all of them. A claim about vacuity that is
vacuous is the same defect the milestone spent the day removing, and it was written while removing
it.

**DERIVING CI COVERAGE BY PRESET NAME IS WRONG FOUR WAYS, NOT TWO.** Seventeen jobs configure 53 of
the visible presets, and a name grep misses in both directions for four distinct reasons: a job name
that is not a preset name at all (one job configures nine presets and no preset carries its name;
another configures a preset into a second build directory under a name of its own), a preset named
only inside a matrix list, a preset built through a shell variable, and mis-attribution even where a
name does match, two jobs each configuring a preset beyond the one they are named for. The instrument
is to enumerate what each job CONFIGURES, expanding the loops and matrices by hand. The same file
answers which emulator each job needs: `tests/static/board_emulators.cmake` reads the same
`kickos_qemu_machine` body a test registration reads, so the binary is derivable rather than
assumable, and no job spans two.

**A STALE PROSE RECORD IS DELETED, AND THAT RULE HAS A PRECONDITION NOBODY HAD WRITTEN DOWN.**
`docs/design-riscv-switch-cost.md` was the candidate, and deleting it fails `doc_names` on three
unresolved references, two of them in records. So the delete-regenerable-prose rule holds only where
nothing cites the page: otherwise the page stays and loses its regenerable half, which is what
happened here. An unattributed `~3.5x` ratio went with it, a figure repeated without ever naming the
span it covered, which is exactly the class the milestone's own MPU correction removed.

**A PIN THAT CANNOT BE SHOWN TO FIRE IS DECORATION, AND THIS ONE WAS SHOWN.** With the arm's
registration disabled, the pinned line reports no tests found and exits non-zero WHILE the step's own
broad selection stays green one test lighter. That difference is the whole argument for naming a test
in CI rather than trusting a label filter, and it is worth more than the pin.

**THE FLEET IS THE WITNESS AND IT WAS TAKEN AS THE ACCEPTANCE TEST RATHER THAN THE CEREMONY.** A
whole-fleet host sweep over every visible preset passed with **zero reused**, which is the only tell
that separates a real sweep from a reprint of an older run's status files. That is what M8.3's
review said was missing, and the by-rule configure change is exactly the class that needed it: it
moves every preset's configure and a hand-picked sample cannot speak for it.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `roadmap.md` -- the milestone plan, and the sub-milestone ledger: the only place a number is
  ASSIGNED. This file carries the locked ORDER and cites those numbers.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `docs/design-multicore.md` -- the M7 contract: the hardware predicate, the SMP seam, the AMP
  window and the partition rules.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
