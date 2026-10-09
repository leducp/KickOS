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

A milestone's own section leaves this file once the milestone after it merges, and git keeps it:
`git log -p -S'<a phrase of the section>' -- STATE.md` finds the commit that removed it.

## Where we are

**M10 HAS MERGED THROUGH M10.5, AND M10.6, THE CLEANUP PASS, IS OPEN.** M9's answer is that the
one big kernel lock stays, with per-core ready queues and per-pair rings under it and CLH
arbitration on x86_64 alone. M10 is static composition: a declared system, admitted on the host,
and an init provider that stays resident. `roadmap.md`'s `### M10` section is the ledger and the
only place its numbers are assigned; the M10.5 section at the bottom of this file says what the
last merged milestone's green runs do not.

**M10.6.4 IS CLOSED ON ITS BRANCH AND WAITS FOR ITS SQUASH AND THE MAINTAINER'S PUSH; M10.6.5 IS
NEXT.** Its record is `TODO.md`'s "M10.6.4, FIRST" entry, and the M10.6.4 section at the bottom
of this file says what its green runs do not. The toolchain 1.1 release also waits on the
maintainer: its branch is to be pushed and the release published, and the RX72M capture of
`chaincheck`'s C99 arms waits on it.

M7 is the multicore milestone and `docs/design-multicore.md` is its contract; every ruling and every
freeze lives there, `roadmap.md` assigns the numbers, and `git log` carries the order things landed
in. **What follows is only the causes, measurements, traps and declines that a green run does not
say and no command re-derives.**

**EVERY M7 SUB-MILESTONE MERGED, BUT M7'S REVIEW HANDED DEFECTS FORWARD RATHER
THAN CLOSING CLEAN**, which is why M8 is cut to run fixes first, then de-duplication, then
optimisation, across eleven sub-milestones. `roadmap.md`'s `### M8` section is the ledger and the
only place those numbers are assigned.

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
build calls it and `tests/static/check_smp_predicate.cmake` calls the same function over SYNTHETIC
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
`irq.cc`, `console_tx.cc` and the two rp2 chip files. Of its readers only `check_trap_redzone.sh`
rejects a stale line, in its depth walk and in its console route clause, both PER SCOPE, **so a
re-pin is only witnessed by a preset that compiles that scope** and `irq.cc` is not pinned in armv8a
at all. Stranded behind that binding are every surviving in-repo doc PATH in code,
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
kill produced a poke**, and `doorbell_xpoke` asserts a service floor on every core the bring-up
check released, core 0 exempt since a reschedule raise owes no answer, and a pairing invariant
instead. **There is no route from an arm to force one**: `KOS_MEM_FLAGS_ALL` is NOCACHE
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
sets one kernel core while the image still drives four. It links the doorbell service, so
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
  - **The PUBLISHED route does not interleave kernel records with each other or with the
    driver's output.** A fault record is held whole for the driver and handed over between two
    of its receives, at once to a parked receiver or ahead of every queued sender at its next
    receive, and the kernel writes none of it at the driver's device (`console.md`, "A fault
    record while a driver owns the console"). **What it does NOT guarantee is a single writer.**
    The kernel pops `wq_pop_highest(recv_waiters)`, so a driver parking more than one thread gets
    its records spread across them with no ordering enforced between their device writes. A
    future multi-writer console driver breaks this property without touching the kernel.
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
  sound for whole records while the driver keeps one writer.
- **THE DARK WINDOW'S WAIT IS WITNESSED ON THE SIM AND THE HOST ONLY.** No emulated board declares
  a console reclaim window, so there the reclaim lands at the driver's task's end and no writer
  ever waits. `sim_driver_death` case 3 is the one image arm where a writer waits it out (a window
  thread at the lowest priority, a writer above it), and the host arms carry the rest. On silicon
  the window is real wherever a chip declares one, and nothing captured there yet shows a writer
  inside it.
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
  the service-list table is compiled by neither. **And the TREE STAMP each tool prints is
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
  **`park_death_point` is now held by a type**: the one `ThreadState::BLOCKED` write takes the
  `ParkToken` only the cancel ask mints, so a park that skips the ask does not compile; the gate
  left beside it only refuses a write past the state cell.
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
  THE PART WORTH KEEPING.** The one-core fold check (`arch.h`, once the `cpu_id_fold` gate) reads the
  count of cores the image DRIVES and does not apply above one of those. It is NOT the kernel-core count, and the difference decides which
  preset can ever close the arm: an AMP image sets one kernel core while DRIVING four, so the
  gate skips there too. What closes it is a GICv3 board that drives ONE core, which
  `imx8mp-evk` is for an unrelated reason, the emulator modelling no secondary release. **A gate's
  skip condition is a claim about which figure it reads, and naming the wrong figure moved an
  obligation onto the wrong milestone for two whole steps.** The multi-core folds above one driven
  core remain unwitnessed and the fold check structurally cannot reach them.
- **NOTHING WITNESSES THAT THE x86_64 DECODE IS FED THE LIVE ATTRIBUTE TABLE**, and the arm that
  used to is gone on purpose. It proved the feed by REPROGRAMMING `IA32_PAT`, which SDM 14.12.4
  makes the operating system's job to sequence and which this port has no reason to spend a cache
  flush on; an external re-review called it out and it was replaced by synthetic tables the decode
  is checked against. So the DECODE is witnessed over four layouts no firmware here provides, and
  the FEED is witnessed nowhere. The two arms that would separate them are vacuous on this bench
  because OVMF leaves the register at its power-up value, and only a machine whose live table
  DIFFERS could tell them apart. A backend handed a constant instead of the register passes
  everything.

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
  divergence to record. **The figure was the machine's CAPABILITY and not the width in force**: at
  the time `TCR_EL1.AS` stayed at 8 bits and the 16 the machine offered was a figure nobody spent.
  M8.10 programs `AS` and the same line now reports the width actually in force, which is the only
  reason the two readings can be told apart at all. Beside it a granule arm that re-reads
  `arch_aspace_granule`'s own constant still sits, which is why that arm was never the
  confirmation.
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
- **The x86_64 board runs the aspace family since M10.1.1.** q35 selects `HAS_ASPACE`, its frame
  pool is the UEFI arena's remainder, and the self-test's family arms run on `qemu-x86_64` and
  its two-core preset. X5 stays, as the registered `x86_64_x5_aspace`, for the out-of-frames
  unwind no application image drives on demand.
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
- **`trap_redzone` NOW RUNS ON EVERY EMULATOR BOARD BUT `sim`, AND THE SWEEP STILL OUTRANKS THE
  SAMPLE.** armv8a, both RV64 postures and x86_64 gained classes at M9.4, so a green emulator pass
  now speaks about those arches' trap stacks. It says nothing about the MCU classes: a change on the
  syscall or console path has taken presets red that nobody named, as a gate failure and never as a
  build error. **Such a change needs the full host sweep, not a three-board sample.**
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

- **THE SELFTEST PARTITION CHECK CANNOT SEE A DELETED ARM.** `p1 + p2 + p3 == total` compares
  CMake literals on BOTH sides, so removing a `TAP_ADD` from the source leaves it green. Found
  in M8.6 while building a positive control for the split, not by the check failing. What does
  catch a deletion is the plan line moving, which only a run sees.
- **AND `check-x86_64-no-got.sh` IS A LIVE BACKSTOP, NOT A DORMANT ONE.** M8.6 established that
  the weak-undef hazard `klink.h` describes cannot reach the PE32+ target today, so the gate has
  nothing to refuse on THAT account -- and in the same milestone it refused a real link, a
  cross-TU address-take going GOT-indirect under `-fpie` when the selftest TU was split. So the
  two statements sit together: dormant for the case it was written for, and firing on another.

- **`check_public_headers.sh --tree` compiles with no `-D` at all**, so a C-facing header's
  other `#if KICKOS_<knob>` arm is compiled by nothing and the gate still reports PASS. NOT fixed
  on purpose: widening wants measuring first.
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
- **AND THE PATH-PLUS-IDENTIFIER FORM IS WEAKER THAN IT READS.** `doc_names` checks that the path
  resolves and that the identifier exists SOMEWHERE in the tree, never that the identifier is in
  the file named. Found in M8.6 when the probe surface left `abi.h`: a design record citing
  the probe's granule op at `abi.h` stayed green with the symbol now in `abi_probe.h`, and the
  citation was fixed by hand because nothing would have reported it. So a move between headers
  silently falsifies every citation naming the old one.
- **Turning CONTAINMENT on flips what a gate may assert.** `kernelhalf` and `stackguard` were
  dying-image gates reading the panic dump; once armv8a joined `KICKOS_FAULT_ISOLATION` the same
  encodings had to be read off the thread-kill record instead, and `check_tap_stream.sh`'s blanket
  refusal of ANY thread-fault record became a by-name permission set. `aspacefault` STAYS a
  dying-image gate and that is the scenario, its read being the kernel's own at the current-EL
  vector where the kill rule declines it.
- **ON QEMU THE BENCH GATES JUDGE ONE REPORT WINDOW, SO A DEFECT THAT FIRST SHOWS IN THE SECOND IS
  GREEN THERE.** `check_bench_irqspan.sh` stops the image at its first `e2e-local` line, and the
  other bench image gates poll too. Found 2026-09-25 by mutation: a `bench_reset` that skipped the
  sweep row left the e2e row climbing 1000, 2000, 3000 across silicon's three windows, and every
  QEMU gate passed it. Only a silicon capture refuses it: `bench.sh`'s own verdict, and
  `BENCH_CAPTURE=<log> check_bench_irqspan.sh <cores>`, which judges each window.

## Board caveats a matrix does not carry

- **`microbit` has no arena slack, structurally.** `.userheap` is `default 0 if CHIP_NRF51`, so
  `__kickos_ram_start` lands exactly at the 32-byte-aligned `_ebss`.
- **The `_ebss`-to-arena gap is NOT slack.** The `.userheap` carve slides up with `_ebss`. The
  shape that really is pinned is an enforcement window.
- **`usbcdcwit` is built by no default configuration of any board** (gated on
  a service-list selection matching `_usbcdc`).
- **`picopi`'s slay capture EXISTS and is not valid for this tree.** The five slay arms passed on
  it in a 2026-08-16 session log, which is why "owed" was the wrong word -- but that log attests the
  M4.9.1 tip, and M6.2 has since changed `arch/arm/armv6m/arch_armv6m.cc` at T6a. A witness is valid
  for a TREE. It is still the fleet's only armv6m enforcement unit, so nothing else can stand in.
- **The bench chain refuses BY NAME on an absent rig value**, and `bench-fleet.sh` ending
  `INCOMPLETE` with a non-zero exit is the EXPECTED result for a fleet pass, frdmk64f being out by
  ruling. **That ruling exists NOWHERE but this line** -- `bench-fleet.sh` still lists and probes
  the board, and an absent one records `ABSENT` and makes the pass `INCOMPLETE`.

## Open, and verified still open

- **`kos_print` does not survive a published console.** `emit.h` exists and there are three
  publish-aware writers, so silence from a `kos_print`-only app is not evidence of a dead driver.
- **USB CDC: bulk OUT is never exercised and `Shared::configured` never clears on unplug.**
  Detail at `TODO.md`.
- **No emulated gate for a buffered-ring panic flush**; the sim's ring is provably empty at
  panic time. Detail at `TODO.md`.
- **Four app SOURCES grant a DEV window a live driver holds**: `xmcspi`, `xmccshold`, `pvprobe`,
  `inprstorm` -- six targets, `inprstorm` now building three ELFs from one source.
  **The app authority macro surfaces only at runtime**, one consumer at boot and no build file
  reading it.
- **T7's OWED LATENCY MEASUREMENT WAS NEVER TAKEN, and there is no instrument to take it with.**
  The doc makes a compact-SVC-frame decision wait on the number; `qemu-arm64` has only a `base`
  preset, `bench-fleet.sh` does not list the board, and no aarch64 round-trip figure exists
  anywhere. Recorded as a debt at T7 and in `TODO.md` since 2026-08-26, so this line is no longer
  the only thing that says so.
- **THE CONSOLE HAS NO CROSS-CORE EXCLUSION ANYWHERE, AND A RED `qemu-riscv64-smp` UNDER LOAD IS
  THAT, NOT NEW BREAKAGE.** No `arch_console_write` backend excludes a second core: seven are bare
  device loops and the other eleven reach the burst producer (since deleted), which held the kernel
  lock for one ring chunk and dropped it between chunks. `kernel/init/console.cc` (`kconsole_write_impl`) states
  the opposite -- "the chip transport locks internally" -- which is why the chip arm deliberately
  takes no lock, and that sentence is true of no backend. The chip arm's writer tally
  counts writers for the publish drain and excludes nothing. **The rate is load-dependent and only
  that**: idle, the preset's whole CI job is green thirty runs out of thirty; loaded it costs the
  TAP stream about seven times in thirty. So the figure to distrust is an unloaded one, and a red
  here is read against what the box was doing. The repair is M9's, which owns console locking;
  `TODO.md` carries the measurements and the two failure signatures.
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

## M10.5: the fleet, and what these green runs do NOT say

**SWEEP 4, THE FULL FLEET ON A QUIET BOX, RAN ONCE ON THE TREE BEFORE THE LAST FIXES.** It was red
on four presets no agent had verified: the plain bluepill-c8 and f302nucleo selftest manifests,
the C6 LP node's clock-order gate, and the LX6 SMP bench build's interrupt depth. Each was fixed and
its preset re-run alone; the CI fixes that followed (the NVIC probe that never quit, QEMU gate
timeouts derived from the boot bound, the out-of-tree app boot moved out of the host step) were
checked by the CI steps they fix, not by another sweep. The maintainer ruled no second sweep.

**M10.5 LANDS AS A STACK OF SQUASHED PRS ON MASTER, IN ORDER.** The first three are merged. A CI
fix found after the stack was cut was carried into every later PR so each one is green on its own;
the top PR holds everything after the last cut. Pushing is the maintainer's.

**WHERE IT RAN ON SILICON.** The final pass ran on the tree just before that last merge, on
xmc4800-relax, esp32c6-wroom (one-core, flat, and the AMP node 0 alone), esp32-wroom one-core and
SMP, f302nucleo, f411disco and rx72m. Every capture passed its judge, rx72m's cxxtest only on a
re-flash (below). Fault records came out whole, writers on a full ring lost no line on the
TX-interrupt boards, and the wallclock image held a 5 s kernel sleep to within 40 us of the host's
arrival stamps on every board it reached (2026-10-06, measured). The C6 AMP node-0 selftest images
were captured by hand: the fleet pass takes only the partition's own image on that variant.

**THE ESP32-C6 KERNEL CLOCK RAN FOUR TIMES SLOW SINCE ITS BRING-UP, AND NO GATE SAW IT.** MTIME
counts the CPU clock, and the board boots from its EN reset on the 40 MHz crystal. The 160 MHz the
conversion assumed came from a July bring-up that found the PLL left on. It was found by timing the
tick against the UART's baud; its witness now is the host's arrival stamps against a kernel sleep,
red at 19.99 s for 5 s on the old conversion. The rate is now read from the clock tree at boot, and
a rate with no exact conversion stops the boot. **A clock error has no witness but an outside
clock**, which is why every capture route stamps its lines and every captured board ships wallclock.
The C6 AMP judge accepts the 40 MHz crystal alone: a PLL bring-up, recorded as open, changes the
judge with it.

**THE FLEET HAD NO SMP VARIANT, SO THE ESP32 SMP C++ IMAGES REBOOT-LOOPED UNSEEN.** Reset called
`__register_frame`, which reached malloc and newlib's reentrancy lookup with no thread pointer set.
The frames are now registered on the root thread before the app constructors, a gate walks the boot
path for thread-pointer reads (it does not follow every indirect call), and the fleet captures every
variant a board declares except the bench builds.

**SWEEPS 1 TO 3 NEVER RAN THE HOST UNIT TESTS.** The sweep script configured the sim without the
GTest prefix, so those sweeps ran only the tree tests. Sweep 4 is the first to run them. A sweep's
preset count says nothing about whether its host tests ran.

**THE RX72M'S STDOUT VANISHED ON ITS FIRST SILICON RUN OF THE DEFAULT SYSTEM, AND ONLY A
PRINTED-LINE CAPTURE COULD SAY SO.** RX newlib calls the plain `write`/`isatty` names, which KickOS
aliased on x86_64 alone, so libnosys answered and the bytes went nowhere. That is why images that
print a verdict carry a capture judge, and why the images an emulator judges are marked owed on
boards that have none, rather than passed.

**A CONSOLE DRIVER'S OWN TASK HAS NO STDOUT.** Once `kos_print` became the one blocking stdout
writer, a console driver's thread printing on cap 0 sent to the endpoint it serves and parked there.
The task that serves the console gets no cap 0; its prints take the kernel route and drop at once
while the UART is its own. A print from a console driver that never shows is this rule, not a lost
line.

**VIRT_RV64, THE I.MX 8M PLUS AND Q35 NOW DROP A KERNEL LINE ON A FULL FIFO.** Their sync writers
stop at the first stall and drop the rest of the line, like every other chip's. A kernel line
missing from one of those QEMU captures under load is this, not a flake to re-run away.

**WHAT IT DOES NOT SAY.**
- No silicon has run the last merge: stdout's byte count on a refused or cancelled write,
  `consoledemo` and `k64console` on their IRQ console drivers, and the XMC4800 and K64F with their
  polled console drivers gone. Sweep 4 and the host tests are its only witnesses.
- picopi, pizero2350 and teensy41 were not reached, because their USB console cable is not on the
  bench host. Owed there: both USB console selftests, `usbcdcwit` (the only witness of the short
  accept under a full TX ring), `wallclock_usb` and the USB identity rows, the RP2350 ACCESSCTRL
  denial (its judge has held only a planted capture), the Teensy LPUART6 RX daisy fix, and the i.MX
  RT1062 with its D-cache on in every image.
- bluepill-c8 and blackpill share one probe and were not reached; their task-budget skips and
  rebalanced selftest regions are derived, not captured. microbit has no bench row.
- The K64F ran on a local rig only, on an earlier tree: blink by eye, k64dspi against the LAN9252
  shield, k64drv, both reclaim witnesses, the selftest three times and mpu_fault. Owed, behind the
  J-Link's daily notice: rootfault, a full fleet pass, the SYSMPU user rights narrowed to the core
  alone with no chip address for a fetch fault, and k64console on k64uartirq.
- An emulator-judged image (stackdepth, rebootdemo, ringppb, the pspguard family, the ESP32's fault)
  is owed on every board with no emulator, and blink is read by eye only. A capture carries no exit
  status, so every "ended the system with status N" clause rests on the emulator gates.
- f411spi's loopback is not wired (no PA7 to PA6 jumper), so its loopback clause is owed and the
  capture judges only the rest.
- rx72m's flasher timed out with E4000003 twice, once per pass on different images, and each passed
  on a re-run. Not root-caused; a pass that hits it ends incomplete.
- The console ISR now wakes a parked writer. The trap red-zone gate bounds that tail where it roots
  interrupts; armv7m, armv6m and rxv3 take interrupts on MSP/ISP stacks no gate bounds, by design
  and older than M10.5, so there the tail has no measured bound.
- A configure writes composition YAML into the source tree, so a tree gate that reads the tree
  while another preset configures can race it (seen once in M10.5, never reproduced).
- The console's dark-window wait is unwitnessed on silicon, and no emulated board declares a reclaim
  window. Host tests and the sim script it.
- The ARMv6-M fault reporter's frame check is held by a host gate only: a wild PSP locks a v6-M part
  up at HardFault entry, so no target arm can reach it.
- The kernel's cache maintenance for uncached frames, and the out-of-lock mapping sync, run on QEMU
  only, which models no cache hazard; no translating silicon is on the bench. The masked spans the
  per-granule window does not cover are listed in `docs/reference/porting.md`. The activation-lock
  assert is debug-only, and no SMP preset builds debug.
- The ESP32-C6 UART flush waits for `ST_UTX_OUT` 0, the ESP32's TX_IDLE encoding, because the C6
  manual prints no encoding for the field. A `c6txidle` capture witnesses it at one baud, 115200,
  and one TX_IDLE_NUM, the reset 256: the field reads 2 while the line shifts and 0 once the last
  frame is out. No other baud or idle count was run.

## M10.6.4: the selftest on the production kernel, and what these green runs do NOT say

- **NO SELFTEST IMAGE HAS RUN ON SILICON SINCE M10.6.4 MOVED EVERY ONE OF THEM.** Every silicon
  capture of record predates it; the list owed is in the TODO entry.
- **THE IRQ ABSENCE CHECKS NEED THEIR DRIVERS STRICTLY ABOVE THE CHECKER, AND NO RUN SHOWS IT.**
  A sim control with the checker at the drivers' priority also failed the planted defects, so the
  rule rests on reading the latch paths. On GIC and RX silicon the pending line can trail the
  enable; no one-core board of either is in the fleet.
- **AN ARM A LEFT-OUT LIST DROPS RUNS ON NO IMAGE OF THAT BOARD.** bluepill-c8 and f302nucleo
  drop arena arms their images cannot back; the build and the bench name them, and nothing runs
  them there.
- **THE ARENA ASK IS NOT CHECKED AGAINST USE.** An ask stated larger than the arm uses, or an arm
  taking a block from another arm's ask it does not name, passes every check.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `roadmap.md` -- the milestone plan, and the sub-milestone ledger: the only place a number is
  ASSIGNED. This file carries the locked ORDER and cites those numbers.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `docs/design-multicore.md` -- the M7 contract: the hardware predicate, the SMP seam, the AMP
  window and the partition rules.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
