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

**M7.6 AND M7.7 ARE ONE TRAIN, not two milestones that happened to be adjacent, and reading them
apart loses what the pair cost.** They branched off the same master as siblings, deliberately, on
a measured claim that their only shared surface was three places: the predicate block, the two
`smp.cmake` files, and two Kconfig symbols. That held -- the merge conflicted in a preset file and
in these records and nowhere else. They were audited together, one external pass returning **do
not merge on both**, and they were fixed and re-audited together to zero open blockers. M7.7 then
rebased onto M7.6, which is how it inherits the RV64 ordering fence. **One fleet sweep witnesses
the integrated tip and nothing witnesses either branch alone any more**, which is the right shape:
a witness is valid for a TREE, and the per-branch trees stopped existing at the rebase.

**WHAT THE TRAIN'S FLEET SWEEP DOES NOT SAY, which is the only half worth writing down.** Both
halves passed with their sentinels and the figures are re-derivable, so they are not here. It was
one sweep of one tip on an idle box: the image half serialises to remove the instrument's own
noise, which is not evidence that these gates are load-independent, and a gate that fails only
under load passes here. Nothing in it is a silicon claim -- for `imx8mp-evk` that is the entire
four-core half of S6b, and for the AMP node it is every ordering claim the window rests on.

**AND THE TRAIN'S OWN INSTRUMENT LIED ONCE, which is the finding neither branch was looking for.**
A host sweep of M7.6 came back with its new board failing to configure while the branch built it
cleanly by hand. The board was fine: both sweep tools ran `cmake --preset` with no source argument
and never changed directory, so the presets file came from wherever the caller stood while the
result was stamped against the tool's own root. Run from the main checkout it configured **59 of
60 presets against master** and reported them as the branch. The only failure was the one preset
master does not have -- **without it the whole run would have been a silent false witness for the
wrong tree, and its tree stamp would have agreed.** A stamp records what a tool was POINTED at,
never what `cmake` actually read.

**M7.7 LANDED S7: AMP.** `qemu-arm64` ships a fourth posture in which the image DRIVES four
cores and ONE kernel schedules on one of them, the instance index comes from the core identity,
and every index and length read out of the shared window is validated as another node's writing.
What follows is what a green run does NOT say.

- **THE PEERS ARE NODES FOR THE WINDOW AND NOT FOR A SCHEDULER, AND THAT BOUNDARY IS THE
  PARTITION LAYOUT THE CONTRACT LEAVES OPEN.** A peer core answers the doorbell, drains its
  inbox, validates it and publishes a reply, all under its own node identity and touching
  nothing the kernel built. It runs no scheduler, and the reason is not effort: the arena is
  ONE linker region with a link-time assert modelling its exact allocation order, so giving
  each node an arena of its own IS the unfrozen question and building one would have answered
  it by accident. So the node whose kernel believes itself alone is core 0's, and what the
  peers witness is the crossing.
- **NOTHING WITNESSES THAT THE INSTANCE HOLDER RESOLVES A PEER'S OWN KERNEL.** What the board
  witnesses is a peer core running the shared service body and reading its own core identity:
  its counters move and node 0's do not. That the instance index IS that same function is a
  two-line header substitution; the fold at one instance is gated, and the host-thread keying's
  end-to-end behaviour is the sim's multi-instance gate. No run on hardware says a peer
  resolved a Kernel of its own, and provisioning four is what makes the claim cheap to believe
  rather than what checks it.
- **THIS POSTURE DOES NOT CLOSE THE GICv3 ONE-KERNEL-CORE OBLIGATION AND MUST NOT BE READ AS
  DOING SO.** `cpu_id_fold` skips on the CORE COUNT and not on the kernel's, so it skips here
  too; the chip port is what made it run on a GICv3 preset. What this posture adds is the
  configuration where the count and the model genuinely DIVERGE, four cores driven by one
  kernel core, which the chip port's single driven core does not reach.
- **THE DEFECT THIS PRESET FOUND WAS AMP-EXPOSED AND NOT AMP-CAUSED, and the difference is
  checkable rather than a claim.** `VirtualRanges` was not a total record of a space's
  mappings: the user stack was installed behind its back and recorded in no range, so a frame
  capability could be mapped ON TOP of a live thread's stack with no overlap refusal, the exit
  path then zeroed that leaf, and the release path re-derived the run's identity by reading the
  leaf back and dropped nothing. Raising the instance provisioning only moved the app half far
  enough for one arm's chosen address to land on a child's stack base, and changing a stack
  size alone reproduces and un-reproduces it with the keying untouched.
- **THE MUTATION THAT WITNESSES THE FIX DOES NOT REDDEN AN ARM, IT KILLS THE RUN.** With the
  stack's range record removed, the framecap map onto root's own stack page is accepted, the
  frame under root's locals is replaced, and root faults at once with the whole stream
  truncated. So the arm asserts the refusal and the CRASH is what demonstrates the defect; the
  arm's own red is unreachable, because the same acceptance that would fail it also kills the
  thread that would report it. THE ADMISSION ARMS BESIDE IT DO NOT SHARE THAT SHAPE: dropping
  the stack from the caller-nameable predicate reddens all three cleanly, the mapping being a
  second live mapping of frames already there rather than a replacement of them.
- **A TOTAL RANGE LIST MADE A LIVE STACK FINDABLE, AND THE ADMISSION PATHS HAD NEVER BEEN
  TAUGHT ABOUT IT.** Before it, `find()` answered null over a stack and the refusal for an
  address the space never reserved covered it BY ACCIDENT; after it, a stack is a reservation
  like any other and each of the three caller-controlled paths was filtering on the image flag
  alone. They ask one predicate over one flag list now. The lesson is the shape and not the
  bug: widening what a record NAMES silently widens what every reader of that record admits.
- **THE STACK'S OWN SELF-GRANT NEVER REACHED ADMISSION, AND A RETYPE IS WHAT DOES.** A thread
  carries its stack as one R|W region, so the syscall's already-reachable short circuit
  answers a plain R|W request 0 without consulting the range list at all. What reaches the
  range list is a request naming a memory type the mapping does not carry, and the GUARD,
  which no region covers. So the arms name a type or name the guard; a plain grant of one's
  own stack page still answers 0 and maps nothing, which is a no-op and not an admission.
- **THE STORED RUN SLOT HAS AN ARM AND IT IS NOT THE LEAK'S.** Dropping the identity check
  reddens the arm that revokes with a DIFFERENT run's capability of the same length. The leak
  itself is no longer reachable to redden anything, the mapping that caused it being refused
  one step earlier, so what stands behind that half is that arm plus the absence of any leaf
  read on the release path.
- **RECORDING THE STACK COST THE RANGE BUDGET AND THE COMPILE-TIME FLOOR DID NOT CATCH IT.**
  A thread's stack takes a slot now, so the budget scales with the thread count. The floor
  refuses a configuration that cannot seat its own threads' stacks, which is a structural
  claim; what actually bit was an APP's demand, the fleet's own selftest losing three arms at
  the old figure, one saying outright that no reservation was left to name a page with and two
  unable to spawn a thread whose stack had nowhere to be recorded. A floor sized for the
  selftest would force every translating board to provision for an app it does not run, so the
  two figures stay separate and only the DEFAULT moved.
- **THAT LOSS PRESENTED AS THREE SKIPS AND NOT AS A RED, which is the failing-declaration
  shape rather than a failing arm.** The self-grant arm reads the free-slot count live and
  takes one more, so it reaches its ceiling whatever the budget is; at zero free slots it seats
  nothing and skips by its own guard. Two of the three said only "thread pool too small",
  which is the spawn refusal's message and names the wrong resource. The skip set needed no
  edit in the end, the skips being gone once the budget was raised, and it was diffed by eye
  for that reason rather than trusted.
- **A SCAFFOLDING OP WAS HANDING OUT AN ADDRESS INSIDE THE FRAME POOL'S OWN WINDOW.** The
  seeded-address probe returned the run's physical base, on the stated ground that nothing in
  the space named it. Every thread stack is mapped at its own output address, so that window is
  exactly where a future stack lands: the promise held for the caller's space at that instant
  and never for a child's. It hands back a low-half address far from DRAM now, checked against
  the range list, which is a question the list can only answer because the stack is in it.
- **A MALFORMED SLOT IS DROPPED AND THE TAIL ADVANCES, deliberately.** Leaving it would let one
  bad publication wedge a ring for good, which is a denial the receiving node must not accept
  from the far side; the verdict is counted instead, so the drop is visible. The refused DEPTH
  is the exception and for the opposite reason: no slot has been identified to drop, and
  advancing on an index this node has just refused to believe would take the ring further into
  the far side's arithmetic. THAT REFUSAL IS BOUNDED NOW rather than permanent: four
  consecutive strikes resynchronise the tail to the far head and count the reset, so a far
  side owns one of its rings for four takes and not for the life of the image.
- **NEITHER SERVICE BOUND IS REACHABLE BY AN ARM, AND THAT IS BY CONSTRUCTION.** One call
  drains a ring's worth per sender and every peer's across the call, which is exactly what a
  static ring set holds, so no arm built out of publications can exceed either. What the arms
  pin is the FLOOR, one call emptying a full ring from every sender, plus two static asserts;
  the ceiling's whole subject is a peer refilling while the receiver drains, and that needs a
  hostile far side no in-tree node can play. What a green run says here is that the bounds do
  not truncate the honest path, never that they hold against one that is not.
- **THE PEER'S LEFTOVER RESTS ON THE DOORBELL BEING LATCHED, WHICH NO ARM CHECKS EITHER.** A
  publication made after a service call started rings the doorbell itself, and the raise is
  taken to survive being made while the handler runs. That is the GIC's and the CLINT's
  behaviour and not this tree's, and there is no seam by which the window could re-raise its
  own: `arch_ipi_send` services the caller's own bit inline rather than sending it, so a node
  cannot ring itself without re-entering the body it is already inside.
- **BOTH SIDES VALIDATE, and the SEND side's half is the one a receive-side test cannot reach.**
  The producer reads a tail the consumer owns, and a producer that believed it would compute a
  free-slot count out of it and overwrite slots the consumer is still reading. One ring per
  ORDERED PAIR is forced rather than preferred: a single inbox shared by every sender has
  several producers on one head, and moving that head needs a read-modify-write.
- **A FORGED PUBLICATION IS THE ONLY WAY AN ARM REACHES A BOUND**, no peer being willing to
  malform its own writing, and one arm is honestly weaker than the rest: a zero-length message
  being a message and not an empty ring is a DISCRIMINATION, and the mutation that reddens it
  is an addition rather than a deletion. Every other bound was reddened by deleting it.
- **THE SEAM FAMILY'S MEMBERSHIP IS AN ALLOWLIST OF IDENTIFIER PREFIXES**, so the first AMP
  member was invisible to the differ until the prefix moved, and the group table's refusal of
  an unclassified member cannot see one the family never admitted. The differ passed clean over
  a member it had never heard of.
- **A UNIT FIXTURE THAT DEFINES `arch_cpu_id` REDDENS EVERY SINGLE-CORE PRESET IN THE FLEET.**
  That gate reads tracked SOURCE and skips only a block opened by the literal
  `#if KICKOS_NUM_CORES > 1`, so a host fixture stubbing the seam is a finding on boards the
  fixture never runs on. It was found by running one single-core preset, never by the preset
  the fixture belongs to.
- **THE REENT DESCRIPTOR IS CHECKED AT BOOT NOW, AND NOTHING WITNESSES THE CHECK.** A thread
  slot number means nothing across kernels, so the app provisions one bank each and a shorter
  descriptor panics where it is read. The in-tree app provisions correctly, so no run reaches
  that panic: raising the required count by one is what shows it is live, and it truncates
  every image test on the board rather than reddening an arm.
- **THE PEERS ARE NOT AN ISOLATION BOUNDARY AND THE HEADER NOW SAYS SO.** `g_minted` sits
  outside the shared window, but every node maps the same writable kernel RAM, so a
  compromised peer KERNEL rewrites the mint, the counters and every other node's kernel data
  whatever the window validates. What is built is defence against a MALFORMED peer. Reading it
  as a privilege boundary is the misreading the label exists to stop, and per-node memory
  partitioning is the partition layout the contract leaves open.
- **THE AMP COLUMN STAYS A RULING EVEN THOUGH THIS STEP BUILT AMP.** What is ported is an AMP
  node on a part the predicate sends to the SHARED kernel, so the parts section 1 excludes
  still get no port and whether they are worth one is still open. And the HETEROGENEOUS case
  has no vehicle at all: the emulated i.MX8MP models the A53 cluster alone, ships no Cortex-M7
  companion, and cannot release a second core of the cluster either, so the two register writes
  the bring-up freeze rests on have no emulated far side on that machine.
**M7.6 LANDED S6b: THE PREDICATE IS DECLARED PER PART, AND THE BOARD THAT MOVED IT THERE BOOTS ONE
CORE OF FOUR RATHER THAN FOUR.** The audit item is discharged. What follows is what a green run does
NOT say, and the first item is a finding the step did not go looking for.

**THE FOUR-CORE HALF OF S6b's EXPECTED RESULT HAS NO VEHICLE, AND THAT IS THE MILESTONE'S HEADLINE
RATHER THAN A CORNER IT CUT.** `qemu-system-aarch64 -M imx8mp-evk` models no way to release a
secondary core at all, measured three independent ways that agree: every CPU object reports
`psci-conduit` 0, so there is no PSCI at any conduit and an `SMC` is an exception to our own vector;
cores 1 to 3 carry `start-powered-off` true with nothing in the model able to clear it; and the
reset controller and the mux registers that would do it on silicon are both `unimplemented-device`,
confirmed by writing a pattern and reading back zero while the model logged the discard. So the
board is refused above one core at COMPILE time rather than left to present as a boot that stops.
**The release was deliberately NOT written**, and that is the decision to argue with: the silicon
sequence is documented in the reference down to the register offsets, but it is an entry-point pair
split across two registers with an ordering the manual does not state, a released core arrives at
EL3 owing the same handover the primary gets, and no preset could compile it or run it. Unrunnable
code carrying a correctness claim was judged worse than a recorded mechanism. It is a raise, not a
ruling.

**THE PART IS WHAT FORCED THE LAUNCH AND THE PREDICATE APART, and the separation is the real
lesson.** Starting a core is not one of the six properties: two parts of one arch can meet every
one of them and still release a secondary by different mechanisms, which is exactly what `virt` and
this die do. Had the predicate absorbed the launch, this part would have been declared predicate-
FAILING, which is false: its cluster meets all six and the declaration says so. The refusal that
fires is the chip's own and says the machine cannot start a core, which is a different sentence.

**THE SPLIT ITSELF IS THREE AND THREE, and the reasoning is what to argue with rather than the
lists.** An arch declares what an ISA hands every part it defines: the exclusion primitive, the
mechanism a core reads its own identity by, and the coherency model the descriptors program. A part
declares what its die and interconnect decide: which inter-core interrupt it has, whether the
controller can point one line at one core, and whether the cores are interchangeable. Coherency
sitting on the arch side carries an OBLIGATION rather than a guarantee: the arch declaration is true
only while every chip of that arch programs shared-attribute descriptors over kernel state, and a
chip that did not would break it silently. Symmetry moved to the part because of this die
specifically, which is symmetric within its cluster and asymmetric across it.

**THE OWNERSHIP IS STRUCTURAL AND NOT CONVENTIONAL, which took a refusal in BOTH DIRECTIONS to
achieve.** The two owners get separate variable namespaces, but each declaration is included into
one shared scope, so nothing but an explicit refusal stops an arch file certifying GIC version,
routing and topology for parts nobody has seen, or one die's file satisfying the ISA's half for an
arch that declares it nowhere. **The second direction was missing when the step first landed**, and
that is worth keeping rather than tidying away: a boundary enforced one way is the naming convention
the namespacing alone would have been, which is the branch's own argument about the arch side turned
back on it. The cure is two parts, and the isolation is the load-bearing one: each owner's variables
are taken OUT OF THE SCOPE across the other's include and put back past it, so a line in the wrong
file cannot stand in for a declaration even when it writes the value the right file would have
written. The refusal on top is what makes that diagnosable rather than silently discarded.

**THE PART CASE COULD NOT BE WITNESSED BY A WHOLE-TREE CONFIGURE, and the gate says why.** No board
in the tree fails the part's three while passing the arch's: every chip that ships a declaration
declares all three. So the predicate became one authority driven twice, the build calling it and the
gate calling the same function over synthetic declaration trees. Seven cases, each against a control
differing in one clause, and each was checked by MUTATION rather than by passing: removing either
overreach refusal, rekeying the refusal on the count, collapsing the two owners into one list, and
deleting the refusal outright each redden exactly the arm that claims them. The AMP-model case is
the one worth keeping: it is a positive control that the refusal keys on the model, and rekeying it
on the count is what reddens it. **And the restatement case is the one that separates the two halves
of the cure**: a chip file setting an arch-owned property its arch ALREADY declares changes no value,
so replacing the scope isolation with a comparison across the include leaves that arm alone red
while the blunt case still refuses.

**THE HETEROGENEOUS CASE HAS NO VEHICLE EITHER, AND NEITHER S6b NOR S7 REACHES IT.** This part is
both an SMP part and an AMP part at once. The Cortex-M7 companion has its own NVIC and its own
instruction set, so it is asymmetric by construction rather than by policy, and it is startable from
the A53 side by two register writes, which is the concrete form of the ruling that a bootloader
owning a companion is a convention. The machine models the A53 cluster alone, ships no companion,
and decodes its tightly-coupled memory as unimplemented, so nothing on this bench is both at once.
Recorded so a later milestone does not discover it: this is not a gap in the AMP work, it is the
absence of any machine that could carry the case.

**WHAT THE BOARD'S GREEN RUN DOES NOT WITNESS.** The EL3 handover is exercised on every run, and
that is the one piece of it a run does witness. Nothing exercises the EL1-entry path, no vehicle
here producing it, and the EL2 refusal beside it has never fired. The UART enable is correct for
silicon and unwitnessable: the model emits on the data-register write whatever the enables hold, so
an image that never enabled the transmitter prints the same. No baud rate is programmed at all,
which the model ignores entirely. And the system counter's own enable is never written, the block
being unmodelled here; on silicon out of reset a stopped counter would read a constant with a
plausible frequency beside it, which is the shape that hangs a bounded wait rather than reddening
it.

**IT CLOSED AN OPEN ITEM NOBODY ASSIGNED TO IT.** M7.3 deferred a one-kernel-core GICv3 preset to
the AMP step, on the reasoning that the only configuration where one kernel core meets a live GICv3
was an AMP image. This board is that configuration for a different reason, so the `KICKOS_NUM_CORES
> 1` folds in the GICv3 backend now have a fleet preset that exercises them, and `cpu_id_fold`,
which skips above one DRIVEN core rather than above one kernel core, RUNS here.

**FIGURES, AND THE TREE THEY WERE TAKEN ON.** Everything measured for this branch was measured on
the branch's own tree with the host unit layer off for the cross presets, on a box shared with a
sibling branch's builds, and every count in this milestone is derivable by `ctest -N` and `ctest`
on the four presets involved rather than written here. NO FLEET SWEEP HAS RUN for this branch. The
board's own numbers were taken once each, not repeated: one core needs no repetition the way a
four-core image does, and no four-core claim is made here at all.

**M7.5 LANDED THE DEATH POINT ON EVERY BOARD AND RETRIAGED THE SKIP SET, both witnessed by a full
fleet sweep against a frozen tree.** Host: 57 presets, 2411 host tests, 57 pass and 0 fail. Image:
57 presets, 490 image gates, 20 run and passing, 0 partial, 0 fail, 0 skipped, 37 presets
registering no image gate. Both wrote their DONE sentinel, and the image half took its expected
empty and skip counts on the first declaration rather than refusing. **The figure item 1 needed is
in neither summary line: `trap_redzone` appears in all 57 preset logs and all 57 passed**, so no
preset moved past its red zone on any of the five classes rooted at `syscall_dispatch`. Host is +2
against M7.2 over the same 57 presets, which is this branch's new unit coverage and nothing else;
the image half is unmoved in every figure, so this branch added no image gate and moved no preset
out of the set registering none.

**M7.4 LANDED S5: THE RV64 BACKEND, ITS DOORBELL AND LOCK, AND THE SMP-SEAM VERDICT.** Four harts
run kernel code on `qemu-riscv64-smp` under one lock. The verdict is below, with the seam records.
What follows is what a green run does NOT say.

- **TWO DEFECTS IN THIS STEP WERE FOUND ONLY BY BUILDING AND RUNNING, AND NEITHER HAD A GATE.** The
  kernel lock was never released across a swap on the new backend: the tree held exactly ONE caller
  of `kickos_switch_unlock`, in the armv8a switch, so the first RV64 core to reach its idle thread
  carried the lock into `wfi` and three peers spun on a word its holder was asleep behind. A release
  the seam requires but one backend calls is invisible to every gate in the tree. And the doorbell
  poll cleared `sip.SSIP` while servicing exactly one of the THREE sources riding that cause, so a
  device line whose raise the poll absorbed left its driver asleep for good.
- **THE INSTRUMENT THAT FOUND THE LOCK DEFECT IS WORTH MORE THAN THE FIX: bisect by MODEL.** The
  same image passes at four harts under AMP and hangs under the shared kernel, which puts the fault
  in the lock and cross-core scheduling in one step and touches no debugger. The emulator monitor
  then named it outright, the lock word held, an owner cell naming core 0, and core 0's PC inside
  the idle wait.
- **A HART PARKED IN `wfi` OBSERVES NO STORE.** `WFI`'s wakeup is gated by `mie` alone and not by
  `mstatus.MIE`, so secondaries parked before anything enabled an interrupt could never leave: the
  release word was state nothing was watching for. The release publishes the word AND rings the
  hart's own `msip` as the edge, and the park drops the edge before reading the state, a
  level-sensitive `msip` left set otherwise carrying into supervisor mode as a doorbell no peer
  asked for.
- **THE SEND HAS NO MACHINE-MODE LEG AND THE RULING BOUGHT A SMALLER TRAMPOLINE THAN EXPECTED.**
  `mideleg` bit 3 is not writable here, measured, so a peer's raise arrives as a machine software
  interrupt that must be lowered; but a SUPERVISOR store to a peer's CLINT `msip` word is permitted,
  also measured, so only the RECEIVE side runs in machine mode.
- **THE SOFTWARE INTERRUPT CONTROLLER'S STATE IS IMAGE-WIDE AND KEYING IT PER CORE IS A DEFECT,
  which is the opposite of the state inventory's classification and of what the rest of S5.4 did.**
  That classification reads them as mirroring per-core interrupt-controller registers, which is
  right for a GIC's banked bank and wrong here: this board implements no controller at all, so a
  line is one logical resource. Keyed per hart, a driver that unmasks on one and an injector that
  raises on another never meet, and the raise sits latched on the injector's hart awaiting an
  unmask only the driver's hart will make. It hangs the four-core selftest at the first arm that
  drives a line, and it hangs rather than reddening.
- **MAKING THOSE CELLS IMAGE-WIDE WAS NECESSARY AND NOT SUFFICIENT, and the first attempt's own
  comment was wrong about why.** It said the kernel lock covers every caller. It does not: the ISR
  path brackets with an EPOCH rather than that lock, and `irq_event_isr` masks a line from inside
  it, so a mask on one core could clobber a rearm's unmask on another and leave the line masked
  with nothing left to unmask it. Two further shapes went with it. A raise was ONE LINE IDENTITY
  for the whole image, so a second producer overwrote the first before any dispatch consumed it;
  it is a SET now, and the dispatch services every line in it. And an inject that found the line
  masked latched it without re-checking, so a rearm reading the pending word an instant earlier
  lost the event; the inject re-reads and takes the bit back, exactly one side taking it. Every
  mutation of the three words is one instruction. The zero-valued encodings stay, being about
  initialisers rather than about keying.
- **`invalidate_all()` MEANS DIFFERENT THINGS ON THE TWO BACKENDS, AND COPYING armv8a'S SEQUENCE
  COPIES AN ASSUMPTION ABOUT BROADCAST THAT DOES NOT HOLD HERE.** Its `tlbi vmalle1is` clears
  every core before it returns, so ordering a free after it is safe there; `sfence.vma` reaches
  the executing hart alone, so the same sequence frees frames while peers still hold cached
  translations into them. The two operations are therefore ONE UNIT here,
  `invalidate_all_everywhere`, and the comment naming the trap sits at the call a porter would
  otherwise reach for. **The next backend written against armv8a's shape inherits this**: check
  what the model's invalidate actually reaches before reusing an ordering built on it.
- **THE CROSS-CORE SHOOTDOWN WAS WIRED ONLY TO ELIDE, WHICH IS THE HALF THAT COSTS NOTHING.** The
  derived active-core set answered "no core holds this space" and skipped maintenance, correctly;
  the case it exists to detect, a space a PEER holds, had no send at all, so unmap and destroy
  invalidated the calling hart and left every peer on a revoked translation. **This backend owes a
  send everywhere armv8a gets peer reach from hardware**: `tlbi vmalle1is` and `vaae1is` are
  inner-shareable, `sfence.vma` has no broadcast form at all, so map, unmap and destroy each
  rendezvous once per call. The execute-permission gate armv8a uses is NOT copied: that is its
  instruction-side optimisation, and gating on it here would leave every DATA removal unsent.
- **AND THE FAR SIDE FENCED IN THE WRONG ORDER, which the send made reachable and an audit
  caught.** The service body executed `SFENCE.VMA` and only then loaded the request, so a peer
  could fence, an initiator could then write tables and raise, and the peer could answer a request
  its fence never saw. The answer was truthful about the fence and silent about which writes
  preceded it. The order is three-part now and stated as such: observe the request, THEN fence,
  THEN answer, with the acquire on the load pairing against the initiator's release of the raise.
  One fence per service rather than one per requester, `SFENCE.VMA` with both operands x0 being
  global.
- **THE SCATTERED STALL IS RETIRED, AND THE TREE THAT SHOWED IT NO LONGER EXISTS.** A run of
  2 failures in 30 was measured against a working tree carrying an IN-FLIGHT version of the
  interrupt-controller repair, never committed in that form. Three trees separate the readings.
  BEFORE the repair, the stall is 3 in 40 and every one of them stops in the same arm,
  `irq_autorearm`, which is the per-core-keying defect recorded above. WITH the repair complete
  it is 0 in 40, and at the tip 0 in 40 twice over, instrument on and instrument off. The
  measured tree sat between those two, and its stalls were in `sem_destroy_quiescent` and
  `call_timeout_reply`, arms nowhere near the interrupt block. **MASTER COULD NOT HAVE CARRIED
  IT, and that is a mechanism rather than a bound:** the per-core block the defect lived in does
  not exist there, `arch/riscv/rv64imac/smp.cmake` does not exist there, and no riscv preset
  there names a four-core board. The lost wake was introduced by this branch's per-core keying
  and cured on this branch, so no longer run against master is owed. **A half-applied fix did not cure
  the lost wake so much as MOVE ITS WINDOW**, which is the reading the three shapes support and
  the one nobody can now confirm directly: that tree is unreachable and its number is not
  reproducible by anyone. 120 clean runs at or after the repair bound the rate under about 2.5
  percent rather than proving it zero.
- **AND THE INSTRUMENT BUILT TO NAME THE CAUSE NEVER CAUGHT ONE, so its discrimination is
  unexercised.** `kickos/smptrace.h` separates a park no waker searched for, a search that came
  back empty, and a wake whose switch never took; it was validated on a PASSING run, where it
  correctly reports that no thread parked and stayed parked, and on 40 instrumented runs it had
  nothing to decode. It is kept, off by default and compiling to nothing, because the next lost
  wake on any backend is what it is for. Nothing here says it works on a real stall.
- **A FOUR-CORE IMAGE NEEDS REPETITION BEFORE "GREEN" MEANS ANYTHING, and two passes in a row is
  not repetition.** Four of this milestone's defects announced themselves as a HANG rather than as
  a red arm, which is the shape a lost wake takes, and a fifth is still open. Every claim about a
  four-core image in this file rests on a run COUNT, and a single-figure count is not a claim; a
  count taken while anything else uses the box is not one either.

**M7.3 LANDED S6: A SECOND INTERRUPT POSTURE ON `qemu-arm64`, GICv3 BESIDE GICv2 RATHER THAN
INSTEAD OF IT.** What follows is what a green run does NOT say.

- **THE GROUP MISMATCH IS SILENT IN HARDWARE AND LOUD IN THIS FLEET, AND NOTHING IN THE TREE
  OBSERVES A GROUP DIRECTLY.** `ICC_SGI1R_EL1` generates Group 1 alone, the controller drops a
  mismatched interrupt with no fault and no log, and the loudness is bought entirely by the timer
  PPI riding the same group decision: put it in Group 0 while the SGI stays Group 1 and six gates
  fail. The protection is therefore INCIDENTAL. A future backend that grouped the doorbell and the
  timer separately would boot, answer doorbells and hang, and no arm would name the reason.
- **THE FOUR REDS THIS STEP WAS PROVED AGAINST, each named by what it refuses.** A wrong
  `GICR_BASE` prints `gicv3: no redistributor frame carries this core's affinity` and TERMINATES
  rather than hanging, which is the whole point of discovering the frame by `GICR_TYPER` instead of
  indexing it by core number. A GIC version neither backend implements refuses at CONFIGURE with
  the value in the message, not at link with an absent triad. A wrong acknowledge-event name fails
  the doorbell gate's timer PARSE CONTROL rather than reporting a vacuous absence of doorbell
  acknowledgements. And the group mismatch above takes six gates.
- **THE GICv3 DISABLE PATHS RETURNED BEFORE THE DISABLE TOOK EFFECT, AND EVERY GATE WAS GREEN
  OVER IT.** Found by external audit after the step was committed. GICv3 applies a cleared
  enable asynchronously and RWP reports the completion; the backend read `GICD_CTLR.RWP` at the
  three control writes and read the REDISTRIBUTOR's RWP nowhere at all, so all five `ICENABLER`
  writes returned early. The operational one is `arch_irq_mask`, the driver teardown path,
  where it is the contract's section 4 gap 2 in a new place: a mask that is not exclusion.
  **Nothing on this bench can show it and nothing on this bench ever will** -- QEMU completes
  these writes synchronously, and a probe that refused the instant RWP read set booted clean
  through the whole selftest and a 64-round doorbell check, so the poll is never once entered
  here. A GIC-500 need not behave that way. The lesson is the shape, not the register: the file
  had the mechanism written down in its own words for one register and applied it to that one
  only, which is the kind of gap a green fleet is structurally unable to report.
- **A NON-PROBLEM I CHASED TURNED UP A REAL SECOND TRUTH BESIDE IT.** I flagged the GICv3 trace
  parse as assuming core numbers below ten; re-reading showed the radix is converted explicitly and
  a planted 18-core log confirmed cores 10, 15, 16 and 17 each match once. What WAS wrong sat one
  line away: the timer INTID was hand-spelled twice, `acknowledged irq 30` and `value 0x1e`, with
  nothing checking the two agreed, so a changed INTID would have left the planted control proving
  the parse against a line the emulator never emits. Both suffixes are now PRINTED from one
  constant. **Verify a risk you wrote down before acting on it; the fix may not be where the note
  says.**
- **THE GICv3 DISPATCH HAD NO HEADER AT ALL until this step.** `arch_armv8a.cc` hand-declared
  `kickos_armv8a_gic_dispatch` beside a backend that defined it, two declarations with nothing
  checking they agreed. It is in `arch/arm64/common/gic.h` now with the rest of the family.
- **EMULATOR-GRADE, AND ONE DEGREE FURTHER THAN SECTION 7 OF THE CONTRACT STATES.** QEMU's GICv3
  model is not a GIC-500, so "the posture matches the silicon target" is a claim about the
  ARCHITECTURE version and not about the i.MX8MP's implementation of it.

**M7.2 LANDED S3 AND S4: THE BIG KERNEL LOCK, THE DOORBELL, THREADS ON EVERY CORE, AND THE
CROSS-CORE MAINTENANCE THEY OWE.** Two external audits closed with no open correctness or security
blocker. What follows is what a green run does NOT say.

- **THE BOOT SELFCHECK USED TO KILL A WORKING MACHINE, AND THE LESSON IS ABOUT INSTRUMENTS RATHER
  THAN ABOUT THE LOCK.** Its contention arm asserted that every peer had taken the lock inside a
  fixed round count, which a peer can only do in the gap between the primary's release and its next
  acquire. Losing every gap made the IMAGE call `kfault_terminate`. Saturating the box reproduces it
  3/3; idle it passes 5/5, which is why it read as weather for a while. **A gate that fails only
  under load is a claim about the host, not about the kernel.** The fix waits, bounded, for the
  state each arm needs instead of racing for it.
- **THAT CHECK'S TWO ARMS WANT OPPOSITE PEER STATES, and making one deterministic breaks the other
  unless they are separated in time.** The emulator arm needs QEMU's GIC to report each secondary
  ACKNOWLEDGING the doorbell, which a core polling in the acquire loop never does; the poll arm
  needs the peers masked and spinning. Hence two phases, interrupts open then peers observed
  spinning. Anyone tightening this must keep both phases or one arm goes vacuous silently.
- **THE POKE'S FIRING IS SCHEDULING-DEPENDENT, AND THAT IS CORRECT RATHER THAN A DEFECT.** A peer
  that has already switched off a dying space took its Context synchronization event in that switch,
  so the rendezvous is owed only while a peer is still on the space. Over six runs of the four-core
  selftest the run-total came out 0, 4, 4, 6, 6 and 8. So NO arm may assert that a task kill
  produced a poke; `doorbell_xpoke` asserts a per-core service floor and a pairing invariant, both
  of which the bring-up check makes deterministic. **There is no route from an arm to force one**:
  `KOS_MEM_FLAGS_ALL` is NOCACHE alone, so unprivileged code cannot create the executable page
  whose removal would owe the poke. Making one forceable is a decision, not a fix.
- **`hello` CAN NEVER WITNESS THREADS ON EVERY CORE, AND NO SAMPLING BOUND FIXES IT.** It holds one
  runnable thread: ping and pong alternate through semaphores with a 400 ms sleep each, so which
  core picks up the single sleeper is the host's draw. Sampled to 180 s under load the vCPU set
  plateaus at two of four. The gate boots the `stress` soak for that reason -- more runnable threads
  than cores, so a core running no thread is one the scheduler left idle beside a ready thread.
- **THAT MAKES THE SOAK'S SIZE LOAD-BEARING, AND ITS BUDGET PROBE CAN SHRINK IT.** `stress` sizes
  itself down against the board's thread and semaphore pools independently of `MAX_PAIRS`, and a
  soak at or below the core count returns the gate to a lottery. The app reports the size it
  realized and the gate refuses anything at or below the core count; without that line the gate
  would pass while proving nothing.
- **`trap_redzone_decls` DOES NOT CATCH A STALE PIN LINE.** `trap_redzone_indirect.txt` binds call
  sites by exact `file:line:column`, and of its readers only `check_trap_redzone.sh` and
  `check_console_reach.sh` reject a stale line -- both PER SCOPE, so a re-pin is only witnessed by a
  preset that compiles that scope. `irq.cc` is pinned in armv6m, armv7m, lx6, rv32imac and rxv3 and
  NOT in armv8a, so an arm64-only run proves nothing about it.
- **THE AMP POSTURE IS REACHABLE BY CONFIGURATION ALONE**, `KICKOS_MULTICORE_AMP` in `Kconfig`, which
  sets one kernel core while the image still drives four. It links `kickos_arm64_doorbell_service`,
  so gates keyed on the kernel-core count silently skip a body that exists. That is why the ISB gate
  is keyed on `KICKOS_NUM_CORES`.
- **THE ACTIVE-CORE SET WAS BUILT DERIVED AND THAT WAS A RULING.** Its frozen form, a readable field
  on the opaque space, is unimplementable: on all three backends the opaque handle IS the root
  page-table frame, so there is no object to hold a field. A per-core installed-root array written
  only where the register is written answers the same question and changes no signature.

**THE FLEET WITNESS FOR M7.2 IS COMPLETE AND BOTH HALVES STAMP THE MILESTONE'S TIP.**
Host: 57 presets, 2409 host tests, 57 pass, 0 reused, 0 fail. Image: 57 presets, 490 image gates,
20 pass, 0 partial, 0 fail, 0 skipped, 37 declared with no image gate. Both carry a `DONE`
sentinel, and the image half took its declared figures on the first attempt rather than refusing.
**The host DELTA is what says this milestone's gates ran fleet-wide**: S1 and S2 witnessed 2276
over the same 57 presets, so +133 is S3 and S4's own arms on every preset that registers them.
**The image figures are UNMOVED from the last run taken before the records commit, and that is the
expected reading rather than a stale sweep**: everything landed since is documentation, so no image
gate was added and nothing could move a preset out of the 37 that register none. Neither half says
anything about `-LE host` under load: the host tool never runs that set, and the image tool
serialises it to remove the instrument's own noise rather than to show the gates are
load-independent.

**M7.2 IS S3: THE LOCK, THE DOORBELL, AND THREADS ON EVERY CORE.** `IrqLock` became the big kernel
lock rather than 129 call sites changing, its own comment having claimed "interrupts off => exclusive"
which is false above one core. A per-core depth takes the lock on the zero-to-one transition only.

**THE LOCK SPANS THE CONTEXT SWITCH, AND THAT IS WHAT CLOSED GAP 3 WITHOUT AN EPOCH.** Until the swap
parks it, an outgoing thread's saved frame still names an earlier run, so the lock is handed THROUGH
the switch and released in `switch.S` where the outgoing frame is parked. The flag that keeps it held
across that window is the "a swap owes the park" signal gap 3 was asking for, so a holder observing
EXITED observes a parked context and the window has ZERO width. **An epoch was deliberately not
built**: it would be a second answer to one question, and the obvious cell to build it on is
pre-seated BEFORE the park, so a guard on it would answer "already off-CPU" in exactly the window it
was guarding. A consequence worth keeping: the lock is never held with interrupts unmasked, so an
interrupt always enters at depth zero.

**THE DOORBELL'S WAIT IS SOFTWARE ON EVERY BACKEND, and section 7 previously said otherwise.** No GIC
version reports to a SENDER that a target serviced a software-generated interrupt: v2's per-source
pending registers are banked to the ACCESSING core, and v3's carry no source identity. The old claim
that the rendezvous is "A64-free" conflated the DATA half, which coherency does supply, with the
acknowledgement half, which nothing does.

**DELETING THE PARKED CORE'S INTERRUPT UNMASK LEFT THE IMAGE'S OWN CHANNEL FULLY GREEN.** WFI wakes on
a pending SGI and the software poll then answered everything, so the count, the postconditions and the
raise total all held. Only QEMU's own GIC trace caught it. That is why the doorbell gate's oracle is
the emulator's and not the image's.

**AND THAT ORACLE CANNOT SEPARATE TWO THINGS, WHICH IS WHY THE GATE IS SERIAL.** A core already
spinning in the lock's acquire loop answers the doorbell by POLLING and acknowledges nothing, so
"the interrupt path never ran" and "the host starved that core" read identically. Under a parallel
`ctest` a correct image went red once in three runs. The refusal says so itself.

**THE SKIP SET'S LABEL WAS A MISDIAGNOSIS, AND THE LABEL COST MORE THAN THE SKIPS DID.** It reads
"a cross-thread progress order is not a property of N kernel cores", and this file used to say 28
arms assert a single-core ORDERING. For at least four of the five tier-1 IRQ arms reworked in M7.5
the progress order was only how the precondition got MANUFACTURED; the real blocker is an ABSENCE
CLAIM, a service, a redelivery or a wake that must not happen, and a non-event raises nothing for a
later read to be ordered after. **THE PROOF is one mutation answering two questions: deleting the
masked window the coalescing property is about reddens `irq_mask_coalesce` and `irq_discard` at one
core and PASSES at four.** Six arms came off the set, 28 to 22. **Ask each of the remaining 22
whether it asserts that something did NOT happen, not whether it assumes an order.** The class is
still skipped whole rather than one at a time, for the reason that has not changed: a failing arm
bails without releasing its pooled object, and one flaky arm became 59 red arms in a single run.

**FIVE ARMS NOW REPORT PARTIAL ABOVE ONE KERNEL CORE FOR TWO DIFFERENT REASONS, and collapsing them
into one loses the distinction.** `irq_spurious`, `irq_mask_coalesce`, `irq_discard` and
`irq_stale_register` each carry a half that is a NON-EVENT, unwitnessable wherever the observer runs
concurrently with the subject; the one-core fleet still checks those halves in full.
`thread_slay_window` is the other reason and not the same one: its control leg needs the victim to
reach a park before the kill lands, an UNESTABLISHED PRECONDITION that the machine grants or does
not, so a spent restage budget is a partial and never a red. A red that is a claim about the host
rather than about the tree is the failure mode this project pays most for.

**`errnoprobe` CANNOT PASS ABOVE ONE KERNEL CORE AND NO PER-CORE KEYING FIXES IT.** newlib's
reentrancy state is reached through ONE word in the process's memory, and it is read at EL0 where a
thread cannot ask which core it is on, so two threads of one process on two cores share an errno. The
seat belongs in thread-local storage. Declined VISIBLY as a skip carrying the mechanism.

**AND THAT DECLINE IS NOW REPRODUCED ON TWO ARCHES RATHER THAN REASONED, which is what M7.4 added
to it.** The claim had never been TESTED: arm64 does not register the test above one kernel core, and
no other four-core board registered it at all, so "would assert a gate that can never pass" was an
argument standing on its own. The four-core RV64 board is the first that would have run it. It runs
the arm and reports the seat's own failure, a preempted thread coming back on the peer's errno; the
same app on the four-core arm64 preset reports the identical one. The RV64 registration carried no
core-count clause only because no four-core RV64 board existed to need one, and it has arm64's now.

**THE SLAY-GUARD PREDICATE IS RIGHT AND HAS NO RUNTIME ARM.** Idle control blocks sit outside the
pool, so no user handle can name one and the branch is unreachable from userspace. It becomes
load-bearing the moment an idle block is poolable, and mutating it today reddens nothing.

**THE SMP SEAM BASELINE MOVED TWICE IN THIS MILESTONE, so S5 no longer measures from the frozen set.**
Cumulatively THIRTEEN records added across S3 and none changed or removed: the lock and its macro,
then the switch release, the peer ready and start pair, and the resched entry, every one of them the
kernel-to-arch direction so no port owes an implementation. S5's verdict will be its own delta, and
this line is where the from-frozen figure lives.

**S5'S DELTA IS ZERO, AND THAT IS A RESULT ABOUT THE SEAM RATHER THAN AN ABSENCE OF NEWS.** A second
backend went in whose identity is a published index rather than a register read, whose doorbell is a
CLINT word lowered through a machine-mode trampoline rather than a GIC software interrupt, and whose
lock is LR/SC rather than load/store-exclusive; not one member of the seam moved. The corpus is real
and not empty: 25 signature records on each side, every group above its floor at extent 2/2, identity
2/2, doorbell 8/8, lock 4/4 and peer 9/9, and the differ's own 47-record known-answer control
answering as expected. What it still cannot see is what a backend DOES behind a member, the coupling
included, so the acquire loop's servicing of a pending doorbell is witnessed by the bring-up check
and by nothing in this verdict.

**THE HOST UNIT LAYER IS OFF WITHOUT A GTest, AND THAT HID A SUITE THAT DID NOT BUILD.** A new
two-core unit suite failed to compile once GTest is provisioned, and a plain preset never revealed it.
A fixture defect from the same cause is worse than the suite: an `arch_start` stub that RETURNS lets a
deliberately-abandoned bracket unwind and underflow the lock depth, after which every acquire in the
process is silently skipped. Derive both counts rather than quoting them, and provision GTest before
believing a unit verdict.

**THE PER-CORE BLOCK IS PADDED TO A CACHE LINE, and this file argued against that before doing it.**
The argument was that padding is a compile-time bet the maintenance code beside it refuses to make,
reading the line size from a register at run time. That was wrong: this family ALREADY gives its
per-core rows a line each, with named constants, so the bet was made twice before. Its size is a
literal the secondary entry's assembly spells, so the alignment and that literal move together.
**What is still unpadded is the ISR, fault-report and dispatch DEPTHS**, which are compact arrays on
the hottest path, and the block now has room to hold them.

**THE BANNER NAMES THE CPU NOW, WHICH MOVED `.rodata` ON EVERY IMAGE OF EVERY BOARD.** The resolution
is the compiler flag's token where a family has one, the chip's own statement where none does, and the
platform's declaration for a host build; no toolchain file states a core name. A first attempt asserted
the core FAMILY in the gate and reddened `qemu-x86_64`, whose name matches no family it listed, so the
witness checks presence and then the placeholder instead.

**AN EXTERNAL AUDIT OF M7.0 AND M7.1 RETURNED CONDITIONAL ACCEPT, AND ITS TWO BLOCKERS WERE BOTH
MISSING ARMS RATHER THAN WRONG CODE.** Both are closed and both redden on mutation. The first is the
one worth carrying: **this file previously recorded the arrival witness as mutation-proven, and that
claim was false.** The mutation had been a hand-run image, not a gate, and turning
`release_secondaries()` into an early `return;` left `ctest` at 44 of 44 green. So the bring-up had
no witness in the tree at all while a record here said it did. `tests/integration/check_smp_arrival.sh`
is the gate now.

**ASSERTING THE BANNER'S CORE COUNT IS A TAUTOLOGY, AND THAT IS WHY THE GATE RUNS A SHORT MACHINE.**
The image prints `KICKOS_NUM_CORES` and a gate expecting that same symbol learns nothing: mutating
both release loops to stop at index 2 releases one secondary of three and the banner still reads 4.
The second arm runs the image on `-smp N-1`, where PSCI answers `INVALID_PARAMETERS` only for a loop
that actually reaches the last index, so the refusal names it. **That is an oracle the image does not
supply, and it is what binds the loop bound.** The same shape is what the predicate refusal owes:
`tests/static/check_smp_predicate.sh` configures four cores on an arch shipping no `smp.cmake`.

**A WRITE NOBODY READS IS NOT A WITNESS, AND THE ARRIVAL LOOP NOW STARTS AT ZERO.** The primary
published its own arrival into cell 0 and the wait loop started at 1, so commenting that write out
changed nothing. The release loop still skips self, because a core does not `CPU_ON` itself; the
ARRIVAL check is the same question at every index, so it asks it at every index. Removing the
primary's publication now reddens the gate.

**FIVE THINGS THE AUDIT RAISED ARE ACCEPTED AND DEFERRED TO THE STEP THAT NEEDS THEM**, and none is
reachable today because secondaries park with interrupts masked. `g_isr_depth` and the fault-report
depth are shared scalars, so one core's interrupt could make another defer its switch once peers run
kernel code; they join `Kernel::idle` and `Kernel::boot` as state owed a per-core key. The per-core
blocks are 24 bytes, so four of them share 64-byte lines and would bounce under real switching.
`ARMV8A_CORE_STACK` and the linker's `_kernel_stack_size` both spell 64 KiB with no cross-check,
which is a second truth. The arrival timeout is a spin count rather than a duration, though
`CNTFRQ_EL0` is already live when it runs. And the SMP predicate is declared per ARCH while GIC
version, routing and topology are properties of a PART, so a future armv8a chip inherits the
declaration unreviewed.

**ONE AUDIT ACTION IS REFUSED: it asked that the configure predicate keep an explicit link to its
contract document.** Code citing an in-repo document is the direction nothing gates, and the refusal
now enumerates the six required properties in its own message, so the reader gets what to implement
rather than a file to open. A diagnostic that names a document is worse than a comment that does,
because the reader is already stuck.

**AN `ESR=0x2000000` AFTER THE PLAN LINE WAS REPORTED AT FOUR CORES AND DOES NOT REPRODUCE.** Four
runs came back clean, one at the parent commit and three with the fixes, and the gate is green. The
likely cause is a build taken while another actor was mutating the tree. It is recorded as
UNREPRODUCED rather than dismissed, because EC 0 is `unknown reason` and four clean runs is not proof
of absence. **What it does illustrate is real: a fault after the plan line is something a
TAP-reading gate can miss**, which is the same blindness the arrival gate existed to close.

**M7.1 IS SQUASHED AND UNPUSHED, and `backup/M7.1-presquash-20260831` is the only copy of its
step history**, local and unpushed, so a `branch -D` loses the step-by-step evidence.
`master` is byte-identical to `origin/master`.

**THE SIX LINE-BOUND FILES BLOCK FOUR COMMENT FAMILIES, not the five doc citations this file used
to name as the whole remainder.** `tests/static/trap_redzone_indirect.txt` binds call sites in
`sched.cc`, `syscall_ipc_fast.cc`, `irq.cc`, `console_tx.cc` and the two rp2 chip files by exact
`file:line:column`, so a comment edit there costs a re-pin of the table. What is stranded behind
that: every surviving in-repo doc PATH in code, three step designators, and the DEFINITIONS of the
`INVARIANT H1..H8`, `D1..D9`, `B1..B3` and `RULE L1/U2/U4` families, whose call sites elsewhere are
converted while their definitions are not. The selftest reads `// H1 mutual exclusion` beside
`// close-of-owned refused`, which is what a half-finished family looks like. **The re-pin is
mechanical rather than manual** (difflib re-pinned 105 records in one step earlier), so the cost is
smaller than the count suggests; the real fix is the `TODO.md` item asking for a binding an edit
does not move.

**CODE NO LONGER CITES AN IN-REPO DOCUMENT, and the direction was the ungated one.** A comment
naming a design document made the code depend on a file that can be renamed, and nothing checked it:
`check_doc_names.sh` validates markdown naming code and never code naming markdown, so a comment
citing a nonexistent document passed every gate. The rule now is to state the constraint and never
cite the document, and a gate's refusal says what to implement rather than which file to open.
EXTERNAL manual citations are untouched and stay, being immutable and outside the repo. **No gate
enforces the new direction yet**, so it will regrow; adding one is blocked on the five sites above.

**M7 IS OPEN AND M7.0 IS LANDED: the multicore contract, and the SMP seam frozen before either
backend.** `docs/design-multicore.md` is the contract. It opens on a HARDWARE PREDICATE rather
than an architecture family, because a family name is a proxy that dates the first time a part
violates it. The MMU is NOT one of the six properties and per-line interrupt targeting IS, which is
what puts the RP parts in the AMP column: the RP2040 datasheet says the same interrupts reach both
cores' own controllers, so a line "granted" to one core is granted only because the other masks it.
**THE LX6 NO LONGER LANDS THERE, AND THIS FILE SAID IT FAILED COHERENCY.** That was measured out of
the ESP32 manual and is too broad: the part's two caches sit only on the external flash and PSRAM
path, their pools carved OUT of internal SRAM rather than covering it, so kernel state placed in
internal SRAM satisfies the coherency requirement by its second clause. Its interrupt matrix is a
real distributor besides, per-CPU map register banks with an unconnected input serving as a routing
sink, so it passes targeting for a stronger reason than the old wording allowed. Its verdict is a
GATE rather than a ruling now, and what gates it is a per-core data book: the Xtensa ISA defines
both remaining primitives, the compare-and-swap and the processor identity, and defines both as
configurable OPTIONS whose realisation no document on this bench states. **The record still says in
as many words that the RP parts CAN run a shared kernel**, FreeRTOS shipping a dual-core RP2040
port, so their exclusion is argued as a value judgement and not re-derived from a link.

**THE FLEET WITNESS FOR M7.0 IS 56 presets, 2247 host tests, 56 pass, 0 reused, 0 fail**, with a
`DONE` sentinel. The figure that matters is the DELTA: M6.5 witnessed 2191, so +56 is exactly one
new gate on every preset and is what says `smp_sigdiff` actually ran fleet-wide rather than being
skipped on most of it.

**FOUR THINGS M7.0 DECIDED THAT A READER WILL OTHERWISE RE-LITIGATE.** The deliverable is a second
core and NOT a lock, because a big lock compiles to nothing at one core, so a correct one and an
absent one are the same image and shipping it alone would ship an empty corpus. One IPC mechanism,
not two, locality never reaching the API, which `KOS_EP_MSG_MAX` at 256 makes affordable since a
ring slot at full message size costs about 1.5 percent of an RP2040's memory. No capability
authorises a core crossing, the inter-core interrupt not being a user-facing operation at all, and
privilege sitting at the two configuration acts instead. And the doorbell seam serves BOTH the
shared-kernel interrupt and the AMP inter-node doorbell, so narrowing its contract to SMP semantics
would break AMP before AMP is written.

**S1 AND S2 ARE LANDED: PER-CORE STATE AT ONE CORE, THEN FOUR CORES BOOTING AND PARKING.** The
witness is 57 presets and 2276 host tests, and the delta against M7.0's 2247 is one new preset's
worth, which is what says `qemu-arm64-smp` ran rather than being skipped. Four cores is a POSTURE of
the arm64 board, stated by a choice the way the paging mode is, so `KICKOS_NUM_CORES` stays
unprompted as its own help text requires.

**RELEASED IS NOT ARRIVED, and that distinction is the only reason the bring-up has a witness.**
PSCI answering SUCCESS says a core was STARTED; a core handed a bad entry, a bad stack or a
translation it cannot walk is started and never reaches its own code. The release waits on each
core's own online byte and refuses by name, then states the count positively, because a build that
released nobody would print nothing and boot clean. Mutation-proven both ways: with the online byte
unpublished the boot dies naming core 01.

**FOUR LATENT DEFECTS SURFACED AND NONE WAS CAUSED BY THIS WORK.** The multi-core arm of
`armv8a_percpu` had NEVER COMPILED, a function sharing a struct's name hiding that struct's
constructor under `-Wshadow` where this tree is `-Werror`; it shipped at M6.2's T9 and is cited in
three files' comments. `cpu_id_fold` had no `SKIP_RETURN_CODE`, so the exit 77 it has always been
able to return read as a FAILURE the first time a preset returned it, this being the first test in
the tree that ever skips. `map_tlbi_elided` asserted a single-core figure unconditionally. And the
fold gate's own scanner could DIE and still report PASS, which was found by writing `and` for `&&`
in its awk and watching it go green over a corpus it never read. **The class is the one this project
keeps meeting: an arm nothing exercises says nothing, and the first thing to exercise it finds
whatever was wrong with it.**

**TWO THINGS S3 INHERITS THAT A GREEN RUN DOES NOT SAY.** `arch_irq_mask`, `arch_irq_unmask` and
`arch_irq_clear_pending` write BANKED GIC registers for a line below 32, so they act on whichever
core calls them; that is correct at one core and is a semantics question the moment threads run on
more than one. And `Kernel::idle` and `Kernel::boot` are classified per-core in the state inventory
and are NOT keyed: harmless while secondaries only park, required before a second core runs threads.

**THE CACHE CLEAN THE HANDOVER OWES HAS NO WITNESS HERE.** The primary publishes a boot record that
secondaries read with their caches off, and `arch_dcache_flush` to the point of coherency is what
makes that legal. QEMU models no data cache, so deleting that flush passes every run on this bench.
It is in the code because the architecture requires it, exactly like the fresh-map invalidate.

**THE SIX WINDOWS A BIG LOCK LEAVES OPEN ARE RECORDED IN SECTION 4 OF THE CONTRACT, and three of
them never had a lock to substitute for.** The audit that produced them refuted two things worth
carrying: `cap_teardown`'s deliberate mid-chunk lock drop is FINE under a shared kernel, and the
authority word's unlocked read is safe because every reader passes the CURRENT thread and no path
reads a peer's. What the fastpath gap costs is a RULING and not a fix, neither M7 backend linking
it. Every verdict there is conditional on the current-thread pointer becoming per-core, which is
M7.1's first item.

**M6.2 is CLOSED, every T-step landed, and `qemu-arm64` is the fleet's first ISOLATING translating
board.** M6.3 is CLOSED, its last step R6, and the aspace-seam VERDICT IS TAKEN, and it is NOT an
empty diff: one member added, `arch_aspace_frame_at`, thirty five records identical. The verdict
was the one piece of M6 evidence that could not be gathered early, F8's empty-diff claim being
about a FAMILY, and what settled it is the property R2 was picked for. The verdict was 35 baseline
records against 36 candidate, the one addition being `FUNC arch_aspace_frame_at`. The seam differ
that took it is gone from the tree, so the verdict stands as a recorded result and is not re-takeable.

**THIS BRANCH IS M6.5, FRAME-LEVEL CAPABILITIES, AND ITS THREE STEPS ARE LANDED.** C0 froze the
capability ABI before the first object kind, C1 added a frame RUN and an ADDRESS SPACE, C2 made map
and unmap capability operations, C3 shared one run into two spaces at two addresses. The milestone's
claim is a NEGATIVE result like M6.3's and M6.4's, and it was measured at 24 capability-seam
records, the only two members that ever entered being plain enumerators (60 and 61). An
address never became a FIELD, only ever an argument, which is what let C1's claim survive C2.

**THE FLEET WITNESS FOR M6.5 IS COMPLETE AND BOTH HALVES STAMP ITS MERGE ON MASTER.**
Host: 56 presets, 2191 host tests, 56 pass, 0 reused, 0 fail, with `trap_redzone` in all 56 logs,
which is the half that matters for two new syscalls, no emulator board registering one. Image: 56
presets, 473 gates, 0 failed, 0 skipped, 37 declared with no image gate. **The image half took four
attempts and only the last is evidence**: two were killed because the tree changed under them, and
the third REFUSED itself for want of `SWEEP_EXPECT_EMPTY=37`. A run of that tool without the
declared figures is not a witness and says so, its `DONE` sentinel being written only when every
clause holds. `qemu-x86_64` ran 10 image gates for the first time, the `emulator_for` row added at
C0 having been what filed them as needing silicon.

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

- **THREE ORDERINGS ON THE arm64 ENTRY AND TIMER PATHS REST ON THE ARCHITECTURE AND ON A
  DISASSEMBLY GATE, AND NO MACHINE ON THIS BENCH CAN WITNESS ANY OF THEM.** `msr SPSel, #1` ahead of
  the first write to SP, and an ISB after each `CNTP_CTL_EL0` disable ahead of the Device write it
  protects, in `arch_timer_disarm` and in `kickos_armv8a_percore_init`. Every emulator here enters at
  reset with `PSTATE.SP` already 1, so the SP_EL0-versus-SP_ELx confusion is reachable only from a
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
  `tests/static/service_lists.txt` is compiled by neither. **And the TREE STAMP each tool prints is a check
  on none of this**: it is taken from `$ROOT` by `git -C` and agrees with itself whatever sources
  cmake actually read, which is how a sweep invoked from another checkout once reported 59 of 60
  presets against a tree it had never compiled.
- **THE SLEEP PATH WAS MISSED ON THE FIRST PASS, AND THE ENUMERATION IS WHY.** The death point's
  class is every site that writes `ThreadState::BLOCKED`, and there are exactly three:
  `wq_block`, `park_queueless` and `ktime_sleep_until`. Enumerating from the two park funnels
  being refactored finds the first two and misses the third, which reaches neither. **A sleep
  self-wakes on its deadline, so the ordinary miss is a latency bug**, a cancelled thread sleeping
  out its remaining delay before dying; `ktime_sleep_ns` saturating to `UINT64_MAX` on overflow is
  what makes the unbounded case real, and that one is the defect item 1 exists to close. The check
  belongs in the prologue for a reason sharper than the empty unwind: **a check at the park would
  exit with the thread already on the sleep queue and the one-shot armed for it, and
  `sched::exit_current` does not sweep the sleep queue**, so the list would keep a pointer into a
  slot the pool re-hands. Witnessed at the unit layer, deterministically. **The on-target
  suite cannot witness it at all: no arm anywhere cancels a SLEEPING thread.** Every
  cancel-facing worker in the selftest is written to park on a semaphore nothing posts, so 64
  sleep call sites and every kill, slay and group-kill site between them never produce the
  interleaving. Recorded rather than closed with a probabilistic arm. **`park_death_point` is
  now a closed class enforced by a gate**, which counts the `ThreadState::BLOCKED` writes
  against a declared set: it cannot check that each park ASKS, the ask being in the caller for
  two of the three sites, but it can refuse a park nobody declared, which is the failure that
  actually happened.
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
  class above one of those. This bullet used to call that the kernel-core count, and the wrong
  word is what made two readers reason wrongly about which preset could ever close the arm: an
  AMP image was expected to, because it sets one kernel core while driving four -- but it drives
  four, so the gate skips there too. What closes it is a GICv3 board that drives ONE core, which
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
  `KICKOS_ASSERT` added on the console route put `kpanic` on the fault-exit descent
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
  for part of M6.4, and two Reference records went on saying so
  afterwards** -- `docs/reference/boards.md` and `docs/design-m6-mmu.md`, corrected 2026-08-29.
  That is worth the line because it is this milestone's own class: a record that was TRUE when it
  was written and stopped being true when the defect it described was fixed, with nothing tying
  the sentence to the fix. When a door closes, the file that named it open is part of the fix. Its declaration is
  `tests/static/console_reach_roots.txt` and it states in its own header what it would fail to
  catch; do not widen that file to quiet it.
- **`bluepill-c8-st` AND `f302nucleo-st` LINK AGAIN, AND WHAT FIXED THEM WAS A FLEET-WIDE COST
  RATHER THAN THOSE TWO IMAGES.** They contributed no trap-stack figure from the commit that
  broke their link until this fix, `trap_redzone` dying at the same link inside its own scratch tree: a build error
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
- **THE ABI-FREEZE MILESTONE IS DELIBERATELY UNNUMBERED, and eight sites had invented a number.**
  It fires when the ABI is ready, which is a state and not a position, so a number would assert a
  readiness nobody has. `roadmap.md` states that and owns it. What had drifted: four design
  documents and `TODO.md` said "the ABI-freeze milestone (M8, the last one)" -- wrong twice over,
  M8 being IPC/IRQ optimisation and the list running to M10 -- and `docs/README.md` listed the
  freeze as M8's second half while omitting M10 entirely. The number is stripped everywhere. Do
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

## Machine-local traps live in CONTEXT.local.md, not here

Two that were in this file and belong there instead, and are now recorded there:
`/var/tmp/kickos-imagesweep` still holds a stale run whose summary reads as current (check its
`finished` timestamp), and `genconfig.py` warns "set more than once" whenever a `-D` knob is
re-passed unchanged, which is noise in a channel documented to mean a declaration is wrong.

## A commit hash is never a record

**RULED 2026-09-01: NO HASH GOES IN THIS FILE.** The workflow is fleet-and-measure, then squash,
then merge, so a hash written down during a milestone names a commit the squash is going to destroy.
Every hash-as-record here was therefore guaranteed to rot, and the file's old design made that worse
by claiming local backup branches kept them alive: it carried a table of eleven hashes said to
"survive on this box only", with a warning that one `git branch -D` would destroy an M4.6.1
prerequisite. Clearing the branch inventory took three of them out, and neither origin, the reflog,
nor 232 dangling objects held them. **Branches are not an archive, and a ref nobody pushes keeps
nothing.**

**SO A CITATION MUST CARRY ITS OWN CONTENT.** A document citing another reproduces what it needs. A
capture that matters states its own numbers. A witness names the MILESTONE it measured and what it
measured, never the commit it sat on -- the tree is what a witness is valid for, and after a squash
the tree survives while the hash does not.

**What was lost with those three, for the record and not for recovery:** the RX72M peripheral-IRQ
demux spike document, whose taxonomy and findings `docs/design-m4.6-irq-driver.md` carries forward
in full, and two M4.9.1 silicon banner strings. Do not create a branch to restore any of it.

## M7.11 SITS ON M7.10, WHICH SITS ON M7.9, AND THIS FILE RECORDS ONLY THE LAST OF THE THREE

Read that first, because nothing below says it and the branch no longer means what its own
history suggests. M7.11 was a SIBLING off the M7.8 merge and was rebased onto M7.10 on
2026-09-04, so it now carries thread placement (M7.9) and the LX6 shared kernel (M7.10) beneath
it. **Neither of those milestones wrote a section here**, on this branch or on either of theirs:
their records are on `master`, in the entry covering the three unmerged branches, and that entry
does not reach this branch until the merge. So the two sections below are M7.11's alone and are
NOT a summary of what this tree contains. Do not read their silence about placement or the LX6
as those milestones having nothing to say.

**AND THE CONTRACT RENUMBERED UNDER THE REBASE.** M7.10 inserted `docs/design-multicore.md`
section 8, thread placement, so "Deliberately NOT frozen" is section 9 now. Every cross-reference
written before that date says 8 and means 9; the ones in N6g were re-pointed by hand, because git
merged them cleanly and wrongly.

**AND THE SAME CLASS BIT A SECOND TIME, IN `ci.yml`, ON THE 2026-09-04 REBASE ONTO M7.10's LATER
TIP.** M7.10 added `fetch-depth: 0` to every checkout THEN EXISTING and a header saying every
checkout carries it; M7.11 had added the `qemu-arm64-amp` job further down. The two deltas never
touched the same lines, so git produced a lossless union in which the new job keeps the bare
default. `witness_reconcile` is registered under a plain `if(KICKOS_BUILD_TESTS)` and labelled
`host`, so it runs on every preset and refuses a shallow checkout BY NAME: a red CI job that no
local run reproduces, because a local tree is never shallow. Re-audited by counting rather than by
reading the diff, which is the only thing that finds it -- eleven checkouts against eleven hits of
the string, one of them the header prose, so ten real blocks and one job uncovered.
**The lesson for the next rebase of this branch: a diff of diffs proves no line was LOST and says
nothing about an invariant that is stated over a whole file.** Check the invariant, per file, after
every rebase.

## M7.11: the partition's port capabilities, what the run does NOT say

The port capabilities landed. `docs/design-multicore.md` **N6g** is the contract and carries
every ruling; this section is only what a green run and that contract do not say.

**THE POSITIONAL DERIVATION IS DEFENDED BY A BOOT PANIC AND NOT BY AN ARM, and no arm can carry
it.** A dynamic install into root ahead of the partition's shifts every constant by one, and the
seating's own handle check refuses to boot past it. That was executed: with a `cap_install` into
root added ahead of `amp_ports_seat`, the image panics `kmain: a partition port landed off its
derived capability slot` before any arm runs. So the arm `amp_port_seating` witnesses the ROLE
half and the panic witnesses the POSITION half, and reading the arm as covering both is wrong.

**AND THE AMBIENT-HANDOUT MUTATION IS REFUSED BY THE CAPABILITY TABLE'S OWN INVARIANT, so that
column of `amp_probe_root_only` has no reachable red.** Seating the partition's first port into
every spawned child, which is the defect shape the column names, trips `cap_install_at`'s
already-live assert against the grant list's own placement and kills the run. What separates that
arm instead is the forge's caller gate. The column stands as a positive statement about what a
non-root task sees and not as a mutated claim.

**THE ARMS WERE COUPLED BY `KICKOS_CAP_REPLY_MAX` AND THAT IS AN ARM DEFECT, NOT A MECHANISM
ONE.** `TAP_CHECK` returns on failure, so an arm that asserts before spending its reply capability
abandons a live one; at a reply ceiling of 1 the NEXT arm's far caller is then refused one and
reddens with it. Two arms redden as one until the reply is sent ahead of every check. Any future
arm holding a reply capability owes the same order.

**WHAT THE SHARED IMAGE AND THE OWN-IMAGE PAIR EACH REFUSED TO SEE, measured rather than argued.**
A role keyed on node ZERO instead of on this node leaves `qemu-arm64-amp` and `qemu-arm64-amp2-n0`
entirely green, all eleven and all nine arms, and kills `qemu-arm64-amp2-n1` at boot. That is
N6c's class exactly, and it is the concrete demonstration that the node-1 build is not a duplicate
of the node-0 build.

**THE SHARED IMAGE'S PEERS CONSTRAIN WHAT THE PARTITION MAY NAME THERE, which reads as an odd
list until the reason is stated.** Its peers run a service body and no kernel, so they bind
nothing: a call to a partition port at one is dropped and only port 0, the window layer's echo,
comes back. That is why that board's list names `1:0` beside `1:3` and why the two behave
differently. On the own-image pair both nodes run kernels and both entries reach threads.

**THE ENDPOINT POOL IS A PARTITION COST NOW AND THE DEFAULT DOES NOT COVER IT.** Every listed
crossing claims an endpoint slot for the life of the image. The fleet default of 4 left the AMP
boards' apps with one, which presented as `kos_endpoint_create` failing in twenty unrelated arms
rather than as anything about AMP. Both AMP defconfigs state 8, and a partition the pool cannot
seat is refused at configure.

**WHAT NO RUN HERE SAYS.** Two kernels never ran at once: every inbound call in these arms is a
forged publication standing in for a peer, and the own-image pair is still two images run one at a
time. The merged single artefact and the ping-pong across two roots are still owed, and the arms
held under the own-image posture are still held.

## M7.11: the vehicle, the ping-pong, and the defect only it could reach

**THE DEFECT IS THE HEADLINE, because it says what the vehicle is FOR.** The GICv3 SGI raise
bounded its target sweep on `KICKOS_NUM_CORES`, which is how many cores THIS IMAGE drives and is
1 under the own-image posture, while the mask it sweeps names the PARTITION's cores. Node 0
raising at node 1 set the lead bit at index 1, the sweep `for (index = 1; index < 1; ...)` never
ran, nothing cleared the bit, and the enclosing `while (pending != 0)` spun that core forever
with its interrupts masked. Not a dropped raise: a hang.

**AND EVERY PLACE IT COULD HAVE BEEN CAUGHT EARLIER IS BLIND TO IT.** Under the shared image
`KICKOS_NUM_CORES` and `KICKOS_DOORBELL_CORES` are both the core count, so the bound is right. An
own-image node running ALONE skips the raise before that loop, its peer never being seated. It
needed a seated peer in another image, which needed the merged artefact. Measured, not argued:
with the bound put back, `qemu-arm64-amp` passes 55 of 55 and `qemu-arm64-amp2-n0` fails exactly
one, the partition gate.

**WHAT THE FIX ADDED BESIDES THE BOUND.** The lead bit now leaves `pending` before the sweep
runs, so the loop's progress is structural: a future bound that stops agreeing with the mask
drops a raise, which is visible, instead of hanging a core, which is not.

**THE ARTEFACT IS AN ELF AND THAT RETIRED A WORRY RATHER THAN ACCEPTING IT.** A flat span across
a 64 MiB stride is 130 MiB of mostly padding; one `PT_LOAD` per node is about 6. Nothing in the
merge restates an address: each node's load address is read from its own ELF and the entry from
node 0's.

**WHAT THE PING-PONG IS AND IS NOT.** It is two roots, two images, two init providers, two apps,
one artefact, and an ordinary `kos_call_timed` answered by a thread parked in the other kernel's
`kos_recv_timed` and replying through `kos_reply`. Neither app names a node identity, a ring, a
window or a doorbell. What it is NOT is a witness that the two kernels are ISOLATED: they share
one machine and one console, and the console interleaves at byte granularity by ruling, which is
visible in the run's own output.

**THE DEFERRED-DELIVERY CLAUSE IS WITNESSED AND ITS TIMING IS NOT.** The peer's own take counter
standing at two is the witness. The real bring-up window, between node 0 releasing its peer and
that peer seating, is still a race node 0's userspace cannot reliably enter; that half is
recorded as a non-witness in the contract's section 7 rather than left to lapse a third time.

**TWO GATE CLAUSES HAVE NO REACHABLE RED, and saying so is worth more than claiming them.** The
window's NOBITS clause is defended by `(NOLOAD)` in the link script, so a C initialiser cannot
reach it and dropping the `(NOLOAD)` fails the link; the clause guards a future link script. And
a peer built as a second node ZERO reddens the partition gate AND the two-ELF gate together,
because it genuinely breaks both claims rather than because the two arms are one.

## M7.11: what is still owed, and the two merge conditions

The two-image AMP vehicle boots and the partition's port capabilities land on it.
`docs/design-multicore.md` N6b through N6g is the contract.

**STILL OWED.** Nothing of the list this section used to carry: the deferred-delivery arm, the
ping-pong across two roots, the merged artefact and both gates all landed. What remains is the
two merge conditions below, and the bring-up window's own timing, which the contract records as
a non-witness rather than as work.

**BOTH MERGE CONDITIONS ARE MET.** They are recorded here with what they turned out to be, since
both had been carried as labels rather than as findings.

**THE FIRST WAS A BINDING JOB AND NOT A DEPTH ONE.** `pizero2350-amp`'s `trap_redzone` was eleven
unbound indirect sites: nine in `console_tx.cc` and the two `chip_rp2350.cc` bootrom pointers.
Every enforced depth class was inside budget the whole time. The nine are the same call sites its
sibling presets bind, and the AMP graph reaches nine of their twelve, so the set was taken from
what the gate reported rather than copied from a sibling block: this gate hard-fails an
over-declaration as loudly as an under-declaration, which is what confirms the set is exact. The
two bootrom sites take NONE for the reason already in that file. The preset's own comment said
those sites exist in the `-st` graph alone, which stopped being true when the AMP defconfig turned
the selftest on.

**THE SECOND COST A PEER, A WIDER PARTITION AND A GATE, and removing the `#if` was the smallest
part of it.** The three arms were held by the posture; they decide at RUNTIME now, off the peer's
own serviced count, because the same image runs both standalone and inside a merged partition and
only the second has a peer. Three things had to exist first. A peer the selftest can talk to,
which echoes the caller's bytes verbatim, since a far call is witnessed by its payload coming
back. A THIRD crossing in the partition: `t_amp_far_reply_guard` needs a call that stays
unanswered so its caller parks, and at a peer running a kernel every listed port is BOUND at
init, so the unanswered one has to be a second port whose receive nobody holds. And a gate that
asserts those arms are `ok` and NOT skipped, because the standalone run is permitted to skip one
of them and a permission with nothing to bound it is how an arm lapses.

**AND THE VEHICLE'S GATES WERE FLAKY UNTIL BOTH CAUSES WERE REMOVED. Both are now RULES in the
contract rather than fixes in a commit** -- the console one in N6h, the counting one beside
N6f's receiving-side rules it follows from -- because the next person to write a gate against
this vehicle meets both on a first attempt. Two failures in ten runs, two different ones, and
neither reachable by running the gate once.

**A GATE MAY NOT READ THE QUIETER NODE'S CONSOLE LINE.** Both kernels write one console with no
lock between them, interleaved at byte granularity by ruling, so the peer's single banner is
regularly cut in half by node 0's TAP traffic. A gate that greps for it is measuring the console.
The peer is witnessed through NODE 0's own reading of the peer's counters instead.

**TEN RUNS IS THIS VEHICLE'S STANDARD AND IT IS WRITTEN WHERE THE GATES LIVE.** A green run of a
two-kernel gate witnesses less than a green run of a one-kernel one, the interleaving being a
fresh draw each time. CI runs the gates ONCE, in the `qemu-arm64-amp` job, and that job's own
comment carries why one run is honest HERE and would not be for a gate that still read the
console: both causes were removed rather than retried. **The job may not join the
`--repeat until-pass` set**, which rides the polled gates in the same file: that flag exists for
host-scheduling artefacts of polling, and retrying an interleaving-sensitive gate would mask the
exact class this vehicle exists to find.

**AND THE JOB'S POSTURE PIN IS WHAT KEEPS IT FROM PASSING VACUOUSLY.** Every AMP gate is
registered by a CMake clause keyed on the posture, so a preset that lost it does not FAIL those
gates, it stops registering them, and ctest then passes on a run that covered plain arm64 under
another name. That is the shape M7.9's smpiso job was caught with. `.github/scripts/amp-pin.sh`
reads the partition's own generated description and compares it against LITERALS the job states,
so nothing in the expectation derives with the knob being checked. Exercised in both directions
rather than assumed: a plain arm64 build is refused for stating no crossing, a node 0 build used
where node 1 is needed is refused by name as N6c's collapse, an own-image build pinned as shared
is refused, and the two correct pairings pass.

**AND AN ARM THAT COUNTS DROPPED REPLIES MUST HAVE NOTHING OF ITS OWN IN FLIGHT.** A far SEND to
a port a peer THREAD serves is answered by that thread, and that answer names no caller, so it
lands as a dropped reply at a moment the sender does not control. `amp_far_reply_guard` counts
dropped replies over a window, and a stray one arriving inside it made the count wrong. The send
now goes to the crossing nothing answers, so nothing trails it. A peer that is a WINDOW LAYER
never produced that stray, which is why the arm was sound for as long as it was held.

**AND TURNING THEM ON FOUND TWO THINGS A HELD ARM COULD NOT.** `t_amp_far_reply_guard` expected
three dropped replies, which needs a THIRD RING for the wrong-ring forge; a partition of two has
none, so the expected count is the partition's width and not a constant. And `t_amp_far_call` sent
before it called, which is harmless against a window layer and wrong against a THREAD: a far call
finding nothing parked is refused on the spot (N6f), so the arm was calling the peer while it was
still replying to the send. The order is load-bearing now and says so.

**BOTH WERE MEASURED BOTH WAYS.** Dropping one of the eleven bindings reddens `trap_redzone` and
restoring it greens it. Making the peer answer with bytes that are not the caller's reddens the
held-arms gate alone, one of fifty-four, and leaves the standalone run of the same image green.

**AND THE FIGURES BEHIND THE FIRST ONE, since they are what turned a label into a job.** The
eleven were `chip_rp2350.cc:577:15` and `:587:15` plus nine in `console_tx.cc` (69:37, 91:28,
141:30, 155:63, 157:28, 259:35, 267:32, 310:27, 356:27). Every ENFORCED depth class was inside
budget throughout: PENDSV 0 of 0, SVCK 744 of 768, EXITK 576 of 584, RET 296 of 312. The two that
read over, SVC 640 of 448 and EXIT 576 of 576, are the classes this image does not enforce, the
entry design they describe not being compiled here. Reading that red as "the trap path got too
deep" sends the next session at the wrong thing entirely.

**ONE MEASUREMENT TRAP AROUND IT, because it is how that red was confirmed to be the same red
across the rebase and not a new one wearing its name.** `/var/tmp/kickos-trap-redzone-<preset>`
is kept per preset and reused, and it does not re-derive Kconfig: without removing it between two
readings the second one IS the first one. With it removed, taking M7.9 and M7.10 underneath moved
the reachable node count from 288 to 292 and the unenforced SVC reading from 632 to 640, and the
eleven sites and every enforced class were byte-identical.

**ONE THING THE CONTRACT RECORDS THAT IS EASY TO READ PAST.** `qemu-arm64-amp` is not a legacy
posture. It and the own-image pair are one instrument: the shared image exercises a keying at
every index, an own-image node at exactly one, and they fail on opposite halves. A node-1 build is
not a duplicate of a node-0 build. N6c carries both halves, and the section above carries the run
that demonstrates it.

## M7.11: the partition-versus-image sweep, and which half of the instrument each defect hid from

The seven items of this pass landed. What follows is only what a green run and the contract do
not say.

**THE TWO HALVES OF THE INSTRUMENT WERE MEASURED AGAINST EACH OTHER, and that is the finding
rather than any of the fixes.** Three mutations of ONE keying redden on opposite sides, executed
rather than argued. A doorbell sweep bounded by the cores THIS IMAGE drives, and a matrix row
taken as zero instead of as this node's, each redden the node 1 build ALONE: node 0 passes both
because its bound and the matrix's agree and because row zero genuinely is its row, and the
shared image passes both for the same two reasons. A total accessor that answers an out-of-range
row with node 0's row reddens node 0 AND the shared image alone, and the node 1 build passes it,
that row being a peer's and quiescent in a node booted alone. N6c states this shape; this is the
run that shows it on three defects at once, and it is why the node-1 build is not a duplicate.

**A CLAIM WITH NO WRITER IS THIS MILESTONE'S RECURRING SHAPE AND IT WAS FOUND TWICE MORE.** The
region every node writes was stated as cleared by the partition primary in two documents and was
cleared by nothing: no C, no assembly and no tool referenced its bounds, and the section is
NOBITS so nothing loads it either. A bench whose RAM starts zeroed cannot see that, which is the
whole reason it survived. The doorbell probe's ABI carried the same shape one layer in: a
documented bit 0 meaning the peer took the message with no raise, never written, and not
implementable synchronously at all, since the probe cannot observe a peer draining at its own
pace. Both consumers read past it. **So the reading rule earned here is that a field's
DOCUMENTATION is not evidence of a writer, and the grep for its writer is one command.**

**WHAT STANDS BEHIND THE REGION'S CLEAR IS TWO MUTATIONS AND NO ARM, and no arm is possible on
this bench.** The primary writing ones rather than zeros reddens four arms and the peer-arms
gate, which is what proves both that the region's initial content is load-bearing and that the
call site is reached at all; moving the clear after arch_init kills the run outright, which is
what proves the PLACEMENT is load-bearing rather than decorative. **Neither is a witness that
the clear is needed**: that claim rests on a part whose RAM does not start zeroed, and this bench
has none. The same is true of a warm start on any board here.

**AND TWO OF THE PASS'S FIXES HAVE NO REACHABLE RED, which is worth more than claiming them.**
The send-side tail forge restores the indices it pushes now, and the residual is a LOST UPDATE
rather than nothing: the tail belongs to the consuming node, the forge deliberately runs outside
any lock the real path holds, and no lock spans two kernels, so a live consumer's move inside
that window is lost and recovered only by the depth resync that bounds it. No arm reddens the
restore, the arm driving it running ahead of any real traffic on that ring. And the refusal for a
map that does not put node 0 on the core the machine resets into is defended by a CONFIGURE
refusal and not by an arm, the map it refuses having killed the boot in the launch handshake
before the refusal existed.

**THE NODE-1 DEFECTS NEEDED AN ARTEFACT THIS TREE DOES NOT BUILD, and that is a standing gap
rather than a step that was skipped.** A gating or keying defect on any node but the first is
invisible to every registered target: a node booted alone has no live peer, and the merged
artefact this tree assembles puts the selftest at node 0, where a sweep from index 1 never meets
itself. What reached them was a partition merged the other way round, the demo caller at node 0
and the selftest at node 1, assembled by hand from the two builds. It is not a target, nothing in
CI runs it, and its own far-call arm is red there because that pairing puts a caller opposite a
caller. It is an instrument for two claims and not a board.

**AND THAT ARTEFACT IS WHY THE DOORBELL ARM CARRIES NO POSITIVE SERVICE CLAIM UNDER ONE IMAGE PER
NODE.** Its rows read zero on either node booted alone and inside the artefact this tree builds,
where the peer only ever answers; on the partition merged the other way round this node's own row
carries services before the arm runs. A count asserted there would be asserting the DEPLOYMENT.
Before this pass that arm asserted the FOLD instead, which is the claim that nothing has ever
answered a doorbell, on the one posture built to be rung: it passed because it read a peer's
quiescent row and because it runs ahead of every arm that rings anything. **Green on arm ordering
rather than on construction is the definition of passing on a draw**, and the cure was to key the
arm on the matrix's own width and its own row, both asked of the kernel, because under the shared
image the row is a core REGISTER and no build constant can answer it.

**A SHAPE DIFFERENCE BETWEEN THE TWO DOORBELL BACKENDS IS A PORT THAT IS OWED, not a line nobody
wrote.** The armv8a backend builds its doorbell half whenever something rings it, which above one
core is a shared kernel's peers and at one core is an AMP node's peer nodes; the rv64imac twin is
guarded on the core count alone, so an own-image RV64 node gets no doorbell backend and fails to
LINK, loudly, which is the shape the seam intends. An out-of-bounds read that looks like a defect
there is unreachable BY THAT GUARD rather than by luck, and an inner guard added to bound it can
never be false. Matching the shape is a port to a backend no board selects, and it is recorded
here as owed rather than half-done.

**THE FORGES REACH ONE SENDER ROW OF THE WINDOW'S TABLE AND THE TABLE IS NOT WHAT IS NARROW.**
The peer every forge names is the lowest node that is not this one, which under the shared image
is a constant, so every forge arm exercises the inbox, the inbound records and the strike count
at ONE sender index and at no other. Those records are keyed per ordered pair and are correct;
it is the arms' reach into them that stops at one row. The ring half of the window arm does sweep
every peer, through a probe that takes an explicit node. Widening the forges is a milestone of
its own and was deliberately not attempted.

**TWO READING RULES THIS PASS PAID FOR, both cheap to lose and expensive to rediscover.** A
non-vacuity guard resting on a counter the path under test does not increment proves NOTHING: the
window's forges write their slots directly rather than through the send path, so the publication
counters do not move across them and a guard built on one reddens while saying nothing about the
claim beside it. Read WHICH LINE failed, never only which arm. And a refusal you have never seen
PRINT is not a refusal you have witnessed, which is a live hazard wherever a console is reclaimed
before a fatal path speaks; the launch-handshake refusal above was seen on the wire on this
board, which is why the configure refusal quotes it verbatim.

**AN INTERLEAVING-SENSITIVE GATE OWES TEN RUNS BECAUSE ITS DRAW IS FRESH EACH TIME, AND ONE CI
RUN IS A REGRESSION PIN RATHER THAN A STUDY.** Those are two different questions and the same
green answers only the first. The count from any particular study is re-derivable by re-running
and is deliberately not written here; the rule is what this file carries.

**AND A GATE THAT SHELLS OUT TO THE BUILD SYSTEM MAY NOT RUN CONCURRENTLY WITH ANOTHER THAT
DOES.** Two ninja invocations on one build directory race on the intermediates they share, which
was proven directly by starting two of these gates' builds together and finding a kernel archive
truncated mid-file with one process dead in ranlib. **The dangerous half is that the same race
can leave a WRONG archive rather than an error.** Under a parallel test runner it is a DRAW, not
a certainty, which is why it resists re-provoking; the tell is a gate failing far too fast to
have built or booted anything, so read the DURATION and not just the name. It is fixed in-tree by
a serial property on the three gates that build artefacts, and no test preset requests
parallelism, so CI was never exposed. The rule outlives the fix for anything copied from them.

## M7.11: the audited findings, and the two audit claims that turned out misattributed

An external audit blocked this branch on three HIGH findings in the AMP window and two MAJOR
ones in the partition tooling. All five are fixed and each has a mutation of its own. What
follows is only what the fixes and a green run do NOT say.

**THE RECORD IS THE RING SLOT, SO THE RESYNCHRONISATION OWNS ITS RECORDS' DEATH.** N6f rules
that a call ring slot is reclaimed when the reply is sent, and the record IS that slot, so
whatever destroys the slot destroys the record. The reply path owns that death on the ordinary
path; the depth reset is the one path that destroys a slot with NO reply to spend it, and
unlike the reply path it frees a record whose CAPABILITY IS STILL LIVE. That is the whole
reason the record carries a generation: it is the reclamation rule followed through, not
caution about a stale index. Left standing, such a record refused a seat to every later call
landing on its masked slot, so that far caller reached a service with no reply capability and
waited out its own deadline; and its holder's release landed on the slot at the same masked
index ONE WRAP LATER, a different call whose reply was still owed.

**AND THE ARM REPRODUCES THAT ALIASING BY CONSTRUCTION AND NOT BY CHOSEN NUMBERS.** The forge
jumps the far head by 2 * RING_SLOTS, and RING_SLOTS is a power of two, so the tail the reset
adopts masks back onto the abandoned record's slot at ANY ring width. An arm that hits a defect
because its constants happen to collide becomes an arm that passes while testing nothing the
first time someone resizes the ring; this one cannot. Anything but a whole multiple of
RING_SLOTS there would be such an arm.

**THE PORT TABLE'S SENTINEL IS THE CLEARED STATE NOW, AND THAT IS WHAT REMOVED A CROSS-NODE
WRITE RATHER THAN ORDERING IT.** window_init used to seat every node's rows. Under the shared
image that is one node writing rows a peer is reading: peers are released by
release_secondaries inside arch_init, which kmain calls BEFORE window_init, and each parks in
the backend's doorbell body, which services the rings. Of the three per-node tables only the
port bindings were not already at their identity at zero, unbound being 0xFFFF so that a row
nobody wrote read as endpoint 0, a real endpoint. Biased by one, the cleared state IS the
unbound state, nothing needs seating on any node or any path, and the shared-image peer that
never runs window_init needs nothing done for it. The mint stays table-wide, port_minted being
asked about peers, but each row is stored WHOLE: a bit-at-a-time or is a read-modify-write on a
word another node is writing, and two nodes interleaving those loops lose a bit for the life of
the image.

**THE RACE ITSELF HAS NO REACHABLE RED AND CANNOT HAVE ONE HERE.** No deterministic run enters
the window between the mint loop and the row loop. What IS measured is that the peer's row
genuinely needs to read unbound and that nothing else seats it: reverting the bias reddens
`amp_window` and `amp_far_call` on `qemu-arm64-amp` and leaves `qemu-arm64-amp2-n0` ENTIRELY
GREEN. That is N6c's instrument split again, and it is the opposite way round from the record
defect above, which reddens at the same line on node 0, node 1 and the shared image, being
keyed per ordered pair.

**TWO OF THE AUDIT'S OWN CLAIMS ARE MISATTRIBUTED, AND BOTH ARE RECORDED BECAUSE A SEVERITY
THAT STANDS ON A WRONG MECHANISM GETS RE-DIAGNOSED BY THE NEXT READER.**

First, the gawk-only conversion in the partition merge was reported as failing SILENTLY, a
wrong address still merging. It does not. Measured end to end on the real two-ELF partition:
mawk exits 2 and busybox awk exits 1, both before a single record, so the script produces
nothing and its existing emptiness check refuses. The severity stands, but the failure is a
REFUSAL NAMING THE WRONG THING, `has no loadable segment`, which accuses the ELF instead of the
awk. Under gawk the same code merges correctly, which is why nothing in the tree could see it:
`awk` on a Debian developer box IS gawk. `tests/static/check_awk_portable.sh` is what makes that
non-silent from now on, and it found a SECOND instance nobody named, in
`tests/static/check_amp_two_elf.sh`, where the same call sat inside a GATE's span arithmetic
and would have had that gate report its own clause instead of the awk.

Second, this tree's 2^53 awk-precision warning DOES NOT APPLY to a PT_LOAD address, and
inheriting it here would have bought a fix for a defect that is not present. Checked rather
than assumed, against real kernel-half addresses: `0xffffff8040000000` and `0xffffff8044000000`
both round-trip through a double EXACTLY, because a p_paddr is at least page-aligned (12 low
zero bits) and the spacing of doubles at 1.8e19 is 2 KiB. The warning is about a SUBTRACTION
that materialises a small delta out of two large addresses, which is a different arithmetic.
The merge reads addresses as hex STRINGS anyway, so it never converts one; the span arithmetic
that genuinely adds is the shell's, which is signed 64-bit and refuses a p_paddr at or above
2^63 by name rather than wrapping.

**AND THE GEOMETRY REFUSALS BELONG TO THE TABLE, NOT TO THE PARTITION.** The three geometries
that used to link cleanly and fault at the first touch of unmapped RAM are refused in
`arch/arm64/chip/virt_arm64/startup.S`, each clause naming its own field, because every
derivation they guard is a truncating division or an index fixed by which level-1 entry that
table hangs off. They are NOT in CMakeLists.txt beside the geometry: that site is arch-agnostic
and also serves a part whose whole share is smaller than one 2 MiB block of this one. Written
against the three raw parameters, so M7.12's per-aperture parameterisation of the same header
carries them unchanged.

## Where to go next

- `docs/README.md` -- the docs map (Book vs Reference, conventions).
- `TODO.md` -- the granular, actionable items.
- `roadmap.md` -- the milestone plan, and the sub-milestone ledger: the only place a number is
  ASSIGNED. This file carries the locked ORDER and cites those numbers.
- `docs/reference/` -- the exact contract; the code wins, drift is a bug.
- `docs/design-m6-mmu.md` -- the M6 contract; section 5 is the step plan M6.3 continues.
- `CONTEXT.local.md` -- local rig ops. Gitignored: it exists only in the main checkout.
