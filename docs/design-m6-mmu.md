<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M6 -- the MMU: a unicore A53 on QEMU `virt`

> **Status: LANDED.** Sections 1 to 3 state the M6 address-space design. Section 5 summarises
> the completed stages; their detailed step record is in
> archived `M6_implementation_record.md`.

This is the DESIGN CONTRACT for M6, not an exploration. `docs/design-mmu-era-exploration.md`
is the exploration and stays one: it enumerates the five places the single-physical-address-space
assumption is baked in, and that enumeration is still the input. What it does NOT decide is the
target. Its section 3 argues x86_64 as the first MMU port; `roadmap.md` aims M6 at QEMU `virt`
with a Cortex-A53 instead, because that is the machine multicore then runs on. So read the spike
for sections 1, 2, 5 and 6, and read section 3 as a captured platform target rather than a plan.

The spike is also numbered against the old order. It is headed `(M7)`; the 2026-08-21 resequencing
swapped the two, so page tables are M6 and multicore is M7. Every "M7" in that document that means
page tables means this milestone.

---

## 1. The freezes

`roadmap.md` requires two decisions be frozen here rather than deferred. Both are, plus three that
were implicit and are cheaper stated. A freeze is a decision the milestone may not reopen without
saying so; the deliberately open questions are section 7.

**A FREEZE MAY REST ONLY ON ANOTHER DECISION, NEVER ON AN OBSERVATION OF THE TREE.** T5c had to
delete a premise from F2 and a consumer from F10 that were neither of them ever decided: both traced
back to one sentence recording what `task_for` HAPPENED to do, written in the present tense, and
load-bearing in a second freeze one step later. Nothing argued for the behaviour, so there was
nothing to reverse -- but a reader meeting the change without this note will hunt for the reasoning
behind the old position and find none. The shortcut is cheap to repeat: when a freeze needs a fact
about the code, say whether that fact is a decision or a measurement, and cite the decision if there
is one.

### F1. The kernel lives at a fixed high range in every address space

A64 selects the translation table from the top bits of the virtual address: `TTBR1_EL1` translates
the top of the space, `TTBR0_EL1` the bottom, with the split set by `TCR_EL1`. So "high-half
kernel" is not a tradeoff on this ISA the way it is on one flat table -- it is the shape the
architecture ships. An address-space switch writes `TTBR0_EL1` and the kernel half is never
touched.

This is the SMP-correct half of the fork, which is the constraint that decided it, and the reason is
worth stating precisely because a loose version of it is wrong. A core taking a trap already has the
kernel's table active: `TTBR1_EL1` is untouched by an address-space switch, so kernel text, kernel
data and another thread's kernel stack are reachable with no address-space juggling at any point.
Separately, the kernel MARKS its own mappings global -- `nG` clear, so the entry applies to every
ASID (DDI 0487 M.b section D8.16.3.1) -- which is a software choice and not something the high half
confers: `TTBR1_EL1` carries an ASID of its own and its entries can be non-global, and the
architecture even documents a sequence for changing base and ASID together that depends on which it
is (Example D8-1).

**What "global" does NOT mean is shared between cores.** TLBs are per-PE, so at M7 a change to a
global kernel mapping needs BROADCAST maintenance and gets no ASID scoping to narrow it; what the
ASID buys is that switching from one process to another costs no maintenance at all. So the
shootdown traffic M7 inherits is user-unmapping traffic, and the kernel's own mappings are rare
rather than free. **Do not compress this to "maintenance concerns only the ASID-tagged low half"**:
that reads as the kernel's own mappings being free, which the architecture does not say. The alternative -- fully separate spaces, with no kernel
mapping in a user space -- buys isolation this milestone cannot spend: it would put a translate
step on the entry path of every syscall, on a unicore emulator, under `fault == kill`.

Meltdown-class exposure is the price and it is named rather than priced. Nothing in M6 depends on
the kernel half being unreadable from EL0 by a side channel, and a split-table mitigation is a later
and separable choice.

**One thing this freeze must NOT be read as saying, and F8 is why.** "The kernel half is untouched by
an address-space switch" is a fact about A64's two-register split, not about high-half kernels. On the
x86_64 and on RV64 there is a SINGLE root register, so loading it replaces the kernel's mappings along
with the user's, and the kernel half has to be present in every address space's table and kept in
step. The freeze is "the kernel lives at a fixed high range in every address space"; whether the
hardware keeps it there for free is per-arch.

**The high half also carries a map of all physical RAM.** That is not decoration: it is what
answers section 3.3, and it is the reason F1 has to be decided before any access seam is written. It
is also cheap here. The physical address range is 40 bits on both ARM reference cores (F7 cites the
registers), though NOT on every target: Sv39 pairs a 39-bit virtual range with a 56-bit
physical one and Sv32 pairs 32 with 34 (RISC-V privileged spec), so on both the physical space is
WIDER than the virtual one, which is why section 3.3 reaches a frame through a seam call and not an
offset, so at 1 GiB block descriptors the whole map is at most 1024 level-one entries. At
8-byte descriptors that is 8 KiB, so TWO table pages and not one, plus whatever upper level the
configured virtual-address size requires. T3 reports the actual table-page budget from the layout it
picks rather than taking this paragraph's arithmetic on trust.

**T3 measured it, and BOTH terms of that estimate are absent from the layout it picked.** The map
costs no page of its own. At `T1SZ` 25 the level-1 table IS the root, so there is no upper level to
pay for; and the map is two of five hundred and twelve level-1 descriptors, one 1 GiB device block
and one table into the existing 2 MiB split over DRAM, so it fits the root page the kernel window
needed anyway. The whole image spends THREE table pages: a root per `TTBR`, and one level-2 table
that both roots reach through their entry 1. The estimate above assumed the full 40-bit physical
space mapped at 1 GiB, which is not what a 39-bit half can address.

**What replaces the arithmetic is a CEILING rather than a page count.** A 39-bit high half describes
at most 512 GiB of physical space, half of this core's 40-bit maximum, so a target whose RAM sits
above that boundary needs `T1SZ` 24 and gains a level-0 page. That is the figure a later backend has
to check, and it is a property of the chosen virtual-address size rather than of the map.

**Address-space identifiers do not appear above the seam at all**, and that is a freeze rather than an
omission. The kernel names spaces by handle; whether the hardware tags translations with an
identifier, how wide it is, and what must be invalidated before one is reused are entirely the
backend's business, discharged inside destroy and activate. An earlier draft reasoned from 16 bits
being available, which is not even universal within one architecture family: RISC-V permits the
identifier length to be hardwired to zero, in which case no identifier exists, and an 8-bit one needs
generation rollover. None of that may reach kernel code.

### F2. A domain becomes an address space, which is what makes a task a process

Terms first, because the target state of this milestone is a statement about them, and the tree
already carries two of the three.

A **task** is the set of threads that share one memory domain -- that is the definition in
archived `M4_task_layer_record.md`, whose own table assigns the task "the `Domain` (the shared region set,
later the address space)". A **process** is a task whose domain IS a full virtualized address
space: zero to the architectural limit of the low half, laid out freely, private. So every process
is a task, and a task is NOT necessarily a process. On an MPU board none can be, because its domain
is a window carved out of the one physical space rather than a space of its own.

**M6 is the milestone where a task on this CPU class becomes a process.** That is the target state
and it is worth naming as one, because it reads as a new object and is not: nothing above the seam
is renamed, no kernel type is added, `domain_ref` and `domain_release` and "threads sharing memory
share a domain" are untouched. `struct Domain` (`kernel/include/kickos/domain.h`) already means
"the memory a set of threads may touch, refcounted by the live tasks holding it, freed at the last
release". What changes is what a domain IS -- an opaque backend handle to a translation root
instead of an array of physical windows -- and a task inherits process-hood from it without
knowing.

F9 states what this does to the per-thread guarantees the MPU fleet provides, which is the part
that reaches the ABI. Two consequences follow immediately here and neither is optional; they are the
substance of section 3.1.

- **The identity map cannot express a process.** With virtual equal to physical, two tasks cannot
  both place their text at the same virtual address. So the stepping stone of M6.1 is by
  construction NOT the target state, and the first image that maps one process's text where another
  process's text also lives is the proof that the translation family is real.
- **The domain dedup has no place in the target state, and deleting it is NOT sufficient.**
  `domain_for` deliberately reuses one domain slot for two tasks granting the same block, and
  archived `M4_task_layer_record.md` records that two tasks landing on one domain stay two tasks. Under the
  process model that is precisely the thing to stop: it would put two kill groups inside one address
  space, and then "the set of threads sharing a domain" names something bigger than a task, which
  makes the definition above false. Two tasks granting the same memory become two processes mapping
  the same frames.
  **What "mapping the same frames" means before M6.5 is answered in F10, not deferred to it.** One
  set of frames really is mapped in both spaces, at the same address, by a kernel-mediated handoff at
  handover time, whether that is an explicit create or a grant-carrying spawn -- because the driver
  bring-up idiom requires exactly that and would otherwise break. **That idiom takes the EXPLICIT
  path**, and this document had it the other way round: `user/src/driver_service.cc` allocates the
  ring block, self-grants it, then hands it to `kos_task_create` and spawns every member INTO that
  task with no memory grant of its own. So the real consumer of the handoff is a task create, and the
  grant-carrying spawn is the secondary one.
  What M6.5 adds is a general way to express sharing; it does not introduce sharing.
  **The existing `domain_share` arm needs a decision, and calling it a sibling test would be wrong.**
  It is two spawns carrying the same reserved range, and a spawn that carries one still takes a task
  of its own (T5c), so those two are not siblings: the dedup is what made them share a domain. After
  F2 they are two tasks, two spaces and two handoffs of one range -- and the arm STILL PASSES,
  because F10 maps those frames at the same address in both. So it is KEPT and re-described as
  the two-handoff witness it becomes, which tests F10 better than it tested F2. A genuine
  sibling-sharing witness is a NEW arm -- one task create with two members, not a rewrite of this one
  -- and it belongs at T6, beside the process witness, since sibling visibility is only observable
  once two members have a mapped image to share.

  **A PREMISE IS DELETED HERE RATHER THAN CORRECTED, and the difference matters to a reader.** This
  paragraph used to say that resolving a spawn's task ALWAYS spends a fresh task slot, so an
  implicit spawn is a new task too. **Nobody decided that, and the ALWAYS was not even true of the
  tree it described**: `task_resolve` has always answered a spawn that NAMES a task, and that arm
  spends no slot. The decision that does license an implicit task for a spawn naming none exists and
  went uncited here -- archived `M4_task_layer_record.md` section 5.3, which rules that naming no task
  creates one holding exactly that thread, and which explicitly leaves the dedup a separable
  question. What the ALWAYS added on top of that decision was what `task_for` happened to do,
  observed once and written in the present tense, and by F10 it was carrying an argument. There is
  no reasoning behind it to look for and nothing here is being reversed: T5c states the intent the
  tree never had (a plain spawn is `pthread_create`), and the sentence simply stops describing the
  code. What survives of it is narrower and is stated above: a spawn that brings its OWN data grant
  still takes a task of its own, because there is no domain of the caller's for that grant to land
  in.
  **The harder half is the singleton BEFORE that dedup.** Every unprivileged task with no explicit
  grant is returned the one immortal default-user domain, which is a shared identity by construction
  and by comment. That is the COMMON case, not an edge: two ordinary tasks would still share one
  root after the dedup arm is gone, and F2 would still be false. On a translating build the
  default-user state becomes a TEMPLATE that each task instantiates, not an identity that each task
  joins. **T5 landed that**: `domain_for` claims a fresh slot for a no-grant unprivileged task on a
  translating build and returns the singleton only on a region build, and the two-task witness is
  registered in both flavours, same-grant and no-grant. The no-grant case was the one that failed
  before it.

### F3. Frame-level capabilities are M6.5, and they GENERALISE sharing rather than introducing it

Splitting memory into distinct page-table and frame capability types, so that mapping is a
capability operation, is a DIFFERENT axis from F2 and it is deliberately not in the same
sub-milestone as first A-profile boot, first exception-level split, first VMSA, first GIC and a new
boot path.

They also land in different sub-milestones, M6.3 and M6.4 being the two falsifying backends
F8 requires.

The two axes meet at exactly one point, and it is the useful way to see both: the dedup F2 retires is
the MPU era's stand-in for a shared frame mapping. Sharing between two tasks does not disappear and,
importantly, **does not pause until M6.5**. F10 contracts the one sharing case the tree actually
needs during M6.2: the reserve, write, hand-it-over idiom, where one set of frames is mapped in two
spaces at the same address by a kernel-mediated handoff. F10 covers BOTH ways a range is handed over,
an explicit task create and a grant-carrying spawn, and above that freeze the distinction does
not show.

So what M6.5 adds is GENERALITY, not the capability to share: a frame that any holder may map
anywhere, rather than the single hard-wired handoff a spawn performs. That ordering is deliberate for
the same reason as before -- the general form wants the cap layer and the port does not -- but the
narrower form is not missing in the meantime.

### F4. One lock spans capability resolve-to-use

Carried forward, not decided here: archived `M7_smp_candidate_spike.md` establishes that holding one lock across
the whole resolve-to-use span survives with or without address translation, where a scheme leaning
on "no address translation exists" does not. M6 introduces translation, so it inherits the
obligation to not widen that span.

### F5. A translation fault kills the faulting thread's task. There is no demand paging

An unmapped access is a fault the kernel reports and contains, exactly as an MPU denial is today:
no swap, no overcommit, no copy-on-write, and no BACKING STORE decision in a fault -- the kernel
never allocates memory, performs I/O, or extends a mapping there. The
M4.7.9 fault-isolation machinery -- `arch_fault_is_user_thread` and `arch_fault_redirect_to_exit`
(`arch/include/kickos/arch/arch.h`) -- is what decides and contains it, with the syndrome register
decoded into the existing report. Stating it matters because "the MMU arrived" is otherwise read as
"demand paging arrived for free".

**IT KILLS THE TASK AND NOT ONE THREAD, and that reading only became distinguishable at T5c.** A
fault ends the faulting thread's whole group (archived `M4_task_layer_record.md` section 6), which read as
"the thread" for as long as a plain spawn was alone in its task. Under a process model the group is
the process, and containing a fault to one member of a shared address space would not contain it.

**THAT MACHINERY WAS NOT REACHED ON THIS BOARD UNTIL T8, AND NOW IS.** `KICKOS_FAULT_ISOLATION`
was gated on an arch list without armv8a, so `qemu-arm64` linked the fallback translation units and
a fault PANICKED instead of being contained; nothing before T8 may be read as witnessing
containment on this board.

**What T8 had to add was a RESUMABLE FRAME, and it turned out to already exist.** The debt was
stated as the vector building no frame a handler could return through, which is true of the
REPORTING slots: they reach C by a plain branch. But the EL0 synchronous entry is not one of those
-- `ENTER_FROM_EL0` saves all thirty-two vector registers, the general file, ELR, SPSR and SP_EL0
onto the thread's own kernel block, because the syscall it shares with must resume. So containment
is a rewrite of TWO fields in a frame the entry already built: ELR to `kickos_thread_fault_exit`
and SPSR to EL1h, then the ordinary `RESTORE_FRAME_AND_ERET`. No second entry path, and no frame
format of its own.

The SP the stub runs on needs no relocation either, and that is a property of this entry rather
than luck: SP_EL1 sits at the block top whenever EL0 is running, so an EL0 fault frame is always
exactly one frame below it, and the restore's pop leaves the stub at `kickos_fault_stack_top()`.
`arch_fault_is_user_thread` therefore tests the frame's address EXACTLY rather than for
containment in the block -- the same test carries both "this frame is trustworthy" and "the eret
will seat the stub where arch.h requires", and it fails closed to the panic dump.

**TTBR0 STAYS ON THE FAULTING SPACE across the redirect**, which is the opposite of what the
terminal path does. The stub prints the dead thread's NAME, and a thread name is a string literal
in app text that only that space maps; the dying space is put back later, by the release path.

**What this freeze must NOT say is "no mapping is ever populated in a handler", and an earlier draft
said exactly that.** On an architecture with a software-refilled TLB the refill exception is not an
error at all -- it is the hardware's normal path for every valid mapping, and the handler is REQUIRED
to install the entry from the software tables. MIPS is the plain case; the same shape appears on
LoongArch, Book-E and SPARC v9. Forbidding it would outlaw a legitimate backend, and worse, would
invite a kernel fault path that treats every translation exception as a kill.

So: classifying a translation exception -- refill of an installed translation versus a genuinely
absent one -- happens BELOW the seam, and the kernel only ever sees "unmapped, contained". A
hardware-walked backend has nothing to classify and the distinction costs it nothing.

**One seam width, WIDENED at S3 so the kill path does not reopen it.** `kickos_fault_record` used to
take its status word as a `uint32_t`, and `FaultRecord` to store one, while `ESR_EL1` is a 64-bit
register. It now takes a `uint64_t`. The reason is not AArch64, where the truncation would in fact
have been lossless -- on Armv8.0 the exception class and the whole instruction syndrome sit in bits
31:0 and `ISS2` arrives at Armv8.7. It is that keeping the narrow word means arguing, per arch, that
nothing above bit 31 is ever set, and on RV64 that argument fails on the register and survives only
on the caller: `mcause` is XLEN-wide and its INTERRUPT bit is bit 63, so what makes a 32-bit
truncation safe there is that `.Lfault` filters interrupts out before recording. An argument about
each caller's filtering is the wrong thing to freeze a seam on, and it would have to be re-made at
M6.3 and again at M6.4.

The widening is also far cheaper than it looks, which is the other half of why it belongs here rather
than in a later milestone. No call site changes: every existing backend passes a narrower value and
it promotes. It is the declaration, the struct field and the one print conversion. Measured on
microbit, the board nearest its arena cliff: `.bss` does not move at all, the struct's tail padding
absorbing it, and `.text` grows four bytes.

What does NOT justify it is ABI-freeze urgency. The alpha ABI stays unstable well past this
milestone, so this could have been left to M6.3 without becoming a migration. It is done here
because M6.3 and M6.4 are the two steps that would otherwise have to re-make the per-caller
truncation argument, and doing it once before them is cheaper than twice inside them.

### F6. The MPU seam is not reinterpreted, and gains a parallel family beside it

Already in the tree as prose, from the spike's QW-6: `arch/include/kickos/arch/arch.h` states that
the region seam is a flat, NON-TRANSLATING protection-region set and stays one, and that a VMSA
port gets its own parallel family. M6 implements that note rather than revisiting it. An MPU
backend keeps its region calls; a translating backend implements the aspace family and no MPU.

**An MPU board MAY be built unenforced; a translating board may NOT.** An MPU is a gate over a
memory system that runs without it, so disabling it is a posture with a cost behind it, which is what
the `-flat` presets are. An MMU IS the memory system: the descriptors that translate carry AP, UXN
and PXN, so translation without enforcement is not a configuration that exists to be chosen, and F1's
high-half kernel does not resolve with translation off. So `KICKOS_MEMORY_ENFORCED`, derived as
`KICKOS_HAVE_MPU OR KICKOS_HAVE_ASPACE`, is not an invitation to a third posture: a chip that
translates has nothing to test, and per-chip Rule 7 tables key on the mechanism they have.

**THE SAME ASYMMETRY DECIDES PORTING ORDER, AND THAT IS WHERE IT KEEPS BEING MISSED.** An MPU port
is brought up flat and gated afterwards, which is a real sequence because the memory system runs
underneath the gate. A translating port has NO flat stage to be brought up in, and all three M6
backends refused one for three unrelated reasons: A64 cannot RUN compiled C with translation off,
Device-nGnRnE mandating natural alignment while the compiler stages struct copies through unaligned
pair loads (S2b); RV64 cannot LINK a full image, the prebuilt C library being medlow and reaching
only the top 2 GiB while RAM sits at the bottom of the space (R1.4b); and x86_64 never gets the
choice, firmware handing over with paging already enabled (F8). Three different layers, one
conclusion.
So "prove the ISA half flat, then add paging" is an MPU INSTINCT and it does not transfer. It is not
wrong incidentally, on this part or that toolchain; it is wrong because there is no memory system
under an MMU to fall back to. A step plan for a translating backend puts the boot table before
anything that has to link or run, and where a plan says otherwise the plan is what moves.

**And WHICH family a target has is a CHIP fact, not an arch fact.** An earlier draft wrote this as
"the A53 backend implements the translation family", which quietly makes memory protection a property
of the instruction set. It is not, and the tree already knows better: `mpu.cmake` lives under
`arch/<family>/chip/<chip>/`, so the memory backend is selected per chip today.

**RISC-V is the counterexample that makes this load-bearing rather than pedantic.** It is a
configurable ISA, and its memory protection lives in the privileged specification rather than in the
ISA string -- so `rv32imac` names the instruction set and CANNOT say whether the part has PMP or a
page-table mode, and two chips sharing that exact ISA string may differ completely in memory
protection. A translating RISC-V target is therefore the SAME arch with a different memory backend,
not a new arch, and the ISA half -- switch, trap entry, timer, console -- is shared rather than
rewritten. An arch-keyed family would force either a duplicated arch directory or a distinction
visible to the kernel, and both are the failure this document is trying to avoid.

So the aspace family is chip-selected on the same axis `mpu.cmake` already uses, and one arch may
carry an MPU backend and a translating backend at once. ARM is the same story read forwards: one
family already spans PMSAv7, PMSAv8 and no-MPU parts.

**But adding a third memory-model arm is NOT the whole of it, and taking it for the whole is
dangerous.** `KICKOS_HAVE_MPU` carried two meanings at once -- "this build has MPU
descriptors" and "memory protection is live" -- and an MMU board sets it to 0 on the first meaning
while needing the second. With it at 0, grant admission degraded to the memory-type check
alone, losing arena confinement and the reserved-block refusal, and the region encoder reported every
region as enforced unconditionally. A self-grant could then add an arbitrary range that `user_range_ok`
trusts and the access helpers dereference PRIVILEGED. On an MPU-less board that is merely honest,
there being no protection to breach; on a TRANSLATING board with a high-half kernel it is a
user-reachable privileged access to a kernel address.

So this freeze has a second half: **the two meanings are split before any MMU board configures.**
**THAT SPLIT LANDED.** Enforcement-live is `KICKOS_MEMORY_ENFORCED`, DERIVED in the top-level
`CMakeLists.txt` from `KICKOS_HAVE_MPU` or `KICKOS_HAVE_ASPACE` rather than declared per board, so no
board can state it and disagree with its own backend. Rule 7 and reserved-block admission are keyed on
it, and T5 made the self-grant a refusal by name rather than a silent widening. **THE SPLIT IS
COMPLETE, and the pair of static-data hooks section 3.3 carries was its last half.**
`arch_user_text_readable` and `arch_user_data_writable` (`arch/common/arch_ram_common.cc`) are keyed on
`KICKOS_MEMORY_ENFORCED`, as is `arch_mpu_probe_addr` beside them, and that file names
`KICKOS_HAVE_MPU` nowhere at all. What the old keying did on this board was take the non-enforcing arm
and admit app globals out of a linker whitelist; the re-key did not widen that whitelist, it replaced
the oracle, and the reasoning is 3.3's. A whitelist answers about an ADDRESS, and an address cannot
express a per-space reservation: a range reserved in task A is not valid in task B. Two oracles for
one question is a second truth, so the real authority is made TOTAL instead -- the per-`Domain` list
`aspace_image_seed` (`kernel/mem/aspace.cc`) seeds from the extents it reserves and grants, consulted
by `user_range_ok` BESIDE the region-set walk rather than in place of it. **T8 SUPPLIED THE HOSTILE WITNESS** (`grant_kernel_word_refused`): an authorized unprivileged
caller self-granting a kernel address is refused `-KOS_EPERM`, with a positive control on a range
the same task did reserve so the refusal is about the ADDRESS and not about the call. What refuses
it on this board is NOT arena confinement -- `grant_region_admissible` is not even reached, the
translating arm of the self-grant calling `aspace_self_grant` instead -- but the narrower rule that
replaced it: the range must be one this task RESERVED, which no kernel address is.

**BUT "a HIGH-HALF address" cannot be that witness on this board, and the reason is the layout rather
than a flaw in the freeze.** `grant_region_admissible` (`kernel/grant/grant.cc`) confines every RAM
grant to `[arch_ram_base(), arch_ram_base() + arch_ram_size())`, with no privileged waiver -- and on
`qemu-arm64` that arena is ITSELF high-half. `virt_arm64.ld` carves it out of a `MEMORY` region based
at `KICKOS_ARM64_VA_BASE`, so `__kickos_ram_start` carries the same high-half prefix as kernel text,
and `arch_ram_alloc` hands out high-half addresses as its ordinary output. Admission contains no
canonical-address or high-half test at all; the only high-half refusal in the tree is below the seam,
inside the map body. So a high-half address INSIDE the arena is admitted BY DESIGN, and one outside it
is refused for being OUT-OF-ARENA -- the right answer for a reason that says nothing about the kernel
half. A witness worded against "high-half" therefore passes on the arena bound and proves nothing.

So the witness names a specific kernel TEXT or DATA address OUTSIDE the arena. `__kickos_rom_start`
and `_sdata` are the self-describing choices, both far below `__kickos_ram_start` on this layout and
both aligned enough that the geometry arm cannot claim the refusal first. **The arena is high-half
here until T6 moves it**, at which point the wording can tighten; T8 does not wait for that.
**T5b.3 supplied the address without naming a linker symbol**, and a hostile witness should take the
same one: `arch_mpu_probe_addr` is now keyed on `KICKOS_MEMORY_ENFORCED` and answers with a word in
kernel-side `.bss`. App text cannot name a kernel-half symbol at all under this board's code model,
so a witness worded against one is a witness that does not link.

Consequence worth stating once: the A53 backend reports no enforceable region granule, which is
the existing signal for byte-granular allocation, so the power-of-two and natural-alignment
discipline the MPU forces on the arena is simply absent there. That is the pow2 tax `STATE.md`
expects this milestone to remove -- removed by the backend answering honestly, not by a rewrite of
the shaping helpers.

### F7. The granule has ONE source of truth, and that source is the arch seam

4 KiB, and the reasoning that decides it here is not the usual one. The server-side argument for a
larger granule is walk depth and TLB reach across a multi-gigabyte working set, which this system
does not have. The argument that decides it at RTOS scale is the opposite one -- internal
fragmentation across many SMALL objects. Every thread gets a stack and, per section 3.4, a guard page
below it: at a 64 KiB granule a guard page costs 64 KiB per thread and a 16-slot board spends a
megabyte on nothing at all. At 4 KiB it spends 64 KiB.

**The value lives in one place, and that place is a SEAM QUERY rather than a kernel constant.** This
is the correction that matters, because the MPU seam already got it right and an earlier draft of this
freeze regressed it: `arch_mpu_min_region()` exists precisely so descriptor granularity is ASKED of
the backend rather than assumed. A granule written into the frame allocator, a Kconfig default, a
linker script and this document is one figure charged four times -- but a single kernel constant is
only better by degree, and it still bakes an architectural fact into the kernel.

So the frozen property is the single source of truth, and the source is the arch family: the
allocator, the guard-page arithmetic and F10's page-aligned ABI wording all ASK. **4 KiB is what the
backends REPORT, not what the kernel believes.** It has to be: SPARC v9's smallest page
is 8 KiB, so there is no 4 KiB to select there at all, and a kernel constant would make that
architecture a kernel change rather than a backend.

**What the granule does NOT retire by itself is the stride tax, and that is a bigger correction than
it looks.** The arena's power-of-two rounding is applied whenever thread-local storage is enabled at
all, and the spawn path additionally refuses a stack block that is not exactly stride-sized and
stride-aligned, with the build refusing a non-power-of-two stack knob and the Reference stating the
constraint as universal. So a translating backend inherits the tax through the generic path even
though its own architecture does not need it.

The reason it is generic is worth knowing, because it decides the fix -- and the fix this paragraph
named was already made, while the tax it was aimed at lives somewhere else. Only the M-profile
backends are FORCED to derive the thread pointer by masking the stack pointer, that ISA offering no
per-thread register; RISC-V and RX mask anyway, deriving it from the stack rather than from the
thread's own block, which is a chosen uniformity and not a requirement.

**A64 never copied the idiom, so the conditional this paragraph set had no unmet half.** It SEATS its
thread-pointer register from the context, and the context derives that value from the block by
SUBTRACTION rather than by a mask -- deliberately, so the stack's low bound owes no alignment past the
ABI's sixteen bytes (`arch/arm64/armv8a/arch_armv8a.cc`, and the field's own comment in that arch's
`context.h`). "Escapes the tax only if it SEATS that register" was an observation about the other
three arches turned into a condition on this one; there was never a masking A64 to argue out of.

**The tax was GENERIC, in two places that knew nothing about the arch, and T6.1 qualified both.**
`tls_stack_admissible` (`kernel/thread/tls.cc`) refused any block that was not stride-ALIGNED and not
EXACTLY one stride, on every backend; and the top-level `CMakeLists.txt` refused at configure time any
`KICKOS_USER_STACK_SIZE` or `KICKOS_ROOT_STACK_SIZE` that was not already a power of two whenever
`KICKOS_TLS` was on. Both now key on the MECHANISM rather than on the feature.

*The mechanism is an arch fact and is declared as one.* `ARCH_TLS_FROM_SP` (`arch/Kconfig`) is
selected by armv6m, armv7m, rv32imac, rxv3 and lx6 and NOT by armv8a, and reaches CMake and C as
`KICKOS_TLS_FROM_SP`. Where it is set the two refusals stand unchanged; where it is clear
`tls_stack_admissible` asks only for the ABI's alignment, both arms still requiring a block strictly
larger than the carve. The stride is still DERIVED on every board, the link-time assert that the
thread-local template fits inside one reading it.

*The witness is the whole board and not an arm.* `qemu-arm64` states 12288 for a pool stack and 20480
for root's -- three pages and five, neither a power of two, neither the same stride -- and every image
and every selftest arm runs on them. Beside it, `tests/unit/tlscarve` gained a THIRD target,
`tls_carve_seat`, one configuration clause apart from the other two: same fabrication, same stride,
same reserve, `TLSCARVE_FROM_SP` 0, so what the two report is attributable to that clause.

**What the granule does remove, and what it does not.** The power-of-two rule taxes TWO axes and the
granule retires one of them. Rounding: a 17 KiB request costs 32 KiB under one MPU descriptor and 20 KiB in
frames -- but a 5 KiB request costs 8 KiB either way, so the rounding win is real and LUMPY rather
than uniform, and a claim of "the pow2 tax is gone" overstates it. ALIGNMENT is the axis that
genuinely disappears: a power-of-two block must also sit on a natural boundary, which fragments the
arena in a way frames never do, and that is what `arch_ram_region_align` exists for.

**4 KiB is the INTERSECTION, and that is the durable argument for it.** A64 defines three granules
and support for each is REPORTED by the implementation rather than guaranteed by the architecture.
The two A-profile reference cores this project holds disagree about one of them: the Armv8.0
Cortex-A53 (DDI 0500J section 4.3.21, Table 4-56) reports `TGran4` and `TGran64` supported and
`TGran16` NOT supported, while the Armv9.2 Cortex-A725 (107652_0002_05, `ID_AA64MMFR0_EL1`) reports
all three. So 4 KiB is the granule that needs no per-target question asked, across twelve years of
the profile.

**On the first target the alternative is therefore 64 KiB alone**, sixteen times the granule and not
four, which is what makes the guard-page arithmetic above decisive rather than merely suggestive.

One trap for anyone re-deriving this from the registers: `TGran16` states the SENSE of its answer the
opposite way round from `TGran4` and `TGran64`. Reading "zero means supported" across all three gets
16 KiB exactly backwards on both cores.

**Two figures both cores agree on, and F1 leans on them:** 16 ASID bits and a 40-bit physical address
range. They match across both, so they are properties of the profile as this project will meet it
rather than of whichever core the emulator models.

That is the SILICON's answer and the first target is an emulator, so S2 confirms the model reports
the same. A divergence there is a fact about the emulator worth recording, not a reason to reopen
this.

**TAKEN at M6.2's close and NOT at S2, and the model agrees with the manuals on all three.**
`arch_aspace_model` (`arch/arm64/armv8a/aspace_armv8a.cc`) reads `ID_AA64MMFR0_EL1` and the
`aspace_model` arm reports it: granules `0x5` -- 4 KiB and 64 KiB supported, **16 KiB NOT** -- with
16 identifier bits and a 40-bit physical range, which is the A53 reset value F7 took from DDI 0500J
Table 4-56 exactly. There is nothing to record as a divergence. The reading is what the arm asserts
rather than a constant of the port's: the granule arm beside it re-reads `arch_aspace_granule`'s own
figure and cannot diverge from it, which is why it was never the confirmation this freeze asked for.
Two figures the port PROGRAMS are compared rather than merely printed: the granule `TCR_EL1.TG0`
selects, and the physical range `TCR_EL1.IPS` claims, both read back out of the register rather than
restated. The identifier width is compared against the RECORD, nothing in this tree tagging a
translation, so `TCR_EL1.AS` stays at an 8-bit identifier whatever the machine offers.

Larger MAPPINGS are not a granule change: at the 4 KiB granule a block descriptor spans 2 MiB at
lookup level 2 and 1 GiB at level 1 (DDI 0487 M.b Table D8-17), which is what the kernel's physical
map in F1 uses to stay small. One granule, three mapping sizes.

**The cost this freeze accepts, and it is permanent.** MMIO isolation granularity REGRESSES against
the MPU fleet. An MPU grant reaches tens of bytes -- PMSA down to 32, PMP NAPOT to 8 -- and
`arch_mpu_region_encodable` exists precisely so an MMIO window is REFUSED rather than rounded,
because rounding over-grants the neighbouring device's registers. A page table cannot express less
than its granule, so the finest MMIO grant an unprivileged driver can hold on this port is 4 KiB, and
two devices sharing one 4 KiB page cannot be isolated from each other at all. This is not an argument
against the choice: a larger granule is strictly worse and A-profile offers nothing finer. But it is
a real loss, it lands on the driver era rather than on M6, and it belongs in the record here rather
than being discovered by the first driver that wants half a page.

### F8. The seam is tested by three unlike backends

The address-space interface names operations rather than a page-table format. A64 was the
first implementation; RV64/Sv39 tested the address-space family against a physical range
wider than its virtual range and a selectable paging mode; x86_64 tested entry and boot
against a firmware-owned translation regime and a syscall that does not switch stacks.
The three implementations kept one interface without forcing a shared root count or a
common hardware identifier. The complete cross-backend derivation and the corrections it
found are in the F8 record (archived `M6_aspace_seam_derivation.md`).

The portable address-space contract does not expose a direct physical map, a fixed number
of page-table levels, a fixed number of roots, or a mandatory address-space identifier.
Backend code owns translation maintenance. A fresh mapping's invalidate obligation depends
on the history of the reused descriptor slot, so a backend may not infer that every
invalid-to-valid edit is exempt from maintenance. The entry path must establish a trusted
stack before any memory access on an architecture whose syscall instruction leaves the
caller's stack pointer in place.

### F9. One portable floor, and each family strengthens it where its own mechanism is cheap

Today a thread's stack region, its granted device window and its self-grants are private to that
ONE thread: a sibling in the same task faults on them. On a translating backend, mappings are
**task-wide**, because per-thread roots would buy page tables, TLB work and the loss of ordinary
process sharing for a guarantee the family does not ask for.

**This is not a guarantee being dropped. It is a constraint-driven strengthening being recognised as
one.** The MPU shares a task's domain because the alternative needs data movement between threads
that those parts cannot afford -- memory first, CPU second -- and per-thread denial then comes almost
free, the region set being reloaded on every switch-in anyway. On an A-class part the cost structure
inverts, so the same strengthening stops being free and stops being the right default. The portable
contract is therefore the floor, and each family strengthens it where its own mechanism already
paid for the strengthening:

  - **The floor:** a task's grants are visible to its siblings. A thread-scoped grant guarantees
    ACCESS TO ITS HOLDER; portable code may not rely on a sibling being denied.
  - **MPU strengthens it:** per-thread effective regions, so a sibling does fault. Kept, documented
    as a backend strengthening rather than as the portable promise.
  - **MMU widens the mapping:** task-wide, while grant AUTHORITY stays thread-local.

**That last clause is load-bearing and it is what keeps the ABI intact.** The privileged-write seam
promises `-KOS_EPERM` when the caller does not hold a device window, and two selftest arms registered
on a board -- not sim-only -- assert exactly that from a thread other than the holder. Both would
break under task-wide mapping, and the reason was precise: possession WAS derived from the caller's
region set, so widening the mapping would have silently widened possession with it. **Possession is
now explicit thread-local authority rather than a walk over what the caller can reach**, landed at
T5 as the precondition this paragraph called for: the answer comes from the thread's own possession
record, seated before the region composition that maps the window, and never from what the thread can
reach (`kernel/thread/thread.cc`, `kernel/syscall/syscall_internal.h`). The ABI promise and the two
arms are unchanged on every backend. Conflating authority with reachability was the actual defect;
the MMU only exposed it.

**What has to be re-stated, and the house already has the wording for it.** The Reference carries a
precedent: the invariant covering the thread-local storage carve says in as many words that it is
naming and not isolation, and that a peer can reach it. The same qualification is owed to the two
device-window invariants, which currently argue that a task-wide window would hand registers to a
peer that never asked; to the private-stack clause of the switch-in invariant; and to the possession
invariant, which is the one the paragraph above repairs rather than qualifies. Four kernel comments
say a sibling cannot scribble another's stack, the architecture reference says it twice, and one Book
chapter teaches it -- while another Book chapter already frames a sibling's access as a legitimate
granted mapping, which is the MMU-compatible telling and the one to converge on.

None of that is a doc-only sweep. The comments state a guarantee the code will no longer make on one
backend, and a reader who trusts them will write code that assumes it.

**It lands WITH T5 and deliberately not before** (decided 2026-08-24). Every one of those statements
is true of the tree as it stands, so rewriting them ahead of the behaviour would put the Reference
and the code in disagreement in the other direction, which is the same bug facing the other way.
`TODO.md` carries the site list so the deferral is tracked rather than remembered.

### F10. Allocation RESERVES a virtual range; the self-grant MAPS it

The public contract already reads this way, which is why this can be a freeze rather than a menu.
The header says allocation returns a **page-aligned** block, that allocating **reserves** arena memory
and **grants nothing**, and that reachability comes either from handing the block to a spawn or from
asking for it explicitly. Under an MPU "reachable" means covered by a descriptor. Under translation it
means mapped. The two-step shape, and the promise that nothing grants implicitly, survive verbatim.

So: allocation reserves a page-aligned range in the CALLING TASK's address space and maps nothing;
the self-grant maps it. The number handed back is a virtual address in that task's space.

**The two alternatives are worse for stated reasons.** Returning a physical frame breaks the use the
number mostly has -- the app dereferences it after granting -- and exposes a namespace no unprivileged
thread can use. Mapping at allocation time contradicts a documented promise to buy nothing the
two-step does not already give, and would leave the self-grant with no meaning for RAM.

**A reservation is a GLOBALLY UNIQUE name that is mapped in a subset of spaces**, and getting that
distinction right is the whole of this freeze's difficulty. **The tempting simplification -- that the
number stops being global, and that only the dedup ever relied on it crossing tasks -- is false, and
the counterexample is the main provisioning path rather than a corner:** root reserves a block, self-grants
it, WRITES it, and hands it to a task create as that task's shared data region. The child has to see
the bytes root wrote.

So the handoff is contracted rather than incidental. **The kernel maps the reserved frames into the
new task's space at the SAME virtual address**, and root's own mapping persists if it self-granted
one.

**And it applies to BOTH consumers, which is easy to miss because only one of them is a create call.**
The explicit path hands a reserved range to a task create. The other path is older and is the one
the allocation header actually names -- a reserved range handed to a SPAWN as that thread's domain
data region. A spawn carrying a grant takes a task of its own (T5c), so that range crosses into a
space the caller does not hold, and it IS this handoff arriving through a different syscall. Today
the dedup is what let two such spawns land on one domain; once F2 deletes it, each is its own space
and each needs the mapping. Two live mappings of one block is not a new state: the task-create contract already warns that
mismatched memory-type flags leave exactly that, and that rule becomes the coherence rule for the
pair.

**A CONSUMER NAMED HERE NEVER EXISTED AS AN INTENT, and it goes with F2's deleted premise rather
than being deferred or narrowed.** An earlier reading of this clause covered every implicit-task
spawn, "which is most of the spawns in the tree" -- and that reach was inferred one step downstream
from F2's observation that resolving a spawn's task always spends a fresh task slot. It was never a
decision about handoff, and with the observation gone there is nothing left of it: a GRANT-FREE
spawn is a thread of the caller's task, sharing the caller's space, so there is no second space for
a handoff to reach and nothing to contract. The handoff's consumers are the task create and the
grant-carrying spawn, both of which really do open a space the donor does not hold. The address and
the refusal freeze unchanged; only the count of callers does.

**WHO FREES THE FRAMES, decided at T4 because destroy forced the question.** An address space frees
what it MAPS, and this handoff deliberately maps one block into two spaces, so the second space to be
destroyed would free frames the first no longer owns. The rule is that the BORROWER unmaps before it
dies, leaving exactly one space holding the block; there is no FRAME-LEVEL refcount, because one here
would duplicate the ownership M6.5 is going to express properly and would have to be unpicked again.
T4 makes a violation loud rather than absorbed: the frame pool counts refused frees and an arm requires
that count to be zero, so a double free fails a test instead of being swallowed by the allocator's own
guard. T5 and T6 honour the rule or T8b replaces it with something better.

**THAT RULE AS WRITTEN COVERS ONE ORDER OF DEATH ONLY, and the missing half was a real hole. Completed
at T10, after M6.2's stage result, on an external audit's finding.** "The borrower unmaps before it
dies" says what happens when the BORROWER goes first and says nothing about the DONOR going first,
which is the order that frees frames under a live mapping: the donor's last task release runs
`aspace_release`, whose destroy walk hands every mapped leaf back to the pool while the borrower still
maps them, and reallocation then turns a live mapping into another process's data or page-table
memory. **The executable check is structurally incapable of seeing it:** a donor that dies first frees
each of those frames EXACTLY ONCE, so `frame_pool_refused()` stays at zero throughout. The rule is
completed rather than replaced, and the completion adds a holder to a counter that already exists:

  - **A SUCCESSFUL HANDOFF TAKES A REFERENCE ON THE DONOR'S DOMAIN**, dropped where the borrower's
    BORROWED entry is unmapped. `Domain` is already "the memory a set of threads may touch, refcounted
    by the live tasks holding it, freed at the last release", so the donor's space cannot be destroyed
    while a borrower maps its frames, and T4's refusal of a FRAME-level refcount stands untouched: the
    unit is still the domain, which is what M6.5 generalises.
  - **The edge is acyclic by construction**, so the cycle two tasks handing each other a range would
    make is not expressible. It is written in exactly ONE place, `domain_for`, onto a domain
    `claim_slot` has just built, so it always points from a younger domain at one that was already
    live; and the reference it holds is what stops that donor's slot being recycled underneath it. A
    handoff CREATES the borrower and cannot add a range to a domain that already exists.
  - **Its witness is `task_handoff_donor_exits`,** and the arm it sits beside is why the hole survived
    review: `task_handoff_readback` kills both borrowers while root, the donor, runs on, which is
    exactly the order that hides this.

**Same virtual address, and not merely the same frames, and the reason is the block's CONTENTS.**
Nothing in the tree guarantees that what root writes into a shared block is position-independent, so
an address root computed can be sitting inside it. Relocating the child's view would silently
invalidate those, which is why a target space that cannot take the range at that address must REFUSE
the create rather than relocate it. The spike's QW-3 -- keep a shared ring offset-addressed, never
holding one side's virtual address -- is precisely what would relax this, and it is still an
unimplemented proposal, so until it lands the same-address rule is load-bearing rather than
conservative.

**What the handoff FREEZES is the address and the refusal, not a global allocator.** The frames are
mapped at the address the donor used, and a destination space that cannot take the range there
REFUSES the create rather than relocating it. That pair is portable, and it is all the contract needs.

An earlier draft went further and required reservation addresses to be drawn from a SYSTEM-WIDE
range, so a given address named at most one live reservation. That makes collisions impossible, which
is why it was tempting -- but it turns a per-space allocator into a kernel-wide virtual-address
allocator, and it is a 64-bit assumption wearing a correctness argument. On a 32-bit architecture the
whole user namespace may be two gigabytes SHARED ACROSS EVERY PROCESS: N processes reserving M bytes
would consume N times M of one budget while each mapped only its own subset. So system-wide
uniqueness is demoted to a POLICY a wide-address backend may choose for itself, and the handoff
carries the donor's space and address while the destination answers.

The number therefore stopped being nameable by any task -- self-granting an address you did not
reserve is refused -- without the contract having to claim it names one thing system-wide.

This also answers what "mapping the same frames" means before M6.5, which F2 leaves open: it means
this, a kernel-mediated handoff at handover time, and it is the only sanctioned cross-task sharing in
M6.2. M6.5 generalises it; it does not introduce it.

**Three ABI-visible consequences, all of them nameable now rather than discoverable later:**

  - **The self-grant must admit only what the caller RESERVED.** Today's contract admits any
    reserved-clear in-arena range, "not only memory the caller itself allocated" -- and under
    translation that sentence IS F6's hole stated at the ABI level. This narrows to the caller's own
    reservations. A strengthening, so no correct caller notices.
  - **Its already-reachable short-circuit re-keys too.** The contract returns success, at no
    descriptor cost, when the range is already reachable with the same memory type. Reachability is
    exactly what F9 stops treating as authority, so under translation that test becomes "already
    MAPPED in this space with these attributes" -- a question about the mapping, asked of the space,
    rather than a question about what the caller can touch. The observable answer is unchanged for a
    correct caller; the thing being asked is not.
  - **The natural-alignment refusal does not arise.** The header already parenthesises that arm as
    holding "under an MPU", and a page-granular reservation cannot trip it.
  - **The region-budget refusal RE-KEYS onto the range list's own bound.** It was written here as
    EVAPORATING, which was inferred one step downstream from section 3.1's descriptor argument rather
    than decided, and 3.1 no longer supports it. Two quantities were collapsed into one: how much
    memory a domain can DESCRIBE has no analogue under paging, and how many ranges it may NAME is a
    property of the validation list and survives paging untouched. **T6.2 moved the admission all the
    way onto the range list and took the region array out of this path**, which the clause above
    still allowed either way: a second, ROUNDED entry beside an exact one is two answers to one
    question, and the array is per-THREAD where the mapping is task-wide, so a sibling reaching a
    self-granted block would have been reading the range list regardless. The `-KOS_ENOMEM` therefore
    comes from `KICKOS_ASPACE_RANGES`, and because ALLOCATION is what spends a slot there, it is the
    allocation that reports it -- as a NULL, the pointer return having no room for a code.

**Ownership follows F9 and is worth spelling out here too**, since this is the syscall pair where a
reader is most likely to assume otherwise: a successful self-grant maps into the TASK's address space
even though the authority that permitted it is held by one thread. Authority thread-local, mapping
task-wide, in the one place the two are easiest to confuse.

**And the gate is the driver framework, not a test app.** Its bring-up runs the whole idiom --
reserve, self-grant, write, hand it over -- and that flow runs UNCHANGED against the frozen meaning,
covering the handoff contract, the two-live-mappings rule and the flags-match rule at once, where no
selftest arm substitutes for it. **It splits across two steps and the split matters:** T5 owes the
flow CONFIGURING and running, and T6 owes the child reading back what root wrote AT THE SAME ADDRESS,
because until the image is mapped a leftover identity map makes that address equality vacuous. An allocation ABI that only satisfies a selftest arm has not been tested against its real
consumer. Three more arms belong with it, and the second is the one this freeze makes possible to
test at all: allocate then self-grant then use; a CROSS-TASK refusal, one task self-granting an
address another task reserved, which is meaningless-by-construction now and must be refused rather
than merely failing; and reservation released at process teardown, which is T8b's counter check
seen from the ABI side.

**AND THE ABI OWES A CLEARED FRAME, which this contract did not say until a fourth external review
found it missing from the code as well as from here.** A frame the pool hands out for a
reservation or a user stack carries no earlier task's bytes: `frame_pool_alloc_user_run` clears
every frame of the run through the pool's own kernel alias before answering, and a frame it cannot
reach returns the whole run rather than a partly cleared one. Two callers keep the uncleared
primitive on purpose, `data_copy` and `data_template_fill` overwriting every granule they take.
The region fleet has no pool and the bump arena never frees, but the THREAD-STACK FREE LIST does
re-hand a dead thread's block under a new thread's region, so the clear belongs at that pop too.
The witness is a reused frame and not a fresh one: the arm asserts the second process landed on the
FIRST one's frame before asking whether it reads zero, and unscrubbed it reads back the whole block.

**THE GATE DOES NOT RUN ON THE TRANSLATING BOARD, and that is the largest single gap M6.2 closes
with.** `drv::bring_up` runs where a board declares a SERVICE LIST; `qemu-arm64` declares none, so
the flow this freeze names as the gate for the whole allocation ABI runs only on region boards and
against host fakes, where nothing translates and the same-address rule is vacuous. What discharged
the readback there is `task_handoff_readback`, which is precisely the substitution this freeze says
no arm may be -- it runs the same four steps and it is not the consumer. Porting a service list to
this board is a step of its own and is carried in `TODO.md`; recording it is all M6.2 does.

**WHAT LANDED OF THIS CLAUSE, and the gate itself did not.** The first arm has been live since
T6.2. The other two were IMPLEMENTED AND UNWITNESSED until M6.2's close and are arms now:
`self_grant_cross_task` puts a worker in a task of its own, has it self-grant a range ROOT
reserved, and requires `-KOS_EPERM` -- with the same worker self-granting a range IT reserved as
the control, without which a refusal from a missing authority reads the same. `reservation_teardown`
gives a member a reservation it never maps and ends its process, requiring every frame back: that
run has no leaf pointing at it, so the destroy walk cannot see it and only `aspace_release` can
hand it back. Both answer down an ENDPOINT, sharing no app global with root, per T6.2's ruling.
The registered arm count went 124 to 127 with `aspace_model` (F7), and T8b's own two arms had
taken it 122 to 124 without this document saying so.

**THE FLAGS-MATCH RULE IS WITNESSED ON ONE CONSUMER AND CANNOT BE ON THE OTHER.** The rule is the
task-create contract's -- the memory type belongs to the BLOCK, so every call that maps it is
passed the same one, or two live mappings disagree about the type. Until M6.2's close every path
in the suite passed flags 0, so the rule was assigned to `task_handoff_readback` and never asked.
It now carries a block of its own through a self-grant and a task create at `KOS_MEM_NOCACHE`, both
sides reporting the type their mapping recorded and the arm requiring them equal. **A block of its
own and not the readback pair's**, because a mismatched alias is exactly what the rule forbids and
reusing one block would have built the disagreement rather than the agreement.
*The other consumer cannot express it at all:* a grant-carrying SPAWN has no memory-type field in
its ABI, so `task_for` passes 0 and that handoff maps Normal whatever the donor holds. No caller
can obey the rule through a spawn, which is an ABI gap recorded here rather than worked around;
closing it means a field on the spawn parameters and belongs to whichever milestone reopens them.

**AND ASKING FOR A MEMORY TYPE WAS REFUSED ON THIS BOARD UNTIL THE SAME PASS, by the wrong seam.**
`grant_nocache_admissible` asked `arch_mpu_nocache_support()`, which a translating board answers
REFUSED for the honest reason that it seats no region descriptor -- so every non-zero memory type
was refused at admission while the map editor reported the type honoured. It asks
`arch_aspace_memtype_support` where the backend translates, which is F6's rule about which family
answers, applied at the one call site that had it backwards. The already-reachable short-circuit
moved with it: a page table always CARRIES the type, so a block reachable cacheably is not already
reachable as a caller asking for another type meant.

**AND THE CONVERSE, which M6.2's audit found unstated and unimplemented.** The typed question was
asked only when the REQUEST carried a memory type, so a request naming none passed on rights alone
and returned success over a block still mapped non-cacheable. That is the way BACK from a DMA
buffer, and it is the transition the type exists for. The check now compares the existing exact
type against the requested one in both directions before taking the short circuit, on both
families: where a REGION descriptor carries the type the descriptor is REPLACED rather than
stacked, one block never carrying two of them. Witnessed by `self_grant_retype`.

**THE KERNEL'S OWN VIEW IS THE ONE MAPPING THE RULE CANNOT BIND.** The kernel reaches every frame
through a map of its own that stays cacheable: armv8a's static TTBR1 blocks map all RAM Normal
write-back, and x86_64 reaches frames through the firmware's write-back identity map. A block a task
maps non-cacheable therefore always has a cacheable alias, and the kernel uses it whenever it acts on
the block for the task: the frame pool's clear at hand-out, and every copy into or out of a user
buffer, IPC included. **The rule for that alias is maintenance, not agreement:** the kernel edits no
mapping of its own. Before it reads or writes a non-cacheable page through its view it cleans the
touched lines to memory then drops them, so no line fetched through the alias, speculatively
included, is read or merged into a partial-line write, and after it writes it does so again, so no
dirty line is evicted later over what the task or a device stores. Every change of a mapping's type
is maintained the same way before the new leaf goes in, in both directions: a non-cacheable leaf
drops the clear's zeroes and any dirty line a cacheable mapping left, and a retype back to cacheable
drops the clean lines the kernel's reads left, which would be stale to it. The type is the leaf's, which `arch_aspace_acquire` reports from the walk it
already makes, so a cacheable page costs the copy one test. rv64imac has no alias to maintain: it
has no Svpbmt, and the PMAs type a frame for every view of it. A region board with a data cache over
the arena has the same alias in its background map, through which the kernel reaches a block whose
non-cacheable region belongs to a thread other than the one loaded, and its copies keep the same
rule against that thread's region set, its grants and retypes the same sweep (`docs/reference/invariants.md`,
`kernel-alias-maintained-over-uncached-memory`).

---

## 2. What the port gets for free, and it is more than the spike expected

The spike was written before M5.2.1 and prices the trusted-entry problem as unsolved. On A64 the
hardware answers it: EL0 runs on `SP_EL0`, an exception to EL1 selects `SP_EL1`, and the two are
different registers. **"No privileged C dispatch runs on a pointer a thread chose" is therefore
architectural on THIS port rather than hand-built per backend** -- and that qualifier is load-bearing,
because it does not carry to x86_64, where the syscall entry does not switch stacks and
the kernel loads its own. Everything in this section is an A64 dividend, not an MMU dividend.

Which means the new arch writes the transfer and nothing else -- no red-zone path beside it -- and
so it selects the mandatory-kernel-stacks capability in `arch/Kconfig`. That stanza says exactly
this case out loud: a new arch that writes only the transfer selects it. The consequence is worth
tracing, because it looks like a problem and is not: `KICKOS_KERNEL_STACKS` takes its first
matching default, which is the mandatory one, so it resolves 1 without the chip declaring an MPU
capability it does not have. No ladder edit is owed.

A64 also has a thread pointer, `TPIDR_EL0`, readable by unprivileged code. So thread-local storage
costs a register write at switch-in, where every existing arch needed something bespoke -- the
kernel-provided helper on M-profile, an emulated-TLS override on RX.

**`TPIDR_EL1` WAS spent as the EL0 entry scratch, and T6a released it.** This paragraph asserted the
collision in the present tense long enough to stop a reader starting T6 three times, which is why it
is corrected here first: section 2 is where an implementer starting that step reads.

The collision the tri-arch spike caught was real, and circular rather than merely conflicting.
`ENTER_FROM_EL0` (`arch/arm64/armv8a/switch.S`) needed a scratch register only because `SP_EL1` could
not be trusted on entry, and `SP_EL1` could not be trusted only because `thread_create` seated
`ctx.kernel_sp` AFTER `arch_context_init` returned, so a new thread's first trap arrived with `SP_EL1`
still on its user stack. One edit answered both ends of that.

T6a made it. `kernel_sp` is seated by `thread_create` BEFORE `arch_context_init`, which READS it to
place an unprivileged thread's first frame, and the field's own comment in that arch's `context.h`
records the ordering; `ENTER_FROM_EL0` seats nothing and spends no scratch, and the comment above the
macro states the invariant and states that this is what releases the register. The reporting slots in
`vectors.S` still seat `SP_EL1` from the kernel's own record, deliberately: a slot this port never
enters proves nothing about the register.

So M7 inherits a FREE `TPIDR_EL1`, and T9 is where that record became per-core: the cell is a field of
`struct armv8a_percpu` (`percpu.h`), reached through an accessor that folds to a link-time address at
one core and reads TPIDR_EL1 above it.

---

## 3. The rewrites, measured against this tree rather than the spike's

The spike names three genuine rewrites below the arch seam. All three survive re-reading against the
current tree and 3.1 to 3.3 are those three; 3.3 also carries a FOURTH that the spike does not name,
and it is the hardest of them. 3.4 and 3.5 are not rewrites -- they are what the port has to build
and where it plugs in.

What matters most is what is absent from this list, and the claim has to be PHASE-SCOPED to be true.
The defensible statement is: **through M6.1, M6.3 and M6.4 the object types, the capability
operations, the handle encoding and scheduler POLICY are unchanged.** M6.5 then extends object and
capability
layers by design -- C1 adds frame and page-table object types, C2 makes mapping a capability
operation -- while preserving the generic handle encoding and the resolve-to-use lock of F4.

Two qualifications belong with it, because the unqualified reading misleads in both. Scheduler
POLICY is untouched; the switch HOOK is not, the only memory-protection thing the switch path invokes
today being the region-set apply, which an address-space activation has to reach at both of its sites.
And "three rewrites" groups subsystems by what changes about them; it is not a count of files or of
call sites, and the sites that must change outnumber the headings.

### 3.1 The region array gains an address space beside it

`domain_region_count` / `domain_region_at` (`kernel/domain/domain.cc`) are already the only
sanctioned readers, and `kernel/thread/thread.cc` composes a thread's set through them. That was
the spike's QW-1 accessor half and it is what makes the representation swap local.

**THE SWAP DID NOT HAPPEN, and this heading claimed it would.** T5 added `space` BESIDE `regions[]`
and `region_count` rather than behind them: `kernel/include/kickos/domain.h` still declares both, and
its own header comment is the accurate version -- on a translating backend the region set is
validation data beside the space rather than the enforcement. `domain_for` still writes
`regions[0]` directly. Nothing argued for hiding the array; the old heading was a description of an
intended end state written as though it had a decision behind it, and the knock-on is F10's
region-budget bullet, corrected there. What IS true is the narrower thing the accessors buy: the
readers are enumerable, so the array can be retired later without a hunt.

**The dedup goes, and it does not come back in this shape.** `domain_for`
(`kernel/domain/domain.cc`) reuses a live unprivileged domain when its single region matches on
base, rounded size and the full attribute word. That key is a PHYSICAL base, meaningful only
because base addresses are global, and per F2 it must stop collapsing two tasks into one space. In
M6.1 the map is 1:1 and the key keeps its present meaning, so the port reaches a running image
without touching it. In M6.2 each task gets its own space and the reuse arm is deleted rather than
re-keyed. What it bought was DOMAIN SLOTS, and that is a real cost to account for rather than to
wave at. **Do not pay for it out of the per-domain region ceiling described two paragraphs below:**
those are unrelated quantities, one being how many domains exist and the other how much a single
domain can describe. What the dedup was standing in for returns as a mapping operation, in M6.5.

**Two further readers of the region shape end with it, and neither is in the allocator.**
`arch_domain_static_regions` exists because an unprivileged thread gets no background-region
default and so faults fetching its own instructions unless its code and static data are explicit
descriptors. A process does not want two prepended descriptors: it wants the app image MAPPED into
its own space, which is the same information expressed as translation rather than as a window. And
`KICKOS_MPU_MAX_REGIONS` -- the ceiling on how much memory a domain can describe at all -- has no
analogue, a page table not being a bounded descriptor list. A domain stops having a maximum
describable extent, which is the first place this port gives something back instead of costing.

The refusal contract does not move. `KOS_EPERM` for an inadmissible grant and `KOS_ENOMEM` for a
full pool are the two answers, and a refusal must still leave no half-built domain -- which under
paging means no frame leaked and no partial mapping installed. A transaction, exactly as a spawn
became one in M5.2.1.

**A domain had no destroy path, which F2's "freed at the last release" quietly assumed away -- and
T4 and T5 built one.** This paragraph used to say there was no destroy path anywhere in the tree,
which was a MEASUREMENT and not a position: release decremented a refcount and did nothing else, and
a slot was reused by scanning for a zero refcount and reinitialising it in place. Under an MPU that
is complete, the region set being pure description. Under paging the same slot reuse strands a
page-table root, its tables, and every data and stack frame the process held -- so a build can pass
the mapping and process witnesses while leaking every process that exits.

**What the tree has now**, so a reader does not re-derive it: `domain_release`
(`kernel/domain/domain.cc`) calls `aspace_release` (`kernel/mem/aspace.cc`) at the last release, and
that call re-activates the BOOT space when the space being destroyed is the running one, unmaps the
extents the space only BORROWED from the image, then hands the rest to `arch_aspace_destroy`, whose
walk frees every still-valid leaf output rather than only the tables. `claim_slot` reaches the same
call on its two unwind arms and on a free slot still holding a stale space. The frame pool counts
REFUSED frees and two selftest arms require that count to be zero, so a double free fails a test
instead of being swallowed.

What is not built is the ordered invalidation being witnessed, the unwind arms being witnessed, and
any frame a process allocated FOR ITSELF -- section 3.4's per-process copy does not exist, the
granted-range list has no production caller and the self-grant installs no mapping, so every frame a
space maps today came from the image. T8b is scoped against that residue and not against a missing
destroy.

Worth stating because it is easy to assume otherwise: **there is no address-space identifier concept
in the tree at all today.** F1's remark that recycling is not a design problem is about the hardware
having plenty of them, not about existing code managing them well.

### 3.2 The bump allocator becomes frames plus a map editor

`arch_ram_alloc` hands out one naturally-aligned power-of-two block per call because one MPU
descriptor must cover it. Paging wants three things instead: a physical frame allocator at page
granularity with no alignment cleverness, per-address-space virtual range bookkeeping, and a
page-table editor. This is the single largest new subsystem in the milestone and the only one with
no existing analogue in the tree.

It is also the part that must not be back-doored by stage 1. An identity map makes physical and
virtual the same number, so every consumer written during stage 1 works whether or not the frame
allocator exists -- which is precisely how an identity map becomes the allocator by accident.
`roadmap.md` already forbids it; section 5 makes it a stage boundary with its own gate.

**And there is a public ABI in the way, and the two halves of it DISAGREE ALREADY.** The
RAM-allocation syscall returns the arena bump pointer verbatim and grants nothing; the self-grant
syscall then takes that same number as a PHYSICAL base and admits it by rounded size and natural
alignment. **What "verbatim" hides is that the header ahead of it already promises PAGE ALIGNMENT**
(`user/include/kickos/sys.h`), and the allocator does not keep that promise here. `arch_ram_alloc`
aligns to `arch_ram_region_align`, which on a backend seating no descriptors is sixteen bytes floored
by the thread-pointer stride, so any request smaller than a page comes back sixteen-byte aligned. The
page-aligned promise is therefore not something F10 introduces: it is in the shipped header ALREADY,
and the first allocation only looks compliant because the arena's own base happens to sit on a page
boundary. A defect in the pair rather than a decision to revisit, and F10 is where the repaired
meaning is frozen.

The number is load-bearing in THREE places, not one: the self-grant, the domain
region a spawn derives from it, and the app's own dereference of it. The driver framework's ring
block goes through exactly this pair, so it is not a test-only path, and the contract is written down
in the public headers.

A frame has no user virtual address to return, so the meaning of that number is a decision and not a
detail. **It is frozen in F10**: allocation reserves a page-aligned range in the calling task's space
and maps nothing, the self-grant maps it, and the number is a virtual address in that space. F10
carries the two rejected alternatives, the one thing that is genuinely lost, and the three ABI-visible
consequences. It is stated as a freeze because M6.5 cannot retroactively repair an ABI that shipped a
milestone earlier, and because M6.2's own gates have to run against a decided meaning.

### 3.3 Validation and access split for real, and the split is per PAGE

Today validation and access are two layers that happen to collapse. `user_range_ok`
(`kernel/syscall/syscall_mem.cc`) walks the running thread's region set and accepts a range inside
one granted region; `user_readable_ok` and `user_writable_ok` add the backend hooks for app text
and static data. Access then funnels through three helpers in that same file --
`kaccess_from_user`, `kaccess_to_user`, and `ep_copy` -- which were identity copies because a
validated user address is directly kernel-dereferenceable. That funnelling was the spike's QW-2 and
it is what turns this from a hunt into three functions.

Two things change, and the second is the one the spike misses.

**Validation should read a range list, not walk a table.** A per-address-space list of granted
virtual ranges keeps validation at the cost it has today and leaves the page tables as the
enforcement rather than the oracle. Walking the tables on every syscall argument would put a
multi-level memory traversal on the entry path to buy an answer the kernel already knows.

**And that list must be SEEDED with the process image, or ordinary code stops working.** The open
decision this section used to carry is CLOSED, and it closed the way the section assumed rather than
the way the tree happened to work. What the tree did was branch `arch_user_text_readable` and
`arch_user_data_writable` (`arch/common/arch_ram_common.cc`) on `KICKOS_HAVE_MPU` rather than on
enforcement, which on this board resolves 0 while `KICKOS_MEMORY_ENFORCED` is 1: the other arm ran,
and that arm is a WHITELIST of the chip's link-time extents plus, since T5b.1, a second weakly
declared pair covering the app's own low window. So app globals were admitted by a linker whitelist on
an enforcing translating board, and the two arms this section assigns to T6 would have passed with no
range list of any kind.

**Both hooks are now keyed on `KICKOS_MEMORY_ENFORCED`, and the reasoning is the one already spent one
level down.** A whitelist answers about an ADDRESS, and an address cannot express a per-space
reservation: a range F10 reserves in task A is not valid in task B. Two oracles for one question is a
second truth, so the real authority is made TOTAL instead. It admits a whole linked window rather
than a process's own mapped ranges, which is the same over-admission F6 removed for descriptors. The
re-key changes ONE board: an MPU board answered false already and a non-enforcing board still
whitelists, both because enforcement and descriptors agree there.

**Where the seeding lives, and it is not on the arch side.** The list is a member of `Domain` beside
the space it describes, seeded by `aspace_image_seed` (`kernel/mem/aspace.cc`) with the same two
extents and the same rights it maps -- text read-execute, static data read-write -- and reserved then
granted, so nothing enters it that no reservation named. `user_range_ok`
(`kernel/syscall/syscall_mem.cc`) consults it BESIDE the region-set walk rather than in place of it:
the region array is what every grant path records and the only oracle a descriptor board has, so the
walk stays live on both, and the list answers for what no region array on this backend describes.
`arch_mpu_probe_addr` (`arch/common/arch_ram_common.cc`) reads as a question about descriptors, and
went with them anyway: on the keying it used to carry it answered 0 on exactly the board carrying the
strongest denial, so it is keyed on `KICKOS_MEMORY_ENFORCED` too and the file names `KICKOS_HAVE_MPU`
nowhere. R3's denial arm takes its address from it.

**What the two arms now witness, measured rather than argued.** `writable_global` (an out-pointer in
an app global) and `readable_global` (a read buffer in app rodata) are both RED with the seeding
removed and both green with it. Removing only the DATA range leaves `readable_global` green and
`writable_global` red, so each arm is pinned to its own seeded extent. Removing only the TEXT range
takes five arms down and the image does not survive to reach either of them, which is a fact about how
load-bearing app rodata is to the suite's own scaffolding rather than a sharper isolation.
`readable_global` runs from ROOT and not from a worker deliberately: root is unprivileged, a worker is
a sibling in root's task holding root's space, so the worker would add nothing while making a refusal
present as a failed spawn.

Where the linker window comes from was stated wrongly here too, and the same way. The app-data window
is not "carved under the descriptors-exist gate": `virt_arm64.ld` says in as many words that the app
window is a LINK split and not a grant -- four output sections in `MEMORY` regions of their own, with
no gate over them -- and F6's split has nothing to do with its being available to a translating
build.

**A range contiguous in virtual memory is not contiguous in physical memory.** This is the new
invariant, and it applies to all three access helpers: a validated user range may span pages
backed by unrelated frames, so an access must be split at page boundaries and each page translated
separately. **The kernel reaches a frame through a seam pair -- acquire a kernel-usable pointer for a
(space, page), release it -- and NOT by adding an offset.** That distinction is the whole portability
of this section. A backend that direct-maps all of physical memory inlines acquire to an addition and
pays nothing; a backend that CANNOT does something else behind the same call. The ones that cannot are
not exotic: a 32-bit architecture whose physical address space is wider than its virtual one has no
offset to add, and a fixed unmapped kernel window covers a fixed span rather than all of RAM. An
earlier draft spent F1's direct map directly in these helpers, which put a 64-bit assumption into
kernel code and would have forced a transient-window rewrite ABOVE the seam on the first such
backend. The loop is mandatory either way, and a helper written as one `memcpy` over
a translated base is correct only until the first range that crosses a page.

**The helpers must carry each end's OWNER, and that is an API change rather than a body rewrite.**
`ep_copy` asserts raw numeric non-overlap before every copy. Two processes with the same virtual
layout is not a corner case under F2 -- it is exactly what T6 exists to produce -- so equal addresses
in different spaces become ordinary, the assert's premise dies, and it trips deterministically. Owner
cannot be recovered from an address either: after T6 an address alone identifies nothing. So the
contracts take each user end's address space and compare `(space, range)`, with distinct kernel/user
and user/user forms, and every call site passes what it already holds.

**And the funnel has a leak that must be closed FIRST, because the whole plan rests on it.** The
tree's own comment says every kernel-side user-pointer dereference funnels through these helpers, and
that is not true today. **"Two endpoint sites" was a MISCOUNT, not a position since revised.**
Nobody decided the leak was a pair; the sentence was written from a reading that stopped inside
`syscall_ipc.cc`, and the site it missed is the one no arm on this board covers. The enumeration
replaces the count. FIVE call sites, all in `kernel/syscall/`, hand a PARKED peer's address to a
helper that resolves against the RUNNING space:

  - `cap_console_deliver` (`kernel/syscall/syscall_ipc.cc`) hands the popped receiver's parked
    buffer to `kaccess_to_user`. This is the payload delivery.
  - `write_recv_info` is reached with a peer's parked out-pointer from three sites in that same
    file: `endpoint_send`'s parked-receiver arm, `cap_console_deliver`, and `endpoint_call`'s
    parked-receiver arm.
  - and from one site in `kernel/syscall/syscall_ipc_fast.cc`, inside `kickos_ipc_fastpath`. This
    document never named that file, and it is the site with no arm behind it on this board: the
    fastpath is not implemented on this arch, so `call_reg_fastpath` exercises the other backends'
    copy of this bug and none of it here.

All five already hold the peer thread, so the owner is available and simply not passed.

**Which is why the fix is not "change the body".** `write_recv_info` is ONE body, in
`kernel/syscall/syscall_mem.cc`, and two of its callers -- both arms of `endpoint_recv` -- pass the
RUNNING caller's own out-pointer and are correct exactly as they stand. The owner therefore arrives
per CALLER, which makes this five edits at four functions rather than one edit at a helper.

One console syscall additionally dereferences a user pointer directly, outside the funnel entirely,
after validating it, and its own comment names the exposure. Under one physical space every one of
these is correct; under F2 the five silently read or write the wrong process and the console site has
no seam to fix. Closing the leak is a precondition of T7 and not a tidy-up after it.

`ep_copy` is the hard one and its own comment already says so: it is the endpoint payload move,
both ends user memory, one of them belonging to a PARKED peer. Today the waker reaches the peer's
buffer through privileged background access to one physical space. Under paging the peer's pages are
not in the running thread's low half at all, so this is a genuine cross-address-space copy: translate
the peer's virtual range through the peer's own address space, page by page, and move the bytes
through the kernel's physical map. F1 is what makes that expressible without temporary mappings,
which is the second reason it had to be frozen first.

The privileged-caller bypass at the top of `user_range_ok` still holds under F1, a kernel pointer
being valid in every address space. It would not under fully separate spaces.

### 3.4 What a process is made of, concretely

The witness F2 owes -- two tasks with the same virtual layout -- forces this to be stated, because
"map the app image" is not obvious in a system that has no loader and no filesystem.

**There is one linked image and there will be no loader in M6.** Kernel and app are linked
together today and that does not change: a process's text is the SAME physical pages as every other
process's text, mapped read-execute at the same virtual address in each space. Sharing text between
processes is therefore the default rather than an optimisation, and it is also the cheapest possible
first user of "two spaces, one frame".

**Static data is where that stops.** App static data cannot be shared, because two processes
writing one physical page of globals are one process with a memory bug. So per-process static data
is a COPY: frames allocated per space, initialised from the image's initialised-data contents, and
the zero-initialised span cleared. This is the first thing in the port that a linked-in image does
not simply provide, and the amount of it is knowable at build time from the app's own low window.

**THE COPY LANDED AT T6.2 AND ROOT IS THE ONE SPACE THAT DOES NOT GET ONE, which is forced rather
than chosen.** `kernel/mem/aspace.cc` maps the app's text extent read-execute onto the image's own
pages in every space; the data extent goes to a frame-pool run per space, copied a page at a time
from the space that holds the image's own data pages. **That space is root's, and it has to be:** the
app's constructors run in `root_entry`, in a thread, after root's space exists (`kmain.cc`), so a
root holding a copy would construct ITS copy and leave the image pages carrying link-time bytes for
every process created afterwards. Making root the template also settles the two consequences the copy
was going to have, without a second mechanism for either -- `kickos_init_args` is written into
app-side `.bss` before any space exists and is therefore in the template, and every ctor's output is
in it too, because the copy is taken from what root has, not from what the linker laid down.

**AND THE TEMPLATE OUTLIVES ROOT, which M6.2's audit found it did not.** Releasing root's space
cleared the record of which space held the image's own pages, so the next space seeded mapped those
pages itself and became a second template: every process created after it copied a LIVE process's
mutable globals rather than root's. Two properties now hold together. The source of a spawn is
its spawner, root or a task, read live, so a global the spawner writes before a spawn is one the
child reads out of its own copy, and the app's own drivers depend on exactly that. An explicit
task's space copies the snapshot below instead, which the first explicit task's seed takes out of
the live root (M10.4, `DOM_CALLER_TASK`), so every explicit task and its restarts start from
root's data as it stood then. On root's way out, where no seed took it first, `aspace_release`
freezes those pages into a snapshot of frames taken off the pool when root was seeded, with
root's mappings still standing, and a space claimed with no spawner copies the snapshot. A seed
reaching a lost home with no snapshot behind it is REFUSED. No process other than root is ever
the snapshot's source. Witnessed by
`process_data_template`, which stages the loss of the home and reads a later process's copy back.

*A space now knows which of its mappings it OWNS.* The range list carries two flags per entry,
BORROWED and IMAGE (`kernel/include/kickos/vrange.h`), and `aspace_release` walks it: a borrowed
range is unmapped and no frame of it is freed, a reservation that was never mapped has its frames
handed back by hand, and everything else is left for destroy, which frees what the space MAPS. That
replaces the two hard-coded extents release used to unmap, and it is what makes F10's handoff
expressible: the block the donor reserved is BORROWED in the receiving space.

**Thread stacks stopped being arena blocks and became mappings, and that LANDED at T6.1.** An
unprivileged thread's stack was an `arch_ram_alloc` block shaped so one descriptor covers it. In a
process it is frames mapped in its task's shared address space per F9, with no power-of-two size and
no natural alignment, and with an unmapped page below it so an overrun faults instead of reaching a
neighbour. `kernel/mem/ustack.cc` is the whole of it, and `ustack.h` carries the contract. It still
goes into the thread's region set at switch-in, which is what admits a stack pointer at the syscall
entry on both backends.

*The address is the frame's own output address, and that is a decision.* Placement is free, so the
cheapest free choice was taken: the run's physical base IS its virtual base, exactly as it already is
for the image. Three things fall out of it. The kernel can write a stack whose space is not the
running one -- a spawn seats the TLS block before the child's first switch-in -- by asking the frame
pool for the same bytes through the physical map, so no per-page translation stands in front of a
memcpy and no second address is carried in the TCB. The range is globally unique, so the guard page
below it cannot be another space's mapping. And nothing needs a per-space virtual-address allocator,
which F10 owns and T6.2 lands.

*The guard page is charged to the FRAME POOL and not to virtual space alone.* The allocation takes
`pages + 1` consecutive frames and maps all but the lowest, so the page below a stack belongs to that
stack and no later allocation can map it. F7 budgets one granule per thread for exactly this. That is
what `FrameAllocator::alloc_run` exists for, and its refusal is by RUN rather than by count: it sweeps
the whole bitmap rather than starting from the single-allocation hint, so a fragmented pool refuses a
run while the free count still allows one.

*The release is at thread EXIT and deliberately not at slot reclaim.* `sched::exit_current` frees the
stack inside its first locked block, BEFORE the task reference is dropped: a space frees what it maps,
and `task_release` can destroy the space, so a later release would hand the pool frames the space had
already returned. The other half is accumulation -- a plain spawn is a thread of the caller's task, so
waiting for the space would hold every dead sibling's frames for the life of the group. It is safe
there because the descent runs on the thread's own kernel block. An unmap that REFUSES frees only the
guard frame and leaves the rest to destroy, that being the one owner left.

*Root's stack comes out of the same allocator, which forces one ordering.* `kmain` resolves root's
task explicitly before allocating, because the frames go into root's own space and that space does not
exist until its domain does -- and it does so AFTER idle is built, `task_for` taking no reference, so
an uncommitted task slot is still free and idle's own resolve would otherwise hand root's slot to the
kernel domain. Idle keeps an arena block: it is privileged, and the kernel's half is mapped in every
space by construction.

*Which means a privileged thread holds NO SPACE, and the switch path must know it.* The kernel's half
is reachable from any root, but the APP's half is not: it is the half that changes per process, and
for a thread with no space of its own the installed root is still the last process's. libc's
reentrant state lives there -- the per-slot array and the word libc resolves from are both app
objects -- so the switch path asks `aspace_seated_for` before it primes or seats either, and writes
neither when the answer is no. Idle is the only such thread and needs no prime.

**ON THIS BACKEND THE SPAWNED-PRIVILEGED-THREAD SCENARIO IS NOT REACHABLE AT ALL, and that is a
hardware fact and not a policy one.** A spawned thread's body is app text, and the app's half is one
level-2 slot of a per-space table whose every leaf carries `PTE_U`
(`arch/riscv/rv64imac/aspace_rv64imac.cc`); the RISC-V Privileged ISA forbids S-mode FETCHING from a
`U=1` page irrespective of `sstatus.SUM`. Such a thread therefore faults on its first instruction and
never reaches a prime. `privilege-escalation-gated` closes it a second time, the privileged population
being unable to grow after boot. So `aspace_seated_for` is not what saves THIS board; it is what
covers the general case on a backend where a privileged thread CAN execute app-half code, and it is
asked unconditionally so that no backend has to re-derive which case it is. Recorded at
`root-unprivileged-idle-alone-privileged` and witnessed by `reent_seating`.

*A caller-supplied stack re-keyed at T6.2 and is no longer arena-confined.* Where a backend
translates, the block must be a range the space the CHILD runs in already maps and that is not the
process image, which is exactly what F10's allocator hands out; the arena predicate does not run
there, describing a bump arena that holds no reservation. It still gets no guard page. **And the
kernel stopped asking the frame pool who owns a stack:** F10's allocator hands the app frames out of
that same pool, so a caller-supplied stack answers `ustack_kptr` exactly as a kernel-allocated one
does, and `Thread::kstack_owned` is what the release paths read instead.

**Kernel objects do not move.** Threads, domains, endpoints and the kernel stacks stay where they
are, in kernel memory reached through the high half, and the object pools in
`kernel/include/kickos/instance.h` are untouched. The arena that `arch_ram_base` and `arch_ram_size`
describe is what the frame allocator takes over; nothing that lives outside it is affected by any
of this.

### 3.4b The activation path, named, because an empty signature diff needs a baseline

F8's verdict is a diff against a frozen API, so the API has to exist before the SECOND backend starts,
which is the RV64 litmus.
Naming four operations is not the same as freezing them: without signatures and a named caller the
verdict has nothing to diff against.

**There are exactly three places the active region set is installed today**, and an address-space
activation has to reach the same three: the switch bookkeeper, which the IPC fastpath also uses; the
first-thread start, immediately before control leaves for the first context; and the self-grant path
mid-syscall, which widens the running thread's own set and makes it effective without a switch.

**Only the first two are ACTIVATIONS.** The third is a MAP of the space already active, never a root
switch, and conflating the two would have had the self-grant reloading a root it never left.

**WHAT SKIPS THE ROOT WRITE IS A CACHE, AND THE CACHE IS PER CORE.** An activation whose space is
already the installed one must not repeat the write: with no translation tag the backend drops the
whole low half on every root change. The cell that records it is one per core (`KICKOS_NUM_CORES`,
indexed by `arch_cpu_id`, which folds to a literal at one core), because a root a second core has
never written is a root that core would then skip installing. Destroying a space clears the cell on
EVERY core and rewrites only the running core's own root, a second core being switched off a dying
space by its own scheduler. At one core this is byte-equivalent to the single cell it replaces; it
is stated here because it is the property M7 would otherwise have to discover.
**The clause that used to close this sentence -- "activate has two callers, map has three" --
corresponded to nothing countable and is DELETED rather than corrected.** Three was the number of
region-set install sites in the paragraph above, of which exactly one is a map; nobody counted map's
callers, the figure was carried across the sentence. A call-site count is not what this seam freezes
in any case, and today's is already different: map is reached from the image seed and from the
selftest scaffolding, and activate from the switch path, from the boot-space restore inside
`aspace_release`, and from that same scaffolding.

**There is no `flush` in this family, and removing it is the single most important thing in this
section.** A flush call names a TLB operation, which is a mechanism, and it hands the KERNEL the
maintenance schedule -- which the kernel cannot get right in general. Two architectures make that
concrete, and one of them is the first backend: A64 requires break-before-make when replacing a live
entry, so the invalidate belongs BETWEEN two writes rather than after one, and a kernel-sequenced
map-then-flush is already the wrong order there; and a hashed-page-table architecture must evict from
a full bucket during a map, so its map can fail for CAPACITY with frames available -- a failure no
flush call can express.

So `map` and `unmap` are **coherence-complete**: when one returns, the change is visible to this
core, and whatever maintenance that took happened inside. They are also total-or-fail, and the
capacity refusal is DISTINCT from out-of-frames, because a caller that sees them as one will retry
forever. Where batching genuinely pays, it is expressed as begin and commit on the address space --
the concept "these edits become visible together" -- and never as a maintenance call.

**And the context does not know its address space.** Context initialisation takes an entry point, an
argument, a stack and a privilege posture, and no memory-domain parameter at all; it has one caller.
So the association has to be created somewhere, and naming where is part of T2 rather than something
the implementer discovers: either the context carries the root and activation reads it, or activation
is driven from the thread's task and the context stays ignorant. Both work; leaving it open does not.

**`map` OWNS THE ENTRY ENCODING, and memory type reaches it as a CONCEPT rather than as bits.** This
is not a stylistic preference; it is forced, and RISC-V documents the force in three layers. The
entry format varies by RATIFIED EXTENSION -- Svnapot adds an N bit and Svpbmt adds page-based
memory-type bits to Sv39, Sv48 and Sv57 entries, and the spec reserves further high bits for future
standard use. It varies by VENDOR: the C906 user manual states that extended page attributes live in
the entry itself. And whether those vendor bits are interpreted at all is a MODE, gated by a
non-standard machine-mode control bit (`MXSTATUS.MAEE` on that part), which an S-mode kernel does not
even own -- firmware sets it before the kernel runs.

So no caller above the seam composes an entry, and no caller learns which bits exist. `map` takes
rights and a memory TYPE the way the region seam already does, and whether the backend can honour a
requested type is a capability QUERY, exactly as `arch_mpu_nocache_support()` is today. That
precedent is the second time in this document that the MPU seam turns out to have already solved a
problem the aspace family was about to get wrong.

What T2 therefore FREEZES: the opaque address-space type; create, destroy, map, unmap and activate;
the acquire and release window pair of section 3.3; the granule query F7 requires; the error and
transaction semantics of each, including a capacity refusal distinct from frame exhaustion; and the
context-association path. **No flush, and no identifier** -- both are mechanisms, and F1 and this
section keep them below the seam. With that published, "scheduler policy is untouched" becomes a claim someone
can check, and it is the claim this document makes -- not that the switch path is byte-identical,
which it is not.

### 3.5 A new arch, and where it plugs in

Mechanically this is the pattern `arch/Kconfig` already sets: an arch stanza that is pure
capability `select`, a chip stanza that decides no knob, and a string default per arch for the
directory names. Three notes, all of them about collisions rather than design:

- The existing `arm` family directory is M-profile. A64 is a different instruction set with a
  different exception model and wants its own family rather than a third sub-arch under that one.
- The chip name `virt` is taken, by the rv32imac QEMU machine. The A53 machine needs a distinct
  chip name even though both are called `virt` by QEMU.
- The toolchain is a separate compiler from the M-profile one, so it needs its own toolchain file
  and its own environment variable rather than a widened hint in the existing one.

The memory model is a `choice` in the root `Kconfig` with two arms today, flat and MPU. The MMU is
a third arm, and that shape is what lets stage 1 ship under the existing flat arm with no new knob
at all. The knob is what stage 1 reuses, not the mechanism: S2b turns translation on regardless,
because A-profile has no way to ask for Normal memory without a table.

---

## 4. What the milestone must produce as evidence

The project rule is that a witness is valid for a tree, so each stage owes one and they are not
interchangeable.

- Stage 1 owes a registered emulator preset whose banner and TAP stream come off
  `qemu-system-aarch64`, in the ctest suite beside the existing emulator presets. An A53 port that
  builds is not a ported arch.
- Stage 2 owes a cross-domain denial: a thread reaching a virtual address its address space does
  not map, reported and contained, with the system surviving. This is the analogue of the existing
  isolation self-test and it is what makes the port ENFORCING rather than merely translating.
- Stage 2 also owes the page-crossing case from section 3.3 explicitly. A range split across two
  frames that are not adjacent physically is the arm that fails if any access helper kept its
  single `memcpy`, and no other arm in the suite can catch it.
- Stage 2 owes the PROCESS witness, which is the one that discharges F2: two tasks whose images sit
  at the SAME virtual addresses, backed by different physical frames, each reading its own. Nothing
  short of that separates a real address space from an identity map with permissions on it, and an
  identity map passes every other arm listed here.

The last one is the lesson of M5.2.1's own review: the mechanism that hides is the one whose only
witness never exercises the interesting geometry.

**The seam verdict is evidence too, and it is the one piece that cannot be gathered early.** F8's
empty-diff result is a claim about the aspace family, so it can only be taken once a second backend
exists. Until then the seam is UNPROVEN rather than proven -- which is a fair thing to say in a
review, and a much better position than believing a one-backend seam is general.

**And the whole existing fleet is the regression gate.** Every freeze in section 1 is chosen so that
nothing above the arch seam moves, which is a claim with a cheap falsifier: the boards that exist
today must stay green with no per-board work at all, on both halves of the suite and across the
image sweep. If an MPU board needs so much as a knob to keep passing, the seam was cut in the wrong
place and that is a finding about this design rather than about the board.

---

## 5. Stage outcomes

- **M6.1:** Bring up the A53 backend on QEMU `virt` without translation.
- **M6.2:** Add task address spaces, frame allocation and enforced user/kernel separation
  on arm64.
- **M6.3:** Implement RV64 Sv39 as a second translating backend and test the shared
  address-space seam against a different ISA.
- **M6.4:** Bring up x86_64 and test the entry and boot paths. Its initial flat user grant
  was removed by M10.1.1; the current q35 contract is in
  [`design-m10-kernel-share.md`](design-m10-kernel-share.md) section 1.
- **M6.5:** Add frame-run and address-space capabilities. Mapping a run pins it; unmapping
  or address-space teardown releases it. Memory authority gates map and unmap.

The detailed steps, measurements and corrected premises are retained in
archived `M6_implementation_record.md`.

## 6. What M7 inherits

**Multicore inherits a live TLB obligation rather than a deferred one, and that reverses what its
own spike says.** archived `M7_smp_candidate_spike.md` was written when SMP came first, and it argued that
cross-core TLB maintenance was "deferred, not inapplicable" -- an MPU having no translation cache to
shoot down -- and that a doorbell carrying only asynchronous notification would need extending later.
With page tables landing FIRST, the milestone that adds the second core arrives with translation
already in the tree, so the blocking all-core rendezvous that spike names is owed by that same
milestone. Its other two claims survive the swap untouched: no MMU-class target is the arch that
cannot emit the atomic a rendezvous needs, and holding one lock across resolve-to-use works with or
without translation.

That correction is IN that document now, at the head of the section it inverts, rather than left for
its next reader to notice -- along with the rename, both SMP records having carried an `m6-` filename
for a milestone that is now M6's MMU work. What M7 still owes itself is the design consequence:
a doorbell that can only be fire-and-forget is insufficient from its first line of code.

The seams this milestone cuts for it are T9, and they compile to nothing at one core.

**One debt M6 must not hand M7 silently.** `docs/design-m7-state-inventory.md` classifies kernel
state as per-core or genuinely global, and it was produced against the state that existed in M5. This
milestone ADDS global state that no such classification covers -- the frame allocator, the
address-space pool, and whatever bookkeeping T4 needs. Each of those is classified in that document
as it lands, by the step that lands it, because M7 rediscovering them by reading the code is how an
inventory stops being worth having.

---

## 7. Deliberately NOT frozen

- **Whether the granted-range list of section 3.3 lives on the domain or on the address space.**
  It is one indirection either way and the answer follows from where M6.5 puts frame capabilities.
- **The virtual layout of a user address space, in what remains of it.** Placement decoupling from
  allocation makes layout free in principle, but F10 and T6 have since frozen a partition of it: the
  image sits at addresses shared across processes, and the handoff carries the DONOR's space and
  address while the destination answers -- which is what lets it reproduce the address, and is not
  the same claim as a reservation address being unique system-wide. F10 demotes that stronger
  property to a POLICY a wide-address backend may adopt for itself, and this entry asserted it as a
  freeze; the two were in contradiction and F10 is the one that reasoned about it. What is still
  open is the layout of everything else, and whether
  the reservation range and the image range are adjacent or far apart, which is a fragmentation
  question rather than a correctness one.
- **Whether a user mapping ever uses a block rather than a page.** The granule is frozen at 4 KiB
  (F7) and the kernel's physical map uses blocks; whether a large user mapping should too is a
  measurement, and nothing in M6 waits on it.
- **The CONSUMERS of the cache-maintenance seam, and not the seam itself.** T9 lands the clean and
  invalidate seam, `roadmap.md` requiring it of this milestone. What is deliberately open is who
  calls it: the heterogeneous case in the spike's section 4 is what makes a maintenance interface
  earn its shape, and a unicore A53 on an emulator exercises almost none of it. The granularity is
  not open either -- the A53 has 64-byte lines for both caches (DDI 0500J section 2.1 states it in
  the level-1 memory system feature list, and `CTR_EL0` in section 4.3.26 carries it).
