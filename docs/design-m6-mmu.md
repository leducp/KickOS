<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M6 -- the MMU: a unicore A53 on QEMU `virt`

> **Status: ACTIVE** -- the design contract for M6, written to be audited before code.
> Section 1 is what it FREEZES, section 5 is the step plan with the expected result of each step.
> An auditor is invited to add steps, split them, reorder them and correct the expected results;
> the step identifiers exist so a finding can name one.

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
`docs/design-task-layer.md`, whose own table assigns the task "the `Domain` (the shared region set,
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
  `docs/design-task-layer.md` records that two tasks landing on one domain stay two tasks. Under the
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
  went uncited here -- `docs/design-task-layer.md` section 5.3, which rules that naming no task
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

Carried forward, not decided here: `docs/design-m7-smp.md` establishes that holding one lock across
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
fault ends the faulting thread's whole group (`docs/design-task-layer.md` section 6), which read as
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

### F8. The seam is held against a litmus, and M6 ships THREE backends

**This is the seam's standing doctrine and not a new rule.** `arch/include/kickos/arch/arch.h` says in
its first nine lines that the porting interface names concepts and never mechanisms, and that the
litmus test is a non-ARM port -- Renesas RX72M -- fitting the seam with NO SIGNATURE CHANGES. Note the
method: RX72M was never ported to make `arch_mpu_region` credible. It was HELD AGAINST the seam, at
design time, and it was a target the project actually intended to ship. Both properties matter, and
the aspace family inherits both.

So the deliverable is a NEGATIVE result -- the seam's signatures do not move -- and where one moves,
that diff IS the finding and it lands in the seam with the other backends updated to match.

**Two backends were not enough, and picking A64's nearest neighbour is why.** An earlier version of
this freeze shipped A64 plus x86_64 and called that the test. Those two agree on 64-bit addresses, a
hardware table walker, a hardware-defined radix format, a 4 KiB granule, a generous high half, byte
order, and fault-then-report semantics. They differ on exactly two axes -- root count and whether the
syscall entry switches stacks -- and this document had already banked both on paper before either
backend existed. An empty diff across that pair would read as proof of GENERALITY when it is proof of
SIMILARITY.

**The three backends, and what each is for:**

  - **A64 on QEMU `virt`, M6.1.** The first implementation. Proves the family runs.
  - **RV64 with Sv39, M6.3. This one falsifies the ASPACE FAMILY**, and it is the litmus. Its
    physical address space is WIDER than its virtual one, which breaks any full-direct-map assumption
    on a mainstream part rather than a contrived one; its paging mode and therefore its LEVEL COUNT
    are selectable on one chip; and the architecture permits the address-space identifier length to be
    hardwired to zero, so a target may have no identifier at all -- the length is discoverable by
    writing ones and reading back, and its maximum is 16 bits for Sv39 but only 9 for Sv32, so even
    the WIDTH is not one number. It is also shippable, which is the
    RX72M property: the C906 core in the Allwinner D1 is a single-core RV64 Sv39 part that can be
    bought, and being unicore it matches this milestone's own constraint natively rather than by an
    emulator flag.
  - **x86_64, M6.4. This one falsifies the ENTRY AND BOOT paths**, which is a different seam from the
    one above: a syscall entry that loads no stack pointer, and adopting an already-live translation
    regime handed over by firmware. Keeping it is worth the milestone; calling it the aspace seam's
    test was the error.

**They go in that order and never concurrently.** The reason the MMU precedes multicore is that two
firsts in one bite leaves the switch path undebuggable; three at once is the same mistake with a
bigger blast radius. Each backend lands and works before the next is started.

**The x86_64 backend earned its place before a line of it existed.** Writing this document against
A64 alone produced three claims it falsifies, and the landed port produced a fourth that only a real
firmware's regime could have shown. Each is corrected where it was made:

  - **F1's "the kernel half is untouched by a switch"** holds on A64's two-register split and fails
    on a single root register, where the kernel's mappings must be replicated per table.
    *And the single root is also where the two backends chosen for unlikeness disagree about ROOT
    COUNT PER PRIVILEGE LEVEL,* which no A64-shaped reading predicts. RISC-V forbids a supervisor
    instruction fetch from a page marked for unprivileged access whatever SUM holds, so one flat link
    cannot give kernel text and app text different user bits under one root and M6.3 ships a root per
    privilege level. x86 forbids that fetch only under supervisor-mode execution prevention, which
    M6.4's port REFUSES rather than assumes, so ONE root serves both levels there and the grant is an
    in-place edit with no root written. The seam absorbed both, which is the result; a design that had
    banked "one root per address space" as the portable shape would have been wrong on one of the two.
  - **Section 2's "the hardware answers trusted entry"** holds where an exception selects a different
    stack pointer, and fails on x86_64 in the sharpest possible way. Its fast syscall
    entry loads the code and stack SELECTORS and the target instruction pointer, saves the return
    address in a register and the flags in another, and masks the flags -- and loads NO stack pointer
    (AMD APM Vol 3, `SYSCALL`; Vol 2 section 6.1.1). So privileged execution begins with the stack
    pointer the CALLER chose. That is not merely "the kernel must load its own stack": it is
    verbatim the hazard class the M5.2.1 audit found as a critical on two backends, arriving here by
    architecture rather than by an implementation slip. The entry's first instructions must reach a
    kernel stack before they touch memory, and the flag mask must be programmed to clear the
    interrupt flag, or interrupts are live on a user-controlled stack.
    *That last sentence leaves out two things, both measured on the landed port.* The flag mask
    covers neither the non-maskable interrupt nor the machine check, so the window it closes is
    narrower than the word "interrupts" suggests; what covers those two is an interrupt-stack-table
    slot each, which builds their frame off the entry's own stack pointer instead of on it. And
    SYSRET loads NO stack pointer either, so the property holds on the way out as well and a return
    through it has to seat one by hand; a port whose return path is an interrupt return off a frame
    never finds that out.
  - **F1's address-space-identifier note** reasons from 16 bits always being there. The second
    backend's identifiers are narrower and feature-gated, so the CONCLUSION survives -- thousands of
    identifiers against a domain pool of tens -- while the premise does not. Do not let the width
    into the design.
  - **F1's "the kernel lives at a fixed high range"**, which is the half of F1 the first bullet does
    not touch, and the claim only an ADOPTED regime falsifies. The kernel's mappings there ARE the
    firmware's low identity map and the image is one flat link inside it, so the kernel lives LOW.
    The port's own range does sit high, in a top-level slot claimed before the first create, and the
    adopted part cannot be moved there without rebuilding the regime this backend was chosen to
    adopt. What follows above the seam is the sharper half: the kernel's convention that a virtual
    address it picks IS the physical address does not survive that regime, a reservation named by its
    own physical address landing inside the slots every space shares. It costs no signature, the fix
    being a bias into the per-space window.

    **AND A SHARPER ONE IN THE SAME PLACE, MEASURED: turning the axis on would make every page table
    this port allocates writable from ring 3.** `ring3_init` grants the unprivileged bit over two
    ranges, the image and the whole conventional-memory arena, and the frame pool the axis needs is
    inside one or the other whichever way it is carved: an in-image reservation is inside the image
    range, and a carve out of conventional memory is inside the arena range. Ring-3 writes were
    measured landing in both. So every table `map_into` and `aspace_create` allocated would be a
    table an unprivileged thread could edit, while `KICKOS_MEMORY_ENFORCED` read 1. This is not the
    naming problem above and a bias does not fix it: what fixes it is separating the halves in the
    LINK, which is what X5's own record says retires the over-grant. **Do not select the memory axis
    on this board before that separation lands.** X5 argued the select out for a related reason, that
    the flat over-grant makes "enforced" a false statement; this is the sharper form of it, because
    the axis would not merely overstate the protection, it would hand the translation tables
    themselves to the level they are meant to contain.

    These two are the things most likely to surprise whoever turns the memory axis on for this board.

**And the two boot paths differ in a way that stresses the SEAM and not merely the port.** On QEMU
`virt` the A53 arrives with translation OFF, so KickOS builds the first table from nothing and there
is no live regime to respect. UEFI hands control over in long mode, 64-bit, **with paging already
enabled**, the regions named by its own memory map identity mapped, selectors flat, at least 128 KiB
of 16-byte-aligned stack -- and interrupts ENABLED (UEFI 2.11 section 2.3.4). That backend must
therefore ADOPT an active translation regime and replace its root WHILE EXECUTING, which constrains
activate in a way the first backend never exercises: the code performing the switch has to be mapped
at the same address on both sides of it. Note the identity map's scope is exactly what the memory map
DEFINES; mappings of anything else are explicitly undefined, so "it was mapped before the switch" is
a claim about the map and not about the machine. That case does not exist on an A64-first port, and it
is the strongest reason to take this backend through UEFI rather than the cheaper boot path.
*And adopting that regime does not hand you an unprivileged level,* which this paragraph reads as
though it did. Measured on the firmware the port boots under: every entry from the root down to the
leaf covering the loaded image carries the user bit CLEAR, and the permission is ANDed along the walk,
so ring 3 could not reach one byte including its own first instruction. The same tables are mapped
read-only with write protection on, so the first entry a grant tries to edit takes a write fault at
ring 0 on the root itself. Both are that firmware's choices rather than the architecture's, which is
the same caution this paragraph already gives about the map's scope, applied to its permissions.
*The handover state itself is now MEASURED rather than cited:* the entry reads the flags, the paging
bit, the long-mode bit and the root register before it executes its own first instruction, and
`tools/run-qemu-x86_64.sh` asserts the four the specification mandates. Interrupts are enabled, paging
is on, long mode is active and a root is live, as this paragraph says.

**Where F7 transfers unchanged, and that is evidence rather than luck.** x86_64 pages in
4 KiB units with 2 MiB and 1 GiB large mappings above them (Intel SDM Vol 3 chapter 5), so the
granule freeze and its "one granule, three mapping sizes" reading hold verbatim on both.

Two things not to carry across, though, and they rhyme with A64's. **The LEVEL COUNT is not fixed**:
4-level paging translates 48 bits of linear address and 5-level translates 57, so a walker or a table
builder that bakes in four levels is wrong on a processor that enables the other mode. And paging
features there are enumerated by CPUID (SDM Vol 3 section 5.1.4) exactly as A64's granules are
reported by an identity register -- so on BOTH backends the rule is the same: ask the hardware, do not
assume. That symmetry is worth more than either fact.

**And the RV64 spike falsified exactly that symmetry, which is what the litmus was for.** Memory TYPE
is not enumerated on RISC-V the way granules and paging features are on the other two: Svpbmt carries
no architectural identification bit, and the C906 gates its vendor page attributes behind a
machine-mode control bit an S-mode kernel neither owns nor can observe. So a backend written as
"read the identity register" has nowhere to read. The durable rule is the qualified one: **ask the
hardware where it answers, and the board where it does not** -- which is the shape
`arch_mpu_nocache_support` already has, being a per-CHIP answer rather than a probe.

**THE RULE HAS THREE SHAPES AND NOT TWO, and the third is this architecture's own.** "Nowhere to
read" is true of memory TYPE and false of the two figures M6.3 actually needed. RISC-V makes several
fields WARL, Write Any values Reads Legal values, so the hardware answers by REFUSING: write a value
and read back what stuck. Measured at R3 and R4's pre-audit, all on the emulated core -- the
exception-delegation register accepts 0xb7ff of a written 0xffff, discarding the non-delegatable
causes on its own (**and the port no longer writes 0xffff**: it writes `0xffff & ~(1 << 9)`, leaving
ECALL-from-S undelegated, and reads the value back into a named refusal. The 0xb7ff is still the
hart's behaviour and now has a second witness in that readback, but the WRITTEN value this port ships
is not the one measured here); the translation-control register accepts four paging modes and leaves itself
untouched for the twelve it does not implement, which is a documented consequence rather than an
accident; and the identifier width answers 16 to ones written across the field. None of that is an
identity register and none of it is a board fact.
So: ask an identity register where one exists, PROBE THE WORKING REGISTER where the architecture
makes it WARL, and ask the board only where neither answers. Leaving it at "nowhere to read" sends
the next porter to a board file for a figure the hart will tell them.
*The catch that travels with it:* WARL probing is DESTRUCTIVE, discovering a field by writing it. The
identifier probe was measured safe under live translation only because it preserves the root, moves
the identifier field alone, and fences on both sides. A probe that clobbers the translation root to
learn about the translation root unmaps the code doing the probing.
*AND AN EXTERNAL RE-REVIEW ON 2026-08-29 PUT A FOURTH SHAPE BESIDE THEM, WHICH IS THE ONE THE OTHER
THREE CANNOT REACH: what the machine DOES, rather than what it stored.* A WARL readback answers about
the value that stuck, and two different machines can store the same value and behave differently.
The boot grant is the case. Every PMP field is WARL and permitted to read as zero, so a `pmpcfg0` and
`pmpaddr0` that both read zero are a hart with no PMP entry, which permits every access, AND a hart
whose entries all read OFF, which denies every supervisor access (Privileged ISA 3.7.1); one readback,
two opposite effects, and no further reading separates them. What separates them is an ACCESS:
`mstatus.MPRV` with `MPP=S` makes a machine-mode load or store checked as a supervisor one, so the
prologue measures the permission it is about to depend on instead of decoding a description of it.
Measured on `qemu-system-riscv64 11.0.3 -M virt`: with both CSRs written zero the readback is
`pmpcfg0=0x0 pmpaddr0=0x0` and that probe takes a load access fault, so on this machine the reading
the port used to accept as "no PMP" is the DENIED case. The catch above travels here too, in a
different form: the probe faults where the grant is absent, so it needs a vector of its own and a
refusal, not a return.
*AND A FOURTH REVIEW ON 2026-08-29 ADDED NO FIFTH SHAPE EITHER. It found the two things a
MEASUREMENT OF AN EFFECT owes that a readback does not, and this port owed both.* An effect is
measured under a REGIME, and it stands for exactly the EXTENT the measurement covers.
*The regime.* `MPRV` with `MPP=S` makes the access TRANSLATED as well as protected as a supervisor
one (Privileged ISA 3.1.6.4, and Table 49 of the hypervisor chapter, where `MPV` decides between a
one-stage HS access and a two-stage VS one). RISC-V DEFINES NO RESET VALUE FOR `satp`: section 3.4
lists what reset establishes, `satp` is not in it, and everything unlisted is UNSPECIFIED. So a
probe that leaves `satp` alone is not a Bare probe, it is a probe under whatever regime the machine
arrived in. Measured by handing the prologue an inherited `satp` of Sv39 over an unwritten table:
the probe takes `mcause=0xd`, a load PAGE fault, and the port refuses with the PMP message on a hart
whose PMP is perfect. The prologue writes `satp` Bare and reads it back, and clears `MPV` and reads
it back, before it measures anything, so the regime under the probe is the one this file
established. On THIS hart the two are already what the probe wanted, `satp` reading `0x0` out of
reset and `MPV` refusing to set at all, so both writes are unfired here and neither is dead code
anywhere else.
*And the fence is the PMP write's, not the `satp` write's.* A hart with page-based virtual memory
may cache PMP verdicts alongside a translation, "including possibly caching the identity mappings
from effective address to physical address used in Bare translation modes and M-mode", so M-mode
must execute `SFENCE.VMA` with `rs1 = rs2 = x0` AFTER writing the PMP CSRs (Privileged ISA 3.7.1);
changing `satp.MODE` needs no fence of its own (Privileged ISA 12.1.11). On this hart the probe's
answer is the SAME with the fence and without it, taken both ways over a revoked grant, so QEMU
synchronises its own PMP writes and the fence is in the code because the architecture requires it,
not because a run here can tell whether it is there.
*The extent.* One address stands for every address only where NO entry can match a sub-range. Entry
0 reading OFF matches nothing, and the LOWEST-NUMBERED matching entry decides (Privileged ISA
3.7.1.3), so a higher entry the platform mandated at reset (section 3.4 permits exactly that) can
permit the one word the probe touches and deny the image, the tables and the console. Measured by
configuring entry 8 as an 8-byte NAPOT grant over the probe word with entry 0 forced OFF: the decode
as it stood ACCEPTS, the port boots, and the first supervisor instruction fetch dies with no console
and no finisher word, which is the failure the guard exists to prevent arriving as a silent hang.
What fixes it is a CENSUS and not a bigger sample. The eight even-numbered `pmpcfg` CSRs hold all 64
entries on RV64 (Privileged ISA 3.7.1), and if every byte in them reads zero then no entry matches
any address, so the one word's answer IS the whole space's answer. **Proving the footprint instead
is the WEAKER answer and not the stronger one**: PMP granularity is four bytes and an entry may name
any NAPOT or TOR sub-range, so any finite set of probed words is still a sample, where the census is
a proof and costs eight CSR reads.
*The extent has a second dimension, and it is the ACCESS TYPE.* `MPRV` covers loads and stores, on
the definition the regime paragraph above cites, so the probe measures those two and instruction
fetch stays out of its reach. On the denial this hart produces that costs nothing: reset leaves
every `A` field off, no entry matches any address, and an implemented PMP then fails every S-mode
access alike, fetch included (Privileged ISA 3.7.1), so one load stands for the fetch at the same
address. Where the two come apart is a hart whose standing entries MATCH and grant read and write
while withholding execute, and there the answer arrives as the first supervisor fetch dying before
the console exists -- the silent hang the census clause above describes.
*The same review found the physical extent, and that one lands on the THIRD shape rather than a new
one.* A PTE's PPN field is 44 bits over a 4 KiB granule in every RV64 mode, which is the
ARCHITECTURE's 56-bit output and not the machine's; RISC-V publishes no identity register reporting
the implemented width, and there is no working register to probe either, because a leaf naming an
output the machine does not implement is not refused when it is written, it ACCESS-FAULTS when it is
walked. So the extent is asked of the BOARD, which is where `arch_mpu_nocache_support` already asks,
and the map editor keeps no width of its own beside the chip's.

*A THIRD REVIEW ON 2026-08-29 DID NOT ADD A FIFTH SHAPE. It corrected the THIRD one, which had been
stated as an equality and is not.* "Write a value and read back what stuck" reads as a compare
against the value written, and on two of this port's own registers that compare is WRONG IN BOTH
DIRECTIONS. A hart may hold a delegation bit read-only ONE for a lower-level interrupt (Privileged
ISA 3.1.8), and this one does: `mideleg` written 0x222 reads back **0x1666**, the hypervisor
extension forcing bits 2, 6, 10 and 12 on, so an exact compare refuses a conforming hart at boot.
And the value written is not the question anyway. What a readback is held against is the set the
PORT DEPENDS ON, derived from the trap entry, the timer path and the doorbell rather than from the
write: `medeleg` bits 2, 3, 8, 12, 13 and 15, and `mideleg` bits 1 and 5. Every other bit is asked
for and not required, and naming one in a refusal would turn a portability guard into a false
refusal on a hart that is fine.
**BUT "THE SET THE PORT DEPENDS ON" IS A PROPERTY OF THE ISA AND NEVER OF THE IMAGE, AND THE FOURTH
REVIEW CAUGHT THIS SECTION GETTING THAT WRONG ON BIT 3.** The bit was excluded because nothing in
the current image executes `EBREAK`, which is a discharge held by CALLER BEHAVIOUR: `EBREAK` is in
the base ISA, so a valid unprivileged application may execute one at any time, and undelegated it
terminates in the machine handler with the whole system rather than killing its thread. Measured
both ways on this hart: with bit 3 delegated a `hello` thread executing `ebreak` prints
`=== THREAD FAULT === thread 'ping' killed, system continues, PC=0x40000052 scause=0x3` and the
system runs on; with bit 3 held read-only zero and the old required set, the guard PASSES, the board
boots, and the same `ebreak` prints the machine-mode trap message with `mcause=0x3` and dies there.
Bit 0 stays out for a reason that IS an ISA property: with the C extension every branch target in
this image is 2-byte aligned and no instruction can name a misaligned one.
*Bits 4 and 6 are the sharp case and they are neither required nor excluded now, they are MEASURED.*
Whether a hart raises them at all is a PMA and therefore per region (Privileged ISA 3.6.2), so no
register answers it and neither blanket answer is right: requiring them refuses the commoner machine
and excluding them leaves an unprivileged misaligned access terminating in machine mode on the one
that does raise them. The prologue performs a misaligned load and a misaligned store on the DRAM
word it has just proved, under a vector that RECORDS the cause and RESUMES rather than refusing, and
adds `1 << mcause` for each one that traps to the set the readback is held against. A hart that
resolves misalignment in hardware never enters that vector and boots exactly as before; a hart that
raises an access fault rather than a misaligned one is held to THAT cause instead. Measured on this
hart: neither access traps, so nothing is added, which is what makes the whole check invisible on
`qemu-riscv64`. What the probe still does not reach is the per-region half of the PMA: it samples
DRAM, which is where an unprivileged thread's misaligned accesses land, and says nothing about a
hart that resolves misalignment there and raises on a device page.
*The delegation failure is also SILENT where the others are loud,* which is why it went unguarded.
Undelegated, `sip` and `sie` hold the matching bits read-only zero (Privileged ISA 12.1.3): the
kernel's `csrs sip, SSIP` stores nothing and its `csrw sie` writes zeros, so the doorbell never
rings and the deadline never fires. Measured with `mideleg` forced to zero, the board prints its
banner and hangs. A CSR that traps announces itself; a CSR that silently narrows does not, so the
readback is the only instrument there is.
*And `mtvec` is a WARL register the third shape had not been pointed at.* It must be implemented but
MAY HOLD A READ-ONLY VALUE, and the values a writable one accepts vary by implementation
(Privileged ISA 3.1.7). Measured here: a write of `.Lmtrap + 2`, a reserved MODE, is DISCARDED whole
and the register keeps what it had, so a vector that did not take is a live vector written for a
different trap. `stvec` is deliberately not read back and the difference is the specification's: its
BASE holds any 4-byte-aligned address, where `mtvec`'s carries no such guarantee.
*The one thing the review got wrong is worth keeping, because it is the intuition anyone re-deriving
this will have.* It read the MPRV probe as able to recurse: a fault under `MPRV` reaching a handler
that never clears it, whose own console stores are then checked as supervisor accesses. That cannot
happen. A trap sets `MPP` to the mode that trapped, so a fault from machine mode leaves `MPP=M`, and
`MPRV` is defined as "as though the current privilege mode were set to MPP" (Privileged ISA 3.1.6.1,
3.1.6.4): it is inert. Measured on the coerced-vector case, the generic handler prints. The clear
stays, in ONE place on the shared fatal path rather than at each entry, so the console is reachable
without resting on that reading.

**The x86_64 spike returned the verdict this section exists to collect, and it is an empty diff.**
Nineteen calls held against a booting image, nine on the entry path and ten on the aspace family,
with NO signature change on any of them, including the two the roadmap picked this architecture to
falsify: a `SYSCALL` entry that loads no stack pointer reaches a C handler on a per-CPU kernel stack
in three instructions, and a single root register keeps the kernel half in step at the cost of two
stores per create. Three findings landed as BODIES rather than signatures, which is exactly the
shape a good seam produces: the parked caller's stack pointer belongs in the frame and not in the
per-CPU slot, an incoming kernel stack must be published to TWO places where A64 needs one, and the
trap path owes a conditional register-base swap the syscall path gets for free. And `acquire`
collapsing to a single addition there is the argument FOR the seam rather than against it: written
as an addition above the seam it would have passed on both 64-bit architectures and then forced a
transient-window rewrite in kernel code on Sv39.

Two negative results worth keeping. `ARCH_ASPACE_ECAPACITY` is unproducible on x86_64 as it is on
RV64, all three M6 backends being radix, so the refusal is untestable rather than untested and stays
for the first hashed-table backend. And the identifier freeze gets hardware confirmation rather than
argument: the machine this was measured on exposes the identifier-invalidation instruction and NOT
the identifier feature itself, so an address-space tag is not merely narrower on this architecture,
it may be absent on a current part.
*X5 CORRECTED THAT MEASUREMENT AND THE CONCLUSION SURVIVES IT.* Re-measured by MACHINE rather than
by probe, one image under five emulated processor models including the widest one the emulator
offers, and NEITHER feature is reported on any of them: the identifier is absent and so is the
instruction that invalidates by it. So the sentence above overstated what was there by one feature,
and the freeze is on firmer ground than it claimed rather than weaker, a backend that has neither
being the case an identifier allocator would have had nowhere to run at all.

**AND THE RV64 VERDICT IS NOT AN EMPTY DIFF. The seam owes a member, and the litmus is what found
it.** Measured at R2.1 by building the backend rather than by reading the header: THREE call sites
above the seam require `acquire` to have the properties of an OFFSET MAP rather than of a window.
The sharpest is not the one that fails, it is the one that lies. `aspace_frame_token` NAMES a frame
by dividing an acquired pointer's distance from a reference by the granule, which is a stable
identity when acquire is an addition and, on a six-slot pool, answers the same small number for every
frame in the system. Fourteen cross-task identity arms would have read "same frame" for different
frames and reported success. The other two fail loudly by comparison: one requires the answer to
equal the frame pool's own pointer for a physical address, the other required consecutive answers to
be consecutive.
What the seam needs is a query that answers a PHYSICAL ADDRESS for a mapped virtual one, zero for an
unmapped one, and spends no window:

    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va);

*Established by construction and not by argument:* forcing acquire to window every frame, rather than
using the kernel window's offset where it applies, turns the translation arm red and panics the
image. So the three callers are not merely inconvenienced, they are wrong, and the reason they are
wrong is invisible on any backend where acquire is an addition.
**This is the sentence above, arriving as a measurement.** This freeze already said that acquire
written as an addition above the seam "would have passed on both 64-bit architectures and then forced
a transient-window rewrite in kernel code on Sv39". It did not force a rewrite; it forced a MEMBER,
which is the cheaper of the two and the reason the seam is held against a litmus at all.
*A second, smaller gap, and it is a shape rather than a signature:* `arch_aspace_memtype_support`
has no "the hardware is already in this state" answer, where the region seam's nocache query has
exactly that. It is this port's own answer: Sv39 carries no memory-type field, and Svpbmt is absent,
measured by the machine reading its own enable bit back as zero. So this backend honours all three
types because it ENCODES none of them, which is a different claim from honouring them, and the seam
cannot currently tell those apart.

**THE DIFF LANDS BEFORE R5 AND R5 ONLY COLLECTS IT.** R2.2's arms are among the three callers, so the
member has to exist for that step to have a witness. The verdict step reports the diff; it does not
originate it. **And the baseline does NOT move:** `tests/static/check_aspace_sigdiff.sh` measures
against a frozen commit precisely so this shows up, so its DIFF exit is the verdict rather than a
regression to be tidied away. Updating the baseline to make it quiet would delete the milestone's
result.

**THE THIRD BACKEND FITTED THE FAMILY UNCHANGED, which is what this freeze was hoping for.** X5 put
the family on a single root over an adopted regime with no signature change of its own: the diff
against the frozen baseline is still the ONE member RISC-V forced, and the seam took nothing from
x86_64. So the family now stands on three architectures that agree on almost nothing else, which is
the property this freeze asked for rather than the guess it warned about.
*And it closed the frame query's case from the far side.* Acquire is an addition there too, the
adopted regime identity-mapping the run the frame pool is carved from, so the
subtract-two-acquire-pointers shortcut WORKS on x86_64 and on A64 and fails silently only on Sv39.
Two backends of three make the antipattern look correct, which is a stronger argument for the member
than the one backend that broke.
*Three seam comments turned out to be right for the wrong reason, and all three are now measured
rather than asserted.* The fresh-map invalidate was justified by "architectures caching negative
translations": only RISC-V permits that (Privileged 12.2.1), A64 never caches a faulting entry (DDI
0487 M.b D8.17 RXCLRD) and x86_64 says invalid-to-valid needs no invalidation (SDM Vol 3 section
5.10.4.3). The obligation survives on all three by the CONDITION those exemptions carry, that every
earlier clearing of the same slot was invalidated, which is a property of the slot's history and not
of the call. Break-before-make is likewise unconditional on A64 and conditional on x86_64, where the
architecture demands the sequence only when a write changes the page SIZE. And F8's own identifier
note was wrong: re-measured across five CPU models, x86_64 reports NEITHER the identifier feature nor
its invalidation instruction, where this freeze had recorded the instruction as present. Each
conclusion survives; each reason was a guess that happened to hold.

A freeze that survives two unrelated architectures is a property of the problem; one that holds on a
single architecture is an untested guess.

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
mutable globals rather than root's. Two properties now hold together. While root lives the source is
root, unchanged, so a global root writes before a spawn is one the child reads out of its own copy
-- the app's own drivers depend on exactly that. On root's way out, `aspace_release` freezes those
pages into a snapshot of frames taken off the pool when root was seeded, with root's mappings still
standing, and every later process copies the snapshot. A seed reaching a lost home with no snapshot
behind it is REFUSED. No process other than root is ever the template. Witnessed by
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

## 5. The plan: steps and expected results

**BEFORE ADDING A PRESENT-TENSE SENTENCE ABOUT THE TREE TO THIS DOCUMENT, READ THIS.** Section 1
states the rule for freezes; it is the same rule and this is the general form of it. A description of
what the code does today is indistinguishable, one paragraph later, from a decision -- and a step then
reasons FROM it. One audit corrected TEN such sentences at once, across the freezes, the rewrite
descriptions and the step text alike, and not one of them was ever chosen by anybody: each was a
measurement that outlived the tree it measured.

**Ten is a FLOOR, and the audit did not retire the habit: four more were corrected in the steps that
landed after it**, the last at T8b, whose own *Expected* line named an unwind arm its injector cannot
reach. Each of the four was found by RUNNING the obligation rather than by re-reading the document, so
an audit is not what closes this class -- a false present-tense sentence reads correct until a step
tries to discharge it. And the cost is not editorial: T5b is a whole step the contract did not have,
found by trying to run T6; T5b's own ordering was then wrong for the same reason and re-ordered
everything after it; and three T6 attempts stopped short before the step was split.

So a sentence about the tree either carries a STEP STAMP (`landed at T5`, `still true at T5b.1`) or it
cites the decision behind it. If it can do neither, it is an observation, and **an observation may not
be a premise**: nothing downstream may derive an obligation, a witness or a refusal from it. When one
turns out to be wrong, say which kind it was -- a decision being revised, or a premise being removed
because nobody made it -- because a reader who cannot tell will go looking for reasoning that was
never there.

Every step carries an identifier so a finding can name one. A step is DONE when its expected result
is observable, and where the only honest result is "it builds" that is what the step says rather
than something dressed up. Steps are ordered by dependency and not by size.

### M6.1 -- the A53 port, no translation

**S1. Toolchain, arch, chip, board, preset.** A toolchain file with its own environment variable, an
arch stanza, a chip stanza that decides no knob, a board and a preset. The backend is declarations
with empty bodies.

**The arch stanza declares NO capability it has not implemented, and this is a rule rather than a
preference.** An earlier version of this step had it select the mandatory-kernel-stacks capability
up front, on the strength of section 2. That is wrong, and the tree proves it mechanically: that
select promises `arch_ctx_redirect` relocates the death path onto the thread's kernel block, and
`death_stack_seating` refuses an arch that claims it without doing it. A capability select is a
promise the gates hold the backend to, so each one lands with the step that keeps it -- kernel stacks
at S7, the thread pointer at S8. Silencing such a gate with an empty conditional arm would be
gaming it, and the gate would then be worth nothing on the next arch.
*Expected:* the preset CONFIGURES, the arch and family strings resolve to the new directories, and
the agreement check between the Kconfig capability and the arch ladder passes. It does not link, and
nothing before S2 should try to.

**S2. Boot to one byte.** Reset entry, initial stack, zeroed BSS, and a direct write to the UART data
register.
*Expected:* a known string on stdout under the emulator, from an image with no kernel in it yet. This
step also DETERMINES which exception level the machine hands us and whether a drop is needed -- record
the answer in the record rather than in a comment, because S7 depends on it. **And it confirms the
granule support the emulator's core reports against what F7 read from the silicon manuals**: a
divergence there is a fact about the model worth recording, not a reason to reopen the freeze.
*That confirmation was taken at M6.2's CLOSE and not here*, nothing in the tree having read
`ID_AA64MMFR0_EL1` until then; the figures and the seam member that reads them are recorded at F7
and T2. The model agrees with the manuals on all three figures, so S2's clause cost nothing by
being late -- which is luck rather than a reason to leave the next such clause unrun.

**S2b. The 1:1 map, and it is not optional.** The identity translation table, Normal memory for
DRAM and Device for the MMIO window, `MAIR_EL1`/`TCR_EL1`/`TTBR0_EL1`, then `SCTLR_EL1.M` with the
caches. **This step was missing from the plan and the hardware supplied it.** While `SCTLR_EL1.M` is
0 every data access is Device-nGnRnE, and Device memory mandates natural alignment whatever
`SCTLR_EL1.A` says; AArch64 GCC stages ordinary array and struct copies through unaligned `ldur` and
`stur` of `q` registers, and the prebuilt newlib does the same inside `memcpy`. So this arch cannot
run compiled C at all with translation off, and the earlier reading -- that the 1:1 map was M6.1's
name for the flat model rather than a thing to build -- was wrong. It is a link-time constant table
that nothing edits: no allocator, no `arch_aspace_*`, nothing per-process, so it takes nothing from
M6.2.
*Expected:* a boot that reaches compiled C and returns from it. **The false pass to watch for is the
opposite of S3's:** with no vectors installed yet the alignment fault goes to a `VBAR_EL1` nobody
set, so the image dies SILENTLY. Silence here is the symptom, and a hand-written assembly probe that
only ever touches aligned addresses prints happily right through the bug -- which is how this step
came to be missing.

**S3. Exception vectors and the fault report.** The vector table, synchronous exception decode, and
the syndrome wired into the existing fault path.
*Expected:* a deliberate wild access prints the report with the syndrome and the faulting address,
then halts. A fault that prints NOTHING means the vector base is wrong; it does not mean the access
succeeded, and the two are easy to confuse at this stage.

**S4. The console seam and the banner.** The console write calls behind the seam.
*Expected:* the full kernel banner, which also proves the version and build-stamp plumbing reached a
new board with no per-board work.

**Almost none of this step is code, and the exception is the interesting part.** The banner needs
nothing per-board and came up complete on the first try. Check it against another board rather than
reading it and nodding: the fields have to DIFFER (`mpu off` against `enforce`, this board's heap
figure against that one's), because a field that is merely plausible is what a stuck default looks
like.

Of the console seams a polled non-buffered UART leaves to their fallbacks, `write_sync` is right
(the fallback aliases the polled writer, which is what this chip's `arch_console_write` is), and
`tx_backend`, `reclaim`, `reclaim_window` and `retune` are right because they are about a userspace
driver owning the device or a clock that moves, both of which belong to other eras. **`flush_sync`
is NOT, and the reason generalises past this port.** Taking that fallback is an ASSERTION that the
chip's console cannot outrun `arch_shutdown`. It holds on `mps2` and `virt_rv32` because their
console is semihosting, synchronous by construction. A PL011 has a FIFO and a shift register that
outlive the core, and the writer polls FIFO-not-full rather than device-idle, so the assertion is
false about the DEVICE -- and true on this target only because QEMU's model hands each byte straight
to its chardev. So the body goes in and says out loud that the emulator cannot witness it, which is
the only reason such a body ever reads as dead code. The general lesson for the two backends still
to come: a fallback whose comment describes a property of the chip has to be checked against the
chip, not against whether the tests pass.

**S2 THROUGH S5 CANNOT PRODUCE A LINKED IMAGE AT ALL, which this plan did not anticipate.** The
blocker is not the kernel: it is `user/src/syscall_stubs.cc` referencing `arch_syscall` and
`arch_syscall64`, and every app in the tree calling some `kos_*`. So those two symbols gate the link
for the whole stage, and they belong to the syscall trap at S7. Each of these steps therefore takes
its witness from an ad-hoc link that supplies what the step is not about, and **S6 is the first step
whose image is the real one**. Two consequences worth stating: a step here is done when its expected
result is observable that way, not when `kickos_build` passes, which it cannot yet; and S9's re-take
is what turns each of these witnesses into a registered one.

**S5. Interrupt controller, timer, clock.** The GIC behind the existing mask/unmask/clear triad, the
generic timer as the tickless one-shot, and the monotonic clock. **The interrupt identity is keyed
`(line, kind)` and not by a flat index**, which `roadmap.md` requires: a private-peripheral interrupt
number denotes a DIFFERENT interrupt on each core,
so a flat integer key is ambiguous the moment M7 adds one, and the timer this step arms is exactly
such an interrupt.
*Expected:* a timed sleep returns within tolerance of its deadline, and the clock never goes backwards
across an arm and disarm. Test "the timer fired" and "the interrupt was unmasked" separately: from
above, a timer that never fires and a line left masked are the same silence.

**WHAT `(line, kind)` TURNS OUT TO MEAN, because the kernel's key is one `int` and stays one.**
`irq_table` is a flat array, `kickos_isr_irq` takes a single integer, and every freeze in section 1
says nothing above the arch seam moves -- so a tuple key was never available to this stage, and
demanding one would have been a five-backend change smuggled into a port. The tree already answers
this question a different way, and RXv3 is the worked example: **the kind is a disjoint VALUE RANGE
within the one integer, and the arch seam branches on it.** RX puts group sources at
`GROUP_LINE_BASE + group*STRIDE + bit` above its vector space and tests the range in
`arch_irq_mask`/`unmask`/`clear_pending`.

A GIC INTID is already such an encoding, which is the substantive difference from the flat NVIC index
`roadmap.md` contrasts it with. Below 32 an interrupt is BANKED -- the distributor gives each core its
own copy of those registers -- so INTID 30 names *this* core's timer and cannot be masked from
another. At or above 32 it is global and additionally needs a target core named in `ITARGETSR` before
it is delivered anywhere at all. One boundary, tested in every `arch_irq_*` body and nowhere else.

**And the residual is worth naming rather than leaving to be rediscovered at M7.** What the range
encoding does NOT fix is that the kernel's table maps one INTID to one handler, so two cores sharing
a banked INTID would share a handler. That is invisible today for the reason the step's own text
gives -- the timer is the only banked interrupt in use, and the timer is not in the table at all,
`kickos_isr_timer` taking no line. So the M7 change is a per-core dispatch table for INTIDs below the
boundary, not a re-keying of the interrupt identity, and the boundary this step introduces is what
that change will hang off.

**S6. Context switch and idle.** Context init, switch, start, and the idle wait.
*Expected:* two kernel threads round-robin in a deterministic order, measured by the interleave arm
the suite already has rather than by a new one.

**S7. The privilege split and the syscall trap.** EL0 for unprivileged threads, the trap to EL1, and
dispatch on the kernel stack the exception-level split provides (section 2). **This is where the arch
selects the kernel-stack capabilities** -- both of them, this arch writing only the transfer and no
red-zone path -- because this is the step that earns them.
*Expected:* an unprivileged thread makes a BLOCKING syscall and resumes, and its kernel block's canary
is intact afterwards. This is where M5.2.1's central claim is inherited rather than rebuilt, so it is
also where to check that it was actually inherited.

**S8. Thread-local storage.** The thread pointer written at switch-in, seated from the thread's own
block rather than derived by masking the stack pointer, per F7. **This is where the arch selects the
TLS capability.**
*Expected:* `user/apps/common/tlsprobe` reads its own `thread_local` back, as it does on the three
arches that witness it today.

**A STACK FIGURE THIS STAGE INHERITS WITHOUT EARNING IT.** `KICKOS_MIN_STACK_SIZE` is declared
per arch in `Kconfig` with the help text calling each value the arch's deepest thread-exit dispatch,
*measured*. There is no `ARCH_ARMV8A` arm, so this port silently takes the generic 1024. That is not
obviously wrong, but it is not measured either, and this arch has reason to be hungrier than the
32-bit ones: its uniform switch frame is 336 bytes against armv7m's ~100, and S6 found the board's
inherited 512-byte idle stack faulting for exactly that reason once idle was interrupted. So the
figure is owed a measurement on the same `-fcallgraph-info` footing as the rest of the fleet, and
whoever takes it should expect it to come out above the default rather than at it.

**S9. Register the preset and take the witness.** The board joins the ctest ladder and
`user/apps/common/selftest` runs.
*Expected:* a complete TAP plan, zero failures, validated through
`tests/integration/check_tap_stream.sh` -- and the enforcement arms SKIP rather than passing
vacuously. Read the skip list and keep it: a long list is correct here, a short one means an arm is
lying, and T8 is scored against this list.

**THE SKIP LIST, KEPT, and it is EMPTY.** S9 says to read it and warns that a short list means
an arm is lying, so it was checked against another board rather than accepted: `qemu-flat` on
armv7m, also non-enforcing, also reports zero skips. On a flat board the
enforcement arms are NOT REGISTERED rather than registered-and-skipped, so there is no list to keep
and nothing is passing vacuously. What T8 is therefore scored against is the facts below it,
not a skip list:
- **one PARTIAL**, `periph_reg_write_unheld`, the sole member of this board's expected-partials set
  (`user/apps/common/selftest/CMakeLists.txt` appends it on the `armv8a` arm and
  `tests/integration/check_tap_stream.sh` matches it by NAME): this chip names no MMIO window a
  driver could be granted, so the arm's free-window half finds nothing to take. A real coverage gap,
  and T8 closing it means minting a window.
- **the arms that do not register at all -- and the list written here was WRONG, in the direction
  that flatters the board.** It named `bus_device_slots`, `uart_service`, `reboot_priv` and
  `call_reg_fastpath`. All four gate on `KICKOS_ENABLE_SELFTEST` alone, this board's defconfig sets
  it, and all four register. Nobody decided those four were absent; the names were read off another
  posture and written in the present tense. The five genuinely absent are `endpoint_bound`,
  `stackbase_arena`, `grant_reserved` and `dev_window_exclusive`, gated on `KICKOS_HAVE_MPU` which is
  0 here, and `periph_reg_write_mask`, gated on `KICKOS_ARCH_SIM` which is 0 too. All five are
  enforcement or simulator arms, which is a different -- and much better -- reading of this board's
  coverage than four missing platform features.
- **and the registered arm COUNT is 112 here, not 101.** 101 is `qemu-flat`'s, and the two are not
  one figure: this board additionally registers `caller_stack_arena` and the ten
  `KICKOS_HAVE_ASPACE` arms. Derive it from `user/apps/common/selftest/CMakeLists.txt`, which
  computes it and hands it to the ctest registration; do not quote either number.

A third fact belongs with them because it was nearly a silent hole. The eleven `irq_*` arms are the
only exercise the GIC gets from the suite at all, and they hung until the board pointed
`KICKOS_SELFTEST_IRQ_BASE` away from its default of 6: on a GIC everything below 32 is an SGI or a
PPI, whose pending state `GICD_ISPENDR` does not set, so an inject took no effect and the arm waited
forever. A default that is merely inconvenient elsewhere is unusable here.

**Stage result:** a registered emulator preset with a TAP witness, under the existing flat `Kconfig`
arm and with no allocator work anywhere in the diff. Note what "flat" now means on this arch and does
not mean elsewhere: translation is ON from S2b, and flat describes the MAP being a constant identity
one rather than there being no map. If a step above needed the frame allocator, the stage
boundary moved and that is a finding about this plan.

### M6.2 -- translation, processes, enforcement

**T1. The frame allocator.** 4 KiB frames over the arena (F7).
*Expected:* host unit arms for allocation, release, exhaustion and double release. This is the single
largest new subsystem and it needs no hardware, so it is tested in the host unit layer and does not
wait for a board. Note that layer is OFF unless CMake is pointed at a GTest, so confirm the arm count
ROSE rather than that the suite passed.

**T2. The parallel arch family.** Declared in the arch seam BESIDE the region calls and never
replacing them (F6). What it freezes is section 3.4b's list, which is longer than the verbs this line
used to name: the opaque address-space type; create, destroy, map, unmap and activate; the page
acquire and release pair of section 3.3; the granule query F7 requires; the error and transaction
semantics of each, with a capacity refusal DISTINCT from frame exhaustion; and the context-association
path. **There is no flush and no address-space identifier.** Until 2026-08-25 this line said
`flush`, against both section 3.4b, which calls removing it the most important thing in that section,
and `roadmap.md`, which already described the family as coherence-complete with no flush call.
*Expected:* the seam still compiles on every existing arch with no MPU backend touched, and the host
unit count does not MOVE. A rise there would mean behaviour landed at T2 that belongs at T4. This is
the cheapest possible falsifier for F6 and it is available before any of it works.
*Decided here rather than discovered later,* section 3.4b having required that it be: activation is
driven from the incoming thread's TASK and a context stays ignorant of its space, so
`arch_context_init` keeps the four parameters it has. The alternative, a root carried in the context,
is a second copy of what the domain already owns and would need keeping in step with it.
*Four corrections the RV64 spike forced back into this freeze,* which is the F8 litmus doing its job
one milestone early rather than at M6.3:
  - **`map` takes a physical address that is NOT `uintptr_t`.** The first cut typed `pa` as a pointer
    width, which F1's own Sv32 citation (32 bits of virtual against 34 of physical) makes
    unrepresentable on a mainstream 32-bit target. It is a dedicated `arch_phys_addr_t`, free on all
    three M6 backends and an ABI break if it had waited.
  - **There is no zero-rights mapping.** A leaf with R, W and X all clear is how RISC-V spells "this
    entry points at the next level", so the encoding is spoken for. A guard page is the ABSENCE of a
    mapping, and the `ARCH_MAP_NONE` the first cut carried over from the region seam was an
    invitation to write the one call that cannot work.
  - **The acquire pair owes a simultaneous-depth floor, and it is at least two.** `ep_copy` holds a
    source page in one space and a destination page in another at the same time. On a backend where
    acquire is an addition the depth is unbounded and the requirement invisible, so a windowed
    backend would have discovered it as a corruption rather than a refusal.
    **"At least two" was a COMMENT and nothing else until M6.2's close, and the tree needed more
    than two.** It is now `ARCH_ASPACE_ACQUIRE_MIN` in the seam header, asserted by the backend
    against a capacity of its own and stated in `docs/reference/porting.md`; a windowed port sizes
    its pool for the figure at compile time rather than meeting it as corruption. The figure is
    SIX, measured and not chosen: the page-split scenario holds four pages across two spaces while
    `ep_copy` acquires one end in each. It counts OUTSTANDING CALLS rather than distinct pages,
    those six covering four pages. And the measurement made a caller's own defect visible --
    `KOS_ASPACE_OP_SPAN` walked 600 pages holding every one of them, which no seam figure should
    have to cover, so it now releases each before taking the next.
  - **`ARCH_ASPACE_ECAPACITY` is unreachable on all three M6 backends,** every one of them being
    radix. It stays, because the caller shape it forces is the reason to distinguish it and a
    hashed-table backend cannot be added later without it, but it is untestable rather than untested
    and no coverage gate may demand an arm for it.

*The freeze gained a SECOND member at M6.2's close, on the same terms:* `arch_aspace_model`, which
answers with what the IMPLEMENTATION reports about its own translation -- the granules it supports,
its identifier width and its physical range -- so F7's manual figures can be checked against the
machine rather than against constants of the port's. Selftest-gated and shaped like
`arch_mpu_probe_addr`, exactly as the member below it.

*The freeze gained one member at T4, and it is recorded here rather than left to a reader to notice:*
`arch_aspace_boot`, which answers with the space the kernel booted under. Nothing else can put the
boot translation back, so without it no scenario can activate a space of its own and survive. It is
selftest-gated and shaped like `arch_mpu_probe_addr`, which is the same kind of member on the region
seam, and it becomes ordinary rather than test-only at the step where the idle thread needs a space.

*A constraint on whichever step lands the body, rather than on this one:* ACTIVATE is not just a
register write. Changing the table base and the ASID together has an architecturally documented
ordering (DDI 0487 M.b Example D8-1, whose shape depends on `TCR_EL1.A1`), so that backend follows
the documented sequence rather than inventing one.

**T3. The kernel into the high half, with the physical map.** Kernel link addresses move to the top of
the space and all physical RAM is mapped there (F1).
*Expected:* every S-step witness still passes, with the kernel executing from high-half addresses --
the banner identical, the link map not. Skipping this and leaving the kernel identity-mapped will pass
T4 and fail T6 in a way that reads as a copy bug.

**T4. Per-address-space virtual bookkeeping and the map editor.**
*Expected:* map a page, write it, read it back, unmap it, and FAULT on the next access. Four
transitions, and the fourth is the one usually missing from a first implementation.
*Where each lands, because the fourth cannot be a TAP arm:* the first three are reached through the
acquire pair and stay inside the selftest, which has to survive them. The fourth is an ordinary load
taken by the CPU under the RUNNING translation, so it ends the image and lives in one of its own
(`user/apps/common/aspacefault`, gated by `tests/integration/check_aspace_fault.sh`), which compares
the address the kernel announced against the dump's `FAR` and pins the syndrome to a level-3
translation fault. An arm reached through the acquire pair cannot see the TLB at all, which is
exactly what the fourth transition is about.
*And one requirement T4 satisfies WITHOUT witnessing, which is worth more than a silent pass:* a map
into a slot that was empty owes an invalidate too, architectures caching negative translations.
Removing that invalidate changes nothing on this emulator, so the arms cannot tell whether it is
there. It is written because the architecture requires it, not because a run showed it, and the
first target that caches a negative translation is where it stops being unwitnessed.

**T5. The domain backend field, the dedup deleted, and possession made explicit.** A backend field
is added BESIDE the region array rather than behind the accessors -- see 3.1, which used to claim
otherwise -- the reuse arm in `domain_for` goes and the default-user singleton becomes a template
(F2), and possession stops being a walk over the caller's reachable regions and becomes thread-local
authority (F9). That last one is a PRECONDITION of the
rest of this step, not a follow-up: widening the mapping first would silently widen possession with
it.
*Expected:* the entire existing fleet green with no per-board change -- the regression gate of section
4 -- and the two board-registered device-window arms still refusing a non-holder on every backend,
which is the arm pair that fails if possession is still derived from reachability. On the new board,
two tasks granting the same block hold two address spaces, AND two tasks granting nothing at all hold
two address spaces, which is the case the same-grant witness misses.
*What T5 discharges of F6's self-grant clause, and what it does NOT:* F6 asks that the self-grant be
either a real map or refused by name. At T5 it is the refusal. A reservation is not page-granular
until F10's allocator lands and the task's space is not activated until T6, so mapping a small
allocation page-granularly there would over-grant its neighbours. What T5 buys is that admission is
restored, the escalation F6 names being refused by name, and that the encoder stops asking a
descriptor question of a backend that seats none. Every outcome is a map-to-be or a named refusal and
none is a silent claim. **T6.2 owns F10's allocator and is where the refusal becomes a map**, and T8
owns the hostile witness.

*Also owed here:* the driver-service bring-up flow CONFIGURES and runs, and the `domain_share` arm is
re-described per F2. **What it was re-described AS is T6.2's answer and not this line's**: the
two-handoff witness became `task_handoff_readback`, an arm of its own that shares no app global,
and `domain_share` itself was repaired to three siblings of root. This line asked for one arm to
become the other, and the per-process data copy is what made that impossible. **The same-address READBACK is not owed
here and must not be claimed here** -- the child is a user thread and its image is not mapped until
T6, so at T5 a leftover identity map makes "the same address" vacuously true and proves nothing about
F10. T5 witnesses two spaces and possession; the executable readback is T6's.

**T6 ALSO INHERITS A LATENT ESCALATION, found by the M6.1 audit and correctly timed here.**
`arch_context_init` fabricates a thread's first frame, ELR and SPSR included, on the thread's
own stack. That is safe while a stack is private, and F9 makes thread stacks task-wide
mappings: a sibling could then rewrite a not-yet-run thread's SPSR to EL1h and choose its
entry PC. So T6 either builds the privileged return state on the kernel block or overwrites
SPSR and ELR from trusted context immediately before the first `eret`, and it owes a hostile
arm where a sibling corrupts a parked frame. Fixing it at M6.1 would have been fixing an
exposure that does not exist yet; shipping T6 without it would be creating one.

**T7 OWES A LATENCY MEASUREMENT BEFORE ITS IPC COPIES.** This arch's exception frame is 800
bytes and every syscall saves and restores all of it with PSTATE.I set, so a round trip moves
about 1.6 KiB with interrupts masked throughout. Whether a synchronous SVC deserves a compact
ABI frame while asynchronous entries keep the full one is a decision, and it needs the number
first rather than an argument. Measure it beside M8's instrument rather than inventing a
second one.

**IT WAS NOT TAKEN, AND IT IS A DEBT RATHER THAN AN OVERSIGHT.** M6.2 closed with no aarch64
round-trip figure anywhere. The reason is the rig and not the step: the instrument is
`KICKOS_BENCH`, `boards/qemu-arm64/configs/` holds a `base` variant alone, and
`tools/bench/bench-fleet.sh` does not list the board -- so there is nothing to run and the
compact-frame decision has nothing to rest on. Standing up a bench variant for this arch is a
step of its own and belongs with M8's instrument, where the comparison it feeds lives; it is
carried in `TODO.md` so it is a known debt rather than a forgotten one. **No compact-frame
decision may be argued before the number exists**, which is what this paragraph froze.

**T5b. The app leaves the kernel's half.** Found while starting T6, and it is a MISSING STEP rather
than an implementation problem, so it is written here rather than worked around. The app links into
the kernel's high half, and the high half GRANTS EL0: both translation roots share one level-2 table
whose code block is read-only at EL0 and whose data blocks are read-write at EL0. Retiring the
identity map therefore removes nothing, and a per-process root cannot map an image living at an
address the low half cannot name. **A process is not expressible until the app links LOW and the
kernel's half grants EL0 nothing.**

Three consequences, and the second is what makes this a step rather than a linker edit:
  - Moving app static data low drags app TEXT low with it. High-half text cannot name a low-half
    symbol under the small code model, measured as a truncated `ADR_PREL_PG_HI21` relocation rather
    than reasoned about. So the split covers app text and rodata as well as data and bss, expressed
    as the archive-selector inversion `mk64f.ld` already uses for the app-data window.
  - Every EL0-reachable leaf carries privileged-execute-never, so the kernel may no longer CALL app
    text. It calls four such functions today: three are the libc reentrant-state seam, one of them on
    every switch, and the fourth pair is reached from `kmain`.
  - **The seam therefore INVERTS.** Its three calls become kernel-side code driven by a base, a
    stride and one pointer address registered at boot. `reent.h` argues for a call because the kernel
    cannot name the state's type and a typed store would violate strict aliasing, with an LTO folding
    hazard on one backend; a `memcpy` answers both without naming the type. The seat runs AFTER the
    target space is activated, so it is an ordinary store into the active space and needs none of
    T7's owner-carrying access, and a child's initialisation moves to its first switch-in for the
    same reason.

*Expected:* the fleet green with no per-board change, and on the new board a user thread executing
text its OWN root maps while the kernel's half grants EL0 nothing. That second half is what makes
T6's witnesses mean anything, so it is CHECKED here rather than assumed there.

**AND T5b's OWN ORDERING IS WRONG, found by running the revocation rather than reasoning about it.**
Two blockers, both measured, and together they re-order the rest of this milestone.

**The kernel's half holds everything EL0 dereferences, not just the image.** Revoking EL0 there faults
immediately: with the whole half EL1-only, an instruction abort at `root_entry`'s high address; with
the code block re-granted and only data revoked, an EL0 WRITE abort whose fault address is root's
stack. Thread stacks and TLS blocks come from `arch_ram_alloc` over an arena linked high, the newlib
heap is a carve in the same high region, and so is the TLS template. So the revocation cannot precede
the step that moves those low, which is T6's stack mapping plus work on TLS and the heap that no step
currently owns. It drags a third thing in: once EL0 passes a low pointer into a syscall, the admission
bounds must name low addresses, and high-half text cannot name a low absolute symbol, which is T7's
access seam arriving early.

**And one runtime archive is called from BOTH sides, which a linker split cannot express.**
`memcpy`, `memset` and `strlen` are undefined in the kernel-side archives AND in the app's, and the
map shows the member pulled by the app rather than by the kernel. A global symbol has one value:
kernel-side, and EL0's call is a high address it cannot reach; app-side, and the kernel calls text
that carries privileged-execute-never. Neither side can opt out, the compiler emitting all three for
aggregate copies and 120 `libc.a` members referencing them. Two copies in one static link need
distinct names, which compiler-emitted names cannot have.

**So this step SPLITS and the revocation moves last:**
  - **T5b.1** decides who owns the shared runtime and lands the app's low link regions. The ownership
    question has no cheap answer: a partial link plus symbol localisation breaks the archive selectors
    every board's script uses; a shared low text region the kernel may call contradicts
    privileged-execute-never on EL0-reachable leaves; a kernel-half code block granted EL0
    read-execute contradicts granting EL0 nothing. It wants a decision, not an implementation.
    **The ownership half LANDED, and the link regions were left to their own pass**, so that the
    fleet's symbol ownership moved on a tree that otherwise works.
    *The decision:* the KERNEL takes private names and the app keeps the ordinary ones. A shared low
    region would make the kernel's ability to execute depend on the ACTIVE user root mapping it, and
    one process root without it is an instruction abort inside a syscall; kernel text in the high half
    is mapped in every space through TTBR1 by construction. The app is the side that cannot be
    renamed anyway, `libc.a` referencing the ordinary names from its own members.
    *The mechanism, and why a source rule was not enough:* the kernel links its own copies of
    `lib/libc/string.cc` and `lib/libc/fmt.cc` under `kmemcpy`/`kmemset`/`kstrlen`/`kfmt_vsnprintf`
    (`kernel/klib/`, declared in `kickos/kruntime.h`), and its explicit calls name those. But only six
    of thirteen kernel-side references were explicit calls. The rest the COMPILER emitted with no call
    in the `.cc` at all, and the population is a property of the toolchain rather than of the source:
    one on aarch64, three on rv32imac, four on armv7m, five on armv6m, none on rxv3. Two of them come
    from `ThreadAttr attr;`, the plain default construction of a struct with default member
    initialisers, whose zeroing lowers to a `memset` libcall at `-Os` that NO flag suppresses
    (`-fno-builtin`, `-fno-builtin-memset`, `-fno-tree-loop-distribute-patterns` and `-O2` were each
    measured and each still emits it). Since a type's defaults are its contract, there is no de-typed
    spelling to write instead, so those are rewritten after `ar` by `objcopy --redefine-syms`
    (`kickos_privatise_runtime`, map in `cmake/kernel_runtime.syms`) over the three archives that hold
    kernel text. `tests/static/check_kernel_runtime.sh` then reads the archive the linker will read
    and refuses an ordinary name in it, which is what catches an archive the rewrite never reached.
    *And it is scoped to `KICKOS_HAVE_ASPACE`, measured rather than chosen:* a region backend serves
    both privilege levels from ONE text mapping, so a second copy there protects nothing, and it does
    not fit. The duplicate costs 624 bytes of text on armv7m (456 of them the formatter) against 368
    bytes of headroom on `bluepill-c8-st` and 360 on `f302nucleo-st`, so a fleet-wide copy overflows
    those two 64 KB parts by 256 and 264 bytes. On a region backend the header aliases the app's names
    instead and every kernel call site reads the same, which also leaves the trap-stack depths those
    boards are measured against untouched.

    **THE LINK-REGIONS HALF LANDED TOO, and the accounting that preceded it found FOUR crossing
    classes an archive-level sweep does not show.** `qemu-arm64` only; the layout is BOOT (16 KiB
    identity) then the kernel's text high with its LMA below the 2 MiB code block, then the app's text
    identity-linked in the rest of that block, then the app's data, bss and heap at the boundary, then
    the kernel's writable state above them. VMA == LMA on both app regions, so nothing is copied into
    place; only `.appbss` is zeroed, by Reset_Handler, off its own pair of symbols.
    *The whole crossing set is SEVEN symbols*, `arch_syscall`/`arch_syscall64` app-to-kernel and
    `kickos_init_args`, `kickos_reent_seam`, `kickos_user_thread_return`, `kickos_init_entry`,
    `kickos_app_build_stamp` kernel-to-app, plus the optional `__register_frame`. No compiler-emitted
    class survived T5b.1's rewrite, so nothing needed a second objcopy pass.
    *`arch_syscall` takes T5b.1's own answer: the kernel is renamed and the app keeps the ordinary
    names.* The app's copy has to be text an EL0 thread executes, so it links low and carries
    privileged-execute-never; the kernel's `karch_syscall` sits in the half TTBR1 maps in every space.
    Both references are explicit calls, so this is a source fix, and the two leaves are `svc`+`ret`.
    It lives in the arch archive under its own section name, `.apptrap`, because the script selects
    the kernel half by ARCHIVE and that archive is kernel-side.
    *A CALL needed nothing at all:* ld inserts a long-branch veneer for an out-of-range
    `R_AARCH64_CALL26` in EITHER direction, measured before designing around it. Two survive in the
    image, both the kernel calling app text (`kickos_init_entry` and `kickos_app_build_stamp`), and
    both are T5b.3's problem rather than the link's.
    *What actually forced a decision was the GOT, and it is the one constraint with no cheap
    alternative.* A static link has ONE `.got`, a slot is reached by `adrp` like anything else, and
    there were users in BOTH halves: the kernel side through weak externs (`__register_frame`,
    `kickos_app_build_stamp`, `_kickos_heap_start`, the MPU-window symbols a flat chip leaves
    undefined), the app side through newlib's `__libc_fini_array` and `__call_exitprocs` reaching
    `__fini_array_start` and `__libc_fini`. It cannot be split and it cannot be placed. The answer is
    `-mcmodel=large` (`kickos_split_image_tu`): it emits no GOT reference at all, which leaves the
    one `.got` entirely to the app, AND it materialises every external address as a 64-bit literal,
    which retires all four kernel-to-app data crossings with no source change. Measured cost on the
    `selftest` link: kernel text 45,561 to 53,000 bytes, in a 1 MiB region.
    `-mno-direct-extern-access` does not exist on this GCC.
    *It is scoped to the six TUs that name the app's half*, not to the three archives, the flag
    costing kernel text on a fixed budget and on the scheduler and syscall paths' I-cache: applied
    whole-target it cost 4,076 bytes of archive text over a fully small build, of which naming the
    six TUs recovers 2,724. The list is derived from two sweeps over the objects of a fully small
    tree, a page-relative one against app-half names and a `_GOT` one, and both are needed: the
    reach class fails the link loudly, a GOT user is silent.
    *An FDE reaches the code it describes with a 32-bit PC-relative field*, so `.eh_frame` and
    `.gcc_except_table` have to sit in the same half as that code. Only the toolchain archives emit
    either here and those are app-side, so both moved down whole and one table still covers everything
    that unwinds. This was a truncated `R_AARCH64_PREL32` out of `libc_a-memchr.o`, not a prediction.
    *LINKER-SCRIPT SYMBOLS ARE A CROSSING CLASS THE ARCHIVE SWEEP CANNOT SEE*, being defined in no
    archive at all. One mattered: `user/src/newlib_sbrk.cc` names `_kickos_heap_start` STRONGLY from
    app text, so the newlib heap moved into the app's window with the rest of the app's writable
    state. T6 was going to move it anyway; the relocation moved it first.
    *And the user-pointer admission had to learn a second extent, which is T7's access seam arriving
    early and pulled in by the LINK rather than by the revocation.* `arch_ram_common.cc` whitelists ONE
    contiguous rom pair and ONE contiguous sram pair, and the split puts the app's rodata and the app's
    `.data` outside both, so a syscall taking a pointer into either would be refused. It now consults a
    SECOND, weakly declared pair the chip script may carve; a chip that carves none leaves all four
    zero and `range_within` admits nothing extra.
  - **T5b.2** is the fleet-wide seam mechanics, which stand alone: three calls become a base, a stride
    and one pointer address, the child's initialisation moves to first switch-in, and the write is a
    `memcpy`. Independent of the linker split, and correct on every board.
    **LANDED**, with four decisions a re-reading cannot re-derive.
    *The descriptor is the seam.* `kickos_reent_seam` (slots base, shared state, seat-word address,
    stride, count) is defined by `user/src/newlib_reent.cc` and READ ONCE at boot into kernel `.bss`
    (`kernel/thread/reent.cc`); every kernel-side answer comes out of that copy. Today the kernel
    names the app symbol directly and the link resolves it out of the RESCAN group, which is exactly
    what the split replaces, and nothing below it. Every member is `void*`/`size_t`/`int` so the app's
    initialiser needs no cast: a cast would sink the object into a ctor, and this file's ctors run
    from `root_entry`, after the kernel has already read it.
    *The pristine image is the PROCESS-WIDE STATE, not a template of the seam's own.* Measured on all
    five toolchains: newlib's `_impure_data` is a `.data` object statically initialised to
    `_REENT_INIT`, byte-identical to what `_REENT_INIT_PTR` produces, and its ONLY relocations point
    at the shared `__sf`, so it holds no pointer into itself that a byte copy would carry to the wrong
    owner. That matters because a template of its own costs one `struct _reent` (240 to 568 bytes) and
    the two 64 KB parts have about 500 bytes of headroom at their selftest preset. What keeps it
    pristine is that only a TCB OUTSIDE the pool is ever seated on it, which is idle, and idle holds
    no capability and runs `arch_idle_wait` alone.
    *Priming is per thread and uniform.* Every pool thread is primed at its own first switch-in, so
    the boot seeding loop in `kmain` and the pool's `reent_stale[]` array both go; the mark is a
    `bool` in `Thread`'s existing padding before `id`, free on every target. The seat moved AFTER
    `mpu.apply()`, both halves writing memory the INCOMING thread owns.
    *The seat must stay a leaf, and `-ffreestanding` is why it is spelled oddly.* The ordinary
    `memcpy` is never expanded under it, so a pointer-width copy lowers to a CALL on every switch;
    `__builtin_memcpy` plus `__builtin_assume_aligned` is expanded regardless and compiles to three
    instructions on all five backends. The prime keeps `kmemcpy`, its size being a runtime value.
    *And the fourth kernel-to-app call went the same way.* `root_entry`'s `kos_shutdown`/`kos_panic`
    become `arch_syscall(KOS_SYS_SHUTDOWN/KOS_SYS_PANIC, ...)`. The TRAP is still required, root not
    always being privileged, but the trap leaf is arch text rather than app text. `arch_syscall` is
    itself called from both sides, which is T5b.1's problem class and takes T5b.1's answer.
  - **T6.1** moves the stack and the TLS block low along with the image (the heap having gone with
    T5b.1's relocation), and the admission bounds learn to name a low address from high-half text.
    **LANDED**; the granted-range list is what names those addresses, and the stack's frames are
    mapped at their own output addresses so the pool answers for the same bytes kernel-side.
  - **T5b.3** revokes EL0 from the kernel's half, LAST, and is the check that makes T6's witnesses
    mean anything rather than a precondition of reaching them.
    **LANDED**, and the accounting that preceded it found the FIRST of the two measured blockers
    still standing, so the claim that both were gone was checked rather than taken.
    *The two roots stop sharing a level-2 table.* `kickos_arm64_kernel_ram_table` describes the
    same DRAM as `kickos_arm64_ram_table` with AP naming no EL0 access and UXN set, PXN clear on
    the code block alone; the boot identity root keeps the grants the untranslated entry path was
    built with. The fourth table page raised `KICKOS_ARM64_BOOT_SIZE` from 16K to 20K, which
    shortens APPTEXT by the same 4K and links clean.
    *`root_entry` WAS STILL KERNEL TEXT, and root is unprivileged from its first instruction*, so
    the earlier attempt's instruction abort at its high address had nothing to do with the app's
    link and was not retired by moving it. It is now `kickos_root_entry` in `libkickos_user.a`
    (`user/src/root_entry.cc`, declared beside `kickos_init_args` because it is the same class of
    app-side storage), which puts it in `.apptext` on the split board and leaves every other board
    unchanged, a region backend serving both levels from one text mapping. It also drags the app's
    ctor ARRAY down: `.kickos_app_init_array` holds app-text addresses but its WALKER is now
    unprivileged, so the array moved from KTEXT to APPTEXT and `__kickos_app_rom_start` names the
    region origin rather than `.apptext`'s. `karch_syscall` goes with it: an app-side walker calls
    the app's own trap leaf, which is what T5b.1's rename left in place for it.
    *The two link veneers did NOT both go, and the reason is the exception LEVEL rather than the
    half.* `kickos_init_entry`'s is gone with `root_entry`, both ends now being app text. The
    `kickos_app_build_stamp` one SURVIVES and is correct: it is reached from `kbanner` at EL1,
    before `domain_init`, so the root installed is the boot identity one whose code block leaves
    PXN clear. Revoking EL0 from TTBR1 does not touch it. What it does depend on is that no
    address space is activated before the banner, which the call site now states; the same
    dependency already carried `Reset_Handler`'s `.init_array` walk, whose bodies moved app-side at
    T5b.1.
    *The witness is `user/apps/common/kernelhalf`*, gated by
    `tests/integration/check_kernel_half.sh`: an unprivileged thread reads the word
    `arch_mpu_probe_addr` names and takes `ESR=0x9200000e`, a level-2 PERMISSION fault on a read
    from the lower exception level, at a FAR the image announced first. A PERMISSION fault and not
    a translation one is the assertion that matters: the kernel's half is mapped at that same
    address for the privileged side of the same core, so a translation fault would mean the
    kernel's own window had been lost rather than EL0's revoked. With the revocation reverted the
    same read returns a value, the image survives and the gate fails, so the arm passes one way
    only.
    *`arch_mpu_probe_addr` was re-keyed on `KICKOS_MEMORY_ENFORCED`*, which is the F6 split applied
    to one more hook: it answered 0 exactly on the board carrying the strongest refusal, and the
    word it names is kernel-side `.bss`, outside every arena and inside no granted range, which is
    what F6 asked a denial witness to name.
    *WHAT IS STILL REACHABLE, and it is not the high half.* The boot identity root grants EL0
    read-write over all of low DRAM, which includes the kernel's own `.data` and `.bss` at their
    LOAD addresses. No unprivileged thread runs under it today, `aspace_activate_for` installing a
    task's own root before its first instruction, and revoking EL0 there as well was MEASURED to
    leave all nine image gates and the whole 117-arm stream green. It is not taken here: the boot
    root is what the fault reporter and `aspace_release` install, and what `arch_aspace_boot`
    hands out, so making it grant EL0 nothing changes what that space MEANS. It wants a decision.

*Sequencing note:* the trusted-initial-frame work T6 inherits landed before this step, F9 having made
the exposure real at T5, so it is recorded as T6a in the history and reads as a T6 prerequisite.

**T5c. A plain spawn is a thread of the caller's task.** A semantic step, not a port step, and it is
here because T6 could not be written without it: per-process static data breaks 71 selftest arms
while every worker is its own task and its own space, since every worker reports to its parent
through an app global. **A plain spawn resolves to the CALLER's task and its domain**, spending no
task slot and no domain; an explicit task create still spends one, and so does a spawn that brings
its own data grant, there being no domain of the caller's for that grant to land in. That is F2's
own definition of a task applied to the spawn path: the set of threads that share one memory domain.

*Exit semantics follow forcibly.* `exit_current` applied task-scoped death to EVERY exit, which is
correct only while the group is one thread; once siblings share a task the first worker to return
would end root. **A FAULT is now the only death that reaches the group from there** -- it is the one
nobody asked for, and every death a caller asked for is a caller who could have named the group,
`kos_task_kill` and `kos_task_slay` each cancelling every member themselves. Section 6 of
`docs/design-task-layer.md` carries the reasoning and what it revises.

*Expected:* the entire existing fleet green with no per-board change, and the fault witnesses giving
their victim an explicit task, which is what makes containment observable at all rather than an
artifact of every thread being accidentally its own process.

*Measured, since the step exists to move this number.* T6's data-copy delta over `qemu-arm64`
fails **71** distinct selftest arms without T5c and **20** with it. The residue is not the same
class: what is left is the arms whose worker carries a spawn grant, so it is still a task and a
space of its own, plus the object-pool exhaustion a per-space copy brings. T5c removes the
sibling-through-a-global class and nothing else, which is what it claims.

**T6. The image mapped, and a task becomes a process.** Shared read-execute text, per-process copied
static data, stacks mapped with a guard page below (section 3.4).
*Expected:* THE PROCESS WITNESS -- two tasks whose text and data sit at the SAME virtual addresses,
backed by different frames, each reading and writing its own. F2 exists for this step and no earlier
step substitutes for it, because an identity map passes everything before it.
*Plus the driver handoff readback, moved here from T5 because this is the first step where it means
anything:* the bring-up flow runs unchanged and the child reads back what root wrote AT THE SAME
ADDRESS, covering the handoff contract, the two-live-mappings rule and the flags-match rule in one
arm. And a stack whose size is NOT a power of two, which F7 owes and which is only expressible once
stacks are frames with a guard page rather than arena blocks.

**THIS STEP SPLIT, and the split is what makes a process EXPRESSIBLE before anything changes what the
app layer sees.** T6.1 is everything that does not need the per-process static-data copy; T6.2 is the
copy, F10's allocator, the handoff readback and the process witness. Three earlier attempts at T6
stopped rather than pull the copy forward, and all three were right to.
  - **T6.1 LANDED.** The app's runtime memory moved LOW -- thread stacks and their TLS carves became
    frames mapped in the task's own space with an unmapped guard page below (section 3.4); the newlib
    heap had already moved at T5b.1. The granted-range list became the validation oracle: both
    static-data hooks re-keyed onto `KICKOS_MEMORY_ENFORCED`, every space's list seeded with the image,
    and `user_range_ok` reading it beside the region-set walk (section 3.3). And F7's stride tax was
    qualified per mechanism, so this board's stacks are three pages and five.
    *Measured:* the two section 3.3 arms both go RED with the seeding removed, which is what says the
    whitelist has stopped answering; a guard page below root's stack takes a level-3 translation fault
    on a write from EL0 at the address the image last named (`user/apps/common/stackguard`,
    `tests/integration/check_stack_guard.sh`); one live thread holds four frames and gives all four
    back with the pool refusing no free (`stack_is_frames`). The registered arm count moved from 112 to
    114 and the host unit layer from 336 to 343.
    *What T6.1 deliberately did NOT do:* a caller-supplied stack is still an arena block and still
    arena-confined, so it carries no guard page and stops working at T5b.3. The low range an app hands
    in is F10's allocator, which is T6.2's.
  - **T6.2 LANDED.** Per-process copied static data (section 3.4), F10's allocator, the handoff and
    its readback, and the arm repairs the copy forces.
    *The allocator, concretely:* `kos_ram_alloc` reserves a page-aligned range in the calling task's
    space and maps nothing, taking a frame-pool RUN and using its output address as the virtual one,
    exactly as a stack does; the self-grant maps that run and admits only a range the caller itself
    reserved; the handoff maps the donor's run into the new space at the same address and records it
    BORROWED. All three of section 3.2's load-bearing places move together, the self-grant, the
    domain region a spawn derives, and the app's own dereference.
    *Two bounds moved with it, and both are stated rather than tuned.* `KICKOS_ASPACE_RANGES` went
    from 8 to 32: it was mirrored from the descriptor bound, and F10 makes it the ALLOCATION budget
    too, a reservation costing a slot that nothing frees, which left an app six blocks for its whole
    life. And the self-grant records NOTHING in the region array on a translating build, so the
    -KOS_ENOMEM F10 keeps is the reservation list's and surfaces at the allocation: the array would
    otherwise carry a second, ROUNDED answer to a question the range list already answers exactly,
    and it is per-thread where the mapping is task-wide.
    *AND F10's HANDOFF COST AN ARGUMENT, WHICH THE REGION FLEET WOULD HAVE PAID FOR IN TRAP
    STACK.* `domain_for` sits on the deepest chain `syscall_dispatch` has, and on a 32-bit ABI
    a seventh argument is a word in its CALLER's frame: `task_for` grew 8 bytes and the armv7m
    SVC depth went 444 to 452 against a red zone of 448, failing `trap_redzone` on the four
    `KICKOS_KERNEL_STACKS=0` presets. Rounding the figure up to 512 was the documented move and
    the wrong one: those four are region boards and the argument is a translating backend's.
    So the two caller booleans, `privileged` and `caller_authorized`, became one posture word
    (`DOM_CALLER_*`), which puts the argument count back where it was and the measurement back
    at 444 exactly. **A control tree confirmed the attribution before the fix**: the same tree
    with the donor argument dropped measures 444 with `task_for` at 24 bytes rather than 32.
    *One seam outside T6.2's list had to move, and it is a piece of T7 pulled in by F10:* the endpoint
    copy reaches a PARKED peer's buffer, which is in a space that is not the running one, and moving
    the app's allocations low left it unreachable. A reservation and a stack are frame-pool runs whose
    virtual address IS their output address, so `ep_copy` reaches both ends through the pool's own
    map; a buffer the pool never handed out is app static data, and reaching another process's copy of
    that is still T7's. **T7 DELETED that stopgap**: its acquire-per-granule path through each end's
    own space answers for a pool run too, so `frame_pool_span` has no caller and is gone.
    *Measured:* the process witness reports three DIFFERENT frame numbers under one address and one
    shared text frame, and with the copy removed it reports the same number three times
    (`process_private_data`); a handoff given frames of its own instead of the donor's takes seven arms
    red including the readback's frame identity (`task_handoff_readback`); a reader spawned with its
    own grant instead of joining the group takes exactly one arm red (`task_siblings_share`); a stack
    block left reserved and never mapped takes exactly one (`caller_stack`); and the pre-T6.2
    `domain_share` shape, two spawn grants, takes exactly one (`domain_share`). The registered arm
    count moved from 114 to 117 and the host unit layer from 343 to 347.
    *What T6.2 deliberately did NOT do:* the same-address REFUSAL is implemented and untested, because
    the reservation namespace this backend chose is globally unique -- an address is a frame-pool
    output address -- so no correct caller can collide, which is the policy F10 demotes system-wide
    uniqueness to.

*Plus the sibling-sharing witness F2 assigns here*, one task create with two members, which is the
arm `domain_share` was mistaken for and which only becomes observable once two members share a mapped
image.

**`domain_share` DID NOT survive this step as it stood, and T5c is what made that visible rather
than what caused it.** Its two workers each carry the shared range as a spawn grant, so each is
still its own task and its own space, and F10's handoff is what makes the range itself work. The
range is not the problem. The arm's `g_dshared`, `g_dreadback`, `g_dwrote` and `g_dread` are APP
GLOBALS that root writes and both workers read, and per-process copied static data gives each
process its own copy: the workers read a null pointer and root reads back a value it was never
written. That was equally true before T5c and no earlier step could show it. The repair is the
ruling, applied: drop the spawn grants so the two workers are SIBLINGS of root.
**The ruling's second half did not survive contact with the region fleet and the applied repair is
narrower.** It said root would self-grant the range and the siblings would inherit it, F9 mapping it
task-wide -- true where a backend translates, and false on a descriptor board, where a grant is the
holding THREAD's region and a sibling that never asked faults on it, which is F9's own floor read the
other way round. So each of the three threads asks for the range itself, which costs a descriptor on
the boards that have them and nothing at all where the already-mapped short-circuit answers. Sibling
VISIBILITY, which only a translating backend promises, moved to `task_siblings_share`, and the
two-handoff witness F2 keeps became `task_handoff_readback`, which shares no app global across the
pair. **All three belong to T6 with the data copy, not to T5c**, which lands the ruling and nothing
else.

**AND THE SAME CLASS REACHED THREE ARMS THE RULING DID NOT NAME**, which is worth recording because
the shape is what a reader will meet next: `endpoint_crossdomain`, `irq_as_event` and the two
`aspace_two_spaces_*` helpers all had a worker carrying a grant -- so a process -- report to root
through an app global. Each is repaired by the channel it already had rather than by a new one: the
endpoint arm makes the worker's verdict a reply over the endpoint under test, the IRQ arm writes what
the driver saw into the ring block root handed it, the same-grant pair answers in the block it was
handed, and the no-grant pair, which by definition shares no byte, answers down an endpoint.

*Plus the two arms section 3.3 assigns here*, which pin the granted-range list being SEEDED with the
image rather than inheriting an oracle that no longer answers: a syscall whose input buffer is an app
global, and one whose out-pointer is. Both are ordinary code today, and both fail on a translating
backend if the seeding is missed -- presenting as a validation bug when it is a seeding bug.

**T7. The access seams, page-split, owner-carrying, and cross-address-space.** The two kernel-user
helpers and the endpoint copy, per section 3.3, plus the five peer-crossing sites that section
enumerates and the one console site outside the funnel. **`syscall_ipc_fast.cc` is named here because
it is the site no arm on this board reaches**, this arch implementing no IPC fastpath; and because
`write_recv_info` is one body whose callers are a mix of correct and wrong, the owner is threaded
through each caller rather than added once to the helper.
*Expected:* three arms, and the second is the one no existing arm can stand in for. A validated range
spanning two non-adjacent frames copies correctly, which catches a helper still written as one
`memcpy` over a translated base. Two processes at the SAME virtual addresses exchange a message and
its receive-info correctly, which catches both the dead overlap assert and any site still resolving a
parked peer against the running space. And an endpoint call and reply crosses two processes.

**AND THE FIRST ARM CANNOT BE AN APP, which is a fact about this backend rather than about the
scaffolding.** A reservation's virtual address IS its output address here, so virtual adjacency and
physical adjacency are one question from userspace and no caller can build the range; the
granted-range list refuses a range spanning two entries in any case, so such a buffer is `-KOS_EFAULT`
before it reaches an access helper. The scenario is therefore built with the map editor behind a new
`KOS_ASPACE_OP_SPLIT_ACCESS`: three CONSECUTIVE frames, the OUTER two mapped at two adjacent virtual
pages, and the middle one deliberately left unmapped. **The middle frame is the instrument**, being
where a copy written as one `memcpy` over a translated base spills, and it is read through the pool's
own route rather than through either space.

*Measured:* the seam takes each end's owner and a NULL owner means kernel storage, which is one
contract for the running end, a parked peer's end and the fastpath's saved trap frame alike; the
running end is translated too, since a backend whose kernel runs under a translation of its own
cannot dereference a user address at all. T6.2's `frame_pool_span` is DELETED, the general
acquire-per-granule path answering for the pool-backed ends it was added for. `ep_copy`'s numeric
non-overlap assert became a `(space, range)` one, and the numeric form restored PANICS the image at
the split arm. The three arms took the count 117 to 120 and the host unit layer is unchanged at 347.
One `memcpy` over a translated base leaves `split_access` at `NONADJACENT|BALANCED` and the other two
arms GREEN, which is the doc's "no existing arm can stand in for it" measured rather than argued.
Delivering the receive-info against the running space at `endpoint_send` takes only
`process_ipc_same_addr` red, and the same at `endpoint_call`, or a reply landing in the running space
at the parked caller's address, takes only `process_call_reply` red.

*What is NOT witnessed here, and where it would be:* the fastpath site, `KICKOS_ARCH_HAS_IPC_FASTPATH`
being 0 on `armv8a`, and no arch that HAS a fastpath (`armv7m`, `armv6m`, `rv32imac`, `rxv3`) has an
`aspace.cmake`, so both of its owner arguments are a compile-time null on every board in the fleet.
`call_reg_fastpath` witnesses the site compiling and behaving with the new signature and nothing more;
witnessing the owner needs a fastpath on a translating arch, which is the first M6.3 backend. And the
console site is now funnelled but no arm asserts console CONTENT: only root writes to the console
here, so a misdirected read has nothing to distinguish it. Truncating the funnelled helper visibly
splices the TAP stream, which is the mechanism witness; the content witness wants the published-console
route, where root reads back what the kernel actually streamed.

**T8. The third memory-model arm, the enforcement split, and the denial witnesses.** This is where F5
is discharged -- the existing fault-isolation path decodes a translation fault and contains it, and
nothing populates a mapping in a handler. **F6's split is NOT what lands here** -- it is a dependency
of T5, per F6 itself, since T5 must not configure an MMU board down the permissive path. What T8 owes
is the WITNESS that the split holds, which is a different thing from the split.
*Expected:* two denials, and the second is the one a permissive gate would let through. An
unprivileged thread touching a virtual address its space does not map is reported and killed, and the
system survives. And an authorized unprivileged caller self-granting a kernel TEXT or DATA address
OUTSIDE THE ARENA is REFUSED by name rather than merely failing to fault -- which is the arm that fails
if the MMU board inherited the no-protection admission path. **Not "a high-half address", which F6
records as unable to separate the two cases while the arena is itself high-half.** Two mechanics that
save a rediscovery: the grant-probe scaffolding is compiled out here, being gated on
`KICKOS_HAVE_MPU`, so the arm goes through a real path -- the self-grant, a task create, or a spawn's
memory grant -- and the address is ASKED FOR rather than written as a literal. **Not a linker
symbol**, which this line asked for until 2026-08-26 and which F6 rules out in its own words: app
text cannot name a kernel-half symbol under this board's code model, so such a witness does not
link. `arch_mpu_probe_addr` is the member that answers, keyed on `KICKOS_MEMORY_ENFORCED`.

*How T8 is SCORED, because the instruction it used to carry has no signal.* "Diff the skip list
against S9's" compares two EMPTY lists on this board -- S9 itself records that a non-enforcing board
does not register its enforcement arms rather than skipping them, so neither list ever had a member,
and the diff was carried over from a fleet where skips exist. Score it against the two things S9 does
leave behind. The registered arm COUNT rises from 112 by exactly the arms this stage adds and by
whichever of the five S9 enumerates it registers, so an arm that failed to register shows up as a
count that did not move; `user/apps/common/selftest/CMakeLists.txt` is the authority and the ctest
registration carries the same figure. And `periph_reg_write_unheld` leaves
`KICKOS_EXPECT_PARTIALS`: minting the MMIO window is what retires the partial, and
`tests/integration/check_tap_stream.sh` reports a listed name that never fired as a NOTE to trim
rather than as a pass, so the entry comes out in the same change.

*What T8 LANDED, scored that way.* The arm count went 120 to 122, the two arms being
`grant_kernel_word_refused` and `fault_kills_task`; `periph_reg_write_unheld` is STILL the only
declared partial, because nothing here mints an MMIO window, so that entry does not come out yet.
Skips stay empty. The ctest total for the board went 32 to 33, the extra one being
`qemu_arm64_faultsurvive`: the fleet's own survive arm became available the moment the arch joined
`KICKOS_FAULT_ISOLATION`, and registering it was a one-line board-list edit. (Both figures were
recorded one low. Re-derived at this pass: the board reports 35, which is this line's 33 plus the
two fleet-wide `host` gates registered since T8, `shell_special_names` at M6.3 and `entry_sigdiff`
at X8. No board-specific arm was added in between, so the dated 33 still reads against 35.)

**No arm moved from partial to full, so enforcement is not what this stage bought** -- the split
already held at T5, and T8 only witnessed it. What it bought instead shows up as a CHANGE OF
OUTCOME in two existing gates: `kernelhalf` and `stackguard` were dying-image gates asserting the
panic dump's `ESR=`/`FAR=`, and both now assert the thread-kill record's `ESR_EL1=`/`ADDR=` with
the SAME encodings, exit 139 rather than 132. Their claims are unweakened; only the survivor
changed. `aspacefault` STAYS a dying-image gate, and that is the scenario and not a gap: its read
is the kernel's own, inside `op_touch_unmapped`, so it arrives at the current-EL vector where the
kill rule declines it.

**One guard had to be narrowed rather than kept.** `check_tap_stream.sh` refused ANY thread-fault
record in the stream, on the premise that the deliberate fault lives in a separate binary. T8 puts
one inside the suite, so the clause became a by-name permission set (`EXPECT_FAULTS`, one entry:
the containment arm's own worker) on the same pattern as the skip and partial sets. An undeclared
faulting thread still fails the gate, which is the strength the blanket clause had.

**T4's rule needed a reading before it could be asserted.** A task emptied by a fault leaves 36
frames out while its CREATOR still holds the slot -- "emptied, not dead", the creator being free to
spawn into it -- so a balance read straight after the joins reports a leak that is not one. The
control that separates them is the same arm ending the group by an ordinary `kos_task_kill`, which
balances exactly. With root's hold dropped first, the fault path balances exactly too, and that is
the one death in the tree that runs out of a redirect stub rather than a syscall.

*Left standing, and worth naming because T8 makes it load-bearing:* armv8a has no record in
`tests/static/trap_redzone_roots.txt`, so the depth the fault-exit stub descends on a 4 KiB kernel
block is UNGATED here. The stub starts at the block top with the whole block under it and the
fault frame already popped, which is the most favourable position any backend gives it, but no
figure is enforced. Declaring the arch is a step of its own.

**T8b. Teardown, and the churn that proves it.** Section 3.1's destroy contract is BUILT, at T4 and
T5, so this step is the residue and not the whole of it: the root, its tables and every still-valid
leaf go back to the pool, the boot space is put back when the dying one is running, and the borrowed
image extents are unmapped first.
*Expected:* three things the existing balance arms do not say. The FORCED-FAILURE half, there being
no failure injection anywhere in the tree, so `claim_slot`'s and `domain_release`'s unwind arms are
written and unwitnessed. Frames a process allocated FOR ITSELF rather than borrowed from the image,
which is unreachable until section 3.4's copy and F10's allocator give a space a frame of its own --
so the count this step balances gets its first non-trivial term at T6, and a churn arm run before
that balances a destroy that freed nothing. And the ordered invalidation before a root or identifier
is reused, which the emulator cannot witness by passing.

*What T8b LANDED, and the mechanism first because everything else is downstream of it.*
`frame_pool_fail_in(nth)` (`kernel/mem/frame_pool.cc`, selftest-gated) refuses the `nth` next frame
allocation and disarms. It sits AHEAD of the allocator at BOTH entry points, so a refused attempt
takes nothing and the caller's unwind runs against the pool it started with, and it is one-shot so
the refusal is attributable to ONE allocation rather than to a pool that ran dry somewhere.
`frame_pool_fail_armed()` is the second half and less obvious: it tells a spent arming from one the
path never reached, which is what lets a sweep know it has covered every allocation rather than
guessing a bound. `KOS_ASPACE_OP_FORCED_UNWIND` walks `nth` up from 1 through
`domain_for`'s no-grant create and stops when a create finishes with the arming untouched.

It reached FIVE injection points, and every one of them refused: the answer was exactly
`KOS_ENOMEM` each time, the pool came back to the frame it started at, and a create after the sweep
succeeded and balanced.

**The balance is read IMMEDIATELY after each refusal and that is the whole point**: a refusal that
left the slot holding a half-built space balances anyway as soon as the next `claim_slot` releases
it, so a reading taken at the end of the sweep cannot see one. The second half of "no partial
mapping" is the REFUSAL COUNTER, which stayed at zero across the sweep: a leaf left standing over a
frame the unwind already returned is not a frame delta at all, it is a second free when destroy
walks the tree, and the pool refuses that rather than swallowing it.

*The same sweep runs a second time over the GRANT-CARRYING create,* the arm handing the op a range
root reserved. What that adds is the successful tail: the space that survives holds a BORROWED
mapping of root's block, so its release has to unmap and free nothing, which is the F10 hazard
`arch_aspace_destroy`'s own contract names.

**But it does NOT reach `domain_for`'s own unwind arm, and the *Expected* line above named it
wrongly.** Both sweeps report the SAME five points, and that equality is the finding: the handoff's
map needs no new table on this board, the donor block sitting under a level-3 table the image
already built, so frame injection has no point inside `aspace_handoff` and every refusal lands in
`claim_slot` ahead of it. Reaching that arm wants a second injector, into `VirtualRanges::reserve`
rather than into the pool, and it is not built. What IS swept is `claim_slot`'s three arms; what is
NOT is the inner unwind of a handoff that mapped and then failed to record.

*The churn arm, with the figures, because an arm that created nothing balances trivially.* Four
process lifetimes, each a task of its own with a member that runs to its end and then the creator
hold dropped, with a forced-failure sweep between the rounds so a refused create and a lived-out one
share one balance. 1946 frames free, **1910 with one process alive** and 1946 after: T8's 36, which
is the figure that reads as a leak if the creator hold is not dropped first, since a task can be
emptied rather than dead. Roots 1 then 1, `KOS_ASPACE_OP_SPACES_HELD` being the root count beside
the frame count. A WARM-UP CYCLE RUNS FIRST for the reason `stack_is_frames` states. The arm asserts
the drop as well as the return, and the sweep's DEPTH as well as its bits, both for the same reason:
every property holds vacuously over a sequence that allocated nothing.

*What the mutations say, one per witness.* Disarming `frame_pool_fail_in` leaves the sweep running
to its limit with nothing refused and both arms fail. Deleting `aspace_release` from `claim_slot`'s
seed-failure arm clears BALANCED and REUSABLE (`bits 27 of 63`). Deleting `arch_aspace_destroy` from
`aspace_release` clears the same pair and also takes `aspace_domain_balance` and `fault_kills_task`.
Leaving `d->space` set in `domain_release` clears NO_DOUBLE (`bits 55 of 63`), the stale space being
released a second time by the next claim, and takes four other arms with it.

*Ordered invalidation was ESTABLISHED BY NEGATIVE CONTROL, not asserted, and the result is worse
than it first read.* NONE of the four orderings has a behavioural witness on this bench, and which
is which was measured by removing each one and running the board.
  - `arch_aspace_unmap`'s per-page invalidate is NOT witnessed, and the arm that appeared to
    witness it was answering a different question. The arm this branch replaced it with reddens
    when unmap is a no-op, and stays GREEN when `invalidate_page_if` alone is removed and the leaf
    is still cleared. What has a witness is the CLEARING of the leaf, not the invalidation.
  - A FRESH map's invalidate is NOT, which `../STATE.md` already recorded and this confirms: the
    whole runtime suite stays green without it. It is in the code because the architecture caches
    negative translations.
  - `arch_aspace_destroy`'s whole-TLB sweep, which stands BEFORE `free_subtree` so no frame returns
    to the pool while a translation to it can be cached, is NOT witnessed either: the runtime suite
    is green without it.
  - Nor is `aspace_release` putting the BOOT space back before destroy frees the dying root. Moving
    that activation to after the destroy leaves every arm passing, so the order is held by
    construction and review rather than by a test. What makes it correct is that the kernel runs out
    of the high half while the low root is invalid; what makes it fragile is that the window is not
    interrupt-masked, and a preemption returning to EL0 inside it would walk a freed root.

Identifier reuse is VACUOUS here rather than witnessed: nothing in the tree allocates an
address-space identifier, section 3.1 having said so, and `write_ttbr0` sweeps the whole local TLB on
every root change precisely because no translation is tagged.

**T9. The multicore seams, compiled to nothing, and the cache seam that is not.** The per-core
pointer, the doorbell as empty macros, and the TLB maintenance INSIDE `map` and `unmap` staying a
local invalidate that a broadcast later replaces. That maintenance is below the seam and never a
call of its own, per section 3.4b, so what M7 changes here is a body and not a signature.

**AND THE TRI-ARCH SPIKE FOUND THAT LAST SENTENCE TOO OPTIMISTIC, which is what T9 now owes a
decision about.** "Coherence-complete when it returns, visible to this core" is a ONE-CORE
definition. The initiator waits on all three architectures; the finding is what it waits on. A64's
`TLBI ...IS` plus `DSB ISH` blocks until every PE in the shareable domain has finished, including
draining its own accesses through the stale entry, with no code on the far side and no way to
deadlock. RV64 and x86_64 have no broadcast, and both specifications prescribe a software protocol
instead, which means waiting on other cores EXECUTING a handler. The strongest evidence that the
wait is intrinsic rather than an artefact of the IPI: the one vendor that did build a hardware
broadcast shipped a separate BLOCKING instruction beside it. So the seam reads broadcast-then-wait,
never IPI-then-wait.

Three consequences, and the third is a decision rather than a note:
  - **Coherence-complete splits in two.** An unmap whose frames are being FREED may defer its
    remote half, because every one of the three specifications puts the boundary at REUSE rather
    than at the edit, which moves that barrier into the frame allocator. An unmap that REVOKES
    cannot defer, and this system revokes capabilities, so the synchronous cross-core wait is
    reachable rather than hypothetical.
  - **The lock and this seam become ONE design.** The far side's handler must take no kernel lock,
    and the lock's acquire loop must service a pending doorbell. Otherwise an initiator holding the
    lock waits on a core spinning to acquire it and the system stops. That is a deadlock A64 cannot
    have and the other two can, so it may not be discovered by the first backend.
  - **DECIDED AT T9, and it is the one freeze this step owes: the ACTIVE-CORE SET IS A READABLE
    FIELD ON THE OPAQUE SPACE, not a parameter on `unmap`.** The rendezvous needs to know which
    cores have the space active, which is knowledge the seam does not currently carry. The two
    candidates were a core-set parameter on `unmap`, which is a signature change and puts a
    multicore concept in every backend's face at one core, and a set the kernel can read off the
    space. **The reasoning is asymmetric cost of DEFERRAL, and that is why it can be decided now
    with nothing built.** `arch_aspace` is opaque, so a field added later costs nothing above the
    seam: no caller names it, no backend that does not need it declares it, and the M6 backends do
    not change. A parameter added later is not free in the same way, because it edits every
    backend's signature and every call site at once, and it would have to be threaded through a
    milestone that has no use for the value. So the choice is between paying nothing later and
    paying a fan-out later, and the tri-arch spike's recommendation and this reasoning agree.
    **NOTHING IS ADDED NOW**: neither field nor parameter is in the tree, because nothing in M6
    reads one and a field with no reader is a second truth beside the scheduler's own record of
    which core runs what. What T9 owes is the DECISION, so M7 implements rather than re-litigates.

*Also corrected by that spike, in `docs/design-m7-smp.md` rather than here:* two claims that do not
hold. The blocking all-core rendezvous is not owed by the milestone that adds the second core on
A64, the data-side rendezvous being the hardware's; what IS owed there is narrower and was unnamed,
`ISB` not being broadcast, so a change to an EXECUTABLE mapping still needs the far side poked. And
`sbi_remote_sfence_vma` is not promised synchronous by its specification, where success means the
request was sent; one implementation happening to block is not a contract. **Plus the data-cache clean and invalidate seam, which `roadmap.md` lists as an M6
deliverable.** The seam lands here; what stays open is who calls it, which is section 7's entry.
*Expected:* `tests/static/check_cpu_id_fold.sh` passes with `KICKOS_NUM_CORES` at 1, so the core
identity folds to a literal and there is no fallback path to drift out of date.

*What T9 LANDED.* Three seams, and the count is unchanged on every board because at one core two of
them expand to nothing and the third has no caller.
  - **The per-core block.** `arch/arm64/armv8a/include/kickos/arch/percpu.h` declares
    `struct armv8a_percpu` and `kickos_armv8a_percpu[KICKOS_NUM_CORES]`; `kickos_armv8a_kernel_sp`
    is gone and its cell is the block's first field, which section 2 said would happen when the
    pointer did. The accessor `armv8a_percpu()` is a MACRO folding to the array's first element at
    one core and a declaration only above it, exactly as `arch_cpu_id` is, so a port that raises the
    knob and ships no definition is a link error. The block's address is a link-time constant here,
    which is what keeps `vectors.S` reading it with an `adrp`/`ldr` pair and no register spent, and
    the `kernel_sp` displacement is `static_assert`ed at 0 because it is spelled on both sides.
    TPIDR_EL1 is still written by nothing, and that is the point: it is the register the multi-core
    arm reads the block out of, and T6a is what freed it.
  - **The doorbell,** `arch_ipi_send` and `arch_ipi_wait` over a core mask, EMPTY MACROS at one
    core. Two calls and not one from the first line, a fire-and-forget doorbell being unable to
    express a rendezvous. Its comment records what it is NOT: on a broadcast architecture the
    maintenance inside `map` and `unmap` does not route through it, because routing it there would
    invent the deadlock A64 cannot have.
  - **The data-cache flush and invalidate seam,** `arch_dcache_flush` and `arch_dcache_invalidate`,
    with the armv8a backend in `arch/arm64/armv8a/cache_armv8a.cc`. Concepts: no line size crosses
    the seam, the backend reading `CTR_EL0` rather than trusting the A53's 64 bytes, because a
    smaller line anywhere in the hierarchy would leave lines untouched at 64. The invalidate uses
    CIVAC and not IVAC, a range whose ends fall inside a line sharing those lines with its
    neighbours. **NO CALLER, which section 7 owns**, so the member is compiled and never extracted
    and no image on any board grows a byte. That also means it is UNWITNESSED and cannot be
    witnessed here: QEMU models no data cache, so an arm exercising it would pass with the loop
    bounds wrong.

*And the TLB maintenance did not move, which was the requirement.* It is `invalidate_page` and
`invalidate_all`, file-local to the armv8a backend and reached only from inside `map`, `unmap`,
`destroy` and the root write. Nothing named a flush crosses the seam, so what M7 replaces with a
broadcast is those two bodies. What each of them is worth as a witness is T8b's negative-control
result, not this step's.

**Stage result:** an enforcing unicore A53 running processes, the whole existing fleet green, and both
sweeps clean -- `tools/sweep_image_gates.sh` and `tools/sweep_host_gates.sh`, because the image half
alone has hidden a failure before.

**T10. The two findings an external audit stopped the merge for, after that stage result.** Both are
recorded here rather than in a changelog because each corrects something this document ASSERTS: the
first completes T4's ownership rule (F10, above), the second retires an arm that was asking an MPU
question of a translating backend.

*T10a. A borrower kept mappings after the donor's frames were freed, and the fix is a lifetime edge
rather than a mechanism.* F10 now carries the rule and the acyclicity argument; what belongs here is
where the reference is taken and dropped, and what the arm measures.
  - **Taken** in `domain_for` (`kernel/domain/domain.cc`), on the line after `aspace_handoff` returns
    0, and only there. Taking it on success is what leaves `aspace_handoff`'s own three unwind arms
    with nothing to surrender: none of them can be reached with the edge already written.
  - **Dropped** by `drop_space`, the file-local helper that is now the ONLY caller of `aspace_release`
    in that file. The unmap of the borrowed entry and the surrender of the reference it rests on are
    therefore one operation that no site can half-perform. Its three callers are `domain_release` at
    refcount zero, `claim_slot`'s recycle of a slot that still holds a space, and `claim_slot`'s
    seed-failure unwind.
  - **The recycle arm is not decoration.** A domain abandoned at refcount 0 -- `task_for` and
    `task_create` both resolve one before they take a task slot -- pins its donor until the pool
    happens to reuse the slot. Both of those failure arms now release the domain outright, and the
    recycle arm is the backstop.
  - **`domain_release` is iterative.** A borrower of a borrower makes the release a chain as long as
    the domain pool, with `arch_aspace_destroy` walking a page-table tree at every link, so the loop
    spends one kernel frame for the whole chain instead of one per link.
  - **The arm is `task_handoff_donor_exits`,** and it stages the order the suite had never staged: the
    donor is a task of its OWN reserving a block of its own, because a block ROOT reserved is BORROWED
    in the donor's space too and its teardown would free none of it. Root joins the donor's thread AND
    drops its own creator hold -- `kos_task_create` takes a domain reference for the creator, so the
    join alone is not the donor's last reference and without dropping it the ordering is unreachable.
    A churn task then takes every one-granule block the pool will hand out and stamps each.
  - **What the mutation says.** Deleting the two lines that take the edge, and nothing else: `spaces
    held 3 -> 2`, so the donor's space was destroyed under a live mapping; the churn task's FIRST
    block landed on the donor's own frame; and the borrower read the churn task's word rather than the
    donor's. With the edge in place the churn window brackets the donor's frame (608..709 against 609)
    without containing it, which is what makes the token check non-vacuous rather than lucky.

*T10b. `region_mode` was STATE-DEPENDENT rather than failing, which is worse.* The arm inferred the
MPU region-encoding mode from the SUBTRACTION OF TWO `kos_ram_alloc` results, and under
`KICKOS_HAVE_ASPACE` the allocator is a first-fit frame bitmap: earlier frees make consecutive
allocations non-monotonic, so the difference reports pool state and not shaping. It was reproduced red
by the auditor and green here, both correctly.
  - **ALLOCATION ORDER IS NOT PUBLIC API on a translating backend**, and that sentence is now in the
    source at both sites that used to assume otherwise. The arm is not registered under
    `KICKOS_HAVE_ASPACE` rather than tuned until one ordering passes; there is no region encoding to
    name there, and what it used to assert about the map editor is asked of `aspace_model` and
    `aspace_seam` instead.
  - **`discover_granule` was the same class** and is the one that mattered, being memoised, so a
    single bad first pair would have poisoned every later consumer for the whole run. Under
    `KICKOS_HAVE_ASPACE` it now ASKS, the probe ABI already carrying the op
    (`KOS_ASPACE_OP_GRANULE`, `user/include/kickos/sys/abi.h`), so nothing was added for this. The
    power-of-two test on the answer is what turns an unavailable probe into a skip rather than into a
    garbage granule.
  - **`sgnp_worker` is the third and is left registered**, with the assumption named. Its "consecutive
    3-granule blocks step through the granule residues" is a bump-arena argument, and where it does
    not hold the arm goes NON-DISCRIMINATING rather than wrong: it falls back to the first block and
    still self-grants a 3-granule one, which is the size question it exists to ask.
  - Every other multi-allocation site in the suite compares frame identities, return codes or frame
    COUNTS, none of which is order-dependent.

**T11. The four performance findings of the same audit, and the DRY one.** Each corrects a cost this
document records rather than a behaviour it asserts, so each carries its measurement. Every figure
below is the `qemu-arm64` `selftest` link, and the two text figures are `.text.init` plus `.text` of
that ELF and the text totals of the three archives holding kernel text.

*T11a. Seeding a space installed on no core costs no TLB maintenance, and that is a DECISION resting
on a property section 5 already records.* Nothing tags a translation, so `write_ttbr0` sweeps the
whole local TLB on every root change; a space whose root this core has never installed therefore
holds neither a cached entry nor a cached negative translation, and a per-page invalidate during its
seed drops nothing. `invalidate_page_if` takes that answer from `installed_here`, which reads
TTBR0_EL1 and compares it against the space's own root, and `arch_aspace_map` and `arch_aspace_unmap`
each ask once per call rather than once per page.
  - **Measured inside the `IrqLock` of ONE `kos_task_create`:** 62 page-invalidation sequences before
    (248 synchronising instructions, four per page across the image's text and data extents), 0
    after. What remains masked is the image data copy, 32 pages of it, unchanged.
  - **The RUNNING space still pays**, which is the half that must not be optimised: a self-grant
    widens the installed root and every page of it is invalidated.
  - **AT ONE CORE `installed` IS THE WHOLE QUESTION**, and the elision is compiled out above one: a
    sibling core's TTBR0 is not readable here, so the multi-core form needs the active-core set T9
    decided and did not build.
  - **Witnessed by `map_tlbi_elided`,** which reads `KOS_ASPACE_OP_MAP_TLBI` either side of a task
    create and either side of a self-grant. Two mutations fail it: gating the elision off reports 62
    issued and 0 elided, and eliding unconditionally leaves the self-grant reporting 0 issued. The
    positive control is not decoration, that second mutation being otherwise INVISIBLE to the whole
    suite: QEMU does not model a stale entry, which is the same result T8b's negative control already
    recorded for a fresh map's invalidate.
  - **NOT DONE, and the reason is the SOURCE rather than the destination.** The audit's direction
    holds for everything a seed WRITES: the tables, the frames and the range list of a space nothing
    can reach need no global mask, and the frame pool serialises its own operations. It does not hold
    for what the seed READS. The template is root's live static data, and the mask is what makes the
    32 pages ONE point-in-time snapshot of it: with interrupts on, a user IRQ handler or a sibling
    thread of root's task can write an app global between two of those pages, and the child then
    starts on an image no writer ever saw, a pointer copied without its target. Nothing in the suite
    would show it. So the copy is masked deliberately from here on, and removing the mask needs a
    scheme that makes the source immutable for the duration (a read-only template with a fault
    handler behind it) rather than a lock change.
  - **And a second blocker sits under that one,** which is why the narrower change is not available
    either: the copy runs inside `spawn_masked`'s function-scope lock, whose closing comment states
    why a spawn is one transaction. A spawner slain in an unmasked gap is redirected to the exit stub
    and never returns, so a domain slot claimed and an image seeded in that gap have nothing left
    running to finish or free them. That would need an abandonment record on the creating thread and
    a reclaim of it in the kill path.

*T11b. `-mcmodel=large` is scoped to the six TUs that name the app's half*, not to the three
archives. The full reasoning and the derivation are at T5b above; the figures are 4,076
bytes of archive text for the whole-target flag, of which naming the six recovers 2,724, and 2,264
bytes of the ELF's kernel-half text. The kernel calls no app text either way: 1,864 kernel-half
branches swept, 0 into `.apptext`, with the same range test over `.apptext` itself firing 3,224 times
as the positive control.
  - **The reach class is loud and the GOT class is silent**, and that asymmetry is why the scoping
    ships with a gate. `kernel/mem/aspace.cc` and `kernel/domain/domain.cc` hold only weak externs; compiled small
    they emit `R_AARCH64_ADR_GOT_PAGE` and the link SUCCEEDS, because the symbols resolve. Nothing
    fails until something else forces the one `.got` to be placed. `kernel_got`
    (`tests/static/check_kernel_got.sh`) reads the three archives and refuses any `_GOT` record, with
    the relocation count as its positive control; dropping `kernel/mem/aspace.cc` from the list turns it red
    and names the four symbols, while the link and the runtime suite stay green.

*T11c. The frame bitmap is scanned a word at a time*, keeping the bitmap and first-fit. Recorded with
its figures and its host arms at `kernel/mem/frame.cc`.

*T11d. Per-domain range bookkeeping is narrowed, and the widths are PROVEN.* `VirtualRange` was 24
bytes and is 16: `pages` is `uint32_t` and `rights` `uint8_t`. Neither width is assumed. Three
`static_assert`s in `kernel/include/kickos/vrange.h` say the widest value each setter can be handed round-trips through its
field, and the setters refuse rather than truncate: `reserve` refuses above `VR_MAX_PAGES` and `grant`
refuses a rights word carrying a bit the entry cannot hold. `Domain::regions` is one descriptor rather
than `KICKOS_MPU_MAX_REGIONS` where a backend translates, the kernel singleton's whole-arena
descriptor being the only one any domain there records, and `region_count` is a byte.
  - **Measured on the kernel instance:** 35,752 bytes to 27,112, of which 5,120 is the range lists
    (20 slots x 256), 3,360 the region arrays and 160 the region counts. Per-domain range bookkeeping
    falls from 43.4 percent of the instance to 38.4.

*T11e. The checked extent arithmetic is shared and the range walks are NOT.* `include/kickos/extent.h`
carries `is_pow2` and `extent_end`, reached by both layers without either including the other's
headers, and `frame_pool_free_run` replaces the two identical frame-run free loops. Left duplicated
deliberately: `VirtualRanges::find`, `covers` and `overlaps` are three traversals of one array with
three different admission rules, and the map editor's leaf walks differ in break-before-make and
attribute handling. Unifying either set takes a flag, and a flag over a security-sensitive traversal
is easier to misuse than the duplication.

### M6.3 -- the RV64 Sv39 backend, and the aspace-seam verdict

Per F8 this is the LITMUS, so a step here is judged by what it proves about the seam and not by how
much of a platform it lights up. QEMU first; the silicon witness is a single-core RV64 Sv39 part.

**R1. The ISA half, where "reusing what exists" means the DESIGN and not the code.** The RISC-V trap
vocabulary and CSR shapes are in the tree and none of them are SHARED: `arch/riscv/` holds one arch
beside a family-level chip directory, and `arch/arm/` is the only family carrying a common layer. So
what transfers is the trap SHAPE, one vector and save-everything and a demux on cause with the
trusted stack pointer swapped through a scratch CSR, and the divergence under it is total. This port
runs in S-mode against a page-table root where the existing one runs in M-mode against PMP, so every
machine CSR renames; `mstatus.MPP` is two bits at 11 where `sstatus.SPP` is one bit at 8; the machine
interrupt causes give way to the supervisor ones; the PMP CSRs are not writable from S-mode at all;
and at XLEN 64 every `lw` and `sw` and the whole frame geometry move.
*Decided at R1.1 rather than discovered later:* `rv64imac` is a standalone sibling arch. Hoisting a
shared RISC-V layer is a refactor of a WORKING port and belongs to whichever milestone wants it, not
to a step whose job is a second backend.
*Expected:* boot to a console byte and a running switch under `qemu-system-riscv64`, with the memory
model still flat. Do not carry over an assumption from the M-mode backend without re-deriving it.

**R1's toolchain clause was arm64's, and it does not transfer.** S1 needed a toolchain file with an
environment variable of its own because `aarch64-none-elf` is a separate tree; the rv64 multilib
ships inside the RISC-V toolchain this project already wires. So R1.1 adds no toolchain file, no
environment variable, no preset family file and no CI toolchain action, and the chip's `cpu.cmake`
names the MULTILIB. Naming the TRIPLE instead is what makes this read as new work: the tree whose
triple says 32 is the one that builds rv64 C++.

**R1 IS SIX STEPS AND WAS WRITTEN AS ONE**, which is section 5's opening note appearing inside its
own step text: "reusing what exists" was an observation, and one paragraph later it read as a reason
the step would be small. M6.1 spent S1 through S9 on this ground. The split follows the S-series so a
finding can name one step, and it ANTICIPATES M6.1's recorded blocker rather than meeting it again:
`user/src/syscall_stubs.cc` reaching `arch_syscall` gates the link for this whole stage, so R1.2
through R1.4 take their witnesses from an ad-hoc link and R1.5 is the first step whose image is the
real one.

  - **R1.1 Arch, chip, board, preset, and the split-image flag that named an arch.** Two sites
    hard-coded `armv8a` inside guards that read `KICKOS_HAVE_ASPACE`, so a second translating arch
    could not exist at all; the code model is now a per-arch flag and both call sites derive their
    translation unit from the arch and the chip. The arch stanza selects NOTHING: a select enrols the
    backend in the gates that build their corpus from it, so each one lands with the step that earns
    it, which is the rule armv8a's stanza has kept since S7.
    *Expected:* the preset configures and both libraries compile, with `arch_syscall` and `_start`
    the only undefined symbols, those being the two files the step does not ship.
  - **R1.2 Boot to one byte, and the machine hands the kernel the WRONG PRIVILEGE MODE.** Reset
    entry, initial stack, zeroed BSS, one byte out. S2's clause about determining which level the
    machine hands us transfers, and a standalone probe answered it ahead of the step: the hart
    arrives in M-MODE, so the port drops itself by delegating the exceptions and interrupts, setting
    the previous-privilege field to supervisor and returning. **A PMP GRANT IS OWED BEFORE THE DROP,
    by a port with no region backend at all:** once any PMP entry is implemented, every S-mode and
    U-mode access fails unless an entry permits it, so an unprogrammed PMP denies a supervisor kernel
    all of memory. One M-mode-only write, and the only piece of the region vocabulary this
    translating port keeps.
    *Expected:* a known string under `qemu-system-riscv64` from an image with no kernel in it.
    That string is scaffolding and R1.4 retires it, the kernel banner being what the rest of
    the system uses; a reader looking for it in the tree will not find it.
    Semihosting answers from BOTH privilege modes, a trap handler's own writes included, so this
    step needs no UART. **The arm must be falsifiable about the DROP and not merely about the
    print:** the probe confirms supervisor mode by an `mstatus` read trapping to `stvec`, and with
    the returning instruction replaced by a jump that read succeeds instead.
    **The confirming instrument cannot be this step's own code, and that follows from the ISA
    rather than from the plan.** Current privilege is not readable on RISC-V; it is observable
    only through what is denied, so the `mstatus` read has to land somewhere, and the landing pad
    is R1.3's `stvec`. The drop arm therefore lives in the ad-hoc harness beside the stub kernel
    entry, and R1.3 is where it becomes repo code. The print alone is blind to the drop: with the
    `mret` replaced by a jump, the boot string, the `.bss` check and the `.data` check all come out
    unchanged. **What the PMP clause looks like when it is defeated:** the drop's first instruction
    fetch takes an instruction access fault (`scause` 1) into an `stvec` of zero, so the image
    spins silently, which is the same silence R1.3 warns a missing vector produces.
  - **R1.3 The trap vectors and the fault report.** The vector, the supervisor cause demux, and the
    reporter behind the existing banner contract.
    *Expected:* a deliberate fault prints a decoded report. S3's false pass applies unchanged: with
    no vector installed the image dies silently, so silence is the symptom.
    *Landed at R1.3,* `arch/riscv/rv64imac/trap.S` plus the reporter and `kickos_rv64_init` in
    `arch/riscv/rv64imac/arch_rv64imac.cc`. Four things a reader would otherwise re-derive:
    **The mode is DIRECT and the rv32 port's vectored choice does not transfer,** that one being a
    core constraint of the ESP32-C6 rather than an arch fact. Vectored mode dispatches only
    INTERRUPTS and sends every exception to the base anyway, so a table of identical jumps buys
    nothing; and it actively costs, because an interrupt of code `i` enters at `BASE + 4*i`, past
    the two `mv`s with which `kickos_rv64_stvec` captures the interrupted `sp` and `ra`. The
    mutation that turns the mode bit on still prints the right cause and a WRONG frame line whose
    `SP` reads plausible, so a banner-only check does not see it.
    **The banner is its own, `=== RISC-V S-TRAP`, and not the rv32 port's `=== RISC-V TRAP`.** One
    banner name per ARCH is the shape the rest of the fleet keeps, the dump lines differ
    (`scause`/`sepc`/`stval`/`sstatus` against the machine four), and two RISC-V boards on one
    ctest ladder would otherwise be indistinguishable on the wire. `tests/lib/panic.ere` gains the
    alternative and `check_panic_banners.sh` gains the file in its reporter list, without which a
    later deletion of the banner reads as a clean tree.
    **The drop confirmation is a BOOT-TIME check and not a selftest arm**, `kickos_rv64_init`
    refusing a hart the probe finds in machine mode. A selftest arm cannot be witnessed until R1.5
    links a real image, and every s-prefixed CSR this backend writes is meaningless in machine
    mode, so the check is a precondition of the backend rather than a test of it.
    `kickos_rv64_privilege_probe` SAVES AND RESTORES the caller's `stvec` rather than reinstalling
    the vector by name: the first cut did the latter, which made the install in `kickos_rv64_init`
    redundant and left a defeated install passing every arm.
    **The causes with no witness at this step, and which one gets them:** 12, 13 and 15 need a
    non-empty `satp`, so R2; 8 needs U-mode, so R1.5; 0 needs a hand-built misaligned fetch; 4 and
    6 have none on this emulator at all, QEMU completing misaligned accesses rather than trapping
    them. Codes 1, 2, 3, 5, 7, 9 and the interrupt half are each witnessed against a
    wrong-but-plausible decode, so what those three page-fault strings are still exposed to is a
    typo and not a mis-indexed ladder.
    *CODE 9 STOPPED BEING WITNESSABLE HERE, and the entry above is now a claim about a NAME TABLE.*
    R6 undelegates `medeleg` bit 9, so an ECALL-from-S no longer reaches the S-mode reporter at all;
    it goes to the machine-mode handler `mtvec` names. The string `"ecall from supervisor mode"` is
    still in `arch/riscv/rv64imac/arch_rv64imac.cc`'s decode and is still worth keeping, a cause
    table being cheaper to keep total than to prune, but nothing on this board can drive the
    reporter to print it. Counted among the witnessed causes it inflates the figure by one.

  **THE LINK BLOCKER FOR THIS STAGE IS TWO SYMBOLS AND ONE RELOCATION, NOT ONE SYMBOL.** R1's
  preamble names `arch_syscall` alone; measured at R1.3, a full `kickos_build` also stops on
  `R_RISCV_HI20` against `__clz_tab` in the libgcc multilib, pulled in by `__clzdi2` from
  `kernel/sched/policy_fifo_rr.cc` and `__ctzdi2` from `kernel/mem/frame.cc`. Those libgcc objects
  are built medlow and cannot reach a `.rodata` at 0x8000_0000, which is orthogonal to the syscall
  trap and so does not go away at R1.5. Whoever takes R1.5 owes it a decision: build the two
  helpers in-tree, or reach the multilib through an indirection, which is the same shape as the
  code-model residual R2 already carries.
  *A third option exists and it belongs to R2 rather than to R1.5, so the stopgap needs removing
  rather than keeping.* Measured directly against the linker: medlow on RV64 reaches EXACTLY the
  top 2 GiB and nothing else, `lui` materialising bits 31:12 sign-extended at XLEN 64. A `.rodata`
  the medlow objects reference LINKS at 0xFFFFFFFF_80000000, and is REFUSED with `R_RISCV_HI20` at
  both 0x8000_0000, which is where this port links while it is flat, and 0xFFFFFFC0_00000000, which
  is the bottom of Sv39's high half and the address a reader picks first.
  **So F1's "fixed high range" has one placement on this architecture that costs nothing and
  several that cost a libgcc workaround,** and the naive one is among the expensive ones. R1.5
  cannot use it, being flat with VMA equal to LMA over RAM at 0x8000_0000, so R1.5 still owes the
  stopgap above; R2 choosing the top 2 GiB is what retires it, and whoever lands the in-tree
  helpers says in one line that they are a stopgap so they are not left standing.
  **NO STOPGAP WAS EVER OWED, AND THE PLACEMENT IS R1.4b's RATHER THAN R2's.** R1.4b ran before R1.5
  and had to, the toolchain making the high link unskippable, so the decision this bullet hands R1.5
  never came due and no in-tree helper was written. That step also corrects the SCALE recorded here:
  the same relocation class covers the whole C library and not two libgcc helpers, which is why the
  placement is the fix; and R2 inherits the placement rather than choosing it.
  - **R1.4 The console seam and the banner.** The console write behind the seam.
    *Expected:* the kernel's own banner, through the seam rather than through the step's own
    probe, which is S4's expected result unchanged and needed no per-board work again: the
    version, board, arch, build stamp and heap figure all came up right on the first ad-hoc
    link of the real image.
    **SO THE BANNER HALF IS PURE DELETION, and that is the result rather than a shortfall.**
    What R1.4 changes is that R1.2's boot mark goes: a direct `arch_console_write` from
    `Reset_Handler` placed to prove one byte could leave, and a second writer nothing reads
    once the banner is on the wire. Retiring it is also what makes the arm honest, since a
    check on "a known string reached the console" passes on the boot mark alone with the
    banner call deleted, and that arm is what the step would otherwise be resting on.
    **The seam half is two chip-side corrections and S4 predicted the second.** The polled
    writer was UNBOUNDED, and `arch_console_write_sync` aliases it through the lone-TU
    fallback, so a wedged UART hung the panic path instead of costing it a tail; it now
    carries the same spin bound the PL011 writer does. And `arch_console_flush_sync` was
    taking its fallback, which ASSERTS the console cannot outrun `arch_shutdown` - false of
    an NS16550A, whose 16-byte TX FIFO and shift register outlive the core while the writer
    polls THRE rather than TEMT. S4 said a fallback whose comment describes a property of the
    chip has to be checked against the chip; the next backend needed exactly that, so the
    lesson generalises as stated.
    *What has no witness on this emulator is the same thing S4 could not witness:* QEMU's
    NS16550A hands each byte to its chardev on the register write and reports TEMT set with
    it, so no truncation can be produced. What IS witnessed is that both loops run, read that
    register and terminate, by pointing the register window at zeroed RAM so neither THRE nor
    TEMT ever asserts: bounded, the image still reaches its exit status; unbounded, it hangs.
    *And a residual the whole fleet shares, found here rather than owned here:* `kpanic` ends
    in `kfault_terminate`, which every chip defines as a bare `arch_shutdown`, so the flush
    is reached only from `kickos_terminate` and a clock retune and a panic can still truncate
    on a board with a real FIFO. Fixing that edits every backend and does not belong in a
    port.
  - **R1.4b The kernel into the top 2 GiB, and it is NOT OPTIONAL.** This step was missing from
    the split and the TOOLCHAIN supplied it, which is S2b's shape in a different mechanism: there
    translation-off made compiled C unrunnable, here it makes a full image UNLINKABLE. The prebuilt
    libc and libgcc multilibs are medlow, and medlow on RV64 reaches only the top 2 GiB, so their
    own references to their own data do not fit a HI20 against RAM at 0x8000_0000. Measured: a
    program pulling `printf` and `malloc` takes ten truncations at 0x8000_0000, against
    `_impure_ptr`, `__malloc_av_`, `__sglue`, `__sread` and `__clz_tab` among others, and LINKS
    unchanged at 0xFFFFFFFF_80000000. **So the earlier reading, that R1.5 owed a stopgap for two
    libgcc helpers, was wrong in scale rather than in kind:** the same relocation class covers the
    whole C library, and nothing short of reimplementing it is a stopgap. The placement is the fix.
    A static boot table maps the kernel's high addresses onto physical RAM and turns Sv39 on, and it
    is a link-time constant nothing edits: no allocator, no `arch_aspace_*`, nothing per-process,
    exactly as S2b's identity table was. So this step does NOT need the aspace family, and
    `HAS_ASPACE` still lands at R2 with the map editor.
    *The table itself did not survive: R2.3 replaced it* with a chain counted as three non-leaves,
    plus one more per level added above them, so the sentence above describes what R1.4b built and
    not what the image carries. R3 swept the same claim out of `startup.S` and left this copy
    standing.
    *Expected:* the banner from an image linked high and running translated, and the full
    `kickos_build` link reaching its remaining undefined symbol rather than a relocation. The false
    pass to watch for is the one S2b names: a hand-written probe that only ever touches addresses
    the boot table happens to cover prints happily through a table that is wrong elsewhere.
    *TAKEN, and the shape is smaller than the A64 analogue in a way worth recording.* 33
    truncations across the fleet's 24 app links became 0, the remaining failures being
    `arch_syscall` AND `arch_syscall64` -- two symbols and not one. The table is ONE 4 KiB root
    page holding TWO level-2 leaves of 1 GiB each (Sv39 numbers the root's own level 2): the low gigabyte identity-mapped for the MMIO
    the chip names by absolute address, and 0xFFFFFFFF_80000000 onto DRAM for the kernel window
    and the map of physical RAM together. *That identity leaf is R1.4b's and R2.2 ended it,* moving
    the devices to the kernel's own alias; R3's comment sweep corrected the source copy of the same
    sentence in `chip_virt_rv64.cc` and did not reach this one. No second table page and no identity
    alias of RAM,
    because **the regime and the privilege change together**: `satp` is written in MACHINE mode,
    where it governs no access, and takes effect on the `mret` whose `mepc` is already the high
    virtual address. So F8's "mapped at the same address on both sides of the switch" is
    discharged by there being no instruction on both sides at all, and the low alias `.text.init`
    ran at is genuinely absent from the running kernel -- which is what turns a dropped
    VMA-to-LMA offset into a LINK failure (medany is PC-relative, so a low absolute symbol is
    2^40 out of an `auipc`'s reach) rather than a silent success. What it costs is one constraint
    to state rather than discover: everything between `_start` and the landing pad is
    PC-relative, so `gp` is seated PAST the switch and nothing before it may make a gp-relative
    access.
    *And this answers a question R2 was carrying:* F1's fixed high range has ONE placement on this
    architecture that costs nothing, and the bottom of Sv39's high half, 0xFFFFFFC0_00000000, is not
    it. R2 inherits the placement rather than choosing it.

  - **R1.5 The switch, the syscall entry, and the first real image.** Context init, switch, start,
    idle, U-mode for unprivileged threads and the trap back. This is the step that supplies
    `arch_syscall`, so it is where `kickos_build` first has every symbol it needs.
    *Expected:* `hello` runs. The two kernel-stack selects and the frame geometry land here, the
    entry having to load the block from `sscratch` before it stores anything.
    *A Kconfig trap this step walks into, found while a second translating arch was wired and
    verified against the ladder itself:* `KICKOS_KERNEL_STACKS` reads `default 1 if
    ARCH_HAS_KERNEL_STACKS && HAS_MPU`, so a chip that TRANSLATES gets no such default and the knob
    resolves 0 even though the arch selected the capability. `ARCH_ARMV8A` never met it because it
    also selects `ARCH_KERNEL_STACKS_MANDATORY`, whose `range 1 1` and `default 1` sit above that
    line. So this step selects the pair the way armv8a does, MANDATORY included, which is correct
    here on its own merits: the trap entry loads the block from `sscratch` before it stores
    anything and no red-zone path is written beside it.
    **What stays latent is the ladder,** whose `HAS_MPU` term is a stale proxy for protection being
    live: a translating arch that is NOT mandatory would resolve 0 and fail in the backend as a
    compile error rather than as a configure refusal. Nothing in the tree is that arch, so the term
    is not being widened here; it is recorded so the next one does not debug it.
    *TAKEN. `hello` runs, and the LATENT LADDER IS NOT LATENT: it was measured.* Dropping
    `ARCH_KERNEL_STACKS_MANDATORY` and keeping `ARCH_HAS_KERNEL_STACKS` configures CLEANLY,
    resolves the knob to 0, and fails as `static assertion failed: rv64imac's trap entry builds
    every U-mode frame on ctx.kernel_sp` plus an undeclared
    `kickos_fault_frame_on_kernel_stack`. The prediction was exact.
    *The frame is 256 bytes*, 28 GPRs (gp and tp excluded and rewritten from the kernel's own
    knowledge on every resume; **that parenthesis was false of `tp` when it was written and is true
    now**, R6's pass forcing `tp` to zero beside `gp` at both anchors, so the code caught up to the
    sentence rather than the sentence being wrong about the design) plus `sepc`, `sstatus` and the sp slot the restore epilogue leaves
    on, with 8 bytes spare that the prologue stashes `t0` in before it knows which stack the frame
    goes on. Its constants live in `arch/riscv/rv64imac/include/kickos/arch/rv64_frame.h` and NOT in
    an `<arch>_trap_stack.h`: `check_trap_redzone_decls.sh` discovers arches by that suffix and
    holds each one to a full `trap_redzone_roots.txt` record, and nothing here has been measured
    under `-fcallgraph-info`. **So this arch now OWES that file an `arch` record, a `preset` record
    and a class set**, and the trap-stack size and the two Kconfig stack figures stay marked
    PROVISIONAL until it has one. Declaring an arch there is its own step, which `armv8a` is still
    waiting for too.

    **THE U-MODE HALF IS A DIFFERENT SHAPE FROM THE ONE THIS STANZA ANTICIPATED, because the ISA
    forbids the one it named.** "Irrespective of SUM, the supervisor may not execute code on pages
    with U=1" (RISC-V Privileged ISA, the U bit). R1.4b's kernel window is ONE level-2 leaf carrying
    kernel text and app text together, so marking that leaf U-accessible does not merely over-grant:
    it takes the kernel's own instruction fetch away. Both directions were measured on the built
    image, one clause changed each time. `U` added to that leaf: the image dies BEFORE the banner,
    the fetch fault reaching an `stvec` nothing has written yet, which is R1.2's silence exactly.
    `U` left clear and an unprivileged thread started anyway: root takes an INSTRUCTION PAGE FAULT
    (`scause` 12) at its own first PC, with the banner already on the wire. So the naive grant is not
    a broad-but-working stopgap to be narrowed later; it is a non-starter, and a level-1 table is the
    FLOOR here rather than the narrowing.

    **AND A SINGLE ROOT CANNOT BE SAVED BY A FINER TABLE EITHER, which is the finding under the
    finding.** At 2 MiB the U bit follows the ADDRESS, so separating what S-mode fetches from what
    U-mode fetches means separating kernel-executed text from app-executed text in the LINK. The two
    sets INTERSECT: measured on this tree, the three kernel-side archives reference `memcpy`,
    `memset`, `strlen` and `kvsnprintf` into `libkickos_lib.a` and `__clzdi2`, `__ctzdi2` and
    `__register_frame` into libgcc, and the app and the C library reference the same names. A static
    link gives a global symbol ONE address, so the shared set has to be DUPLICATED under private
    names, which is `kickos_privatise_runtime` plus in-tree helpers, which is T5b. A single-root
    split at this step would therefore have been R2.

    **THE SHAPE THAT RAN AT THIS STEP WAS TWO ROOTS, ONE PER PRIVILEGE LEVEL, AND R2.2 RETIRED ALL
    OF IT.** Everything in this paragraph is R2.1's record of what R2.1 shipped, kept because the
    measurements under it are what justified the later collapse; read it in the past tense. What no
    longer exists: the root pair itself, the trap entry's kernel-root write, the restore epilogue's
    user-root write on `SPP` = 0, the `sfence.vma` at each privilege transition, and the forced
    `sstatus.SUM` (R2.2 collapsed the pair, and R6 went further and never sets `SUM` at all, every
    kernel touch of process memory going through the `kaccess` seam instead). The kernel root leaves the whole
    window `U`-clear, so S-mode fetches anything; the user root points its level-2 entry at one
    level-1 table whose leaves carry `U` above a PRIVILEGED SPAN of one 2 MiB leaf, and the level is
    what selects the root: the trap entry writes the kernel root before it fetches anything outside
    that span, and the restore epilogue writes the user root only when the frame it is popping says
    `SPP` is 0. `virt_rv64.ld` puts `.text.init`, a new `.text.privtrap` and `.mmu_boot` in that span
    and starts every other section at its top, so the page tables themselves are out of an
    unprivileged thread's reach. `sstatus.SUM` WAS forced on by the restore epilogue rather than
    carried per frame, because the prologue stores through the trap stack while the user root was
    still live. **NOTHING SETS THAT BIT ON THIS PORT ANY MORE**, R6 having removed the write
    outright, so the present-tense reading of this sentence is false and a kernel touch of process
    memory goes through the `kaccess` seam's acquire pair (`arch_aspace_acquire` /
    `arch_aspace_release`, one granule at a time) and never through the translation the core is
    running. Three placements were load-bearing at THIS step and each was measured by deleting it:
    the user-root write (root cannot fetch at all), the span placement of `.text.privtrap` (the
    first U-mode trap cannot be fetched by S-mode, so the image goes silent after the banner), and
    the `SUM` force (same silence, one trap earlier).
    *What it costs, stated rather than hidden:* three 4 KiB tables instead of one, a 2 MiB span of
    which a few kilobytes are used, and an `sfence.vma zero, zero` on each privilege transition
    because both roots use ASID 0. Distinct ASIDs retire both fences and need no other change; what
    they needed first was a MEASUREMENT of `ASIDLEN`, which is WARL and may be 0, an unmeasured ASID
    being a silently shared TLB entry rather than a slow one. **R4 TOOK THAT MEASUREMENT**, by a
    standalone M-mode probe writing ones across `satp.ASID` and printing what stuck: `rv64`,
    `thead-c906`, `rva22s64`, `veyron-v1`, `sifive-u54` and `max` all answer `0xffff` on QEMU 11.0.3,
    sixteen bits and contiguous. So the figure is no longer owed on this bench; what R4 records as
    still unwitnessed is a zero-width MACHINE, which no model here provides, and the port allocates
    no identifier at all (R4).

    **WHAT THE OVER-GRANT EXPOSES, and it is a real R2 obligation and not a source comment.** Above
    the privileged span the user root marks every 2 MiB leaf of the image `U` with R, W and X, so an
    unprivileged thread can read, write and execute: all `.rodata`, the whole of `.data` and `.bss`
    (the scheduler's state, the capability table, the TCB array), THE PER-THREAD KERNEL STACK BLOCKS
    and the trusted trap stack, the frame pool, the heap and every other thread's stack. What it can
    NOT do is the part worth keeping, and the FIRST HALF OF IT WAS WRITTEN TOO WIDE AND IS CORRECTED
    HERE: what an unprivileged thread cannot execute is the PRIVILEGED SPAN, the 2 MiB leaf holding
    the reset path and the trap entry, and NOT "kernel text". `virt_rv64.ld` puts `.text.init` and
    `.text.privtrap` in that span, five text symbols, and sends every other text symbol to
    `.apptext` through `*(.text .text.*)`, which the user root marks `U` with R, W and X: 307
    symbols, the scheduler, `syscall_dispatch`, `arch_switch` and the fault reporter among them.
    Measured by running it: an unprivileged victim thread CALLED kernel `arch_mpu_min_region` at
    `0x80200552` and it returned 0 with no fault, and a U-mode read of `arch_switch`'s text answered
    `0x23`. The same harness faults in both directions, so it is not reporting one blindly: a U-mode
    read of `kickos_rv64_stvec` INSIDE the span takes `scause` 0xd, and so does the UART LSR read.
    The second half stands and is witnessed the same way: an unprivileged thread cannot reach ANY
    device register, the MMIO identity leaf existing in the kernel root alone, and a thread reading
    the UART's LSR takes a load page fault at `0x10000005`.
    *The posture does not change and the correction is still worth making.* The enumeration above
    already concedes `.data`, `.bss` and the kernel stack blocks are U-writable, which defeats
    isolation on its own, so nothing rested on the execute claim. What may never stand is a claim
    that something IS protected when it is not, and R2.2 keys on this enumeration.
    *So the kernel-stack transfer this step ships is a ROBUSTNESS mechanism here and not yet an
    isolation one*, which is the same thing `KICKOS_KERNEL_STACKS`'s own help text says of a chip
    with no MPU, and it says so for exactly this reason.
    *The cost to narrow it is known and it is R2's, because R2 re-decides the same boundary.* Every
    further boundary is another 2 MiB leaf plus an archive selector: kernel `.rodata`/`.data`/`.bss`
    below it and the app's above, which also needs `Reset_Handler`'s single copy range and single
    zero range split in two, as `virt_arm64.ld` already does. Three more boundaries is at most 6 MiB
    of pad on a 64 MiB DRAM share, and the level-1 table itself is already paid for here.
    **A W^X SPLIT ON KERNEL TEXT IS NO LONGER BLOCKED**, that level-1 table being what R1.4b said it
    was waiting for; it is not taken here because the span this step needs is a PRIVILEGE boundary
    and the write-execute one falls elsewhere.

    *FAULT CONTAINMENT LANDED HERE and was not in the step's list, because the step made the old
    answer false.* `arch_fault_is_user_thread` read "no trap entry builds a resumable frame here", and
    this one does. The redirect rewrites THREE fields of that frame and not the AArch64 sibling's
    two: `sepc`, `sstatus`, and `F_SP`, because the exception return reloads `sp` out of the frame
    where `eret` switches to a stack pointer register of its own. The third is witnessed by
    `faultsurvive_off`, whose worker parks its sp OUTSIDE its own stack: with the `F_SP` write
    deleted the death stub runs privileged on that buffer, the console output comes out corrupted
    mid-word and root dies after it.

    *THE INTERRUPTED U-MODE STACK POINTER IS STORED, CARRIED AND RESTORED, AND EVERY DEREFERENCE OF
    IT HAPPENS AT U-MODE, which is what makes it safe to accept whatever the thread parked there.*
    The prologue's first instruction swaps `sp` with `sscratch`, so the whole trap runs on the
    trusted trap stack and the thread's own value reaches memory as a store into the frame's `F_SP`
    slot (`arch/riscv/rv64imac/switch.S`). `.Lrestore` loads it back into `sp` in the instruction
    before `sret`, so the first access through it is the thread's, under its own translation, and
    F5's rule ends the task that chose it. `arch/riscv/rv32imac/switch.S` and
    `arch/rx/rxv3/switch.S` build their trap frames ON the interrupted stack, so their prologues
    test it before the first store.
  - **R1.6 Interrupt controller, timer, clock, and the preset witness.** The controller behind the
    existing mask/unmask/clear triad, the tickless one-shot, and the board joining the ctest ladder.
    *Expected:* the selftest passes and the preset is registered. **The timer needs no M-mode
    resident shim here and that is a CHIP fact rather than an arch one:** the emulated core carries
    Sstc, so supervisor code arms its own comparator directly, while the part F8 names as the silicon
    witness has no Sstc and supplies its own path from the chip layer where the timer already lives.

    *TAKEN. hello's ping-pong alternates, the board carries 38 of 38 on its own ladder, and the
    step corrected R1.5 on a hole neither of us saw.* No PLIC: the console is polled and the
    timebase is the LOCAL supervisor timer, so no external source exists in the image and a
    controller driver would have had no arm. What `arch_irq_*` means here is a software bitmask
    whose one raise rides `sip.SSIP`, which supervisor code may set itself, and the selftest's
    eleven irq arms go red when the doorbell is deleted.
    *The masking question R1.5 handed over is ONE decision and not two, which is the finding.*
    Dispatch stays masked, as arm64 does and rv32 does not, and that is what keeps every interrupt
    frame a resumable thread context on a stack outliving the trap. Because interrupts now exist,
    an interrupt taken in supervisor thread context produces a frame that may BECOME a thread's
    saved context, so the entry's supervisor leg had to widen to keep it on the interrupted stack
    pointer rather than the shared trap stack. Masked dispatch is what makes that widening
    sufficient; unmask it and it stops being. The cost of unmasking would land on the kernel BLOCK
    and not the trap stack, three frames plus a dispatch depth against a `static_assert` that
    checks two, on figures nothing has measured under a callgraph pass.
    **AND R1.5 LEFT A HOLE THE STEP HAD TO CLOSE, found by reading the LINKED image rather than the
    archives.** Twelve kernel globals resolve gp-relative in the linked kernel, 22 accesses, and
    among them are STORES to the current-context pointer and to both translation roots. The global
    pointer is writable by an unprivileged thread and R1.5 re-anchored it only on the way out, after
    the dispatch, so a thread could choose where the kernel wrote the incoming context pointer and
    which root was installed. The entry now re-anchors before it calls, two instructions per trap.
    *The method matters more than the fix:* an object-level relocation sweep reports NO gp-relative
    references in any archive, and that is a false clean bill, because gp addressing is created by
    LINKER RELAXATION out of ordinary upper/lower pairs. It does not exist until link time, so only
    the linked image can be asked. The arm is unwitnessed, nothing in the tree setting a hostile
    global pointer, and R1.5's own over-grant means it buys an attacker nothing until R2 narrows it.
    *One partial, declared rather than noted:* `periph_reg_write_unheld`. A translating arch refuses
    every region shape, so the device window is declined at every base the arm tries. The control
    that says the predicate is the ARCH and not the enforcement posture is `qemu-riscv-flat`, which
    also has no region enforcement and reports zero partials, its PMP port encoding a window
    regardless. An undeclared partial FAILS the stream gate, so this is a declaration and not a
    comment.
    *Four gates are held back specifically because this board does not isolate memory* and each is
    earned by R2: the region-fault arm, the root-fault arm, the translation-fault arm and the
    kernel-half arm. The last is the ironic one, this kernel being in the high half already, but the
    gate's mechanism is the aspace one.
    *R2.2 SETTLED THAT LIST DIFFERENTLY IN BOTH DIRECTIONS.* Three of the four are registered there,
    the root-fault, translation-fault and kernel-half arms, and so is a stack-guard arm
    (`qemu_riscv64_stack_guard`) this list does not name. The region-fault arm (`mpu_fault`) is NOT
    registered and is not owed: a task-wide mapping cannot refuse a sibling domain's region, so
    registering it would assert a strengthening this backend deliberately does not make, which is
    F9's floor rather than a gap. Three more wait on other steps: the thread-pointer probe,
    the floating-point switch this ISA has no extension for, and the nesting arm that is rv32-only
    by construction.

**R2. The aspace family on a single root, with PA WIDER THAN VA.** Build, activate, map, unmap, and
the page-window pair of section 3.3.

**R2 IS THREE STEPS, SPLIT BEFORE IT RUNS RATHER THAN DURING.** R1 was written as one step and became
six, twice discovering the split by running into it, and this stanza has since accumulated ELEVEN
obligations from the steps beneath it. The split follows the dependencies, which are real: the map
editor can be written and witnessed while the image is still one linked whole, and everything that
narrows a permission waits on the app leaving the kernel's half, because that is the boundary the
permissions key on.

  - **R2.1 The frame allocator, the family, and the window pool.** `HAS_ASPACE` and the translate
    opt-in come back, the frame allocator lands over the carved pool, and create, destroy, map, unmap
    and activate get bodies over the layout R1.4b built. The acquire pair is a REAL transient window
    and not an addition, which is the litmus arm: this is the first backend in the fleet that can
    fail `ARCH_ASPACE_ACQUIRE_MIN`, a figure A64 asserts against an unbounded capacity. Name whether
    the table frames are reached another way or the walk itself runs through the pool, which this
    stanza already owes. The reporting member records a width a HUMAN wrote, 16 for this class of
    part, per R4's pre-audit; no identifier is allocated, generated or scoped, at this step or any.
    *Expected:* the aspace arms of the selftest pass on this board. The privilege-paired root pair
    survives this step untouched.

    **RESULT. TAKEN: the map editor, and it passes every arm the seam has for it.** Sv39 over the
    layout R1.4b built, written generically over a root level of 2 and a leaf level of 0 so R3 is a
    two-constant edit; create copies the boot user root's level-2 entries and destroy skips any root
    entry that still equals the template, so the kernel half's tables stay shared; activate writes
    the root the unprivileged level runs under and the switch path installs it, which is why activate
    owes no fence of its own. **TWO CLAUSES OF THAT SENTENCE WERE OVERTAKEN, one below in this
    stanza and one at R2.2.** The generic walk held, but three constants beside it were Sv39 by
    value, so R3 was a five-constant edit rather than a two-constant one; its stanza has them. And
    activate DOES own a fence: the unwitnessed-and-why paragraph below rests on
    `arch_aspace_activate`'s own full fence, and once R2.2 deleted the switch path's root write that
    fence is the only invalidate a root change gets, one of the ten `sfence` instructions R4 counts.
    Nine arms green on the forced-on tree: `aspace_seam`, `aspace_model`
    (`granules 0x1, 16 ASID bits, 56 PA bits, verdict 0x7`), `aspace_map_cycle`, `aspace_translate`,
    `aspace_refusals` (all seven), `aspace_span`, `aspace_balance`, `aspace_domain_balance`,
    `split_access` (63 of 63), plus `aspace_acquire_balance`, `self_grant_retype`,
    `grant_kernel_word_refused` and `reent_seating`.

    **THE TABLE WALK IS ANSWERED AND IT IS THE FIRST OF THE TWO OPTIONS.** Table frames are reached
    another way: the frame pool is carved inside the image's DRAM share, which the kernel root's one
    level-2 leaf already maps, so a table is read at its output address plus
    `KICKOS_RV64_KERNEL_WINDOW_SIZE`'s delta and the walk spends no window slot. The window is for
    frames OUTSIDE that leaf's span, and the leaf's span is where the delta is DEFINED rather than
    merely convenient: `startup.S` maps exactly one gigabyte there and the constant is asserted
    against the leaf.

    **THE WINDOW POOL, ITS SIZE, ITS ASSERT AND ITS FLOOR, MEASURED.** Six slots per core, sized
    `ARCH_ASPACE_ACQUIRE_MIN` and asserted against it and against the 512-entry level-0 table the
    chip supplies. `KICKOS_RV64_WINDOW_VA` is `0xFFFFFFFFC0000000`, a level-2 slot of the kernel root
    that nothing else fills, with the two non-leaf entries under it filled by `_start` and the leaves
    written by the editor. At the floor and past it, over a 600-page device mapping where every
    acquire spends a slot: 6 of 8 answered, the seventh and eighth null, the first slot at the window
    base; after releasing all six, an acquire answers the base again. So the refusal is a null answer
    a caller already checks, distinguishable from corruption because no slot is disturbed and the
    pool recycles. Cutting the capacity to five fails the `static_assert` by name at compile time:
    this is the first backend in the fleet that can fail that constant.

    **AND THE LITMUS PRODUCED ITS FINDING, WHICH IS THAT THE WINDOW CANNOT BE THE WHOLE ANSWER.**
    Three places above the seam require `arch_aspace_acquire` to hand back a pointer with an OFFSET
    MAP's properties, and none of them can be satisfied by a transient window:
      - `op_alias` (`KOS_ASPACE_OP_ALIAS`) requires the answer to equal `frame_pool_ptr(frame)`
        exactly, so that the pool's own route and the seam's agree. A window slot never equals it.
      - `aspace_frame_token` (`kernel/mem/aspace.cc`) names a frame by subtracting two acquire
        pointers and dividing by the granule. With six slots that arithmetic answers the same small
        number for every frame, so the fourteen arms that compare frame identity across tasks would
        read "same frame" for frames that differ.
      - `op_span` required page `i` to answer `first + i * granule` across 600 pages.
    The first two are why acquire adds the kernel window's delta for a frame inside that window and
    windows only what falls outside: the offset is not a shortcut, it is what those arms mean. The
    third IS fixable above the seam and was fixed here, to per-page checks that survive both kinds of
    backend: every page answers, the answer keeps its offset within the granule, and it is a
    different granule from the one held beside it. **What the seam owes, and it is not taken here
    because arch.h is frozen for this step:** a query that names the frame backing a page,
    `arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)`, answering 0 for
    an unmapped page. With it, `aspace_frame_token` stops doing pointer arithmetic and `op_alias`
    compares frames rather than pointers, and a backend with no offset map at all becomes expressible.
    Measured rather than argued: forcing acquire to window EVERY frame turns `aspace_translate` red
    and panics the image. The signature diff for R5 is otherwise EMPTY, 35 baseline records against
    35 candidate.

    **A SECOND SEAM FINDING, and the region family already solved it.** `arch_aspace_memtype_support`
    returns a bool where `arch_mpu_nocache_support` returns three states, and the third,
    `ARCH_MPU_NOCACHE_ALREADY`, is exactly this port's answer: an Sv39 entry carries permissions and
    no memory type, Svpbmt is absent (`menvcfg.PBMTE` reads back 0 on `-cpu rv64`, measured), and no
    data cache on this board reaches a frame an outside observer would miss. So all three types are
    honoured because none is encoded, which is what the rv32 PMP port says of its own arena. A part
    where the attribute is NOT free by construction cannot express the difference through this query
    and would have to answer false and lose the grant.

    **NOT TAKEN, AND THIS IS THE STEP-PLAN FINDING: `HAS_ASPACE` IS NOT SELECTED, BECAUSE THE SELECT
    DEPENDS ON R2.2 AND NOT THE OTHER WAY ROUND.** The split assumed the map editor could be
    witnessed while the image was still one linked whole. The editor can; the SELECT cannot. Selecting
    `HAS_ASPACE` derives `KICKOS_MEMORY_ENFORCED`, and under enforcement
    `arch_user_text_readable` and `arch_user_data_writable` return false unconditionally, leaving one
    oracle: the granted-range list `aspace_image_seed` builds from `__kickos_app_rom_start` and
    `__kickos_app_sram_start`. `virt_rv64.ld` defines neither, and cannot at this step: one flat link
    puts app text and app data inside the kernel's fixed high range, where a per-space mapping would
    have to edit the level-2 entries every space shares. Measured with the select forced on: 60 of
    132 arms fail and the first failure is arm 1, `kos_kconsole_write` refusing its own buffer.
    Retrofitting the window over the flat image was tried and does not work either: the two extents
    round to overlapping pages, `.data` needs a granule-aligned boundary, and once both are fixed the
    per-process data copy is still refused, so `process_private_data`'s three distinct frames remain
    unreachable. **So R2.2 carves the app window and flips one Kconfig line, and R2.1's editor is
    written and measured under a forced-on tree rather than gated on the board.** The three aspace
    arms that need the seed are named: `aspace_forced_unwind` and `aspace_churn` (a create allocates
    one frame with nothing seeded, against a floor of `KOS_ASPACE_UNWIND_MIN_DEPTH` = 4) and
    `map_tlbi_elided` (the seed maps nothing, so 0 elided against a floor of 32).

    **TWO MECHANISM FINDINGS THAT FAIL THE LINK RATHER THAN AN ARM.** `__kickos_frame_pool_delta` is
    an ABSOLUTE linker symbol whose VALUE is the whole offset, and `frame_pool.cc` reached it
    PC-relatively: on a high-half kernel that is a `R_RISCV_PCREL_HI20` truncation, by 2 MiB on this
    port, and the arm64 pair clears the same boundary by one gigabyte. It is now one relocated word,
    `volatile` so the value does not propagate back into each caller and become PC-relative again.
    Second, a mapping refusal on this backend can come from either of two guards: the low-half bound
    or the shared-table check that a level-1 leaf is not a table, and for `arch_ram_base()` the second
    one answers, so deleting the first leaves `KOS_ASPACE_REFUSE_HIGH_HALF` green.

    **A FRESH NON-LEAF ENTRY GOT NO FENCE THE ISA ACCEPTS, and the comment that said otherwise was
    carried across from `aspace_armv8a.cc`.** Raised by an audit of this branch (2026-08-28),
    silicon-only, and confirmed against the specification rather than argued. `map_into` covered a
    newly-valid NON-LEAF entry with the per-leaf `sfence.vma va, zero` issued below it. RISC-V
    Privileged Architecture, version 20260120, section 12.2.1, on exactly that form: "If rs1!=x0
    and rs2=x0, the fence orders only reads and writes made to leaf page table entries
    corresponding to the virtual address in rs1, for all address spaces." The same section: "This
    specification permits the caching of PTEs whose V (Valid) bit is clear." So on a hart whose
    page-walk cache holds invalid non-leaf entries, and the C906 that F8 names as this port's
    silicon witness is such a part, a self-grant that allocates a fresh level-1 or level-0 table
    leaves the new pages invisible, the app's first access to its own granted range faults, and F5
    kills the thread. **The remedy is the section's own and there is no narrower one:** "If software
    modifies a non-leaf PTE, it should execute SFENCE.VMA with rs1=x0", with rs2 at x0 too because
    nothing here tags a translation. `invalidate_nonleaf` issues it, gated on `installed` the way
    the per-page one is, so a space installed nowhere still pays nothing.
    *The cost, stated:* a whole-hart flush, once per FRESH TABLE and never once per page, so a newly
    touched 2 MiB span costs one and a newly touched 1 GiB span one more. No operand names the
    non-leaf entries for a single address, so the `installed` gate and the once-per-table placement
    are the whole of the narrowing available.
    *UNWITNESSABLE HERE, AND THE ARMS SAY SO RATHER THAN THE PROSE.* Four isolated single-site
    mutations over a rebuilt TU, each with the build log naming `aspace_rv64imac`, against a
    baseline of 22 image gates: deleting the `invalidate_all()` inside `invalidate_nonleaf` is 22 of
    22 green with every figure unchanged; multiplying its ISSUED bump by 100 leaves the figures
    unchanged too, which is the sharper result, because it proves the issued path never executes in
    this suite at all; multiplying its ELIDED bump by 100 moves the seed from 47 to 245, so exactly
    2 of the 47 are non-leaf and the elided path IS reached; and the equivalent-edit control
    reproduces the baseline exactly. QEMU models no negative translation caching, so nothing here
    can distinguish the fence from its absence and the fix rests on section 12.2.1.
    *A64's twin comment was checked and is CORRECT, so it is left alone.* Arm ARM DDI 0487 M.b,
    D8.17 RRVJDB defines a TLB entry for maintenance purposes as "any structure that holds a
    translation table entry, including intermediate TLB caching structures", and D8.17.5 IDMCXY
    makes the `L` suffix the thing that RESTRICTS an invalidate to the final lookup level, so the
    unsuffixed `TLBI VAAE1IS` this backend issues does reach the intermediate entries. **The two
    backends differ because the architectures do, and A64's is the stronger of the two:** D8.17
    IWZCBG says of an invalid-to-valid change that "TLB invalidation is not required because an
    entry that generates one of the listed faults is never cached in a TLB". One clause of the A64
    LEAF comment was wrong in the other direction and is corrected in place: it claimed A64 caches
    an absence like a presence, which RXCLRD denies. The invalidate stays, because what it covers
    is the slot that was not in fact empty, which nothing above it proves.

    **`map`'s ROLLBACK CANNOT RESTORE A MAPPING IT BROKE, and the seam contract now says so.** Same
    audit, LOW, and the shape is inherited from M6.2 rather than introduced here: a map over a range
    partly already mapped that fails at a later page leaves the earlier pages UNMAPPED rather than
    as they were, so "total or fail" reads as "total or unmapped". Falsified as reachable today,
    which is why it is one clause in `arch.h` and no code: every live caller either maps a fresh
    reservation or re-maps a range already wholly mapped, where the inner call can no longer fail.
    The sentence is in the contract so a caller that stops satisfying that precondition finds the
    limit written down instead of discovering it. **What that first caller then owes is a narrower
    rollback**, and it has two expressible forms: stop the unwind at the first page whose leaf this
    call did not create, or refuse at admission a map that meets a live leaf outside the pages the
    call has already installed. Neither is built, nothing needing it yet.

    **UNWITNESSED, AND WHY. THE STATED REASON DIED AT R2.2 AND ONE OF THE FOUR LABELS WAS WRONG,
    both corrected by an audit of this branch (2026-08-28).** What this stanza used to give as the
    reason is that every privilege transition writes `satp` and issues `sfence.vma zero, zero`, so
    no translation survives into kernel context and no invalidate the editor issues can be observed
    to matter. **R2.2 deleted both instructions:** `switch.S` writes no root and fences at no
    privilege transition. The conclusion survives, by an argument that is not a patch of that one.
    `installed_here` asks `satp` about the single running root, so an edit to the RUNNING space
    issues its invalidates FOR REAL, and a space installed nowhere is covered by
    `arch_aspace_activate`'s own full fence at the moment it becomes the running one. Measured off
    `map_tlbi_elided`'s own diagnostics: the image seed issues 0 and elides 47, a running-space
    widening issues 1. Under the OLD regime `installed_here` was always false from kernel context,
    so EVERY editor invalidate was elided and the elision itself was unwitnessable; both of those
    sentences were true when they were written and are false now, which is why the paragraph is
    rewritten rather than amended.

    **AND `THE INVALIDATE AN UNMAP OWES` IS NOT WITNESSED EITHER, which is the label that
    mattered and which this branch had to take back.** The arm the row rested on was deleted here
    for reaching a permission refusal rather than the revoked mapping; re-measured against its
    replacement, each of the four is an isolated single-site mutation over a rebuilt TU:
      - break-before-make removed: 0 failures. Unwitnessed, as claimed.
      - the fresh-map invalidate deleted: caught ONLY by `map_tlbi_elided`'s counter floor, never
        behaviourally. Held by a figure and not by an access, which is weaker than an arm and
        stronger than review. It is the ONE site of six that anything holds.
      - the invalidate an unmap owes deleted: `qemu_riscv64_aspace_ufault` stays GREEN, and so does
        every other arm on both paging modes. The arm reddens for a no-op unmap, so what it holds
        is the leaf being cleared. UNWITNESSED.
      - destroy's sweep moved after the frees: 0 failures. Unwitnessed, as claimed.
    A wrong "unwitnessed" label is what gets a future refactor of that line waved through, so the
    correction belongs in the record and not in a note beside it.

    What mutation DID catch: destroy walking into the
    shared kernel half (the image dies mid-suite), the write-execute refusal (`aspace_refusals` red),
    windowing every frame (`aspace_translate` red plus a panic), a capacity below the floor (the
    `static_assert`), and a create that copies no kernel half (the image hangs after the banner). The
    rig carries an equivalent-edit control that reproduces the baseline exactly. The identifier
    width-zero case stays unwitnessable on this bench, as R4 predicted: the hart reports 16 and the
    port records 16.

    **NOTHING WAS ADDED TO THE RESTORE EPILOGUE'S WINDOW.** `switch.S` is untouched, the epilogue is
    the same length, and the editor introduces no faultable access into it: `arch_aspace_activate`
    writes `g_rv64_satp_user` from ordinary kernel C under `arch_irq_save`, and the epilogue already
    loaded that word. The one new property is that the word is now written at run time rather than
    once at init, and it is safe for the reason the window is: `SIE` is 0 across the epilogue and the
    write is masked, so no torn read exists.
    *AND R2.2 DELETED BOTH HALVES OF THE MECHANISM THIS PARAGRAPH IS ABOUT:* `g_rv64_satp_user` and
    the epilogue's load of it are gone, `arch_aspace_activate` writing `satp` and fencing directly.
    So the measurement above is R2.1's image; after R2.2 there is no epilogue window to keep safe
    rather than a safe one.
  - **R2.2 The app leaves the kernel's half, and the root pair dies with it.** T5b's analogue, and on
    this architecture it is the CODE MODEL rather than a second linker split: there is no
    `-mcmodel=large` here, medany reaching a signed 32-bit displacement against roughly 256 GiB, so a
    kernel reference to app text goes through an indirection the linker fills. The shared runtime has
    to be duplicated under private names, R1.5 having measured the two text sets intersecting on
    memcpy, memset, strlen, the kernel's own formatter and three libgcc helpers. Archive selectors
    put the kernel's read-only data and its two writable sections below the boundary and the app's
    above, which splits the reset path's single copy range and single zero range in two. Decide which
    half owns the global pointer, its window reaching 2 KiB either side of one anchor. Then the two
    roots collapse to one and the translation write at each privilege transition becomes the write at
    each space activation, which is what this family needs anyway.
    *Expected:* the four gates R1.6 held back for want of isolation, and the three the fault arms
    need in a NEW shape rather than with substituted constants, this architecture's fault cause
    carrying no level field where the A64 syndrome does.

    **RESULT. TAKEN, and the board carries 46 of 46 with `KICKOS_MEMORY_ENFORCED` live.** The seam
    member lands, the app leaves the kernel's half, the root pair is gone, and the differ reports
    exactly one record: `FUNC arch_aspace_frame_at arch_phys_addr_t (struct arch_aspace *,
    uintptr_t)`, added, with all 35 other records identical. `qemu-arm64` is 33 of 33 with the same
    member fitted, so the diff is a MEMBER and not a divergence.

    **THE APP'S HALF IS LOW, ITS LMA IS HIGH, AND NO PLACEMENT MAKES THEM ONE.** This is the finding
    the step did not have: `aspace_image_seed` mapped `text.base` onto `text.base`, an identity that
    is a property of A64's link and not of the family. The prebuilt libc and libgcc multilibs are
    medlow, which reaches `[0, 0x7FFFFFFF]` and `[0xFFFFFFFF80000000, 0xFFFFFFFFFFFFFFFF]` and
    nothing between, while every byte of this machine's DRAM is at or above `0x80000000`. So an
    identity-linked app window does not exist on this board at any address: the app links at
    `0x40000000` and loads at `0x80200000`, one uniform delta for both its regions, published as
    `__kickos_app_load_delta` and added by the seed. Two symbols of `kernel/mem/aspace.cc` move with
    it and nothing above the arch seam does.
    *And the placement is one whole level-2 slot, index 1, which is the load-bearing part.* A space
    is built by copying the boot root's own entries, so any slot that root FILLS is shared by
    every space and a per-space mapping inside it would edit a table another space is walking. At
    the mode this step ran under the boot root IS the level-2 table and it fills three slots, all
    three in the high half, so the whole low half is per-space; a deeper mode shares fewer root
    slots and not more (R3).

    **THE DEVICES MOVED OUT OF THE LOW HALF AND THAT WAS FORCED, not tidied.** The chip named the
    UART, the CLINT and the test finisher by identity, which put a 1 GiB device leaf in level-2 slot
    0 of every space, and a per-space mapping anywhere in the first gigabyte was then refused for a
    reason no caller could see: 13 of 132 arms red, `op_span`'s `0x201FF000` among them. They are
    named at `KICKOS_RV64_VA_BASE + physical` now, the SAME delta the kernel window uses, so one rule
    turns any physical address below 4 GiB into the kernel address that reaches it.
    `arch_reserved_blocks` keeps the PHYSICAL values, which is what a grant names on a translating
    backend and what identity had made indistinguishable.

    **THE SHARED RUNTIME NEEDED NO NEW MECHANISM AND ONE NEW MAP.** `kickos_privatise_runtime` and
    `kernel/klib` are both already gated on `KICKOS_HAVE_ASPACE`, so selecting it gave the kernel its
    own `kmemcpy`, `kmemset`, `kstrlen` and `kfmt_vsnprintf` and rewrote every compiler-emitted
    reference; the whole of `libkickos_lib.a` went app-side with the toolchain archives. What did NOT
    transfer is the two bit-count helpers: rv64imac names no bit-manipulation extension, so
    `__builtin_clzll` and `__builtin_ctzll` lower to libgcc calls, and libgcc is app-side.
    **The first cut defined them in-tree under the ORDINARY names and that is wrong in a way no build
    in the tree could show.** A kernel-side `__clzdi2` is the definition the whole link sees, so an
    APP-side libgcc member that needs it makes a call the halves cannot carry. No shipped app pulled
    soft float, so every image linked; the first probe that printed a `double` failed to link on
    `__adddf3` and `__floatunsidf` calling `__clzdi2`. They carry private names now, through
    `cmake/kernel_runtime_rv64imac.syms`, a per-arch map beside the fleet-wide one, and
    `check_kernel_runtime.sh` refuses both ordinary spellings so the rewrite cannot silently stop
    reaching.

    **THE APP OWNS THE GLOBAL POINTER, AND THAT DECISION HAS A GATE UNDER IT.** The app's
    `-fexceptions` translation units need gp-relative small data, and gp anchors one point, so the
    anchor is in the app's window and the KickOS-owned archives are built `-msmall-data-limit=0`
    here as they are under PMP. **The hazard that creates is the one R1.6 found, arriving from the
    other side.** gp addressing is MADE BY THE LINKER: a kernel reference to an app-half symbol that
    lands within `gp +/- 0x800` is RELAXED onto the app's anchor and links silently, where the same
    reference out of range is an auipc truncation and fails loudly. Measured, not reasoned:
    `kmain`'s store to `kickos_init_args` came out as `sd a1,-2000(gp)`, a kernel store through a
    register an unprivileged thread writes. Every cross-half reference is a relocated 64-bit word
    now, and **a local `volatile` pointer does not do it**: that stops the value being folded and not
    the address being materialised inline, which is why the words are at namespace scope, as
    `frame_pool.cc`'s delta already was. `tests/static/check_riscv_kernel_gp.sh` reads the LINKED
    image, the only place gp addressing exists, and refuses every instruction naming gp bar the
    anchor loads: 42,990 instructions of kernel text across three images, nine anchor loads, no
    other hit.

    **AND THAT GATE COVERS A SUBSET DECIDED BY WHERE THE LINKER PUT THE ANCHOR, which an audit of
    this branch (2026-08-28) established and which the step did not see.** The reasoning above says
    the halves are out of each other's reach, and the linker script asserts it, but the ASSERT and
    the reasoning are both about `medany`'s DISPLACEMENT. `0x40000000` is inside `medlow`'s
    ABSOLUTE reach, so a kernel reference to an app-half symbol is not truncated: the linker
    RELAXES it to `lui`+`addi` and the link succeeds. **Measured over the same three images with one
    store to `kickos_init_args` injected inside a syscall:** in `cxxtest` the symbol landed 1800
    bytes from that image's anchor and came out as `sw a5,-1800(gp)`, which the gp gate refuses; in
    `selftest` and `hello` it landed outside the anchor's 2 KiB reach and came out as an ordinary
    `lui`+`addi` store, which the gp gate PASSES. So the gate saw one image of three, and which one
    is an address-layout accident rather than a property of the reference. The full image boots and
    runs: at the time of that audit `sstatus.SUM` was set for the life of kernel context and the
    running space mapped the app's half `U`, so both the read and the write SUCCEEDED, and the suite
    was 132 of 132 with a kernel store landing in whichever process was on the core. **R6 REMOVED
    THE `SUM` WRITE AND NOTHING SETS THE BIT NOW**, so on this tree such a store FAULTS instead of
    landing: the kernel reaches process memory only through the `kaccess` seam's acquire pair,
    which names the frame the target SPACE's own tables hold and reads it from the kernel's half,
    never through the translation the core is running. The audit's finding about the gate's corpus
    is unaffected, and the injected store is still the way to reproduce it.

    **THE GATE THAT CLOSES IT IS `tests/static/check_riscv_kernel_apphalf.sh`, and its corpus is the
    kernel archives' RELOCATIONS rather than the image's disassembly.** That is the opposite of the
    gp gate's answer and the reason is that a value scan cannot work here: `grant_hits_reserved` and
    `grant_region_admissible` materialise the Cortex-M bit-band constants `0x40000000` and
    `0x40100000` in generic kernel code, and those are the same numbers as `__kickos_app_rom_start`
    and `__kickos_app_sram_start` on this board. Separating them from a real reference would need a
    pattern allowlist, which is the one thing this gate must not have. A relocation instead names
    the SYMBOL an instruction operand resolves to, and relaxation rewrites the encoding without
    changing that name, so the archives are exact. Which symbols are app-half comes from the IMAGE,
    the only place that fact exists: every GLOBAL or WEAK symbol in
    `[__kickos_app_rom_start, __kickos_app_sram_end]`, closed at the top because a one-past-the-end
    materialisation reaches app bytes by subtraction and `_kickos_heap_limit` is exactly that
    address. Binding is load-bearing and a LOCAL symbol would be a false positive: `kfmt.cc`'s
    anonymous-namespace `Sink::put` and `emit_uint` carry one mangled name at BOTH an app-half and
    a kernel-half address in every image here. Corpus on this board: 311 app-half GLOBAL/WEAK
    symbols, 4,614 instruction and 3 data relocations in kernel `.rela.text`, 4 symbols reached.
    *The allowlist is four names and each is individually load-bearing, measured by removing it:*
    `kickos_root_entry` and `kickos_user_thread_return` are PCs that are jumped to and never
    dereferenced; `_kickos_heap_start` and `_kickos_heap_limit` are the two ends of one printed
    subtraction. Removing any one turns the gate red naming that symbol and the kernel function it
    came from, and an entry no instruction reaches any more fails as STALE, so the list cannot drift
    into a record of what used to be true.
    *What it does not catch, stated:* a literal address written in kernel source, which carries no
    relocation. The class the finding is about is a reference to a SYMBOL, and a literal
    `0x40000000` in kernel code is indistinguishable from the bit-band constant that is already
    there. A relocated 64-bit word is out of scope by design, that being the sanctioned mechanism;
    the gp anchor word in `.text.privtrap` is the live instance and the data words are counted and
    NAMED in the corpus line so a new one is a figure that moved rather than an invisible pass.

    **WHAT ELSE THE KERNEL REACHED INTO THE APP'S HALF FOR, and none of it was in the step's list.**
    Three boot-time reads and writes, all before any space exists, so the app's virtual addresses
    name nothing yet: `kickos_init_args`, the `kickos_reent_seam` descriptor, and the per-app build
    stamp. The first two go through `aspace_image_alias`, the kernel's own alias of those bytes,
    which needs no space and is portable because F1 puts a map of all physical RAM in the kernel's
    half. The third could not be fixed that way because it was a CALL: `kickos_app_build_stamp` was a
    function in app text, and kernel text may neither fetch it before a space exists nor call it
    afterwards. It is `kickos_app_build_raw`, a weak DATA array of `__DATE__`, `__TIME__` and the
    zone, and `kmain` does the reformatting. The banner line is unchanged on every board.
    *A64 reached the same call and survives it for a reason that is its boot table's,* which the T5b
    record already states: the boot identity root leaves the app's code block privileged-executable.
    That is a property of an identity-mapped app window, not of translating backends, and this port
    has none.
    *The unwind-table registration moved for the same reason and in the same direction.*
    `__register_frame` is libgcc's and app-side, and the table it walks describes app text, so
    `Reset_Handler` cannot reach either. It is `kickos_root_entry`'s now, under
    `KICKOS_HAVE_ASPACE`, ahead of the app ctors that might throw. Both translating chips drop it
    from their reset path; on A64 the symbol is absent from every image measured, so that call was
    dead there already and `cxxtest` is unchanged on both boards.

    **THE ROOT PAIR IS GONE AND SO IS THE EPILOGUE HAZARD.** `arch_aspace_activate` writes `satp` and
    fences, which is the only place the root moves after boot; `switch.S` writes no root and issues
    no `sfence.vma` at any privilege transition, and `g_rv64_satp_kernel` and `g_rv64_satp_user` are
    deleted. **The window the hazard lived in does not exist any more rather than being shortened:**
    the restore epilogue's root write and its fence are the instructions that are gone, so there is
    no interval in which supervisor mode runs with a root the entry would have refused, and the
    entry's supervisor leg no longer asserts anything about which root is live. `installed_here` asks
    `satp` itself, one fewer shadow of the hardware to keep in step. No identifier is allocated,
    generated or scoped, at this step or any (R4).
    *What the single root COST at THIS step was one line and it is not obvious:* `sstatus.SUM`. Under
    the pair the kernel ran on a root where the image was U-clear, so a kernel read of app memory
    needed nothing; with one root every app page carries `U`, and S-mode may not touch such a page
    without SUM. The epilogue already forced it into every resumed frame; what had no owner was the
    window BEFORE the first resume, where the boot path copies out of the app's half and there is no
    frame to carry it, and R2.2 gave that window to `kickos_rv64_init`. **THAT LINE IS GONE AND THE
    COST WAS PAID DIFFERENTLY IN THE END:** R6 removed every write of the bit, and the kernel reaches
    process memory through the `kaccess` seam's acquire pair instead, so no reader of this paragraph
    may restore a `SUM` write on its authority. A kernel dereference of a low-half pointer FAULTS on
    this port, which is the hole R6 closed.
    *AND THE SECOND THING IT COST IS A CAPABILITY, recorded here because nothing else records it.*
    "One root serves both privilege levels" is true only while no S-mode thread executes app-half
    text, and what makes that hold is a hardware refusal rather than a choice: every per-space leaf
    carries `PTE_U` (`arch/riscv/rv64imac/aspace_rv64imac.cc`) and S-mode may not FETCH from a `U=1`
    page irrespective of `sstatus.SUM`. **The PAIR could do exactly what the single root cannot**,
    name one virtual address with a different execute permission per privilege level, so under it an
    S-mode thread could have fetched app-half text. That capability is gone.
    *It costs nothing today*, there being no such thread: `arch_syscall` and `arch_syscall64` are in
    `.apptrap`, which `virt_rv64.ld` folds into `.apptext`, so the ecall stub is app-half text that
    only an unprivileged thread can fetch, ECALL-from-S has no caller and cannot acquire one, and
    `arch/riscv/chip/virt_rv64/startup.S` undelegates `medeleg` bit 9 on that evidence rather than
    routing a demux nothing calls.
    *What a future step that WANTS a privileged thread running app-half code would pay* is one of two
    prices, and neither is cheap. A second root restores the epilogue's root write and its
    `sfence.vma` at every privilege transition, and with them the window this step deleted, in which
    supervisor mode runs with a root the entry would have refused. Otherwise the ecall stub and every
    other app-half instruction such a thread executes move kernel-side, back under the archive
    selector and out of the app's reach, which also puts them back inside the half the app may not
    read.

    **THE OVER-GRANT IS NARROWED, MEASURED IN BOTH DIRECTIONS OVER R1.5'S OWN ENUMERATION.** Thirteen
    single-access images, each built in a copy of the tree with one address and one access mode baked
    in, run as an unprivileged thread. REFUSED, each with the thread-kill record naming that exact
    address: kernel `.data` (store, cause 15), kernel `.bss`, the trusted trap stack, the frame pool,
    the Sv39 tables themselves, the boot/trap stack top, a READ of `arch_switch`'s text (cause 13), a
    CALL of `arch_mpu_min_region` (cause 12, the instruction fetch), the UART's LSR at the kernel's
    alias, and the UART's LSR at the identity address it used to have. GRANTED, and still working:
    the app's own data read-modified-written, and a read of the app's own text. R1.5's list is
    discharged item for item, and the granted half is not an argument: 132 selftest arms and hello's
    ping-pong run on the same image.
    *What is NOT refused, and it is F9 rather than a hole:* a sibling thread of the same task reaches
    another thread's stack and another domain's region, because a translating backend maps task-wide.
    The cross-TASK case is refused and `qemu_riscv64_rootfault` witnesses it.

    **THE GATES, AND ONE OF THE FOUR DOES NOT APPLY.** Registered: the translation-fault arm
    (`qemu_riscv64_aspace_ufault`, and see the correction below: it was
    `qemu_riscv64_aspace_fault` until a review found that arm could not reach the unmap here), the
    kernel-half arm (`qemu_riscv64_kernel_half`), the stack-guard
    arm (`qemu_riscv64_stack_guard`) and the root-fault arm (`qemu_riscv64_rootfault`), whose guard
    moved from `KICKOS_HAVE_MPU` to `KICKOS_MEMORY_ENFORCED` because the arm is about confinement and
    not about which mechanism enforces it. NOT registered: the region-fault arm (`mpu_fault`), and
    the reason is F9's floor rather than a gap. It writes a SIBLING domain's region from the same
    task, which per-thread effective regions refuse and a task-wide mapping does not; measured on
    this board, the write completes and the app prints its own "OK where the MPU is a no-op" line.
    Registering it would assert a strengthening this backend deliberately does not make.
    **CORRECTION, 2026-08-29: THE TRANSLATION-FAULT ARM NEVER REACHED THE UNMAP ON THIS BOARD, AND
    R6's OWN FIX IS WHAT DID IT.** `aspacefault`'s read is the KERNEL's, inside `op_touch_unmapped`,
    and `leaf_attrs` sets `PTE_U` on every leaf a space maps. With `sstatus.SUM` never set, a
    supervisor load of such a page faults whether the leaf stands or not, so the FIRST read faulted
    one instruction before `arch_aspace_unmap` ran (measured: `scause=0xd sepc=0xffffffff80004218
    stval=0x10000000 sstatus=0x200000100`, bit 18 clear, `sepc` naming the `lw` four bytes before
    the `jal`). The gate asserted `scause=0xd` and `stval=`<the announced page>, which a PERMISSION
    refusal satisfies identically, so its own header sentence, that a fault raised anywhere else by
    anything else cannot stand in, was false. A backend whose `arch_aspace_unmap` did nothing passed
    it unchanged, and T4's fourth transition was UNWITNESSED here.
    *No syndrome field separates the two on this architecture,* which is why the fix is not a
    stronger assertion: `scause` is 13 for an absent leaf, for a leaf without R, and for a leaf with
    `U` clear read from S-mode, and `stval` carries the address alone. Reading `sepc` against the two
    load sites tells WHICH load faulted, but under `SUM` = 0 the first one always faults, so the arm
    can never reach the second. A kernel-half ALIAS of the frame is readable and is never the page
    the unmap removes. The one level that can tell a revoked mapping from a refused access is the
    UNPRIVILEGED one, where a mapped page READS BACK.
    *So the arm moved there.* `KOS_ASPACE_OP_MAP_HERE` maps a seeded frame into the CALLING task's
    own space and hands back the address, the process reads it (the gate's control marker),
    `KOS_ASPACE_OP_UNMAP_HERE` takes the word the process read as proof both routes reached one
    frame and then announces and unmaps, and the process reads again. That read faults, contained
    rather than fatal, and `check_aspace_ufault_rv64.sh` asserts the control marker, the thread-kill
    record's `ADDR=` against the announced page, `scause=0xd` and exit 139.
    *PROVEN BY THE MUTATION THE OLD ARM COULD NOT SEE.* With `arch_aspace_unmap` returning
    `ARCH_ASPACE_OK` before clearing anything, in a copy of the tree: `qemu_riscv64_aspace_ufault`
    FAILS on `the image reported a failure instead of faulting`, while the retired gate run by hand
    over the same mutated image still PASSES with `0x10000000 faulted (load page fault) and the
    system stopped with 132`. That pair is the finding and its fix in one measurement.
    *The kernel-side arm stays registered on `qemu-arm64` and is sound there,* EL1 being permitted to
    read an EL0 page with no PAN set, so its read reaches the unmap and its syndrome pins a level-3
    translation fault. The retired RV64 script is DELETED rather than left unregistered: a gate that
    runs nowhere is re-registered by the next reader who finds it.

    **THE THREE FAULT GATES ARE THIS ARCHITECTURE'S OWN AND PARAMETERISING THEM WOULD HAVE BEEN
    DISHONEST.** Two of the three differences are cosmetic: the banner, and `stval=`/`scause=` where
    A64 prints `FAR=`/`ESR=`. The third is not. `check_kernel_half.sh`'s decisive assertion is that
    the syndrome names a PERMISSION fault and not a translation fault, because the kernel's half is
    mapped at that same address for the privileged side of the same core, so a translation fault
    would mean the kernel's own window was lost rather than the unprivileged level's revoked.
    **`scause` cannot express that at all:** 13 is a load page fault whether the leaf is absent,
    present without R, or present with `U` clear, and there is no fault-status field and no level
    beside it. Substituting a constant would have looked like a port and would have dropped the one
    thing the arm is for. What stands in for it is written into the new script: the kernel's window is
    demonstrably alive because the record is printed by kernel text and the system continues, and the
    fault is contained rather than fatal. What is NOT claimed is that the leaf exists and refuses.
    *Two arms of `faultsurvive` stay unregistered and the measurement is worth keeping.* Their guard
    reads `NOT KICKOS_HAVE_MPU`, and the comment above it says they need a hardware stack guard. The
    guard is not the requirement: they turn on the fault handler being handed an UNTRUSTWORTHY frame,
    and that needs a backend that builds the frame on the FAULTING stack. This one never does, a
    U-mode entry building every frame on `ctx.kernel_sp`, so both arms fail here by reporting a clean
    redirect where they expect an escalation. The proxy excludes the right boards for the wrong
    stated reason.

    **THE MUTATION MATRIX, ten arms over a wiped build directory each, with an equivalent-edit
    control that reproduced the baseline exactly (46 of 46) and a build-log check naming the mutated
    translation unit on every run.** CAUGHT: the seam member answering 0 for every page (8 selftest
    arms, `aspace_translate`, `aspace_span`, `process_private_data`, `task_handoff_readback`,
    `task_handoff_donor_exits`, `process_ipc_same_addr`, `process_call_reply`,
    `process_data_template`); the member answering ONE CONSTANT frame for every mapped page, which is
    the defect F8 named, 7 of those same arms; `sstatus.SUM` unset at init (21 image tests, and R6
    removed that write, so this one arm cannot be re-taken on the tree);
    `arch_aspace_activate` writing no root (21); `aspace_image_alias` dropping the load delta (21);
    the cross-half word in `kmain` reverted to a direct reference (`riscv_kernel_gp` ALONE, which is
    the gate's positive control and it is the only witness there is); the new kernel-half gate's
    asserted cause changed from 13 to 15 (`qemu_riscv64_kernel_half` alone, so that assertion is
    load-bearing rather than vacuous); and `U` added to the kernel window's leaf (21 image tests,
    though that one is not a clean over-grant mutation and the distinction is worth keeping: the ISA
    forbids S-mode FETCHING a `U` page, so the kernel cannot execute its own text, and the trap entry
    is in that same leaf, so there is not even a fault dump. `hello` produces NO output at all and
    times out. The kernel-half arm does go red, but as one of 21 collateral timeouts rather than as a
    gate catching a privilege leak, which is a weaker signal. It is R1.5's finding arriving again.
    **NOT CAUGHT, both deliberate and one worth stating carefully.** Deleting `activate`'s
    `sfence.vma` is green, for the reason below. And reverting `aspace_frame_token` to the acquire
    arithmetic the member replaced is ALSO green, which is not a blind spot: the defect is LATENT on
    this board rather than undetected. Every frame that function is ever asked about lives in the
    frame pool or the app image, both carved inside the kernel window's span, so
    `arch_aspace_acquire` takes its OFFSET route for all of them and the subtraction is arithmetically
    right here. It stops being right the moment a frame it is asked about falls outside that span,
    which is what R2.1's "the same small number for every frame" describes and which no arm in this
    tree arranges. So that caller's conversion is held by review; what IS witnessed is the member's
    answer itself, by the seven arms the constant-frame mutation turns red.

    **THE MEMBER LANDED ON A64 WITHOUT THE MASK ITS RV64 TWIN CARRIES, and the mask belongs in the
    BACKEND.** An audit of this branch (2026-08-28) raised it as a hypothesis and it is a real one:
    `arch_aspace_frame_at` walks the tables, the map unwind clears leaves and frees tables under one
    mask, and a walk interleaved with it reads a table the pool has already handed out as data. The
    rv64 twin masks and says why; the A64 one did not. Neither call site supplies the mask,
    `KOS_ASPACE_OP_FRAME_AT` and `aspace_frame_token` both taking no `IrqLock`, and neither should:
    **only the backend knows its walk reads pool-owned memory.** A region backend has no tables to
    read and would be paying for nothing, and a rule that lives half in the backends and half in the
    callers is how a caller added later gets it wrong on one board only. The contract says nothing
    about a caller-held mask and now does not have to.
    *`arch_aspace_acquire` had the same unmasked walk on A64 and is fixed with it,* found by the same
    argument rather than by the audit: on this backend acquire is an addition over `leaf_entry`, so
    it reads the same tables the unwind frees. Its rv64 twin masks.
    *NOT MUTATION-TESTABLE, and the reason is what it is.* The defect is an interleaving, so the
    witness would be a preemption landing between two specific levels of one walk; at one core with
    a deterministic emulator nothing arranges it. Measured anyway, as the negative control: with the
    mask taken back off, `qemu-arm64` is 11 of 11 image gates green over a wiped build directory
    whose log names `aspace_armv8a`, exactly as it is with the mask on. It is held by the shape of
    the two data structures and by parity with the twin that already carried it.

    **UNWITNESSED, AND WHY.** The `sfence.vma` inside `arch_aspace_activate` has no arm: at one core
    a root that was never installed has no cached translation to drop, and the mutation that deletes
    it is green. It is held by the architecture's rule rather than by measurement, and a second core
    is what would turn it into an arm. The rv32 backend is untouched and PROVEN so: 24 of 27 objects
    byte-identical against the parent commit under `-ffile-prefix-map`, the other three differing
    only in `DW_AT_decl_line`, shifted by exactly the 14 lines `arch.h` grew. **The same-tree control
    is the part worth recording:** the first run reported 27 of 27 objects differing on IDENTICAL
    input, because the build directory is embedded as `DW_AT_comp_dir`, so a byte comparison of
    objects is unusable without a prefix map and would have read as a regression in this port.

    **NOT TAKEN, AND ARGUED RATHER THAN DEFERRED: the memtype answer F8 records beside the member.**
    `arch_aspace_memtype_support` returning three states where it returns two would be a SECOND
    signature move in the same verdict, and the case for it is weaker than the case for the member in
    the one way that matters: nothing would read the third state. `ARCH_MPU_NOCACHE_ALREADY` exists
    because a caller behaves differently under it, admitting a grant it would otherwise refuse; the
    aspace family's only callers are grant admission and `KOS_ASPACE_OP_MEMTYPE`, and both treat
    "honoured" identically however it is honoured. It would also be unwitnessable on this bench,
    since no arm here can tell ALREADY from true, so it would land as a member the code has and no
    run exercises, which is exactly the class section 5's opening note warns about. And it would
    dilute the verdict: a reader of the diff could no longer tell which move the litmus forced. It
    stays a recorded finding, and the backend that makes it urgent is one where the attribute is not
    free by construction.
    *An external audit re-raised it on 2026-08-29 and the position is unchanged, with one half of
    the case sharpened.* Refusing the types outright, which that report asked for, is wrong for
    DEVICE: with Svpbmt absent the attribute IS the physical address's PMA, and for an MMIO address
    that is already I/O, so a refusal would deny a type the platform delivers. Refusing NOCACHE
    alone has a live caller, `grant_nocache_admissible` asking this member on every translating
    board. So the honest answer differs by PHYSICAL ADDRESS where the member takes none, which is
    the gap this record already names, and the change that would close it is a per-chip seam rather
    than a third state. Detail at `TODO.md`.
  - **R2.3 The kernel's own mapping stops being writable and executable at once.** Named by R1.4b
    as what a level-1 table would unblock, and deliberately not taken at R1.5, whose boundary was
    a privilege one.
    *This bullet's premise was wrong twice and running the step is what showed it.* R1.4b built no
    level-1 table at all, its record being one root page holding two level-2 leaves; R1.5 built
    one, under the USER root of the privilege-paired pair; and R2.2 DELETED that pair and its
    table with it. So the mechanism this step was recorded as inheriting did not exist when the
    step ran, and R2.3 builds a level-1 table of its own under the kernel window's slot. The cost
    is one table page and one non-leaf entry, which is what R1.4b priced, so the plan's arithmetic
    survives its attribution.
    *Expected:* kernel text maps read-execute and kernel data read-write, with an arm that fails if
    either is widened.

    **RESULT. TAKEN, and the board carries 47 of 47 with the selftest at 132 of 132**, 0 skipped
    and the one declared partial (`periph_reg_write_unheld`) unchanged. The 47th is the new arm.
    `check_aspace_sigdiff.sh` is untouched by this step and still reports DIFF with exit 2 over
    one added record, 36 candidate against 35 baseline: nothing here reaches the seam.

    **ONE MORE LEVEL AND NO FINER GRANULE, AND THE BOUNDARY WAS ALREADY DRAWN.** The kernel
    window's level-2 slot holds a level-1 table now (`kickos_rv64_kernel_l1`), 2 MiB per leaf,
    and the write-execute boundary is the KTEXT region boundary `virt_rv64.ld` has drawn since
    R2.2: leaf 0 covers that region read-execute and every leaf above it is read-write, U clear
    throughout. `KICKOS_RV64_KTEXT_SIZE` is 2 MiB exactly, so the split is ONE leaf against 511
    and no level-0 table is needed anywhere. **The table is written over the region size
    rather than over that 1**, and both guards on it are measured: `startup.S` refuses a size
    that is not a whole number of 2 MiB leaves, by name at assembly time, and `virt_rv64.ld`
    refuses kernel `.data` linked below the boundary, by name at link time when `.data` is moved
    into KTEXT. At 4 MiB the same source gives 2 read-execute leaves against 510 and the board is
    still 47 of 47, so the multi-leaf path is exercised and not merely written. The app's load
    window needs no ASSERT of its own: the delta ASSERT R2.2 added already pins it to exactly
    `KICKOS_RV64_DRAM_BASE + KICKOS_RV64_KTEXT_SIZE`.
    *What the level costs is one non-leaf entry `_start` has to fill*, for R1.4b's own reason:
    a non-leaf entry holds the next table's frame number and a shift of a relocatable symbol is
    not a relocation any object format carries. So the image carries a zero in that root slot and
    the arm below reaches the table by SYMBOL rather than by walking the root.
    *The whole gigabyte stays described, as the single leaf did*, so a frame anywhere in
    `KICKOS_RV64_KERNEL_WINDOW_SIZE` is still reached by adding the delta and
    `arch_aspace_acquire`'s offset route is unchanged. Narrowing the map to
    `KICKOS_RV64_DRAM_SIZE` would move `in_kernel_window`'s bound and reroute that decision, so
    it is a different one from this step's and is not taken here.

    **THE BOOT PATH INSTALLS THE FINAL PERMISSIONS DIRECTLY AND NARROWS NOTHING.** The three
    writes `Reset_Handler` makes are all on the writable side already: the `.data` copy is the
    self-copy R2.2 left it as, the `.bss` zero is in the RAM region, and the `.appbss` zero goes
    through the kernel's alias of the app's load window, which is leaf 1. Nothing it writes and
    nothing it fetches crosses the boundary, so there is no window in which the kernel runs
    under wider permissions than it ends with.

    **WHAT THE KERNEL WRITES INSIDE WHAT BECOMES READ-EXECUTE IS A PAGE TABLE, and it is the
    finding.** `arch_aspace_acquire` and `arch_aspace_release` write the transient window's
    LEVEL-0 table on every slot they take and drop, and that table sat in `.mmu_boot` beside the
    root. It is `.mmu_leaves` now, its own section homed in the writable half, loaded rather than
    NOLOAD because `_start` fills the level-1 entry naming it before anything zeroes `.bss`. Its
    three siblings stay read-execute and are correct there: the root, the kernel window's own
    level-1 table and the transient window's level-1 table are never written after boot, and
    `_start`'s three non-leaf fills happen untranslated in machine mode, where PMP and not a leaf
    governs the store. Measured both ways: left in `.mmu_boot` the image fails the new arm by name
    AND dies at run time, a store page fault on the first acquire with the selftest stopped after
    arm 1.
    *Nothing is read out of what becomes read-write-no-execute*, and R2.2 is why: the app's build
    stamp is already a weak data array rather than a call, the cross-half references are already
    relocated words, and the `.init_array` walk is a read. What did need correcting is two output
    sections that carried SHF_WRITE from their INPUT sections while sitting in the read-execute
    leaf, `.init_array` and `.kickos_app_fini_array`. Both are `(READONLY)` now, as
    `.kickos_app_init_array` twenty lines away already was. Both are empty on every image the
    fleet builds today, so this changes no byte of any current image; without it the first
    kernel-side global constructor would put a writable section under an executable leaf and the
    arm would refuse an image that is not actually wrong.

    **THE ARM READS THE LINKED IMAGE, AND THE BOUNDARY IT HOLDS THE TABLE AGAINST IS THE IMAGE'S
    OWN.** `tests/static/riscv_kernel_wx.py`, registered as `riscv_kernel_wx` over the same three
    images `riscv_kernel_gp` reads. It decodes the 512 leaves out of the ELF and refuses a leaf
    that is writable and executable at once, one that grants the unprivileged level, one that is
    invalid, out of order or no longer a leaf, and one that sets a bit above the output address.
    Then it cross-checks: every section the image marks executable inside the window needs X
    under it, every section it marks writable needs W, and the segment that LOADS inside the
    window while linking outside it (the app's half) needs W over the kernel's alias of it. A TLS
    section is exempt from the W demand, being the template each block is copied FROM and never
    written through that address. Floors on all three counts, so a corpus that read nothing fails
    instead of passing clean.
    **MUTATED FOUR WAYS OVER A WIPED BUILD DIRECTORY, each in a copy of the tree, with a
    build-log check naming `startup.S` on every run and an equivalent-edit control that reproduced
    47 of 47 exactly.** Kernel text made writable: 46 of 47, `riscv_kernel_wx` the ONLY red arm,
    naming leaf 0. Kernel data made executable: 46 of 47, the only red arm, naming leaf 1. The
    transient window's leaf table moved back into the read-execute leaf: 45 of 47, the arm plus
    the selftest. Reverted to R1.4b's 1 GiB read-write-execute leaf: 46 of 47, the arm refusing
    the image because the symbol is absent rather than skipping it.

    **THE HARDWARE ENFORCES IT AND NO STATIC ARM CAN SAY SO, so two privileged probes were built
    and run.** A kernel store into kernel text takes `scause` 15 with `stval` the exact address
    and the dump reading `taken from supervisor-mode`; a kernel fetch out of kernel `.data` takes
    `scause` 12 at that address. **The negative control is the half that matters:** the same two
    probes on the parent commit, where the window is one read-write-execute leaf, both COMPLETE
    and hello's ping-pong runs on afterwards. So the probes measure this step and not the port.
    They are measurements and not arms: a privileged fault is a panic here rather than a contained
    kill, so shipping them would mean a diagnostic image whose expected outcome is the panic dump,
    which is a gate of a different shape.

    **UNWITNESSED, AND WHY.** No shipped RUNTIME arm, for the reason just given, so what holds the
    hardware side is the probe pair above and not a ctest entry.

    **THE 480 LEAVES ABOVE THE IMAGE'S SHARE ARE NOT ALL OVER DEAD PHYSICAL SPACE, and the sentence
    here said they were. Re-derived 2026-08-28 and it splits in two.** The window is
    `KICKOS_RV64_KERNEL_WINDOW_SIZE` = 0x40000000, so 512 leaves of 2 MiB; the image's DRAM share is
    `KICKOS_RV64_DRAM_SIZE` = 0x04000000, so 32 of them; 480 sit above it and every one is read-write,
    `KICKOS_RV64_KTEXT_SIZE` = 0x00200000 making exactly ONE read-execute leaf at the bottom. The
    machine is where the two halves part: the harness passes no `-m`, and `qemu-system-riscv64 -M
    virt` reports 128 MiB in its own device tree, which is 64 leaves. So leaves 32 to 63, physical
    `0x84000000` to `0x87FFFFFF`, **32 leaves, are read-write over REAL RAM the machine answers and
    the image does not own**, and leaves 64 to 511, physical `0x88000000` to `0xBFFFFFFF`, 448
    leaves, are over addresses no RAM answers. 32 plus 448 is the 480.
    *What it means, and it is documentary rather than an exposure:* `U` is clear on every leaf of this
    table, so no unprivileged thread reaches any of them; what the kernel gains is the ability to
    write RAM outside its own share by adding the window's delta to a physical address it should not
    have. Nothing does, and nothing reports it if something starts. **What would bound it** is either
    a window sized to the share instead of to a round gigabyte, or a leaf count derived from
    `KICKOS_RV64_DRAM_SIZE` with the remainder left invalid; both cost the offset route's uniformity,
    which is why the whole gigabyte is described today, and neither is taken here.
    *And this step draws no boundary INSIDE the read-execute leaf:* the boot table pages and the TLS
    template share it with kernel text, so read-only data is not separated from executable text,
    which would need a level-0 table and buys nothing the write-execute split does not. **The table
    page count is MODE-DEPENDENT and was written as a constant.** `.mmu_boot` holds one page per
    level the mode adds over Sv39 plus the high level-2 table, the kernel window's level-1 table and
    the transient window's level-1 table: measured off the built images by symbol address, THREE
    pages at Sv39 (`kickos_rv64_root` and `kickos_rv64_high_l2` resolving to one address there) and
    FOUR at Sv48. The one table page the kernel writes after boot, `kickos_rv64_window_l0`, is in
    `.mmu_leaves` and lands outside the read-execute leaf in both postures.
    *The rv32 port is untouched and PROVEN so:* 27 of 27 objects of `kickos_arch_rv32imac` and
    `kickos_chip_virt_rv32` byte-identical against the parent commit under `-ffile-prefix-map` of
    both the source and the build directory, with the same-tree control run FIRST and reporting
    0 of 27 differing. **The 27 re-derives and the byte-identity does not, stated so.** The
    denominator is a build away and holds: 25 objects in `kickos_arch_rv32imac` and 2 in
    `kickos_chip_virt_rv32`. The comparison itself is against a commit, so re-taking it needs a
    second checkout, which this audit did not make; what it checked instead is the premise, that no
    file under `arch/riscv/rv32imac/` or `arch/riscv/chip/virt_rv32/` has changed since, and none
    has.
*Expected:* the acquire/release window is exercised on a backend that does not direct-map physical
memory, which is the arm that fails if any helper still assumes a fixed offset. This is the single
most valuable expected result in the milestone, because it is the one A64 and x86_64 cannot produce
between them.

**THE WINDOW IS A CHOICE ON THIS TARGET AND THIS STEP USED TO CALL IT A CONSTRAINT.** Sv39 pairs a
39-bit virtual range with a 56-bit physical one, so "cannot direct-map its whole physical address
space" is true of the ARCHITECTURE and was carried across the sentence as though it bound the board.
It does not: the emulated machine reports 128 MiB of RAM with every MMIO block BELOW it, so the whole
board fits an offset map inside a 256 GiB high half with three orders of magnitude to spare, and the
same holds for any part whose RAM is smaller than that half. What genuinely has no offset to add is
physical space above the map, not the frame pool.
So the windowed acquire is DECIDED here rather than forced, and the reasons are worth stating because
a reader who takes it for a constraint will not understand why a later backend may skip it:
`ARCH_ASPACE_ACQUIRE_MIN` is asserted by A64 against a capacity that is unbounded, so nothing in the
fleet can fail it and a windowed backend is the only thing that turns that constant and its assert
from a shape into a gate; and an offset map would make the second backend agree with the first
exactly where agreement proves nothing, which is the SIMILARITY trap F8 exists to avoid.
*What the decision then owes,* because it is the part that does not follow from itself: with no
offset map the backend cannot read the tables it is walking, so this step NAMES whether the table
frames are reached another way or the walk itself runs through the window pool. Leaving it to the
implementer is how a transient-window rewrite ends up in kernel code.

**AND R2 HAS NO ANALOGUE OF T3 OR T5b, WHICH F1 REQUIRES IT TO DO.** This step names build, activate,
map, unmap and the window pair, and none of those move the kernel to a fixed high range or take the
app out of it -- work that cost M6.2 two steps, one of which the contract did not have until somebody
tried to run the step after it. F1 binds every backend, so R2 either carries that move or it is
another missing step, and saying which belongs here rather than to whoever hits it.
**HALF OF IT IS DISCHARGED AT R1.4b AND NOT HERE, because the toolchain made it unskippable:** the
kernel is at its fixed high range with translation on from that step, which is T3's analogue. What
R2 still owes is T5b's -- taking the APP out of that half -- and on this architecture that is the
code-model problem the paragraph above names rather than a second linker split.

*And R2 owes the kernel leaf's PERMISSIONS, which R1.4b deliberately did not lay down.* That step's
two leaves are 1 GiB blocks, so the kernel window is one RWX mapping: splitting text from data wants
a level-1 table and a 2 MiB pad, and R2's app-out-of-the-half work re-decides exactly that boundary,
so laying it now means laying it twice. What makes the deferral safe rather than merely cheap is that
`U` is clear on both leaves and that image has no unprivileged mode at all, so nothing which should
not reach the mapping can. **Neither of those two facts survives R2**: the step that gives the app a
half gives it a privilege level too. So a write-execute split on the kernel's own mapping is R2's
obligation and not a later tidy-up, and it is recorded here because a permission left broad while
nothing can exploit it is exactly the kind that stays broad once something can.
*AND R1.5 CHANGED WHAT THAT OBLIGATION IS, in the direction that makes it urgent rather than tidy.*
The two facts the deferral rested on are already gone: R1.5 has an unprivileged mode, and its user
root marks every 2 MiB leaf of the image above a one-leaf privileged span `U` with R, W and X. So an
unprivileged thread on the R1.5 image can read and write the kernel's `.data` and `.bss`, the
per-thread kernel stack blocks, the trusted trap stack, the frame pool and every other thread's
stack; what it cannot do is execute the PRIVILEGED SPAN or reach any device register, those being
the two things the split it does have actually buys. Not "kernel text": five text symbols sit in
that span and 307 sit above it under `U`, and a U-mode call of a kernel function above it succeeds
(the R1.5 stanza carries the measurement). R1.5 also PAID for the level-1 table, so the mechanism
R1.4b was deferring exists. What R2 owes is therefore no longer "a level-1 table and a 2 MiB pad"
but the ARCHIVE SELECTORS that put kernel `.rodata`, `.data` and `.bss` below the boundary and the
app's above it, plus the split copy and zero ranges that costs `Reset_Handler` (`virt_arm64.ld`
already carries the shape). Three more boundaries at 2 MiB each is at most 6 MiB of a 64 MiB DRAM
share. **A W^X split on kernel text is unblocked and NOT taken at R1.5**: the boundary that step
needed is a privilege one and the write-execute boundary falls elsewhere, so it stays R2's.
**DISCHARGED, ACROSS R2.2 AND R2.3, AND ONE CLAUSE ABOVE WAS ALREADY STALE WHEN R2.3 RAN.** R2.2
took the archive selectors and the split copy and zero ranges; R2.3 took the boundary itself, one
2 MiB level-1 leaf read-execute against 511 read-write. **The over-grant enumerated above is closed
and not merely re-scoped:** R2.2 re-ran R1.5's list as thirteen single-access images, and kernel
`.data`, kernel `.bss`, the trusted trap stack, the frame pool, the tables themselves, kernel text
read and kernel text called, and the UART at both its identity address and the kernel's alias are
each REFUSED with a thread-kill record naming the address; what is granted is the app's own data and
its own text. That stanza carries the figures. What did not hold is "R1.5 also PAID for
the level-1 table, so the mechanism R1.4b was deferring exists": the table R1.5 paid for hung under
the USER root of the privilege-paired pair, and R2.2 deleted the pair. R2.3 therefore paid for a
level-1 table a second time, under the kernel window's own level-2 slot. The lesson is the general
one at section 5's opening note in its other direction: a mechanism recorded as available can be
retired by a LATER step without the record noticing, so a step inheriting one re-derives it from the
built image rather than from the paragraph that promised it.
*And the privilege-paired root pair is R2's to DELETE and not to keep.* It exists because one flat
link cannot give kernel text and app text different `U` bits in one root; the step that gives the app
its own half and its own root retires the pair, and the `satp` write at each privilege transition
becomes the `satp` write at each space activation, which is what R2 needs anyway.
*A mechanism that does NOT transfer, found at R1.1:* `kickos_split_image_tu` hands one translation
unit a code model that materialises a whole address in the referencing section, because
`arch_context_init` names app text. RISC-V has no such code model: medlow reaches a sign-extended
32-bit ABSOLUTE range and medany a signed 32-bit PC-relative DISPLACEMENT, and neither materialises
a whole 64-bit address, so no flag spans the roughly 256 GiB between a high-half kernel and low app
text and the reference has to go through an indirection the linker fills instead. R1.1 leaves the
flag EMPTY on this arch, which is correct while the model is flat and
becomes this step's problem the moment it is not. Record it in the verdict: it lands as a MECHANISM
rather than as a signature, which is the shape F8 says a good seam produces, and a verdict reporting
only the signature diff would not see it.
*And its SIBLING, measured at R1.2 and the same shape:* this port emits gp-relative small data,
24 symbols of it across the kernel and arch archives, because `-msmall-data-limit=0` is applied
only where the arch is rv32imac AND a region backend is present. `check_riscv_no_smalldata.sh`
registers under the same condition, so it does not run here, and that is CORRECT rather than a
coverage hole: the hazard it names is a gp window sitting inside a PMP `.appdata` grant, and this
port carves no such window. **What makes it this step's problem is the split, not the flag.** gp
anchors one point and reaches 2 KiB either side of it, so a kernel in the high half and app data in
the low one cannot share an anchor, and any small-data reference across that boundary is the code
model's problem again in a second spelling. So this step either drops the small-data window or
states which half owns gp; joining the rv32 gate is not the answer, its premise being a grant this
port does not make.

**R3. Level count and paging mode as backend facts.** The mode is selectable on one part, so the
level count is not a constant.
*Expected:* the granule query of F7 answers, nothing above the seam names a level count, and switching
the configured mode changes no kernel code.

**PRE-AUDITED AHEAD OF THE STEP, and both halves of the expected result came back qualified.**
*Nothing above the seam names a level count, and that is now established rather than trusted:* seven
patterns over a 629-file corpus, plus the 1085-line seam header separately, with every hit
adjudicated. The seam is clean. Six real hits sit above it and only THREE are executable: a span
address and a page count in the aspace syscall's own test scaffolding encode 512 entries in the
last-level table, which survives Sv39, Sv48 and Sv57 unchanged and breaks on Sv32; the rest are
comments carrying ARM's level NUMBERING, where the leaf is level 3 and on this architecture it is
level 0. R3 owns those.
*And "switching the mode changes no kernel code" is TRUE, measured, and VACUOUS.* Measured: the tree
builds 71 objects, a control that touches the chip's startup and rebuilds changes none of their
hashes, and applying Sv48 changes exactly one, which is that startup's own object. Vacuous: THERE IS
NO CONFIGURED MODE. It is a preprocessor constant inside one chip file, named by no Kconfig symbol,
no CMake variable and no board knob anywhere in the corpus. So R3 either creates the selector, which
is what makes the claim testable, or restates the expected result as what was actually measured: the
mode is one chip-file constant and no kernel object depends on it.
*What the mode change costs, enumerated by doing it:* the mode constant, an index macro per added
level, one more table page per level, and the root's entries STOP BEING LINK-TIME CONSTANTS. A
non-leaf descriptor shifts a frame number into place and a relocation carries no shift, so those
entries must be written by the pre-translation prologue. The kernel's link address needs no rework in
any mode, all three booting at the same virtual base, so R2 inherits the placement free as R1.4b
promised. The emulated core accepts Sv39, Sv48 and Sv57 with no property asked for; the part F8 names
as the silicon witness accepts Sv39 alone.

**RESULT. TAKEN, and BOTH postures are 48 of 48 with the selftest at 132 of 132**, 0 skipped and the
one declared partial (`periph_reg_write_unheld`) unchanged, out of ONE tree with no source edit
between them: `boards/qemu-riscv64/configs/{base,sv48}/defconfig` and a preset each.
`check_aspace_sigdiff.sh` is untouched by this step and still reports DIFF with exit 2 over one added
record, 36 candidate against 35 baseline, `arch.h` unmodified. `qemu-arm64` 33 of 33, `qemu` 58 of 58,
`qemu-riscv` 49 of 49 and `sim` 48 of 48, which the span arm below is why they were re-run.

**THE SELECTOR IS A KCONFIG CHOICE, AND THE ARGUMENT FOR IT IS THE PRE-AUDIT'S OWN.** A constant is
what the tree already had, and the pre-audit's finding is not that the constant sat in the wrong file
but that no configuration names it, so the expected result cannot be tested without editing the tree.
`RV64_PAGING_SV39` and `RV64_PAGING_SV48` under `ARCH_RV64IMAC`, resolving one unprompted int,
`KICKOS_RV64_SATP_MODE`. Three things fell out rather than being built: `genconfig.py` already routes
EVERY numeric `KICKOS_*` symbol into `board_config.h` by rule, that header already says in its own
generated preamble "Pure integer macros only: also included from startup.S", and
`include_directories` already puts the generated directory on every target's path. So the mode
reaches C, the assembler and CMake with no new plumbing at all. It is NOT in
`boot_layout.ld.h`: that file is the chip's and is read by the linker script through a `cpp` run with
no arch include directory, while the level count is an ARCH fact, and the mode is not needed by the
script in any mode.

**THE LEVEL COUNT IS DERIVED FROM THE MODE'S OWN FIELD ENCODING, in one header both sides read.**
`arch/riscv/rv64imac/include/kickos/arch/rv64_paging.h`: satp.MODE is numbered so that each
successive value adds one 9-bit level, 8 for Sv39 and 9 for Sv48 and 10 for Sv57, so
`KICKOS_RV64_PAGE_LEVELS` is `MODE - 5` and `KICKOS_RV64_LEVEL_ROOT`, `KICKOS_RV64_VA_BITS` and the
`VPN` macro follow from it. No depth is stated anywhere, and a mode outside the range the board is
measured on is an `#error` rather than a computed depth. The header refuses to default the mode too:
without the symbol it errors instead of assuming Sv39, so a build that lost the generated header
fails rather than quietly translating at three levels.

**AND THE MODE THE BACKEND WRITES IS DERIVED FROM THE DEPTH IT WALKS, which is what turns one
`static_assert` into an oracle.** `SATP_MODE` is `LEVEL_ROOT + 6`, and
`static_assert(SATP_MODE == KICKOS_RV64_SATP_MODE)` holds the depth every walk runs at against the
mode the chip's prologue programs by its own route. It is not a tautology: `LEVEL_ROOT` is the value
`index_at`, `map_into`, `leaf_entry` and both sweeps use, and it is exactly the place a level count
gets stated by hand.

**THE WALK WAS ALREADY GENERIC AND R2.1's CLAIM HELD, and three constants beside it were not.**
`index_at`, `span_at`, `map_into`, `free_subtree`, `prune_empty` and `leaf_entry` are all written over
`LEVEL_ROOT` and `LEVEL_LEAF` and needed no edit. What was Sv39 BY VALUE, which the "two-constant
edit" claim did not cover: `LOW_HALF_END` at `1 << 38`, `SATP_MODE_SV39` at 8, and `PA_BITS_SV39` at
56. The first is now `1 << (VA_BITS - 1)`, the second derived as above, and the third the one figure
that is genuinely mode-independent on this ladder and is named that way.

**WHAT Sv48 ACTUALLY COST, STAMPED AGAINST THE PRE-AUDIT'S ESTIMATE, which was right in three of
four items and missed the one that moved the guards.**
  - *The mode constant:* it is the Kconfig choice, one line in a defconfig.
  - *"An index macro per added level":* NOT NEEDED, and this is the item the estimate got wrong in the
    cheap direction. Every kernel-half address takes the SAME slot at every level above 2, the
    kernel base having every bit above 32 set, so one slot number serves the whole chain and the
    prologue fills it with a LOOP that walks from the root down and stops at the level-2 table's own
    symbol. Zero iterations at Sv39, where that symbol IS the root, so both modes run the same
    instructions and no `#if` selects between them.
  - *One more table page per level:* exact. `.mmu_boot` goes from 0x3000 to 0x4000 bytes and the
    image's text grows 4072 bytes.
  - *The root's entries stop being link-time constants:* exact, and the image says so. At Sv39 the
    root page carries ONE valid entry, the device gigapage at slot 508, the other two being the
    non-leaf entries `_start` fills. At Sv48 the root page carries NO valid entry at all.
  - *What the estimate did not have, and it is what the assembler guards had to be rewritten over:*
    the root's slot NUMBERS change with the mode. At Sv39 the kernel half occupies three level-2
    slots of the root, 508 and 510 and 511, and the app owns root slot 1. At Sv48 all three are slots
    of a level-2 table, the ROOT holds one entry at slot 511, and the app owns root slot 0. So the
    shared half shrinks from three root slots to one and the guard that refused an app base colliding
    with the kernel half had to be rewritten from level-2 indices to ROOT-level indices, which is the
    only spelling correct in both.
  - *And the pre-audit's "the kernel's link address needs no rework in any mode" is now CHECKED rather
    than believed:* a high address is canonical only above the mode's own virtual width, so the mode
    re-decides it. `startup.S` refuses a kernel base or a window base that is not canonical in the
    configured mode, by name at assembly time.

**SWITCHING THE MODE CHANGES NO KERNEL OBJECT, AND THE SAME-TREE CONTROL IS THE HALF WORTH KEEPING.**
One source path and one build path with the content swapped between runs, so nothing is compared
across a differing `DW_AT_comp_dir` and no prefix map is needed. 118 objects. **The control, the same
tree rebuilt, reports 32 of them DIFFERING on identical input**, and every one of the 32 carries a
build timestamp: the generated build stamp and every app `main.cc`, which compile `__DATE__` and
`__TIME__`. So a byte comparison of app objects is unusable without pinning the clock, which is the
`DW_AT_comp_dir` trap of R2.2 arriving in a second mechanism. Against that floor, base versus sv48
differs in 34, and the two beyond the floor are exactly
`aspace_rv64imac.cc.obj` and `startup.S.obj`, one per side of the chip to arch seam. The expected
result therefore reads: two objects, both of them the ones that own a depth, and no kernel object at
all. It is no longer vacuous, there being a configured mode to switch.

**THE MODE IS LIVE AND NOT NOMINAL, and the register is what says so.** Read out of the hardware with
the debugger stopped at the landing pad, which is the first instruction that runs translated: satp is
`0x800000000008000b` on the base posture and `0x900000000008000b` on the sv48 one. Same root frame,
different MODE field. The SHIPPED arm that reads it is `aspace_model`, whose granule and
physical-range verdict bits are gated on the mode standing in satp matching the one the backend
compiled for, so a mode that did not take clears them.

**THE EMULATOR'S REFUSAL IS NAMED RATHER THAN A HANG, AND THE PROLOGUE IS WHAT NAMES IT.** satp.MODE
is WARL, so a hart that does not implement the configured mode keeps the field it had and translation
is simply not on; the `mret` then jumps to a virtual address with no `stvec` written, which is R1.2's
silence exactly. The prologue reads the field back and, on a mismatch, writes a line to the console
and the fatal status to the finisher, both at their PHYSICAL addresses in machine mode. Measured:
`-cpu rv64,sv48=off` on the sv48 image prints "KickOS: satp.MODE did not stick: this hart does not
implement the configured paging mode" and QEMU exits 132. **Both controls run:** the base image on the
SAME `-cpu` boots and runs normally, that flag gating Sv48 and Sv57 and not Sv39, and the sv48 image
on the default core is 48 of 48. The one thing this buys the fleet beyond the emulator is the part F8
names as the silicon witness, which accepts Sv39 alone: a board on it that asked for Sv48 now gets a
sentence instead of a dead machine.

**THE THREE EXECUTABLE LEAKS ARE GONE AND NOTHING ABOVE THE SEAM STATES A TABLE'S WIDTH.** `op_span`
encoded 512 entries per last-level table in two constants, a span address one page below a 2 MiB
boundary and a count of `1 + 512 + 87`. Nothing above the seam is told a table's width and no query
answers it, so the fix is a BOUND rather than the figure: an entry is at least four bytes on every
backend this seam is held against, so one table covers at most `granule / 4` pages, and a run one
page longer than that entered one page below a boundary of that span crosses two boundaries and
starts on a last slot whatever the real geometry is. At a 4 KiB granule that is 1112 pages from
`0x203FF000` rather than 600 from `0x201FF000`. The arm is unchanged in what it asserts and the change
is fleet-wide, which is why `qemu-arm64` was re-run. The two comments carrying ARM's numbering, one
at `op_span` and two in the portable selftest, name the LAST-LEVEL table now and no level number at
all.

**THE STALE COMMENTS THE CHANGE FALSIFIED, AND TWO THAT WERE ALREADY FALSE.** `startup.S`'s
"A LINK-TIME CONSTANT NOTHING EDITS" was already gone, R2.3 having replaced it with a three-non-leaf
count, which is now the three plus one per added level; `virt_rv64.ld`'s "the Sv39 root" names the
boot page tables and the chain instead. Two were false BEFORE this step and are corrected with them,
both R2.2 leftovers in `chip_virt_rv64.cc`: the MMIO addresses were said to be identity mapped, which
R2.2 stopped being true when it moved the devices to the kernel's own alias, and the file still
described a USER root that R2.2 deleted. The chip's UART, finisher and fatal status are now named once
in `boot_layout.ld.h` and read by both the prologue and the C, the prologue's refusal path being a
second consumer of addresses that had only one.

**THE MUTATION MATRIX, eleven arms, each a SINGLE-SITE edit in its own copy of the tree over a WIPED
build directory whose log was checked to name the mutated translation unit.** The copy CARRIES `.git`,
and the equivalent-edit control is what said so: the first rig excluded it and the control itself came
back 14 of 48 red, every one a host gate whose corpus is `git ls-files`, which is a figure about the
rsync and not about any mutation. With `.git` carried the control is 48 of 48 exactly.
  - *The backend states three levels while the configured mode has four:* BUILD REFUSED,
    `static assertion failed: the depth this backend walks is not the depth the configured paging
    mode has`. This is the arm the derived level count exists for.
  - *The same edit on the BASE posture, where 2 is the right value:* 48 of 48. So the red above is
    the disagreement and not the act of editing that line.
  - *The chip reserves no chain pages while the mode names one:* BUILD REFUSED at assembly time,
    "the boot tables reserve a different number of pages above level 2 than the configured paging
    mode names". The guard reads the DISTANCE between the root's symbol and the level-2 table's, so
    the loop that fills the chain cannot disagree with the layout it walks.
  - *The kernel base made non-canonical in the configured mode:* BUILD REFUSED at assembly, by name.
  - *The chain's slot above level 2 forced to 0:* BUILD REFUSED at assembly, by name, the guard being
    the one that holds the devices, the kernel window and the transient window to one slot.
  - *`index_at` flattened so every level indexes at the granule:* 21 of 21 image tests red, 27 of 48.
    The generic walk is EXERCISED at four levels and not merely written.
  - *`arch_aspace_model`'s mode comparison shifted by one:* `qemu_riscv64_selftest` ALONE red. The
    register readback is load-bearing and no other arm covers it.
  - *The prologue's readback guard deleted:* 48 of 48, and that is the correct answer on a hart that
    implements the mode. What it guards is a hart that does not, and the witness for that is the
    `-cpu rv64,sv48=off` run and its two controls above.
  - *The span bound narrowed to `granule / 8`, the figure an eight-byte entry gives:* 48 of 48. NOT
    witnessed, and the reason is what it is: on a backend whose entries are eight bytes the wider run
    merely crosses MORE table boundaries, so both values pass here. `granule / 4` is what a
    four-byte-entry translating backend needs and this fleet has none, so the bound is held by the
    argument and not by a run.
  - *The span's base moved off the last slot onto the boundary itself:* 48 of 48. The arm catches a
    backend that miscounts a last slot, and on a correct backend every shape passes, so the SHAPE is
    held by review; what IS witnessed is the backend, by R2.1's and R2.2's own mutations of it.

**UNWITNESSED, AND WHY.** Sv57 is one entry in the choice and one arithmetic step in the header away,
the emulated core accepts it, and it is NOT offered: a mode the board does not run is a claim with no
arm behind it, and the header refuses it by name rather than computing a depth for it. The two guards
that fire only on a mode change are witnessed by mutation and not by a posture, there being no mode in
range that makes the kernel base non-canonical. And the transient window's own two tables are
untouched by the mode: they hang under a level-2 slot, so a deeper mode adds nothing above them that
`arch_aspace_acquire` can see.

**R4. No address-space identifier, legitimately.** The architecture permits the identifier length to
be hardwired to zero.
*Expected:* the port works with no identifier at all, which is F1's claim that identifiers stay below
the seam, tested rather than asserted.

**PRE-AUDITED, and F1's claim holds in the seam and in the kernel and FAILED in the test suite.**
The seam header names no identifier parameter, no allocation and no flush; the kernel's own
install cache names the CONCEPT correctly and already assumes no tag, per core, which is right for a
width of zero. The leak was one line of the PORTABLE selftest, which asserted that the reported
identifier width was NON-ZERO before believing the verdict word. Its intent was a vacuity guard
against a report that decodes to nothing, and the guard survives on the physical range and the
granule set; requiring the identifier to exist is what F1 forbids. **Fixed ahead of R2 rather than
left for R4, because the step that selects the translating backend inherits that arm and its suite
would have gone red for a reason belonging to a later step.**
*And the width-zero arm is NOT WITNESSABLE on this bench, which is a result rather than a gap.*
Measured by writing ones across the field: the emulated core reports 16 bits, and so does the model
of the part F8 names as the silicon witness. No emulator property narrows it, and a paging mode of
Bare does not gate the field either. So R4 may not report a pass on the width-zero case here. What it
CAN prove, and what the claim actually rests on, is that the port never allocates, generates or
scopes anything on an identifier, so a hart reporting zero would change nothing below activate. That
is a code property provable by inspection, and it needs no zero-width machine.
*One trap for whoever implements the reporting member:* the verdict bit means the machine's width
matches the port's RECORD. Recording zero on the grounds that this port tags nothing makes the
comparison false against a 16-bit hart; discovering the record at run time makes it tautological. The
only non-vacuous shape is a constant a human wrote for the class of part, compared against the
measurement, which is exactly what the A64 backend does.

**RESULT. TAKEN, and the width-zero case is WITNESSED AS A CODE PROPERTY while staying unwitnessed
as a MACHINE one.** Both postures carry 48 of 48 with the selftest at 132 of 132, 0 skipped and the
one declared partial (`periph_reg_write_unheld`) unchanged, out of one tree with no source edit
between them; the model line reads `granules 0x1, 16 ASID bits, 56 PA bits, verdict 0x7` on both.
`qemu-arm64` is 33 of 33, `qemu` 58 of 58, `qemu-riscv` 49 of 49 and `sim` 48 of 48.
`check_aspace_sigdiff.sh` is untouched by this step and still reports DIFF with exit 2 over one added
record, 36 candidate against 35 baseline, `arch.h` unmodified.

**THE THREE THINGS THE PRE-AUDIT SAID HAD LANDED WERE RE-READ AND ALL THREE HOLD.** The portable
arm asserts `pa != 0 and grans != 0` and DIAGNOSES the identifier width beside them, so the vacuity
guard rests on the physical range and the granule set; `arch_aspace_model` compares a recorded 16
against the WARL measurement, the shape A64's `ASID_BITS_RECORDED` already had; and the port's
identifier state is zero, read off the OBJECT rather than the source: `aspace_rv64imac.cc.obj` has
nine static objects, 152 bytes, and they are the six chip-handover words, the 96-byte slot array of
(space, page) pairs and the two selftest counters. `switch.S` names `satp` and `sfence.vma` in
comments only.

**THE CODE PROPERTY THE CLAIM RESTS ON, ESTABLISHED POSITIVELY OVER A NAMED CORPUS.**
**RE-DERIVED 2026-08-28, AND THE FIRST THING THAT PASS FIXED WAS THE METHOD.** Six of the original
nine figures did not reproduce, and the reason was not drift in the tree: the stanza stated no CASE
DISCIPLINE and no COUNTING UNIT, so `identifier` case-insensitively matched
`SPDX-License-Identifier` in all 932 files and `satp` counted lines where the breakdown counted
occurrences. Both are stated per figure now, with the command, and every breakdown below SUMS to its
total. A grep that found nothing is worth nothing without the count beside it, and a count whose
breakdown does not sum is worth less than no count.

*THE COUNTING UNIT is the OCCURRENCE and not the line*, `grep -o <pattern> <files> | wc -l` for hits
and `grep -l` for files, everything under `LC_ALL=C`. The two corpora:

```sh
# MAIN, 932 files
git ls-files | grep -E '\.(cc|c|h|S|ld|py|cmake|awk|sh)$|(^|/)CMakeLists\.txt$|(^|/)Kconfig|defconfig$'
# SUB, 75 files and 32,959 lines: every MAIN file naming aspace, plus every tracked
# file of arch/riscv/rv64imac/ and arch/riscv/chip/virt_rv64/
```

Six patterns run over MAIN and two over SUB, the SUB ones because their vocabulary is ordinary
English. **One of the original nine is DELETED rather than patched**, and the reason is under it.
  - *`sfence`, case-INSENSITIVE, 10 hits in 3 files, is the decisive one.* `aspace_rv64imac.cc` 8,
    `switch.S` 1, `startup.S` 1. Nine are instructions and one is a comment, and EVERY instruction
    names `zero` as rs2. There is no identifier-scoped invalidate in the port to lose, which is what
    makes a zero-width hart cost nothing in maintenance rather than cost a wider sweep.
  - *`satp`, case-INSENSITIVE, 87 hits in 9 files, reduces to THREE writes.* `aspace_rv64imac.cc` 49,
    `startup.S` 17, `rv64_paging.h` 8, `arch_rv64imac.cc` 5, `Kconfig` 4, then one each in the `sv48`
    defconfig, `switch.S`, `virt_rv64.ld` and `chip_virt_rv64.cc`. Sums to 87. The three WRITES:
    `startup.S` writes `PPN | MODE`, activate writes `satp_of()` which is `PPN | MODE`, and the probe
    writes ones and restores. So `satp.ASID` is 0 in every root the port installs and the only
    non-zero identifier the hardware ever sees is the probe's, under a mask with a fence each side.
    (The original 64 was a LINE count over the same 9 files.)
  - *`asid`, case-INSENSITIVE, 47 hits in 7 files.* `aspace_rv64imac.cc` 24, `aspace_armv8a.cc` 10,
    the selftest's `main.cc` 4, `syscall_aspace.cc` 4, `abi.h` 2, `arch.h` 2, `sys.h` 1. Sums to 47.
    Adjudicated: the rv64 backend's own field position, record, probe and report; A64's; the model
    bit's name and shift with the static_asserts holding the two spellings in step; the selftest's
    decode and diagnostic; and the English word "aside" in `user/include/kickos/sys.h`. None is a
    parameter and none is state.
  - *`identifier`, case-SENSITIVE lowercase, 63 hits in 25 files, AND THE CASE MATTERS HERE MORE THAN
    ANYWHERE.* Case-insensitively this is 1,008 hits in all 932 files, because every SPDX header
    carries `SPDX-License-Identifier`; the figure is only meaningful lowercase. 49 are the C sense of
    the word in gate scripts, awk and CMake (a declared identifier, an identifier prefix); 7 are the
    two translating backends' own prose; 5 the seam header, the ABI header and the selftest; 2 the
    remainder, `newlib_sbrk.cc` and `rx72m.ld`. Sums to 63.
  - *`space_id`, case-INSENSITIVE, 18 hits in 6 files, is the one that could have been a leak and is
    not.* The selftest's `main.cc` 12, `syscall_aspace.cc` 2, then one each in `abi.h`, `domain.h`,
    `aspace.h` and `domain.cc`. Sums to 18. `domain_space_id` is the domain SLOT INDEX biased by one,
    so a task can be told apart from another without a kernel address crossing the syscall boundary.
    It is never written to a register, never reaches the seam, and it exists on a backend with no
    identifier at all.
  - *`TCR_EL1|TTBR|VMID|PCID|nG|INVPCID`, case-SENSITIVE and it MUST be, 48 hits in 10 files. THE OLD
    CLAIM THAT EVERY ONE IS INSIDE `arch/arm64/` IS FALSE, AND 5 OF THE 48 ARE NOT.* Inside
    `arch/arm64/`, 43: `startup.S` 17, `aspace_armv8a.cc` 15, `virt_arm64.ld` 6,
    `chip_virt_arm64.cc` 3, `switch.S` 1, `arch_armv8a.cc` 1. Outside it, 5, and all five are the
    two-letter `nG` matching something else: `MAIR_DEVICE_nGnRE`, an ARM memory-ATTRIBUTE encoding,
    in `regs_v8m.h` twice and `arch_arm_pmsav8.cc` once, and the input-line label `DXnA..DXnG` of the
    XMC USIC in two `usic.h` copies. 43 plus 5 sums to 48. Case-insensitively the pattern is 14,149
    hits in 753 files, `ng` being ordinary English, which is why the flag is part of the figure. The
    claim that survives is the one that was wanted: no other architecture's identifier REGISTER
    spelling appears above the seam, and the five exceptions name no address space.
  - *The generator vocabulary, `generation|rollover|generator|generate`, case-INSENSITIVE over SUB,
    67 hits in 15 files.* 41 are the build system's and the tooling's "generate"/"generator"/
    "GENERATED" (`CMakeLists.txt` 26, `Kconfig` 4, and 11 more across the two other `CMakeLists.txt`,
    `kickos.cmake`, `kicktrace.py`, `genconfig.py`, `check_public_headers.sh` and the selftest's own
    `CMakeLists.txt`); 25 are the capability, thread and task HANDLE generation counters, a software
    reuse guard over pool slots that names no hardware (`main.cc` 15, `abi.h` 5,
    `syscall_thread.cc` 2, `kmain.cc` 2, `sys.h` 1); 1 is the rv64 backend's own comment. Sums to 67.
    There is no identifier generator and no rollover.
  - *`flush`, case-INSENSITIVE over SUB, 27 hits in 14 files, and ONE of them IS a translation flush,
    which the old text denied.* `aspace_seam.awk` 5 (its own `flush`/`flush_def` functions),
    `main.cc` 3, `syscall.cc` 3, `arch.h` 3 (two console, one dcache seam member), `kicktrace.py` 2,
    `chip_virt_rv64.cc` 2, `arch/CMakeLists.txt` 2, and one each in `sys.h`, `user/apps/common/
    CMakeLists.txt`, `grant.h` ("flush against", meaning abutting), `aspace_rv64imac.cc`,
    `arch_rv64imac.cc`, `arch_armv8a.cc` and `Kconfig`. Sums to 27. The one real translation flush is
    `aspace_rv64imac.cc`'s comment on the whole-hart fence, and it names no identifier, which is the
    point the original sentence was reaching for and overstated.
  - *`tag` IS DELETED AND NOT PATCHED, and the deletion is the finding.* As a substring over SUB it
    is 163 hits in 24 files, of which the majority are `stage`, `staged`, `staging`, `stages`,
    `stage_wait` and `stage_release`. It cannot be made to sum to anything meaningful without a
    word-boundary rule that would be chosen to produce the answer, so the figure is gone. **What it
    was evidence FOR survives without it and is stated directly:** `arch.h` mentions a translation
    tag three times and all three are PERMISSIONS for a backend that has one, not obligations, and
    the two kernel sites reason explicitly FROM there being no tag, which is the install cache
    skipping a root write per core. That is a claim about four named sites and is checked by reading
    them, not by a count.
  - *And nothing above the seam reads the width at all,* which is the sharpest form of F1's claim:
    `ARCH_ASPACE_MODEL_ASID` and its shift have exactly two producers, one per translating backend,
    and one consumer, the selftest's `tap::diag` line.

**THE CONCLUSION SURVIVES THE RE-DERIVATION, and it is the only thing the figures were for:** the
port allocates no address-space identifier, generates none, and scopes nothing on one. The three
figures that carry it are `satp` reducing to three writes that all set ASID 0, `asid` and `space_id`
finding no parameter and no state, and the register-spelling sweep finding no foreign identifier
above the seam. The two figures that were deleted or corrected carried none of it.

**THE WIDTH WAS A POPCOUNT AND IS A WIDTH NOW, AND THE FALSE PASS IT ALLOWED IS MEASURED.** satp.ASID
is WARL, so the legal values need not be a contiguous low run and the readback's population count is
not the field's width. `measure_asid_field` reports the highest bit that stuck plus one, and reports
CONTIGUITY beside the figure rather than folding it in; the verdict bit is gated on both, because a
span that matches the record through bits the hart does not all implement is not the field the record
names. **Graded controls, each a single-site edit of the written mask in its own copy of the tree over
a WIPED build directory whose log names `aspace_rv64imac.cc.obj`:** 16 bits is 48 of 48 with verdict
0x7; 8 bits reports `8 ASID bits, verdict 0x5` with `aspace_model` the ONLY red arm; 1 bit reports 1.
So the figure follows the field and is not a constant being printed.
*The pair that shows the fix earning its keep is a hart implementing the HIGH eight bits of the field
against a class record of 8:* span 16, population count 8, non-contiguous. With the popcount the
suite is 48 of 48 and the verdict is 0x7, a machine reporting a width its field does not have and
MATCHING the record on it. With the width and the contiguity check the same machine reports
`16 ASID bits, verdict 0x5` and `aspace_model` goes red naming it. On a 16-bit record no
non-contiguous pattern can false-pass at all, every 16-bit value of population count 16 being
contiguous, so the defect needed a narrower record to expose and the equivalent-edit control
reproduces the baseline exactly.

**THE PORT WORKS WITH NO IDENTIFIER, AND THAT IS NOW AN ARM RATHER THAN AN ARGUMENT.** The mask
written by the probe set to zero is a hart on which no identifier bit sticks, which is the
architecture's hardwired-to-zero case seen from the port's side. Two arms, each a single-site edit
over a wiped build directory:
  - *The field zero, the record still 16:* 47 of 48, `qemu_riscv64_selftest` the only red test and
    `aspace_model` the only red arm of 132. Every aspace, process, handoff, span, balance and fault
    arm is green. So a hart reporting zero changes NOTHING below activate, and the single consequence
    is that the machine stops matching the record.
  - *The field zero and the record 0, which is the class of part such a hart belongs to:* 48 of 48,
    132 of 132, `granules 0x1, 0 ASID bits, 56 PA bits, verdict 0x7`. R4's expected result, taken.
*What that does NOT witness, and the distinction is the whole of R4's honesty:* a zero-width MACHINE.
Re-measured this step rather than carried from the pre-audit, by a standalone 40-instruction M-mode
probe that writes ones across satp.ASID and prints what stuck over the `virt` UART, so the figure
comes from the hardware model and not from any KickOS code: `rv64`, `thead-c906`, `rva22s64`,
`veyron-v1`, `sifive-u54` and `max` all answer `0xffff` on QEMU 11.0.3. Sixteen bits and CONTIGUOUS on
every model this bench has, the c906 being the model of the part F8 names as the silicon witness.
**And the absence of a narrowing property is asserted with a positive control**, which is the only way
an absence is worth stating: `-cpu rv64,asid-bits=off`, `asid_bits=off` and `asidlen=off` are each
refused as `Property 'rv64-riscv-cpu.<name>' not found`, while `-cpu rv64,sv48=off` on the same
command line is ACCEPTED and the probe still answers `0xffff`, which re-confirms that a paging mode
of Bare does not gate the field either. The shipped image does not boot on `-cpu thead-c906` at all
(no output, one `zfa` privilege-spec warning, killed at the timeout), so the c906 figure is the
probe's and not the suite's.
*The contiguity leg is therefore held by the graded controls and not by a machine:* every core model
here implements a contiguous 16-bit field, so nothing on this bench distinguishes the width from the
population count except a mutation.
*One trap for a later port, stated because the record is a per-CLASS constant:* a board on a hart with
no identifier sets `ASID_BITS_RECORDED` to 0 for that class of part. Leaving it at 16 is not a
malfunction, it is the model arm reporting a divergence between the machine and the record, which is
exactly what that arm exists to do.

**FOLDED FIX 1: A MISPAIRED RELEASE REFUSES INSTEAD OF SURRENDERING A LIVE HOLD.** `window_drop`
keyed on (space, page) and returned silently when nothing matched, so a release beside a live hold of
the same page took THAT hold and a release beside no hold at all was invisible. It reports whether it
matched now, and `arch_aspace_release` refuses through `kickos::kpanic` on the one case that can only
be a caller defect. **The refusal cannot be "no slot matched", and that is the part the fix turns
on:** a hold the OFFSET route took spends no slot, so nothing to drop is the correct answer for every
frame inside the kernel window, which is every frame the pool hands out. So the release asks the same
question the acquire asked, `in_kernel_window` over the leaf's own output address, and refuses only
where the frame is outside the window and a slot is therefore the only route there was.
**Mutation-tested four ways, each in a copy of the tree over a wiped build directory whose log names
the mutated translation unit:**
  - *An extra release of an out-of-window page injected into `op_span`, with the refusal live:* 47 of
    48, `KERNEL PANIC: arch_aspace_release: no acquire of this page answered`, the image stopping
    after arm 105 which is the arm before the injected one. WITNESSED.
  - *The same injection with the refusal taken back out:* 48 of 48. That is the latent trap measured,
    and it is why the fix is not a tidy-up: nothing in the suite can see a mispaired release today.
  - *The refusal made unconditional, dropping the route question:* 27 of 48, all 21 image tests red
    and the image dead before arm 1. So the route question is what keeps the legitimate callers
    alive, and those callers are exercised on every green run: `op_alias` and `split_access` release
    unconditionally over pool frames, and `access_copy` checks each acquire before releasing anything.
  - *The equivalent-edit control:* 48 of 48 exactly.
*One caller was made contract-clean with it, in the same file the refusal now judges.* `op_span`
released inside its loop whether or not the acquire had ANSWERED, over a device frame that is always
windowed, so a pool exhaustion there would have met the new refusal rather than a red arm. It holds
two slots at a time against a floor of six and cannot exhaust, so this changes no run; it is one
`if (at != nullptr)` and it stops the arm depending on that.
*And two comments that were already false are corrected with it:* `kernel/mem/aspace.cc` and
`kernel/include/kickos/aspace.h` both said `arch_aspace_release` is a no-op "on the backend that
translates today", which stopped being true when this port grew a real window pool.

**FOLDED FIX 2: `arch_aspace_frame_at` CARRIES acquire's CHIP-HANDOVER GUARD.** One line, and the
guard is substantive rather than symmetric: `leaf_entry` reaches every table through the kernel
window's delta, which is 0 until `kickos_rv64_aspace_boot` runs, so a walk before the handover would
read a table at its OUTPUT address. Unreachable today, no space existing before the handover and
`arch_aspace_create` refusing while the boot root is null, which is why it is a guard and not a bug.
*A64's twin was checked and needs nothing:* that backend takes its base from a non-weak LINKER symbol
rather than from a runtime handover, so its `acquire` and `frame_at` are already symmetric and there
is no asymmetry there to close. `qemu-arm64` is re-run anyway and is 33 of 33.

**FOLDED FIX 3: THE KERNEL WINDOW'S BOUND IS A DECISION AND IS RECORDED AS ONE, IN THE CODE.**
`in_kernel_window` covers the DRAM gigabyte the boot tables describe and NOT every address the same
delta reaches: R2.2 put the devices at `KICKOS_RV64_VA_BASE + physical`, the very delta the kernel
window uses, so an MMIO frame could be answered by an addition too and is windowed instead. The
routing is UNCHANGED and the reason is the litmus: every frame the pool hands out lies inside the
DRAM span, so the device frames are the only thing that exercises the window pool at all, and
widening the bound would leave the fleet's one backend that can fail `ARCH_ASPACE_ACQUIRE_MIN` with
no live caller. The comment is at the bound rather than in this document, because that is where a
reader would otherwise "fix" it.

**UNWITNESSED, AND WHY.** A zero-width machine, for the reason above: no core model on this bench and
no emulator property produces one, so the arms that stand in for it are mutations of the port's own
probe. A non-contiguous field, for the same reason. The refusal's SURRENDER, as distinct from its
refusal: what an arm shows is that a mispaired release is refused and that removing the refusal is
green, while the surrender itself (two holds of one page, one extra release taking the second) is the
R2.1 audit's measurement and the shape of `window_drop`'s key, not a run. And the chip-handover guard
on `frame_at`, which no caller can reach.
*The rv32 port is untouched and PROVEN so:* 26 of 27 objects of `kickos_arch_rv32imac` and
`kickos_chip_virt_rv32` byte-identical against the parent commit under `-ffile-prefix-map` of both the
source and the build directory, with the same-tree control run FIRST and reporting 0 of 27 differing.
The one object that differs is `arch_rv32imac.cc.obj`, identical in size and in every section, whose
only divergence is two `DW_AT_decl_line` values shifted by exactly the ONE line `include/kickos/diag.h`
grew: no instruction, no relocation and no symbol moved. `arch/riscv/chip/virt_rv32/` is byte-identical
outright.

**THE GATES, WITH THEIR CORPUS COUNTS, ALL GREEN, MEASURED AT THIS STEP AND NOT RE-TAKEN SINCE.** `riscv_kernel_wx` 3 images at 512 leaves each, 1
read-execute leaf against 511 read-write; `riscv_kernel_gp` 3 images, 43,380 instructions of kernel
text, 9 anchor loads and no other gp reference; `riscv_kernel_apphalf` 312 app-half GLOBAL/WEAK
symbols, 4,650 instruction and 3 data relocations, 4 symbols reached; `kernel_runtime` 3 archives,
568 undefined references, none to the app runtime; `kernel_got` 3 archives, 105,469 relocation
records, no GOT reference; `test_labels` 48 tests as 26 host, 21 image and 1 build fixture;
`kconfig_gen` 7 refusals, 4 accepted overrides, 55 defconfigs resolved; `trap_redzone_decls` 5
trap-stack headers, 26 class records over 25 depth figures, 50 of 55 presets in scope.
*Two of those figures MOVED and the deltas are accounted for at the parent commit rather than
assumed:* the same three gates on a pristine build of `577744d8` report 43,281 gp-scanned
instructions against 43,380, 4,638 apphalf relocations against 4,650, and 105,345 GOT-scanned records
against 105,469, all three the added kernel code; the app-half SYMBOL count is 312 on both sides and
the symbols reached is 4 on both, so nothing new crosses the halves. The new diagnostic string
`kickos::diag::kAspaceRelease` lands at `0xffffffff8000ac20`, in the kernel's half, which is what the
apphalf gate passing means here.

**R5. The verdict.**
*Expected:* an EMPTY signature diff on the aspace family. A non-empty diff is the finding F8 predicts,
and it lands in the seam with A64 updated to match.

**IT IS NOT EMPTY, and F8 carries the measurement.** One member is owed, a query answering a physical
address for a mapped virtual one, because three callers above the seam require `acquire` to behave
like an offset map and one of them reports SUCCESS while naming every frame identically on a windowed
backend. The member lands at R2.2, whose own arms are among those three callers, so this step
COLLECTS the verdict rather than producing it.
*What this step therefore does:* run `tests/static/check_aspace_sigdiff.sh` against the frozen
baseline, record its output as the verdict, and update A64 to the changed family. The instrument's
DIFF exit IS the result. **Do not move the baseline to quiet it**, which would delete the finding the
milestone exists to produce.
*And two things the verdict must say that a signature diff cannot show*, both recorded at R2's
stanza: the split-image code model has no spelling on this architecture, so a kernel reference to app
text goes through a linker-filled indirection rather than a compiler flag; and the entry-and-exit
sequence pays no identity alias of RAM, because the regime and the privilege change together. Those
land as MECHANISM rather than as signatures, which F8 says is the shape a good seam produces, and a
verdict reporting only the diff would miss both.

**RESULT. TAKEN, AND THE VERDICT IS THE INSTRUMENT'S OUTPUT RATHER THAN A SENTENCE ABOUT IT.**
`tests/static/check_aspace_sigdiff.sh`, run from the repository root with no arguments, exits 2 and
prints this. The rule paragraphs it also prints are elided here and nothing else is:

    == address-space seam signature diff ==
       baseline  f0360d3ae88a4958358ecd8a2769119bff455265 (3 seam header(s))
       candidate working tree (3 seam header(s))
       members   35 baseline signature record(s), 36 candidate
                 FUNC 12/13, ENUMERATOR 10/10, MACRO 9/9, TYPEDEF 1/1, TAG 3/3
                 group calls     14/15 (floor 12)
                 group codes     13/13 (floor 11)
                 group memtag    1/1 (floor 1)
                 group mapbits   6/6 (floor 5)
                 group physaddr  1/1 (floor 1)
       family    identifiers matching arch_aspace / ARCH_ASPACE / arch_map / ARCH_MAP /
                 arch_phys_addr, wherever they stand in the seam headers

    DIFF: the signature records moved. Per F8 this diff IS the finding.
          < baseline, > candidate
          15a16
          > FUNC	arch_aspace_frame_at	arch_phys_addr_t (struct arch_aspace *, uintptr_t)	-

That is the whole of it. One record added, thirty five identical, and the exit code IS the verdict:
0 for no difference, 2 for a difference, 1 for a comparison that could not be made, which is UNKNOWN
and not clean. `tests/static/aspace_seam_records.txt` is untouched by this step, as it was by R2.3,
R3 and R4. *And this stanza's "update A64 to the changed family" clause was discharged at R2.2 and
not here:* the member landed on both backends in that step, and its A64 body gained the walk mask its
rv64 twin carries after an audit of this branch, so what R5 had left to do was the collection.

**BOTH MILESTONES' VERDICTS COME OUT OF ONE INSTRUMENT AGAINST ONE BASELINE, AND THE UNION IS
MEASURED RATHER THAN PREDICTED.** When this stanza was first written the two halves stood on two
branches and the union was called "not yet measurable"; the halves are one linear stack now, so the
sentence is replaced by the figure. Run from the tree that carries BOTH, the same script against the
same `f0360d3a` exits 2 with 36 candidate records against 35 baseline and one added record,
`FUNC arch_aspace_frame_at`. That is exactly M6.3's own verdict, unchanged by the x86_64 half, so the
x86_64 port took NOTHING from this family: X5's claim arriving from the aspace instrument rather than
from the step that made it, and now from a tree that can no longer be accused of measuring one half.
It also settles F8's "the diff against the frozen baseline is still the ONE member RISC-V forced",
which is a statement about the UNION: measured over the union it holds. M6.4's own subject is the
entry and boot family, and its instrument reports PASS with 54 records against 54; that one is X6's
and is read here only to keep the two verdicts from being reported twice from two places.

**WHAT THE VERDICT SAYS THAT A SIGNATURE DIFF CANNOT SHOW.** Eight things. The first two are the
ones this stanza named before the step ran and both were re-read against the tree R2.2 and R2.3 left;
the next four are the later steps' and each is judged rather than transcribed; the last two came out
of the ten-angle review of the two closed milestones and are the sharpest of the set, both being
CONTRACT movements the instrument reported as no movement at all.

  - **THE SPLIT-IMAGE CODE MODEL HAS NO SPELLING ON THIS ARCHITECTURE, and the mechanism that
    replaces it is STRICTER than the flag rather than weaker.** `KICKOS_SPLIT_IMAGE_CODE_MODEL` is
    set only for `armv8a`, so `kickos_split_image_tu` is inert here by construction and not by
    omission: medlow reaches a sign-extended 32-bit ABSOLUTE range and medany a signed 32-bit
    PC-relative DISPLACEMENT, and neither materialises a whole 64-bit address in the referencing
    section. What carries a cross-half reference instead is a
    relocated 64-bit word at namespace scope, which also covers the CALL a long-branch veneer
    absorbs on A64 and which RISC-V has no veneers for. A signature diff sees none of this: the seam
    declares no code model and no linker mechanism, and the two backends satisfy the same
    declarations by different means.
  - **THE ENTRY AND EXIT SEQUENCE PAYS NO IDENTITY ALIAS OF RAM, and R2.3 did not change that.**
    `satp` is written in MACHINE mode, where it governs no access, and takes effect on the `mret`
    whose `mepc` already holds the landing pad's virtual address, so no instruction has to be valid
    on both sides of the switch and F8's "mapped at the same address on both sides" is discharged by
    there being no such instruction. R2.3 added a level-1 table under the kernel window and one more
    non-leaf entry for `_start` to fill, and R3 added a fill LOOP above level 2, all of it still
    inside that one machine-mode prologue. The low alias `.text.init` runs at is absent from the
    running kernel, which is what turns a dropped VMA-to-LMA offset into a link failure rather than a
    silent success.
  - **THE FRAME QUERY'S ANSWER IS WITNESSED AND ITS CALLER'S CONVERSION IS NOT, and that asymmetry is
    the sharpest thing this verdict has to say.** Seven selftest arms go red when the member answers
    one constant frame for every mapped page, which is exactly the defect F8 named, and eight when it
    answers zero. Reverting `aspace_frame_token` to the two-acquire-pointer arithmetic the member
    replaced is GREEN. That is not a gap in the mutation matrix, it is the defect being LATENT on
    this board: every frame that function is ever asked about lives in the frame pool or the app
    image, both carved inside the kernel window's one gigabyte, so `arch_aspace_acquire` takes its
    OFFSET route for all of them and the subtraction is arithmetically right here. The seam member
    exists because the arithmetic stops being right for a frame outside that span, and no arm in this
    tree arranges one. So the family gained a member on evidence a diff carries and its callers were
    converted on evidence only a reading carries.
  - **THE DEATH-PATH FRAME REWRITE COSTS TWO FIELDS ON A64, THREE HERE AND FIVE ON X86_64, THROUGH
    ONE SEAM CALL.** `arch_fault_redirect_to_exit` writes `ELR` and `SPSR` on A64, because `eret`
    switches to a stack pointer register of its own; `sepc`, `sstatus` and `F_SP` here, because the
    restore epilogue reloads `sp` out of the frame; and `rip`, `cs`, `ss`, `rflags` and `rsp` on
    x86_64, because an interrupt return reloads all five and the instruction refuses a return whose
    stack selector does not carry the privilege the code selector names. One signature, three field
    counts, and the third field here has a witness of its own: with the `F_SP` write deleted,
    `faultsurvive_off`'s worker runs its death stub privileged on a buffer outside its own stack and
    the console comes out corrupted mid-word.
  - **THE GP HAZARD EXISTS IN BOTH DIRECTIONS AND A DISPLACEMENT CHECK CATCHES NEITHER.** R1.6 found
    it from the kernel's side, twelve kernel globals resolving through `gp` in the linked image with
    stores to the current-context pointer among them, and closed it by re-anchoring `gp` at the trap
    entry. R2.2 found it from the app's side, `kmain`'s store to `kickos_init_args` relaxed onto the
    app's anchor as `sd a1,-2000(gp)`. `virt_rv64.ld` asserts the halves are out of each other's
    reach, and that assert is about MEDANY's signed 32-bit displacement: gp relaxation happens inside
    2 KiB of ONE anchor, which is the opposite end of the range, and medlow's absolute reach covers
    the app's base outright so the same reference links as an ordinary `lui`+`addi` with no
    truncation anywhere. That linker script's comment said the assert is "what turns a missed
    cross-half reference into a link error", which R2.2's audit falsified; it was this step's one
    reported defect and the pre-audit step below corrected it in place. What actually holds the two
    directions is two gates over two different corpora, `check_riscv_kernel_gp.sh` over the LINKED
    image because gp addressing does not exist before link time, and
    `check_riscv_kernel_apphalf.sh` over the kernel archives' RELOCATIONS because a value scan
    cannot separate an app-half address from the Cortex-M bit-band constant that equals it.
  - **MEDLOW REACHES EXACTLY THE TOP TWO GIBIBYTES AND THE BOTTOM TWO, AND THAT DECIDED F1's
    PLACEMENT ON THIS ARCHITECTURE.** Re-measured at this step against the linker rather than carried
    from R1.4b, with one HI20 reference per link: `0x00000000`, `0x7FFFF000` and
    `0xFFFFFFFF80000000` and `0xFFFFFFFFFFFFF000` link, while `0x80000000`, `0xFFFFF000`,
    `0xFFFFFFC000000000` and `0xFFFFFFFF7FFFF000` are all refused as `relocation truncated to fit:
    R_RISCV_HI20`. The prebuilt libc and libgcc multilibs are medlow, so the whole C library's
    references to its own data decide where a high-half kernel may live, and the address a reader
    picks first, the bottom of Sv39's high half, is among the refused ones. F1 says the kernel lives
    at a fixed high range; this architecture leaves exactly one such range that costs nothing, and
    R2 inherited it from R1.4b rather than choosing it. No signature carries a code model's reach.
  - **THE ADDED MEMBER'S CONTRACT MOVED AFTER THE MEMBER LANDED, AND THE DIFF THAT REPORTED THE
    MEMBER CANNOT REPORT THAT.** `arch_aspace_frame_at` was written against "the same half map and
    unmap take", which reads as a universal because both backends that had it then keep their kernel
    in a high half. It is not universal. x86_64 ADOPTS a live regime whose kernel lives LOW, in the
    top-level slots the firmware already has present, so a high-half boundary would name the wrong
    side outright. The contract the three backends actually share is weaker and more precise: AN
    ADDRESS THE SPACE MAY LEGITIMATELY NAME, which each spells in the test its own map and unmap
    already ask, `low_half_page` on rv64 and A64 and `range_ok` on x86_64, and which the seam now
    states in those words. Without the guard the walk reads the index bits of the address alone, so
    an address outside the range either ALIASES onto one the space may map and is answered with THAT
    page's frame, or indexes into the kernel entries every space shares, which discloses the kernel's
    map. A backend's own diagnostic probes still need the unguarded walk, and they take it from a
    backend-PRIVATE helper rather than from the seam member, which is what keeps the member's answer
    one thing. The signature is byte-identical across the whole movement.
  - **AND ITS `0 MEANS NOT MAPPED` IS EXACT ONLY BECAUSE OF WHERE THE FRAMES COME FROM.** 0 is also
    what a page mapped onto physical frame 0 would answer, and a backend does accept `pa == 0`, so
    the answer is unambiguous by a property of this tree rather than by the encoding. No low-half
    page is ever mapped there: the frame pool is carved inside the image's DRAM share, so its
    lowest output address is far above 0 and 0 is its allocation failure besides; the image route
    names the app window's load address; and the one boot leaf whose output IS frame 0 is a device
    mapping in the KERNEL half, which the member's own range guard refuses. What the seam keeps is
    the RULE that follows, that no low-half page may be mapped onto frame 0, because a caller or a
    backend that breaks it makes `aspace_frame_token` and the arms in
    `kernel/syscall/syscall_aspace.cc` read a live mapping as absent.
  - **THE SEAM STATED ITS CENTRAL GUARANTEE TWICE, CONTRADICTORILY, SIXTY LINES APART, AND A
    SIGNATURE DIFF SEES NEITHER STATEMENT.** The result enum's banner promised that a failed map
    leaks no frame; `arch_aspace_map`'s own paragraph says a failed map leaves its range UNMAPPED
    rather than as it was. Only the second was ever true, and the mechanism is in the backends:
    `prune_empty` frees TABLES and returns at the leaf level without freeing anything, so a leaf that
    was already mapped and gets cleared by a rollback loses the name of its frame permanently. The
    banner carries the map member's reading now. Two paragraphs of a seam header can disagree for as
    long as nobody reads them together, and no extractor in this tree extracts prose.

**THE DIFFER GAINS X6's PER-GROUP FLOORS, BECAUSE THE HOLE X6 DESCRIBES IS PRESENT HERE AND WAS
MEASURED.** X6's argument is that a total floor passes an extraction that loses one group on BOTH
sides, that being a clean empty diff. This instrument floored per KIND, and a kind is a declaration
FORM while a group is a family alternative, so the two partitions cross-cut and neither subsumes the
other. Measured over the five alternatives of the extractor's PREFIX, each dropped in its own
hard-linked copy of the tree, run first against the pre-floors script and then against the floored
one:

  - *`arch_map` dropped:* PRE-FLOORS SILENT. Exit 2, the same one-record diff, the same finding text,
    with the counts reading 34 baseline against 35 candidate and `TAG 2/2`. It costs exactly one TAG
    record of three, which clears `MIN_TAG` of 1 and every other per-kind floor and the total floor
    of 24. FLOORED: exit 1, `group memtag: 0 record(s), floor 1`.
  - *`arch_aspace` dropped:* caught either way. Pre-floors on `FUNC: 0 record(s), floor 8` and
    `total: 21`, floored on those plus `group calls: 0, floor 12`.
  - *`arch_phys_addr` dropped:* caught either way, pre-floors on the TYPEDEF floor, floored also
    naming `group physaddr: 0, floor 1`.
  - *`ARCH_MAP` dropped:* caught either way. Floored reads `group mapbits: 3 record(s), floor 5`, and
    the 3 that survive are worth knowing: the memory-type enumerators are admitted through their
    TAG's name and not their own, so only the three anonymous rights bits go.
  - *`ARCH_ASPACE` dropped:* caught either way, floored naming `group codes: 4, floor 11`.
  - *The PREFIX matching nothing at all:* exit 1 with every group at zero beside the total floor.
  - *A group line deleted from the table, so six records classify into no group:* exit 1, the
    classification refusal, which is what binds the table to the PREFIX rather than leaving it to
    drift.
  - *Two group regexes claiming one record:* exit 1, the same refusal from the other side.
  - *The equivalent-edit control, a comment appended to the extractor:* exit 2 with 36 against 35
    exactly, so the rig is not reporting a copy as a kill.

So four of the five alternatives were already covered and one was not, and the one that was not is
the smallest group in the family. The fix is X6's shape rather than a raised `MIN_TAG`, because
raising a kind floor closes today's instance and not the class: a member added under a NEW alternative
that lands in an already-populated kind would be silent again, and the classification check is what
makes that impossible. Groups of ONE carry a floor of 1, so a legitimate removal of
`arch_phys_addr_t` or of the memory-type enum tag does trip the floor and force it to be re-decided,
which is the right cost on a family this small.

**HOW A READER WHO WAS NOT HERE RE-TAKES THIS.** From the repository root, on a checkout that has its
`.git` (the instrument reads BOTH sides through `git`, so an exported tarball cannot answer):

    sh tests/static/check_aspace_sigdiff.sh

No arguments, no build, no toolchain and no environment file: it needs `git` and `awk` and nothing
else.

**IT IS DELIBERATELY NOT A CTEST TEST, AND THE ABSENCE IS THE DECISION AND NOT AN OVERSIGHT.** The
rule is about which verdict the instrument EXPECTS, not about what kind of instrument it is. This
one's expected verdict is DIFF, exit 2, one added record: registering it would put a test on the
ladder whose passing state is a non-zero exit, so either the ladder goes red while reporting the
milestone's own result, or the registration inverts the exit code and the ladder then goes GREEN on
the exit 0 that means the member has gone, which is the failure this instrument exists to catch.
A signature differ whose expected verdict is PASS has the opposite shape and belongs on the ladder,
and a sibling milestone registered its own differ for exactly that reason: its seam gained no member,
so exit 0 is both its expected verdict and ctest's success. The two calls are the same rule applied
to two expected verdicts. Do not register this one to make the set look uniform. The baseline is the
frozen records themselves, checked in as `tests/static/aspace_seam_records.txt`: the seam as T2 left
it, held as text, so the verdict rests on no commit that has to stay reachable. An optional argument
takes a candidate ref in place of the working tree, so the verdict can be re-taken at any commit;
`KOS_SIGDIFF_KEEP=<dir>` copies both record sets out, and `KOS_SIGDIFF_REGEN=1` rewrites the
baseline from the candidate and takes no verdict at all. THE RESULT IS THE EXIT CODE AND THE DIFF
BODY: exit 2 with one added `FUNC arch_aspace_frame_at` record is this milestone's verdict, exit 0
would mean the member had gone, and exit 1 means the comparison failed and the verdict is UNKNOWN.
Do not move the baseline to make it quiet.

**THE HEADER SENTENCE R4 DISPUTED IS REWRITTEN, AND R4 WAS RIGHT.** `arch_phys_addr_t` was justified
as "wider than uintptr_t because Sv39 pairs a 39-bit virtual range with a 56-bit physical one and
Sv32 pairs 32 with 34", and that reasoning does not hold in every mode: at XLEN 64 `uintptr_t` is
already 64 bits and 56 physical bits are NARROWER, and Sv57 pairs 57 virtual bits with 56 physical
and inverts the inequality outright. Only the Sv32 clause ever carried the claim. The sentence now
names the PROPERTY, that the physical width does not track the virtual one in either direction, with
Sv32 as the case where a pointer cannot hold a physical address and Sv57 as the case where neither
width bounds the other. It is a comment and it moves no record: 36 candidate against 35 baseline,
unchanged.

**THE FLEET, A DATED RECORD RE-TAKEN 2026-08-29 ON THE FINAL M6.4 TREE `aea89358`.** A count is a
figure `ctest` answers, so read this as what that run said and re-derive rather than quote. Both
RV64 postures are **52 of 52** with the selftest at **134 of 134**, 0 skipped and the one declared
partial (`periph_reg_write_unheld`) unchanged, out of one tree with no source edit between them, and
both report `granules 0x1, 16 ASID bits, 32 PA bits, verdict 0x7`. `qemu-arm64` is **42 of 42** with
the same 134-arm selftest, `qemu` **61 of 61**, `qemu-m33` **60 of 60**, `qemu-riscv` **52 of 52**,
`microbit` **46 of 46**, `qemu-x86_64` **38 of 38** with its selftest at **102 of 102**, and `sim`
**51 of 51** on the ladder every board shares, **366 of 366** once GTest is provisioned through
`CMAKE_PREFIX_PATH` and the host unit suite registers alongside it. Take a `sim` figure without that
prefix as the ladder alone.
*THE FOUR TRANSLATING BOARDS GAINED ONE ARM EACH, `console_reach`, AND ALL FOUR ARE GREEN.*
Registered totals re-derived 2026-08-29 by `ctest -N`: `qemu-arm64` 42, both RV64 postures 52,
`qemu-x86_64` 38. The gate walks the `-fcallgraph-info` graph from the fault-record console route
and fails on a reachable `kpanic`. **It was RED on three of the four for part of the same day** and
this paragraph said so: at `517449e5` the reachable panics were `reent_seat`/`reent_prime`,
`ep_copy` and, on RV64 only, `arch_aspace_release`. `58b43d62` closed the first three and
`d4977780` the fourth, and the sentence recording the red was not moved with them. Re-measured on
`aea89358` over wiped scratch trees, each board reports `no panic terminal is reachable from the
console route`, and the reading is not an absence over an empty corpus: the empty-directory floor
clause fires first, the walk reads 116 `.ci` files and 931 nodes on `qemu-arm64`, 115 and 973 on
each RV64 posture and 119 and 907 on `qemu-x86_64` against a declared floor of 90 and 700, both
witness symbols are reached, `kpanic` is present with 34 call sites in every corpus, and the
planted-edge control still produces a path.
*THE RV64 MODEL LINE READS 32 WHERE EVERY DATED MEASUREMENT ABOVE READS 56, and the counts did not
move with it.* The 56 was the PPN field's width, which is the architecture's figure for every RV64
mode; the 2026-08-29 re-review's second finding is that the field is not the machine, so the figure
is now the physical extent the CHIP publishes and the map editor refuses against the same bound. The
board's own map is entirely below 4 GiB. Read a `56 PA bits` in a dated step record above as what
that step measured, not as what a re-run answers.
*WHY EACH BOARD MOVED, stated here once rather than by rewriting twenty dated measurements.* THREE
changes are FLEET-WIDE and lift every board by one each: `tests/static/check_shell_special_names.sh`,
`tests/static/check_entry_sigdiff.sh` and, on 2026-08-29, `tests/static/check_whitespace.sh`, each
registered unconditionally under the `host` label. TWO more are BOARD-LOCAL and landed the same day:
five `qemu_arm64_panicgate` arms, which take `qemu-arm64` from 35 to 40 before the fleet-wide one,
and two x86_64 static gates, which take `qemu-x86_64` from 34 to 36 before it. The RV64 total moved
only by that fleet-wide gate and its COMPOSITION moved without it:
`qemu_riscv64_aspace_fault` was retired on 2026-08-29 and `qemu_riscv64_aspace_ufault` registered in
its place, so a 50 stamped before that date names a different set of fifty.
A per-step figure elsewhere in this document reads lower and is NOT stale: a step's RESULT is a dated
measurement, and the mutation controls in particular are COMPARATIVE, so rewriting one side of a
"48 of 48 exactly" control against a 48 baseline would break the comparison it exists to make. Read
any earlier fleet figure as the count at that step, and this paragraph as the count now.

**THE GATES, WITH THEIR CORPUS COUNTS, ALL GREEN, staged before they ran so nothing is invisible to a
corpus built from `git ls-files`.** The ladder figures are a LIVE claim and move with the tree: on
2026-08-29 they are 31 of 31 on the `host` label of `qemu-riscv64` and 52 of 52 on the full ladder,
both having gained `whitespace` and then `console_reach`, which is green since `58b43d62` and
`d4977780`.
**The CORPUS COUNTS that follow are a DATED RECORD RE-TAKEN 2026-08-29 on the staged M6.4 tree
`aea89358`**, over the `qemu-riscv64` tree except where a figure is per-tree and said to be. The
figures they replace were taken on 2026-08-28 on the RISC-V branch, before the x86_64 stack was
rebased onto it, so every corpus that counts tracked files reads higher here by what that stack
adds; nothing below is a gate that weakened. A corpus is a figure a command answers, so re-derive
rather than quote: these gates print their own counts and `ctest -L host -V` is where they come
from.
`doc_names` 96 doc files against 1228 tree identifiers and 1546 tracked paths;
`extern_c_linkage` 122 of 554 tracked C/C++ files pairing a block with a namespace; `ascii` 1118
tracked files, every byte of each; `spdx` 1125 tracked files of which 1115 can carry a header;
`whitespace` 1124 of 1125 tracked files, every line of each;
`include_guards` 258 tracked headers and 588 C/C++ files; `shell_special_names` 105 tracked shell
scripts; `cpu_id_fold` 588 files plus the seam
header; `forever_loop` 588 files and 121 `while (true)`; `dash_punct` 949 tracked source files;
`panic_banners` 38 banners from 10 files, 3 resolved from a name slot; `death_stack_seating` 7 arches,
7 of them defining `arch_ctx_redirect`; `trap_redzone_decls` 5
trap-stack headers, 5 declared arches, 26 class records over 25 depth figures, 50 of 56 presets in
scope and 50 registered, plus 3 translating presets against 4 registered in
`console_reach_roots.txt`; `atomic_rmw` 588 files and 33 plain atomic accesses over 1 harvested atomic
identifier; `service_lists` 15 providers in the tree and 15 declared (3 default, 12 select) over 56
presets; `kconfig_gen` 7 refusals, 4 accepted overrides, 56 defconfigs resolved;
`kconfig_forwarding` 24 of 106 declared symbols; `test_labels` 52 tests as 30 host, 21 image and 1
build fixture over 115 declared programs on that tree, and 366 as 339 host, 26 image and 1 fixture
on a provisioned `sim`; `c_headers` 28 C-facing headers of 256 tracked, 24 guarding
a block and 4 reached by include; `no_privileged_tls` 4 privileged
archives and 1 archive member over 75 enumerated; `kernel_runtime` 3 archives and 570 undefined
references; `kernel_got` 3 archives and
107,659 relocation records; `seam_defaults` 16 fallback members over 16 seams, 13 resolved from a
fallback; `class_backend` 99 class symbols declared, 65 defined and 4 on the link line;
`riscv_no_smalldata` 4 archives; `riscv_kernel_gp` 3 images, 46,860 instructions of kernel text and 9
paired anchor loads; `riscv_kernel_apphalf` 309 app-half GLOBAL/WEAK symbols, 4,968 instruction and 3 data
relocations; `riscv_kernel_wx` 3 images at 4 boot tables and 512 entries per
table. Three of those are per-tree and differ where the ISA does: `kernel_got` reads 36,871
records on `qemu-arm64`, `kernel_runtime` 572 undefined references there, and `no_privileged_tls`
0 archive members over 63 enumerated on `qemu-x86_64`.
*THE THREE FIGURES THAT MOVED AT R4 HAVE ALL MOVED AGAIN, and that is the opposite of what this
sentence used to say.* R5 recorded them as standing still, which was the check that its header edit
was only a comment. R6's pass is not only comments: the gp scan is 44,820 instructions against R5's
43,380, the apphalf relocations 4,751 against 4,650, and the GOT-scanned records 106,109 against
105,469, while the app-half symbol count FELL from 312 to 311. The three that grew grew because R6
added real kernel text (the machine-mode diagnostic, four CSR readback refusals, the trap-stack
canary and the DRAM probe); the one that fell fell because a symbol left the app's half. All four are
figures that moved, none is a gate that weakened, and each is refused by its own gate at the new
value. **Those four are a COMPARATIVE record at R6 and are deliberately not rewritten**: both sides
of a comparison have to be the same measurement. All four have moved again on the M6.4 tip, where
the corpus paragraph above reads 46,860, 4,968, 107,659 and 309.

*The rv32 port is untouched and PROVEN so, and the proof cost a control and a near-miss.* 23 of 25
objects of `kickos_arch_rv32imac` and 1 of 2 of `kickos_chip_virt_rv32` are byte-identical against
`dabfaf62` under `-ffile-prefix-map` of both the source and the build directory with a pinned
`SOURCE_DATE_EPOCH`, and the same-tree control was run FIRST and reported 0 of 25, 0 of 2 and 0 of
121 differing. The three that differ have identical file sizes, identical section tables, identical
symbol tables and identical relocations, and the ONLY section that differs in any of them is
`.debug_info`: the whole-file differing byte counts of 1, 8 and 3 equal that section's differing byte
counts exactly. Every hunk of the DWARF diff is a `DW_AT_decl_line` shifted by exactly +1, on DIEs
whose `DW_AT_decl_file` resolves to `arch.h`, which is the one line the comment grew. No
`DW_AT_low_pc`, no `DW_AT_location` and no type attribute moved.
*The near-miss is worth the line it costs:* the first comparison invoked a
`riscv32-none-elf-readelf` that is not on `PATH`, `.session/env.sh` exporting
`KICKOS_RISCV_TOOLCHAIN_BIN` without adding it, and the resulting EMPTY dumps compared equal and
reported all three objects identical. An absence-of-difference verdict taken from a tool that
produced no output is the same false pass this step's own floors exist to rule out, one level down.
The figures above are from the re-run, with the dump line counts printed beside them.

**WHAT M6.3 HAS NOT WITNESSED IS IN `STATE.md`**, in the register that file keeps for exactly this,
rather than duplicated here. Two items of it were re-measured at this step rather than carried:
`-cpu thead-c906` still produces NO output from the shipped `hello` image, one `zfa` privilege-spec
warning and a timeout kill, with the default core printing the banner as the control; and no emulator
property narrows the identifier field, `asid-bits=off`, `asid_bits=off` and `asidlen=off` each refused
as `Property 'rv64-riscv-cpu.<name>' not found` while `sv48=off` on the same command line is accepted.

**Stage result:** the aspace seam proven against a target chosen for unlikeness rather than
convenience, and the portable T-step arms carried over with two edits and no parameterisation: R3
widened `op_span`'s bound and corrected its comments and the portable selftest's. The three fault
gates are this architecture's own scripts, written at R2.2 rather than reused, for the reason that
stanza gives.

**R6. The pre-audit pass: three claims a mechanism could not support, and one fleet defect.** No new
design, no new backend behaviour bar one call per chip. The step exists because three separate steps
of this milestone each found a comment or a record asserting something its own mechanism cannot
deliver, and a fourth instance is a class rather than a coincidence.

  - **THE PANIC DEAD-END NOW DRAINS THE DEVICE, AND THE PER-BOARD ANSWER IS WHY THAT IS SAFE.**
    `kpanic` reaches `kfault_terminate`, and SIX chips defined it as a bare `arch_shutdown`:
    `virt_rv64`, `virt_arm64`, `virt_rv32`, `mps2`, `nrf51` and `sim`, the last of which the
    deferral record had missed. Only `kickos_terminate` and a clock retune called
    `arch_console_flush_sync`, so `arch_shutdown` could stop the core with the transmit FIFO and the
    shift register still loaded. Each of the six gained the call. **The hazard that made this need
    care is documentary and not a runtime assert:** the lone-TU fallback ASSERTS in its comment that
    the chip's console cannot outrun a shutdown, and R1.4 measured that false for any 16550 with a
    FIFO. Measured per board rather than assumed: only `virt_rv64` (NS16550A, TEMT) and `virt_arm64`
    (PL011, BUSY) carry real bodies, and the fallback's assertion is TRUE for the other four by
    TRANSPORT, `virt_rv32`, `mps2` and `nrf51` publishing through semihosting `SYS_WRITEC` which the
    host consumes inside the trap, and `sim`'s sync writer looping on `write(1, ...)` until the host
    has taken every byte. In the linked image the fallback is one instruction, `bx lr` on both ARM
    boards and `ret` on `virt_rv32` and `sim`, out of
    `arch_console_flush_sync_default.cc.obj`, so nothing can fire and nothing can hang. The RING
    needed no call: `kpanic_enter` sets `g_console_panicking`, which routes every panic-time write
    through `arch_console_write_sync`, leaving only the device.
    *Witnessed by mutation on `qemu-riscv64`, the same instrument compiled into all three arms:* the
    flush is REACHED on the panic path after the banner and before the exit; deleting the one call
    makes it unreached with the banner and the 132 status unchanged, which is exactly how the defect
    stayed invisible; and with the device mutated never to report idle the loop still terminates on
    `UART_POLL_BOUND`, so the new call cannot turn a fault into a harness timeout. **Truncation
    itself stays unwitnessable here**, QEMU handing each byte to its chardev on the register write
    and reporting the shift register idle with it, so the fix rests on the TEMT and BUSY semantics
    and not on a bench capture. The `arch_shutdown` after `kmain` in every `Reset_Handler`, and
    `sim.cc`'s after `kickos_isr_fault`, were swept and left: both run only when a noreturn path
    returned, so a drain there would state a reachability the code denies.
  - **THE LINKER SCRIPT'S ASSERT NOW SAYS WHAT IT DOES, AND THE SWEEP FOUND THE CLAIM IN FOUR MORE
    PLACES.** R5 reported the comment at `virt_rv64.ld`'s cross-half `ASSERT`, and it is corrected:
    the assert keeps the halves out of each other's medany reach, which is F1's placement invariant
    and no more, and it does NOT turn a missed crossing into a link error, the app window at
    `0x40000000` being inside medlow's absolute reach. The same false mechanism was standing in the
    file's own HEADER 320 lines above, in `chip_virt_rv64.cc`'s link-time-words comment, in
    `kernel/mem/aspace.cc`'s and in `kernel/thread/reent.cc`'s, the last citing the very gate whose
    header says the opposite. `kernel/init/kmain.cc` had already been corrected in this milestone
    and was the model. *What made all five decidable in one reading:*
    `tests/static/riscv_apphalf_allowlist.txt` names FOUR kernel-text instructions that do
    materialise an app-half address and must keep doing so, and its own rule is that every line
    must match, so the image links today with four such references.
  - **AND A SECOND FALSE MECHANISM IN THE SAME FAMILY: `-msmall-data-limit=0` IS NOT WHAT KEEPS A
    KERNEL ACCESS OFF gp.** Three sites said it was, `virt_rv64.ld`'s header, `startup.S`'s gp
    seating and `switch.S`'s trap re-anchor. The flag governs where the REFERENCING archive's own
    data lands; gp addressing is MADE BY THE LINKER out of any upper/lower pair whose TARGET falls
    within `gp +/- 0x800`, which is precisely a kernel reference to an app-half symbol, and R2.2
    measured one as `sd a1,-2000(gp)` on this board with the flag already applied. What holds it is
    the trap entry's re-anchor plus `check_riscv_kernel_gp.sh` over the linked image, and all three
    sites now say so.
  - **THE GATE'S OWN FORGEABLE EXEMPTION IS CLOSED BY POSITION RATHER THAN BY NARROWING THE ANCHOR,
    which the measurement chose.** `check_dash_punct.sh` exempted a `--` that ends a command's
    options, recognising a command position on `^`, `;`, `&`, `|`, `(` or `$(`; prose inside a
    comment can spell one, so `# see: rm -rf; ls --color -- and then the prose` was exempt. Requiring
    the operand after the `--` to look like an operand was tried and rejected on evidence: it breaks
    two real corpus lines whose pathspec operand is a bare word, an operand-shape survey of all 34
    real terminators puts 2 behind a bare letter and none behind a `$` or an end of line, and the
    forge survives it by quoting two words. SEP reads shell and a comment is not shell, so the erase
    now reaches only as far as the line's first comment opener. Zero corpus churn over 886 files,
    and the rig moved as a narrowing should: positives 7 to 8, negatives 13 to 14, the `separator`
    arm 8 to 9, one arm added at 7, and the five arms keyed on `SEP_ERE` itself unmoved because
    `SEP_ERE` did not change. `TODO.md` carries the numbers.
  - **THE ONE FINDING THIS PASS FILED RATHER THAN FIXED WAS ALREADY FIXED BY THE SAME COMMIT, and
    that is the inversion recorded rather than repeated.** The aspace differ's baseline file's
    header called the EMPTY diff this milestone's deliverable, which R5 falsified in capitals. The
    reason given for deferring it was that the pass had to hold that file byte-identical so the
    differ's verdict was taken against an unmoved baseline; that reason does not reach a COMMENT,
    the instrument ignoring comments and blank lines and taking its verdict over the bare ref alone.
    The header was corrected in the same commit and now says the diff IS the result. The rule the
    deferral broke is that a finding is fixed or raised as a decision and never both, and the
    `TODO.md` entry it opened is closed on that basis.

  - **WHAT R6's SECOND PASS LANDED IN CODE, and the document is brought up to it here rather than
    leaving five stanzas describing the old behaviour.**
    *The boot path names its own refusals.* `mtvec` goes in FIRST in `_start`, before any other
    machine-CSR write, with a machine-mode diagnostic (`.Lmtrap`) and a shared emit-and-finish tail
    behind it, so a CSR that does not exist on some future hart lands in a named refusal instead of
    an undelegated fault reaching a vector nothing wrote. Every machine CSR this port writes is then
    READ BACK, each with its own message: these are WARL, so a write that did not stick is silent
    and the readback is the only witness. A DRAM readback probe runs in machine mode before the
    drop, and a provisional `stvec` goes in inside `kickos_rv64_boot_high`, which closes the window
    in which a supervisor fault had no vector at all.
    **THE READBACKS ARE NOT THE WHOLE GUARD AND THE LIST HERE USED TO SAY THEY WERE.** Three of this
    prologue's checks are MEASUREMENTS rather than readbacks, each because no register answers the
    question: the PMP grant's effective permission under `mstatus.MPRV`, the census of the other 63
    PMP entries that makes one probed word stand for the space, and the misaligned load and store
    that decide whether `medeleg` bits 4 and 6 are required on this hart at all. F8's fourth shape
    carries what each is for and what each still does not reach.
    *`medeleg` bit 9 is UNDELEGATED and the demux arms are DELETED*, not merely unreached: both
    `KICKOS_RV64_CAUSE_ECALL_S` arms and the macro itself are gone from
    `arch/riscv/rv64imac/switch.S` and `rv64_frame.h`. The evidence is in R2.2 above and the cost is
    recorded there as a lost capability. Deleting the arms rather than keeping them is the right call
    on this port and would be the WRONG call on a port that grows a privileged thread running
    app-half text, which is why the price of that step is written down.
    *A fault taken while already on the trap stack is TERMINAL and says which kind it is.*
    `.Ltrap_reentry` reaches `kickos_rv64_supervisor_refuse`, and a canary at the row's base
    separates a trap-stack OVERFLOW from a plain fault inside the reporter. Before this, the second
    frame went on a row that was already unusable and the diagnostic was lost with it.
    *`tp` is forced to zero at both points `gp` is re-anchored*, which is what makes the frame
    sentence in section 3.1 true; see the note there.
    *`sstatus.SUM` IS NEVER SET, and that is the honest close of finding 6.* R2.1 forced it on in the
    restore epilogue and R2.2 moved the force to `kickos_rv64_init` for the boot window; R6 removes it
    outright. What made that possible is that the two raw kernel dereferences of app-half storage,
    `reent_prime` and `reent_seat`, now take the target `struct arch_aspace*` explicitly and go
    through the `kaccess` seam (`kernel/thread/reent.cc`, `kernel/include/kickos/aspace.h`), which
    reaches the frame the SPACE's own tables name from the kernel's half. So there is no residual
    window: S-mode may not load or store a `U` page at all on this port, and a kernel dereference of a
    low-half pointer FAULTS rather than silently landing in whichever process is installed.
    **"NEVER SET" IS ESTABLISHED BY ENUMERATION AND NOT BY SEARCHING FOR THE WRITE THAT IS GONE**,
    which is the only form of that claim worth making. Three instructions in the whole backend write
    `sstatus`, and each is safe for its own reason: `.Lrestore`'s `csrw sstatus, t0`
    (`arch/riscv/rv64imac/switch.S`) writes the frame's own word, and nothing anywhere puts bit 18
    into a frame; `arch_irq_save`'s `csrrci %0, sstatus, 2` only CLEARS bit 1; and
    `arch_irq_restore`'s `csrs sstatus, %0` only SETS, with its argument masked to `SSTATUS_SIE` by
    the paired save (`arch/riscv/rv64imac/arch_rv64imac.cc`), so bit 18 is unreachable through it.
    A `csrs` whose operand were not masked would be the one way back in, which is why the mask is
    part of this record and not just a nesting-safety comment.

    **WHAT THE SEAM ROUTE COSTS, AND THE RULING THAT ACCEPTED IT.** The reent seating path went from
    about 2 instructions to a static count on the taken path of **367 at Sv39 and 427 at Sv48**, with
    **zero `sfence.vma`**: the kernel window covers all 64 MiB of DRAM, so the window-slot route is
    dead code here and the seam pays one page-table WALK rather than a fence. *Those two figures were
    391 and 451 in this record, and the reason given for not re-taking them was false:* the claim was
    that at `-Os` the whole seam inlines, so no symbol in either image carries the path. Nothing on
    this path is inlined. It is NINE discrete out-of-line symbols joined by `jal`, which is what the
    build makes it: the callers and the callees sit in different translation units
    (`kernel/thread/reent.cc` calls `kaccess_to_user`, defined in `kernel/syscall/syscall_mem.cc`,
    which reaches `arch_aspace_acquire` in the backend) and no build in this tree turns on
    link-time optimisation, so a cross-unit inline is not available to the compiler at any `-Os`.
    *Both numbers are sound and one measurement shows it:* the old 391 and 451 reproduce EXACTLY
    under the pre-optimisation `kmemcpy`, so the 24-instruction gap between the pairs is that
    primitive's word-path win and not a disagreement between two counting methods.
    *THE CHEAPER ALTERNATIVE WAS OFFERED AND DECLINED, and the decision is recorded here so a later
    reader meets a ruling rather than what looks like an oversight.* The alternative was bracketing
    `sstatus.SUM` around the one copy, roughly 4 instructions instead of 366, at the price of two new
    `arch.h` seam members. **The ruling was correctness first and optimisation later, on the grounds
    that a buggy kernel is unreliable by essence; the seam route stays and the cost is accepted.**
    What the declined alternative would have kept open is the thing worth naming: inside the bracket,
    a kernel dereference of a WILD app-half pointer still succeeds against whichever process is
    running, so the bracket buys back exactly the class of bug the removal of `SUM` closes, on a
    window a future edit widens for free. A re-opening of this needs to beat that, not the
    instruction count.
    **The route that was NOT taken is worth recording because it is silently green.** Sending those
    two through `aspace_image_alias` passes every arm and is wrong: the alias names the LOADER's
    frame, while `aspace_image_seed` gives only the FIRST space the image's own pages and every later
    space a `data_copy` into fresh frames. Measured, three frames behind one app virtual address
    (`# process: data frames 257/592/617, text frame 1`). An alias-based prime would write the first
    space's copy for every process and the suite would stay green.
    *The map editor gained a same-half VA guard* on `arch_aspace_frame_at` and on
    `arch_aspace_acquire`/`arch_aspace_release` on both backends; arm64's `release` deliberately got
    none, its body being empty, which is a difference to keep rather than to smooth. `PTE_G` is set on
    the transient window's runtime leaves, `aspace_image_alias` gained the domain check recorded in
    F6 above, and `libkickos_lib.a` is dropped from the kernel `.init_array` in BOTH linker scripts.

  - **AND ONE OF THOSE WAS A REAL W^X HOLE ON THE OTHER BACKEND, CLOSED BY A LINKER KEYWORD, which is
    the finding of this pass rather than a tidy-up.** `arch/arm64/chip/virt_arm64/virt_arm64.ld`'s
    kernel `.init_array` lacked `(READONLY)` where `virt_rv64.ld`'s twin carried it. The input
    sections carry `SHF_WRITE`, so without the keyword the output section does too, and with a real
    kernel-side constructor present the kernel's own LOAD segment came out **RWE** where rv64's twin
    reads **R E**. So the W^X property R2.3 defends on the RISC-V side was OPEN on the arm64 side, and
    open through a keyword rather than through a table. It is fixed, `.kickos_app_fini_array` got the
    same keyword beside it, and the fix is witnessed: no LOAD segment of the arm64 selftest image is
    both writable and executable (three read `R E`, two read `RW`), against the rv64 control which is
    the same shape.
    *THE RECOMMENDATION, and it is the transferable part.* A W^X gate reading SEGMENT FLAGS would have
    caught this; `riscv_kernel_wx` reads page-table LEAVES and structurally cannot, because the leaves
    were right the whole time and the hole was one level up in the image the loader consumes. The two
    are not substitutes: leaves answer what the MMU enforces, segment flags answer what the image
    ASKS FOR, and a mismatch between them is exactly this class. Such a gate must be scoped to boards
    that CLAIM W^X: a flat board legitimately has no split to assert, so a fleet-wide version would
    either need an exemption list or would be asserting a property most of the fleet does not have.

**Stage result:** three claims corrected where their mechanism could not support them, the same class
swept across the 82 files the milestone touched, the fleet's panic path draining its device on
every board that shuts down, and a second pass that named the boot path's refusals, retired
`sstatus.SUM` outright and closed a W^X hole the arm64 linker script had left open.

### M6.4 -- the x86_64 backend, and the entry-path verdict

Per F8. The purpose is falsification, so a step here is judged by what it proves about the seam and
not by how much of a PC it lights up.

**RING 3 HERE CAN READ AND WRITE KERNEL MEMORY, AND THAT IS WHERE THIS MILESTONE ENDS RATHER THAN A
DEFECT IN IT.** An external audit of the branch raised it as its top Critical. The ruling is that
M6.4 is the x86_64 PORT and that isolation is the milestone after it, on the same sequencing armv8a
already ran: M6.1 brought the backend up and M6.2 made it isolate. M6.3's R1.5 records the identical
posture on RISC-V, and `arch/x86/x86_64/ring3_x86_64.cc`'s own header states the exposure in these
words, so nothing here is being disclosed late.
- **GRANTED.** One flat link, so the leaves carrying an unprivileged thread's own text also carry
  kernel text, the scheduler's state, the capability table, the per-thread kernel stacks and the
  arch layer's statics, and the conventional-memory grant covers every thread's stack rather than
  the caller's. Read and write, over all of it.
- **NOT REACHABLE, AND IT IS ONE CLAUSE RATHER THAN THREE.** The port grants the user bit over
  exactly two ranges and leaves every other entry as firmware left it. x86 ANDs the permission down
  the whole walk (Intel SDM Vol 3 chapter 5), so an entry ungranted at any level is ungranted at its
  leaf, and that keeps DEVICE REGISTERS out of an unprivileged thread's reach: MEASURED, over a walk
  of the whole live hierarchy, zero reachable leaves in the local APIC band and zero in the low
  legacy range, with the same figures under `-bios` and under `-cpu max`. It also cannot execute a
  privileged instruction, touch a port or raise its own level, and it cannot write
  `IA32_KERNEL_GS_BASE`, which is where the entry takes its pointer from. X4's two denial arms and
  the swap invariant are what hold the last of those.
- **REACHABLE AND WRITABLE, AND THIS RECORD SAID OTHERWISE UNTIL IT WAS MEASURED.** Two of the three
  clauses above used to read THE TRANSLATION TABLES THEMSELVES and THE PER-CORE POINTER THE SYSCALL
  ENTRY LOADS. Both are false on the running hierarchy:
  - `g_kwin_table[2]`, the port's own kernel-window level-3 table, installed by `aspace_init` and
    reachable and writable at CPL3;
  - `g_cpu`, the per-core block `IA32_GS_BASE` names, with `kernel_sp` at offset 0. A ring-3 read
    returned the live kernel-block top.
  Both are `.bss` statics of the one flat link, so they sit at their identity addresses inside the
  2 MiB image leaf the grant opens, and the unit this hardware grants is a leaf. **It is NOT the
  firmware-sibling mechanism an external reviewer proposed**: pre-grant the user bit is set NOWHERE
  in the hierarchy, leaf or non-leaf, so no dormant entry is uncovered by opening an ancestor. That
  hazard is UNFIRED on this firmware and UNESTABLISHED in general; both halves of that sentence are
  the record.
- **THE EXPOSURE DID NOT GET WORSE, TWO BOUNDS THAT MADE IT DESCRIBABLE WERE WRONG.** Once kernel
  RAM is writable, writing a translation table or `kernel_sp` grants nothing an attacker did not
  already have: the scheduler's state and the capability table are writable in the same leaf. The
  ruling on the finding was to leave the grant alone, correct the record and make the instrument
  able to see what it was blind to.
- **WHY THE OLD ARM READ CLEAN, and this is the transferable part.** Its corpus was the tables the
  grant WALKED, three of them against every table in the live tree, and it ran inside `ring3_init`, BEFORE
  `aspace_init` installs the table it would have found. Two blindnesses, either of them sufficient.
  The whole-hierarchy census in `arch/x86/x86_64/probe4_x86_64.cc` runs after `aspace_init` and
  asserts the device clause while PINNING the two exposed pages by role, so a third reachable table
  or an anchor that is not the per-core block reddens an arm and is named on its own line.
- **WHY ONE FLAT LINK DECIDES IT.** At the granularity of the adopted regime the user bit follows
  the ADDRESS, so what ring 0 may touch and what ring 3 may touch can only be separated by
  separating them in the LINK. An in-place edit of the map firmware left has nothing finer to spend.
- **THE NARROWING.** Separate the halves in the link, which is a private half plus its own root.
  That is word for word the work R1.5 names on the sibling backend, and it is a milestone of its own
  because the shared-symbol duplication (T5b's `kickos_privatise_runtime` problem) comes with it.

**X1. Boot to one byte.** Firmware hands over in long mode with paging already on and INTERRUPTS
ENABLED (F8 cites the section), so the entry disables them before anything else. Take the memory map,
leave boot services, land in KickOS.
*Expected:* a known string on COM1 under `qemu-system-x86_64` with UEFI firmware, from an image with
no kernel in it. The handoff state is documented rather than discovered, so a step that behaves as if
it were unknown is doing unnecessary work. **And then the same image under a SECOND firmware**, which
is the same discipline F8 applies to backends applied to the boot path: a hypervisor's own EFI is a
different implementation of the handover contract, and it runs under hardware virtualisation rather
than emulation. One firmware proves the image boots; two prove the contract was read rather than
fitted. This step also FIXES the boot contract and the image format. If UEFI bring-up starts
to become the milestone, the documented fallback is a multiboot hand-off with no firmware -- but taking
it forfeits the adopt-an-active-regime case that X5 exists for, so record that choice deliberately
rather than drifting into it.

*Landed at X1.* `arch/x86/x86_64/entry_x86_64.cc` for the firmware ABI entry, with
`arch/x86/x86_64/include/kickos/arch/uefi.h` and its `portio.h` neighbour for the tables it
reads and the port accessors it needs; `arch/x86/chip/q35` and `boards/qemu-x86_64` for the chip and the board, selecting no
capability this step had earned; `cmake/toolchain-x86_64-uefi.cmake`,
`cmake/x86_64_boot.cmake` and `cmake/presets/x86.json` for host objects linked to a PE32+
application through `i386pep`; and `tools/esp-x86_64.sh` plus `tools/run-qemu-x86_64.sh` for
the boot partition built without root and the witness over it. `ninja x1-run` is the arm. The
boot contract and the image format are fixed here, and the multiboot fallback was not taken.

**THE SECOND FIRMWARE IS A SUBSTITUTION, and it is ONE implementation in TWO PACKAGINGS rather
than two implementations.** This step asked for a second firmware IMPLEMENTATION, running under
hardware virtualisation. What the witness actually takes is `KICKOS_X1_FIRMWARE=pflash`, the
split `OVMF_CODE_4M.fd` plus a writable copy of `OVMF_VARS_4M.fd`, and
`KICKOS_X1_FIRMWARE=bios`, the combined `/usr/share/ovmf/OVMF.fd` through `-bios`. Both files
ship in the same Debian `ovmf-generic` package, and the image reads the same
`firmware=Debian distribution of EDK II` at the same `revision=0x0000000000020046` under each,
so what the two arms separate is the LOAD path and the variable store, not two readings of the
handover contract. The virtualisation half is unavailable rather than skipped: `/dev/kvm` on
this bench belongs to a group the invoking user is not in, so every run in this milestone is
TCG. So the claim this step earns is the narrower one: the handover state is MEASURED against
UEFI 2.11 section 2.3.4 rather than fitted, and it is measured under one implementation. A
genuinely second implementation is what would close the ask, and none is on this box.

**X2. Descriptor tables and the fault report.** The segment descriptors x86 needs even without
segmentation, the interrupt table, and a task-state segment carrying the kernel stack.
*Expected:* a deliberate fault reports and halts with its vector and error code. Note that an absent
or malformed interrupt table triple-faults, which presents as the EMULATOR RESETTING rather than as a
fault report -- so an early fault that reboots the machine is this, not a bad handler.
*Landed at X2,* `arch/x86/x86_64/desc_x86_64.cc` for the tables,
`arch/x86/x86_64/trap_x86_64.S` for the entry and `arch/x86/x86_64/fault_x86_64.cc` for the report,
with `arch/x86/x86_64/include/kickos/arch/desc.h` and `arch/x86/x86_64/include/kickos/arch/trap.h`
beside them. Seven things a reader would otherwise re-derive.
**FIRMWARE'S GDT AND IDT ARE REPLACED, AND ONLY PAGING IS ADOPTED.** Both tables sit in memory the
UEFI memory map calls boot-services data, which is free for reuse the instant `ExitBootServices`
returns, and its gates name handlers that stop existing at the same moment; neither carries a
task-state-segment slot this port could claim either. So the adopt-an-active-regime case F8 picked
this backend for is about PAGING alone, and `CR3` is the one piece of handover state X2 leaves
untouched. The install runs AFTER `ExitBootServices` for the converse reason: replacing the tables
while boot services are live takes the ground out from under the firmware's own calls, that call
included.
**The banner is its own, `=== X86_64 EXCEPTION`,** on R1.3's rule of one banner name per ARCH: the
dump lines differ from every other backend's and two boards on one ctest ladder would otherwise be
indistinguishable on the wire. `tests/lib/panic.ere` gains the alternative inside its EXCEPTION group
and `check_panic_banners.sh` gains `arch/x86/x86_64/fault_x86_64.cc` in its reporter list, without
which a later deletion of the banner reads as a clean tree.
**The report cannot use a pointer table, and that is an image-format constraint rather than a style
choice.** The PE32+ image is position independent and carries an EMPTY base-relocation directory, so
an array of `char const*` vector names would be resolved to link-time addresses and be wrong the
moment firmware loads the image anywhere else. Every name is a `return` of a literal, which the
compiler reaches PC-relative. The same constraint shapes the vector table: the 256 stubs are reached
through a table of OFFSETS from a base symbol, computed in the assembler as same-section differences,
and never through pointers or an assumed pitch.
**ALL 256 GATES ARE FILLED and they are interrupt gates, not trap gates.** A vector with no entry
escalates to a double fault, so a spurious or unplanned vector must still report; and an interrupt
gate clears the interrupt flag on entry where a trap gate would leave it as the interrupted code had
it. THREE gates name an interrupt-stack-table slot, not one: double fault, the non-maskable interrupt
and the machine check (`arch/x86/x86_64/desc_x86_64.cc`). The double fault's slot is the obvious one,
a double fault arriving BECAUSE the interrupted stack could not take a frame, so it is the one vector
that cannot report on it. The other two are not decoration and they are not symmetry: they are the
ENTIRE argument that closes the three-instruction window in the fast syscall entry. `IA32_FMASK`
holds off maskable interrupts, and neither of those two is maskable, so on those two vectors the only
thing that keeps a frame off the caller's stack pointer is a stack the descriptor names. The report
says which stack it ran on, and on the double-fault arm the stack selector is nulled, which is what
distinguishes a real stack switch from a reused stack.
**THE STEP'S CONTRACT WAS RIGHT ABOUT THE RESET AND UNDERSTATED THE CASE THAT PRODUCES IT.** Measured
both ways: an interrupt table loaded with a zero limit triple-faults as the text says, and so does
NOT LOADING ONE AT ALL. After `ExitBootServices` the firmware's still-loaded table is no longer
serviceable, so a defeated `lidt` presents exactly as a malformed one rather than as a firmware dump.
And under `-no-reboot`, which every witness here passes, the reset presents as the EMULATOR EXITING
before the report rather than as a second boot; the reset loop is only visible with that flag off.
Defeating `lgdt` is a third spelling of the same silence, `ltr` then faulting against a firmware
descriptor with no table yet loaded to report it.
**ONE IMAGE PER FAULT CLASS, because the report ends the image.** Ten of them, `none` plus nine
faults: `arch/x86/x86_64/probe_x86_64.cc` is compiled once per class and only the class differs
between the images; `tools/run-qemu-x86_64-x2.sh`
takes the class and the image and holds the report against it. Each arm prints the instruction pointer
it EXPECTS, taken from a label in the assembly rather than from the frame, so a frame read at the
wrong offset reports a plausible address that does not match. The `none` class keeps X1's expected
result reachable from a tree that has X2 in it.
**A DECODE WITNESSED AT ONE VALUE IS NOT WITNESSED.** The first page-fault arm reads an error code
of zero, so every bit of the decode prints 0 and a ladder shifted one position along passes it. The
store and instruction-fetch arms exist for exactly that: three distinct error codes make the write
and fetch bits falsifiable, and the selector arm loads a selector past the end of the table so its
index leg has a non-zero value to get wrong. What still has NO producible witness here: the present,
user, reserved, protection-key and shadow-stack bits, all of which stay zero; the table leg of the
selector decode, since a complete 256-gate table at ring 0 refuses nothing and this port installs no
local table, so `idt` and `ldt` become producible only at X4; and the vectors real silicon raises,
18, 20 and 21 among them, which this emulator does not.
**TWO OF THOSE PREDICTIONS DID NOT COME TRUE, and F8's re-read below carries the correction**
rather than this paragraph being quietly rewritten: `idt` and `ldt` did NOT become producible at
X4, and the `user` bit did not either, ring 3 taking no page faults under a flat over-grant.
**THE ARCH DOES NOT GROW A STANZA IN `arch/CMakeLists.txt` HERE, and X1's note that it would was one
step early.** That file builds a static library for the kernel ladder to link, and the seam bodies
that ladder needs, the console above all, land at X3. X2's sources join X1's object libraries and its
images are still linked by the custom command, so X3 is the step where the arch moves.

**X3. Console, timer, interrupts, switch, idle.** A polled COM1, the local APIC timer as the tickless
one-shot, the monotonic clock, the switch, and the halt instruction for idle.
*Expected:* the S5 and S6 PROPERTIES hold on this arch, witnessed at the arch seam. The polled
console is a deliberate scope cut: it keeps the IO-APIC and the legacy interrupt controller out of the
milestone entirely and leaves the APIC timer as the only interrupt source. Mask the legacy controller
anyway -- an unmasked one delivers spurious vectors into an interrupt table that has just started
working.

*Landed at X3.* `arch/x86/x86_64/arch_x86_64.cc` for the ISA-generic half of the seam,
`arch/x86/x86_64/apic_x86_64.cc` for the timer, the clock and the doorbell,
`arch/x86/x86_64/switch.S` for the frame and the entries, and
`arch/x86/chip/q35/chip_q35.cc` for the platform's own edges, with
`arch/x86/chip/q35/com1_q35.cc` split out of the old chip TU so the kernel-free images still print.
`arch/x86/x86_64/probe3_x86_64.cc` and `tools/run-qemu-x86_64-x3.sh` are the witness. Twelve things a
reader would otherwise re-derive.

**THIS STEP'S EXPECTED RESULT WAS WRONG, and the correction is the same shape the plan already
carries twice.** "The S5 and S6 arms pass with no change to the arms" names TAP arms of
`user/apps/common/selftest`: `rr_interleave` for the interleave, `sleep_order` and the timeout arms
for the deadline, `irq_spurious` and `irq_mask_coalesce` for the mask-versus-unmask split. Every one
of them is an unprivileged app reaching the kernel through `arch_syscall`, which is X4's, and the
harness they report through is itself a syscall. So they cannot pass here, and the S2-through-S5 note
above says exactly why: **a step in this position takes its witness from an ad-hoc link that supplies
what the step is not about, and X4 is the first step whose image is the real one.** M6.3 recorded the
same thing for R1.2 through R1.4. What X3 owes is therefore the PROPERTIES, not the arms: a deadline
honoured and not honoured early, a clock that never goes backwards across an arm and a disarm, the
timer firing and the line being unmasked tested SEPARATELY, and two threads round-robin in a
deterministic order. Twenty one arms, each printing its own name and outcome so a missing arm and
a failing arm are different failures. (Re-measured at X6, which said twenty; the twenty first is
the doorbell pair the 2026-08-29 entry-path audit added.)

**THE ARCH GROWS ITS STANZA AND THE EARLY RETURN GOES, BUT NOT AS A DELETION.** X2 predicted the
`return()` in the top-level `CMakeLists.txt` would simply go once the console seam existed. Two
independent things hold the APPLICATION layer back rather than one, and only the first was on X2's
list: `arch_syscall` gates every app link, and the image firmware loads is a PE32+ application, which
no `-T` through the compiler driver can write. So the LIBRARY ladder configures and builds (the
kernel, the arch and the chip archives all link against this seam), `KICKOS_BUILD_APPS` is off for
this arch, and `cmake/x86_64_boot.cmake` keeps linking the images over `ld -m i386pep`, now beside the
ladder instead of in place of it. The X3 image links the two ARCHIVES rather than their objects, so a
fallback TU and the chip's own definition of the same seam resolve by member order exactly as the
application link would resolve them.

**TWO SHARED FILES NEEDED AN ENTRY, and both edits are inert for every existing arch by construction.**
`arch/CMakeLists.txt`'s chip block reads the chip linker script as TEXT with no existence guard, so
the configure died on a missing path before it reached anything else: the read is now guarded, and
every other chip ships a script. And `user/CMakeLists.txt` had two postures, the sim and the cross
toolchains; this target is a THIRD, building with the host compiler against no C library at all, so
it takes neither the newlib porting layer nor `sim_exit.cc`.

**THE TIMER'S FREQUENCY IS MEASURED, and F8's "ask the hardware" rule has nowhere to ask.** There is
no identity register for the local APIC timer's input clock, and the CPUID leaves that report the core
crystal clock (0x15, 0x16) are absent on this emulator's processor model, so the figure comes from a
window against the platform's i8254. Two consequences worth stating. The timestamp counter's rate is
unknown for the same reason and is measured in the SAME window, so the two figures are consistent with
each other whatever the reference's own error is. And the reference returns the ticks it ACTUALLY
observed rather than the ticks it was asked for, which is what lets a dead counter be refused instead
of producing a plausible frequency: a spin bound with no such report would have turned a wedged i8254
into a wrong deadline. Measured under QEMU 11.0.3 TCG: the APIC timer at about 1.0 GHz and the
timestamp counter at about 2.0 GHz, against the i8254's 1193182 Hz. THE DIGITS BELOW THE FIRST ARE
PER RUN and do not reproduce: both figures come out of one calibration window against a host-paced
emulated reference, so quoting them to five places would be quoting noise. What is stable is the
ratio of about two between the two counters and the reference's own exact rate.

**X2APIC IS NOT AVAILABLE ON THIS MODEL, so the port reaches the register block through memory after
all.** The backend prefers x2APIC where CPUID reports it, and not for speed: in that mode the whole
block is MSRs, so the step touches no memory the firmware's identity map has to cover, which is
exactly the claim F8 warns is about the MAP and not about the machine. The emulator's default CPU does
not report it, so the xAPIC window `IA32_APIC_BASE` names is what the DEFAULT witness exercises, and
that window IS reachable through the map OVMF built. **That is a fact about this firmware and this
machine, and X5 may not inherit it as a fact about the architecture.** BOTH LEGS ARE EXERCISED,
though, which corrects this paragraph's own earlier claim that the x2APIC path was present and
unwitnessed: under `-cpu max` and under `-cpu Skylake-Server` the processor reports x2APIC, the
backend takes the MSR path, and the arms pass. What stays true is that the DEFAULT model takes the
memory path, so the leg an ordinary run exercises is the one whose reachability depends on the
firmware's map.

**THE INITIAL COUNT IS RELATIVE WHERE A64'S COMPARE IS ABSOLUTE, and that changes what the arm owes.**
`ktime_rearm` calls `arch_timer_arm` on every context switch with the same value, and the sibling
backend's comment says the write is idempotent because the register names an INSTANT. Here it starts a
countdown, so a repeated arm would push the deadline later on every switch. Two things answer it: the
delta is recomputed from the clock at every arm, and the arm dedups on the requested deadline, which
is the quantity `ktime_rearm`'s own comment says the backends dedup on. TSC-deadline mode would give
the absolute compare and is not used: it is gated on its own CPUID bit and would still need the
calibration above for the nanosecond conversion.

**HLT IS NOT WFI, and this is the one place the idle seam does not transfer.** An ARM WFI wakes on a
pending interrupt whatever `PSTATE.I` holds, so every sibling backend can spell `arch_idle_wait` as
one instruction and be called with interrupts masked. A HLT under a mask parks the processor until
reset. The emulator hides nothing: it idles with the core halted and the image never returns, which
presents as a hang and not as a fault. So the body reads the flag and, where it is clear, sets it for
the halt and puts it back, `sti` leaving a one-instruction shadow in which no interrupt is taken. The
kernel's own idle thread runs with interrupts live and would not have exposed this; the arm that does
is the one that enters under `arch_irq_save`.

**THE LEGACY CONTROLLER IS REMAPPED AND THEN FULLY MASKED, AND NEITHER IS THE OUTERMOST GUARD.**
Firmware leaves the master i8259 based at vector 8, which is the DOUBLE FAULT vector, so a legacy line
that asserted once would arrive as a double fault on the interrupt-stack-table stack and be reported
as one: the mask is what stops the assertion and the remap is what stops the lie. Defeating either was
UNWITNESSABLE, and finding out why is the useful part. The i8259 reaches an APIC-era core only through
the local APIC's LINT0 entry in ExtINT delivery mode, and `apic_init` masks every local-vector source
the version register reports, LINT0 among them. So the guard that actually holds is one level up from
the one this step's text names. Opening LINT0 as unmasked ExtINT is what makes the other two
witnessable, and then they separate cleanly: with the remap intact a legacy line reports as vector 32,
and with the remap defeated as well it reports as vector 8, a double fault that never happened. Both
measured. The step's own text called an unmasked controller a source of "spurious vectors", which
understates the vector-8 case by one word, and it credits the wrong register with the protection.

**THE INTERRUPT CONTROLLER IS SOFTWARE, with a self-directed local-APIC interrupt as its one physical
doorbell.** X3's scope cut leaves no I/O APIC, so a logical line has no hardware pending state: the
mask and the one-deep latch are two bitmaps and the doorbell carries the identity, which is the shape
`arch/include/kickos/arch/arch.h` already names the SINGLE-DOORBELL CONTRACT for the software
backends. Nothing about it is x86-specific except which instruction rings the bell.

**THE TWO CONSOLE DEFECTS M6.3'S R1.4 FOUND WERE BOTH PRESENT ON COM1, and one of them is
unwitnessable here.** The polled writer was UNBOUNDED, and `arch_console_write_sync` aliases onto it
through the lone-TU fallback, so the panic, fault, assert and pre-arm paths would have hung on a
wedged UART; it is bounded now. And `arch_console_flush_sync`'s fallback ASSERTS the console cannot
outrun `arch_shutdown`, which is false of a 16550 whose 16-byte transmit FIFO the bring-up enables and
doubly so of a writer that polls the holding register rather than the transmitter: the real body polls
transmitter-empty. QEMU's model hands each byte to its chardev on the register write and reports both
bits set with it, so a truncation cannot be produced and the body is kept anyway, exactly as the two
sibling chips keep theirs.

**AN ADDRESS TAKEN UNDER `-fpie` IS SILENTLY WRONG IN THIS IMAGE FORMAT, and X2's note on the empty
base-relocation directory did not go far enough.** Taking the address of a function DECLARED in
another translation unit emits `R_X86_64_REX_GOTPCRELX`, a load through a global offset table. GNU ld
relaxes that form to a `lea` when the symbol turns out to be local to the link; `ld -m i386pep` builds
no such table and performs no such relaxation, so the LOAD survives with its displacement resolved
against the function itself. The thread-exit trampoline's address came back as the trampoline's own
first eight bytes, which is a non-canonical value and a general-protection fault the moment the
thread returned through it. Three things about the diagnosis are worth keeping. `-fvisibility=hidden`
does NOT fix it, measured both ways: it covers what a translation unit defines and leaves an undefined
external reference on the GOT form, so each such declaration carries the attribute instead. The
relocation's name is TRUNCATED by readelf to `R_X86_64_REX_GOTP`, so a grep for the full spelling
reports zero on a file that carries it. And `tools/check-x86_64-no-got.sh` refuses the relocation
before every link in this arch, because the link itself is clean and the failure arrives much later.

**A 64-BIT TARGET THAT DOES NOT TRANSLATE IS A NEW COMBINATION, and the TCB budget had never seen
one.** `kernel/include/kickos/thread.h` prices `cap_width` and `cap_reply_live` at four bytes wherever
the capability table has more than one chunk. The CHUNK COUNT HERE IS TWO, not one: configure prints
`cap table: 2 chunks of 8 = 16 slot(s) reserved per run`, so this board is on the many-chunk side and
the sentence that put it on the other side was wrong about which case it is. What the measurement
actually says survives that correction and is the part worth keeping: at 64 bits the members before
that pair end four bytes short of the TCB's eight-byte alignment, so the pair lands in tail padding
that exists whether or not the pair does. It is FREE either way, and a one-chunk build measures the
same as a many-chunk one. The budget now says so, and the companion assert that the last member closes the
struct is stated against the struct's ALIGNMENT rather than exactly, which still refuses a hole a
reordered member opened. Both edits leave every 32-bit target and every passing 64-bit board
unchanged, which is what the fleet run checks. This is the mirror of that budget's own warning about
measuring on the host.

**A `DEPENDS` ON AN OBJECT LIBRARY IS ORDER-ONLY, and that turned a stale image into a passing
witness.** Each image here is an `add_custom_command` over `$<TARGET_OBJECTS:>`, and naming the
OBJECT library targets in `DEPENDS` does not make its object FILES inputs of the link: an edited
source rebuilt its object and the link was not retaken, so the image on disk was one revision behind
its own sources and the witness read as green. `DEPENDS` now lists the objects. Worth knowing because
the failure is invisible from the run: the serial log is a real log of a real boot, just of a
different program. A `$<TARGET_FILE:>` on a static library does not have the problem, which is why
the archives the X3 image links were never affected and only the probe was.

**WHAT REMAINED BEFORE THIS BOARD JOINED THE CTEST LADDER, measured rather than surveyed.
X4b DISCHARGED EVERY ITEM ON THIS LIST and the board is on the ladder: read the list as X3's
inventory of the debt, not as a debt that stands.** X2's list had four items and two were done at
this step: the top-level `return()` was gone, and a `BOARD` branch in `kickos_add_qemu_test` plus a
way past `$<TARGET_FILE:>` were still owed. Three more were not on it.
`arch_syscall`, `arch_syscall64` and their two kernel-half twins are X4's and gate the app link
outright. `tests/lib/gate.sh` boots every image as `-kernel <elf> -semihosting`, which cannot start a
PE32+ application at all: this board needs OVMF pflash and a FAT system partition, so either that
library grows a branch or the x86 scripts stay standalone as they are. And `tests/static/test_classes.txt`
plus `check_test_labels.sh` classify every registered test by the program ctest resolved for it, so
each x86 script owed a line there. The three witnesses stayed `ninja` targets until X4b, which took
all of it: `tests/lib/gate.sh` carries the `KICKOS_BOOT=uefi-pe` branch that boots the image off an
OVMF pflash pair and a FAT system partition, `cmake/presets/x86.json` registers the `qemu-x86_64`
test preset, and the five witnesses are `ninja` targets BESIDE the ladder rather than instead of it.

**X4. Ring 3 and the syscall entry.** The fast syscall pair, with the kernel stack loaded EXPLICITLY
because F8 shows the instruction loads none, the flag mask programmed so the interrupt flag is clear
on entry, and the per-core pointer through the segment base x86 uses for exactly that.
*Expected:* S7's arm passes, and the kernel stack is demonstrably loaded BY the entry rather than
inherited from the caller. This is the step where M5.2.1's claim is re-earned by hand on a second
arch, which is itself worth knowing.

*Landed at X4.* `arch/x86/x86_64/ring3_x86_64.cc` for what makes an unprivileged level reachable
and for the fast pair's registers, `arch/x86/x86_64/switch.S` for the entry and the two syscall
leaves, `arch/x86/x86_64/pe_image.ld` for the section layout every image on this board now
needs, and `arch/x86/x86_64/include/kickos/arch/ring3.h` for the numbers switch.S and the C++
side both read. `arch/x86/x86_64/nokernel_x86_64.cc` carries the declining kernel half the X1,
X2 and X3 images link, and `arch/x86/x86_64/probe4_x86_64.cc` plus `.S` and
`tools/run-qemu-x86_64-x4.sh` are the witness: **94 arms**, each printing its own name and outcome.
(A LIVE figure, re-derived by running it on 2026-08-29. This record said 72, X6 re-measured it at
74, X8's table-exposure arm took it to 75, and the CPL3 reachability census took it to 94: fifteen
new arms, three asserting the device clause, three the walker's floor and its two controls, and
nine pinning the measured exposure.)
Fourteen things a reader would otherwise re-derive.

**ADOPTING A LIVE TRANSLATION REGIME DOES NOT GIVE YOU RING 3, and both this stanza and F8 read
as though it did.** Measured on the firmware this port boots under: every entry from the root to
the leaf covering the loaded image, its data and the conventional memory the map names carries
the user bit CLEAR. The permission is ANDed down the walk (Intel SDM Vol 3 chapter 5), so ring 3
could not reach a single byte, its own first instruction included. And the tables are mapped
READ-ONLY with `CR0.WP` set, so the first entry the grant tried to edit took a WRITE page fault
at ring 0 on the root itself, which presents as a page fault inside `arch_init` and reads like a
wild pointer. Both are this firmware's choices rather than the architecture's, and X5 may not
inherit either as a fact about the machine. The grant lifts `CR0.WP` for the edits and puts it
back, with interrupts still masked and nothing else running; it is the only writer of a
translation table in the image.

**ONE ROOT SERVES BOTH LEVELS HERE WHERE M6.3's R1.5 NEEDED TWO, and the asymmetry is an ISA rule
rather than a design difference.** RISC-V forbids a supervisor instruction fetch from a page
marked for unprivileged access irrespective of SUM, so one flat link cannot give kernel text and
app text different `U` bits in one root and that backend ships a root per privilege level. x86
forbids it only under supervisor-mode execution prevention, which this port REFUSES rather than
assumes: with it on, a flat image's own kernel text would be inside the granted leaves and ring 0
could not fetch it. Supervisor-mode access prevention is refused for the mirror reason, nothing
here issuing the instructions that lift it per access. Both are clear on this processor and both
are read and reported rather than assumed. So the grant is an in-place edit of the adopted regime
and no root is written. Measured: 212 leaves granted, none already carrying the bit, and NONE of
the tables walked to perform the grant lies inside a granted range. **That last figure answers a
narrower question than it reads as**, its corpus being the three tables the grant walks and its
moment being the end of `ring3_init`; the whole-hierarchy census taken after `aspace_init` finds
one of the tables in the live tree reachable and writable at CPL3, the kernel window's own
level-3 table. The M6.4 stanza carries it.

**THREE INSTRUCTIONS RUN ON THE CALLER'S STACK POINTER, none of them dereferences it, and none
can fault or be interrupted.** `swapgs`, a store of `rsp` through the gs base into the per-core
block, and a load of the kernel stack from the same block. The swap takes no operand; the two
moves address a fixed kernel object rather than anything the caller chose, so no store lands on
caller-chosen memory and no load can fault on one. **That block IS reachable and writable from ring
3 on this posture**, measured, so the argument here is about the caller's stack pointer and not
about the block's contents; what covers the contents is that kernel RAM is writable anyway.
Interrupts are already masked when the first of the three
executes, `IA32_FMASK` naming the interrupt flag and SYSCALL clearing every flag the mask names.
The two classes the flag does not hold off, the non-maskable interrupt and the machine check, have
interrupt-stack-table slots of their own, so even those build their frame off this stack pointer
rather than on it. **F8's note understates the hazard by one class:** it says the mask must clear
the interrupt flag "or interrupts are live on a user-controlled stack", which is true and leaves
out that the flag covers neither of those two vectors at all.

**SYSCALL RECORDS NO PRIVILEGE LEVEL ANYWHERE, so the branch belongs in the leaf and not in the
entry.** It saves the return address in `rcx` and the flags in `r11` and keeps no note of where
it came from, so the entry cannot ask. Both siblings get the answer from hardware, RISC-V from
`sstatus.SPP` inside its own entry and AArch64 from which vector slot was taken, and AArch64 in
fact does not service an EL1 `svc` at all. Here `arch_syscall` reads its own `cs`, which is
readable at either level and cannot be forged, and a privileged caller takes a PLAIN CALL to
`syscall_dispatch`: it is already in privileged thread context on its own continuation, which is
exactly what arch.h's syscall contract asks for and what the sim backend's `arch_syscall` has
always been. That branch is also what lets the entry's `swapgs` be unconditional.

**THE SELECTOR LAYOUT IS THE FAST PAIR'S AND NOT A PLAUSIBLE ARRANGEMENT.** SYSCALL loads CS from
`IA32_STAR[47:32]` and SS from that field PLUS 8; SYSRET loads SS from `IA32_STAR[63:48]` plus 8
and CS from the same field plus 16, forcing both privilege fields to 3 (AMD APM Vol 2 section
6.1.1; Intel SDM Vol 2). So the kernel data descriptor must sit one entry above the kernel code
descriptor and the user data descriptor one entry BELOW the user code descriptor. Three
`static_assert`s state it. Two consequences X2 had hard-coded: the task-state segment descriptor
moved from selector 0x18 to 0x28 and the table limit from 0x27 to 0x37, so
`tools/run-qemu-x86_64-x2.sh` names both new figures.

**SYSRET LOADS NO STACK POINTER EITHER, and F8's note on SYSCALL does not say so.** One probe arm
enters ring 3 through `SYSRETQ` with the ring 3 stack pointer seated by hand, and the stub it
lands on reports its own `cs` and `ss` back through the trap. That arm exists because the port's
own return path is an `iretq` off the frame, so nothing else in the image would execute a SYSRET
and half the layout above would be a manual claim with no witness.

**THE INCOMING BLOCK IS PUBLISHED TO TWO PLACES WHERE THE SIBLINGS NEED ONE.** An interrupt gate
entered from ring 3 takes its stack from the task-state segment's `rsp0`; the fast entry takes
its own from the per-core block. AArch64 writes `SP_EL1` and RISC-V writes `sscratch`, and one
write serves every entry class there. Both are written together on every switch, and a blockless
context publishes ZERO rather than the last thread's value, so a ring 3 entry that should not be
possible faults at once instead of building a frame on another thread's block.

**THE WRITABLE-REGISTER CLASS R1.5's GLOBAL POINTER BELONGS TO EXISTS HERE, and what closes it is
an MSR rather than a re-anchor.** The analogue is the `fs` and `gs` segment BASES, which is what
the kernel resolves the per-core block against. Ring 3 can change the live base at will: loading
a selector into `%gs` is unprivileged and sets the base from the descriptor, and with
`CR4.FSGSBASE` on it could write the base directly. What makes that harmless is `swapgs` plus
`WRMSR` being ring 0 only: the value the entry ends up with comes from `IA32_KERNEL_GS_BASE`,
which ring 3 cannot write, and the invariant the epilogue maintains is that the MSR holds the
per-core pointer exactly while ring 3 is executing. That is why the trap entry owes a CONDITIONAL
swap on the interrupted `cs` and the shared resume epilogue owes one on the FRAME's `cs`: the
epilogue's is on the frame and not on where the frame came from, because a switch resumes a frame
some other entry built, and what matters is the level the `iretq` delivers control to. Without the
epilogue's swap the next thread to reach ring 3 leaves a stale user base in the MSR and its first
syscall loads a stack pointer from it. **`CR4.FSGSBASE` is clear on this processor and would not
open the hole if it were set,** which is stated because the opposite reading is the natural one.
**And `r11` is NOT a second instance of the class:** it is an OUTPUT of SYSCALL, so a thread
cannot hand chosen flags in, and the frame masking in switch.S is defence in depth behind the
hardware rather than a closed hole. Measured: deleting the masking changes no arm.

**FAULT ATTRIBUTION READS MEMORY WHERE BOTH SIBLINGS READ A REGISTER, and that decides the order
of the two tests.** RISC-V reads `sstatus.SPP` and AArch64 the exception level out of `SPSR`, both
of which arch.h calls always-valid because they come from a register; x86's only record of the
interrupted level is the `cs` the hardware PUSHED. So `kickos_fault_frame_on_kernel_stack` comes
first and is what makes that word believable, the frame of a ring 3 fault being placed by the
hardware at the top of the running thread's own block from `rsp0`. **The second test is not
redundant with the first**, and one arm exists to prove it: a fault taken by the DISPATCH, at
ring 0, lands on that same block, so the block test alone would credit it to the thread, and its
pushed `cs` names the kernel selector. Both directions are in one image, the ring 0 control being
stepped past rather than ending the run.

**THE REDIRECT REWRITES FIVE FIELDS, where RISC-V rewrites three and AArch64 two.** An `iretq`
reloads the code selector, the stack selector, the flags and the stack pointer along with the
instruction pointer, so each is a decision. The stack selector is not optional: `iretq` refuses a
return whose stack selector does not carry the privilege the code selector names.

**THE TWO USER-POINTER ORACLES READ THE IMAGE'S OWN SECTION TABLE, and an extent pair was never
going to work here.** X3 left both failing closed and named the loaded-image extents as what they
owed. The extents alone are not enough for two reasons measured on the built image. The board is
compiled `-ffunction-sections -fdata-sections`, so the writable set is forty-odd separate
sections interleaved with read-only ones and no pair of bounds separates them. And the debug
sections carry the discardable attribute, which a loader is free not to map at all, so the image's
own extent is not a claim that every byte in it is present. So the read oracle admits a range that
lies wholly inside ONE mapped, non-discardable section and the write oracle additionally requires
that section's write attribute, both walked from the base the image was LOADED at through
`__ImageBase`, which is PC-relative and therefore right wherever firmware put the image. The arena
is deliberately not admitted: a thread's own stack is a region the kernel composes, so the region
walk above these two answers it, and admitting the conventional-memory range here would hand every
caller every other thread's stack.

**A LINKER SCRIPT ARRIVED WITH THIS STEP AND IT IS NOT OPTIONAL.** X3's note that this chip ships
no script was right about the CHIP and wrong about the IMAGE. `ld -m i386pep` has an internal
script that collects `*(.text.*)` and names no wildcard for `.data.*`, `.rodata` or `.bss.*`, so
every such input became a PE section of its own, page-aligned. X2's and X3's images survive that
at 63 sections; the X4 image does not. Past about seventy sections `SizeOfHeaders` passes 0x1000
while the first section's virtual address stays there, and EDK II's loader refuses a section whose
virtual address lies inside the headers: it presents as `failed to load ... : Unsupported` with
NOTHING at all on the serial line, which reads like a bad image rather than like a layout.
`--strip-debug` does not fix it, the count coming from the data sections. `arch/x86/x86_64/pe_image.ld`
is `ld -m i386pep --verbose` verbatim plus three wildcards, and it takes the X4 image from 63
sections to 14 and `SizeOfHeaders` from 0x1200 to 0x600. Every image on the board now links
through it, so X1, X2 and X3 were re-witnessed.

**KICKOS_BUILD_APPS IS STILL OFF, AND THE REASON IS NO LONGER THE SEAM.** X3 listed five things
between this board and the ctest ladder. `arch_syscall` and `arch_syscall64` exist and are
witnessed, and the PE section layout above is settled, so a FULL KERNEL image was held against the
remaining list rather than surveyed: linked by hand over the eight archives the root
`CMakeLists.txt` already groups, it boots, prints the banner, runs `sched::start`, reaches
`kickos_app_main` and returns through `kos_shutdown`, with a 20 ms `kos_sleep_ns` measuring 21.1 ms
so the idle thread and the tickless one-shot are both live. Four things it needs that this step does
not supply. `kickos_reent_seam` is named by `kernel/thread/reent.cc` under `!KICKOS_ARCH_SIM` and
`user/CMakeLists.txt` compiles no newlib layer on this arch, so a no-libc THIRD posture is a real
gap in that seam rather than a missing file. `__kickos_app_init_array_start` and its end are stated
by every chip script and by nothing here, so the app-constructor window has to come from the PE
script. `libkickos_kernel.a` carries EIGHT global-offset-table relocations where every other
archive carries none, one of them fatal (`kickos_root_entry`, whose address came back as its own
first eight bytes) and seven of them WEAK declarations that hidden visibility does not fix, since
a PC-relative form cannot encode "absolute zero"; `tools/check-x86_64-no-got.sh` never saw them
because it is handed the image objects and never the archives. And a full image carries a NON-EMPTY
base-relocation directory, from a static function-pointer table in the scheduler policy, so the
toolchain file's claim that `-fpie` leaves that directory empty holds for the kernel-free images
only. A `BOARD` branch in `kickos_add_qemu_test`, a way past `$<TARGET_FILE:>` and
`tests/lib/gate.sh`'s `-kernel <elf> -semihosting` assumption are all still owed, and
`tests/static/test_classes.txt` owes a line per x86 script. The five witnesses stay `ninja` targets.

**X4b. THE LADDER, and every item on that list is discharged.** `KICKOS_BUILD_APPS` is on, the board
runs its own `ctest`, and the five `ninja` witnesses are unchanged beside it. Each of the eight took
a different answer and three of them are worth carrying:

- *The reent seam's third posture is an ABSENCE, not a descriptor.* `KICKOS_LIBC_REENT` replaces
  `!KICKOS_ARCH_SIM` at every reent site, and it is 0 on the sim because that libc owns its own
  state and 0 here because there is no libc: two different reasons for the same 0, which is why it
  is a knob and not a second reading of `KICKOS_ARCH_SIM`. A descriptor of zero slots was the
  tempting alternative and it is wrong: `reent_seat` writes THROUGH the seat word, so an empty
  descriptor is a null store rather than a no-op. The one C-library name KickOS's own userspace
  layer calls is `exit()` (`user/src/uart_service.cc`), and `user/src/nolibc_exit.cc` defines it
  onto the same `KOS_SYS_EXIT` dispatch the other two postures reach.
- *The seven weak references are fixed at COMPILE time and the linker script is the other half.*
  Hidden visibility does not help a WEAK undefined symbol, measured both ways: gcc reaches it
  GOT-indirect at any visibility. So `KICKOS_LINK_OPTIONAL` (`kernel/include/kickos/klink.h`) drops
  the `weak` on a target whose linker resolves no weak undefined, and `pe_image.ld` STATES all six
  windows as empty, exactly as the sim states an empty ctor window. `kickos_root_entry` is the
  eighth and needed only hidden visibility, its reference already being strong.
  `tools/check-x86_64-no-got.sh` now takes archives as well as objects and reports the member name
  for each hit, which needed `LC_ALL=C`: a localised binutils prints `Fichier:` and the member-name
  parse goes silent while the untranslated type column keeps matching.
  `tests/static/check_x86_64_no_got_selftest.sh` is its positive control, over a purpose-built
  object, an archive whose FIRST member is dirty, an archive whose SECOND is, a file readelf cannot
  read and an archive with no members.
- *The exit status crosses a SEVEN-BIT device, and that is what keeps one arm off the ladder.*
  `isa-debug-exit` reports `(status << 1) | 1`, so `tests/lib/gate.sh` recovers the image's own
  status and every gate keeps comparing the number the app passed to `kos_exit`. A status of 128 or
  more does not survive the shift through an 8-bit exit code: measured, `fault` prints its
  `=== THREAD FAULT ===` dump correctly and exits 11 where the arm asserts `KOS_EXIT_FAULT` = 139.
  So `fault_dump` is NOT registered here, and the fix is a full-byte exit path rather than a
  per-arch expectation. `faultsurvive` asserts 0 and is registered.
  **CLOSED at X7 below**, which is where that full-byte path landed and where `fault_dump`
  joined, taking the board to ten image arms.

*What is registered,* and the rule is that a gate keyed on something this build does not provide
would report a protection it does not have: `hello` (the headline arm: firmware handover through to
two ring 3 threads exchanging semaphores), `selftest` at 102 TAP arms, `sched_exit` (at X4b the only
arm asserting the exit status; `fault_dump` joined it at X7 and asserts 139), `panicgate1` through
`5`, `faultsurvive`, and the two host gates
`seam_defaults` and `class_backend`, which work unchanged because `nm` reads a PE32+ image and their
`readelf` use is on the ELF archives; the `-Map` rides the `ld` line. `periph_reg_write_unheld`
reports PARTIAL for the same reason it does on `qemu-arm64`: q35 mints no free MMIO window.
*What is not, and why,* by class: no `HAS_ASPACE` on this board, so `aspacefault`, `stackguard` and
`kernelhalf` do not build and `rootfault`/`mpu_fault` have no confinement to be refused by, all four
X5's; no C library, so `libc_exit`, `errnoprobe`, `rootauth`, `rebootdemo`, `reclaimwit` and
`gpioblink` cannot link; no `ARCH_HAS_TLS`, so `tlsprobe` is absent by design; and `stress` has no
emulator gate script on any board.

**WHERE THREAD-LOCAL STORAGE LANDS ON THIS ARCH, so the step that turns `ARCH_HAS_TLS` on does not
have to re-derive it.** x86_64 reaches thread-local storage through `FS`, and the syscall entry
anchors on `GS`, so that step has a base register of its own to spend. The block address belongs in
`struct arch_context` (`arch/x86/x86_64/include/kickos/arch/context.h`) beside `kernel_sp`, and the
switch writes it to `IA32_FS_BASE` at the point it already publishes that one. The trap frame holds
the registers the entry pushes and the restore pops.

**A FULL IMAGE'S BASE-RELOCATION DIRECTORY IS NON-EMPTY AND THAT IS CORRECT.** The toolchain file
used to claim `-fpie` leaves it empty; it now says what `-fpie` does do, which is keep every
reference from CODE PC-relative. A pointer stored in DATA is an absolute address and no code model
changes that: every full image carries 24 bytes of `.reloc`, seven `DIR64` entries, one per member
of the scheduler policy's static dispatch table. Firmware applies them wherever it loads the image.
The delta is zero in an ordinary run, OVMF loading at `0x400000`, the images' own `ImageBase`, and
X4's `image=` line says so. That is the DEFAULT and not a limit: relinking at a different base makes
firmware relocate by the difference and the image still runs (see the not-witnessed list below, where
this item was overstated as unwitnessed until the five-base sweep).

**THIS STEP'S EXPECTED RESULT WAS WRONG IN THE SAME WAY X3's WAS.** "S7's arm passes" names a TAP
arm of `user/apps/common/selftest`, and the application layer is held back by a second,
independent thing this step does not own: the image is a PE32+ application and no `add_executable`
through the compiler driver can write one. So X4 takes its witness from an ad-hoc link too, and it
is the first step whose SEAM is the real one rather than the first whose IMAGE is.

**WHAT THE OVER-GRANT COSTS, stated rather than hidden, and it is R1.5's posture for R1.5's
reason.** The image is one flat link, so the leaves carrying an unprivileged thread's own text also
carry kernel text, the scheduler's state, the capability table, the per-thread kernel stacks and
the arch layer's own statics; and the conventional-memory grant covers every thread's stack rather
than the caller's. An unprivileged thread here can read and write kernel memory. What it cannot do
is reach a device register, execute a privileged instruction, touch a port, raise its own privilege
level or write the MSR the entry takes its pointer from, and the last of those is what the two
denial arms and the swap invariant are about. **What this paragraph used to add and cannot: a
translation table IS reachable and writable, and so is the per-core block the entry loads through.**
The kernel window's level-3 table and `g_cpu` are `.bss` statics of the flat link, so both sit
inside the 2 MiB image leaf the grant opens; the M6.4 stanza above carries the measurement and the
two blindnesses that let the old arm read clean. Neither adds a power: kernel RAM is writable
either way. The narrowing is the same work R1.5 records: separate the halves in the LINK, which is
a private half and its own root, and X5 re-decides that boundary anyway.

**WHAT HAS NO WITNESS HERE, separated by why.** NOT PRODUCIBLE ON THIS EMULATOR: the non-maskable
interrupt and the machine-check interrupt-stack-table slots, since QEMU raises neither, so the one
window the flag mask cannot close is closed by argument; the supervisor-mode execution and access
prevention refusals, both bits being clear on every processor model this machine offers; and
`CR4.FSGSBASE` being set. NOT PRODUCIBLE BY CONSTRUCTION: the frame flag masking, measured
uncaught, because the hazard its shape suggests does not exist on this entry. NOT IMPLEMENTED YET:
`arch_mpu_probe_addr` still answers 0, and the reason is the knob rather than the hardware, every
leaf the grant did not touch now refusing ring 3 while the chip selects neither region descriptors
nor an address space, so `KICKOS_MEMORY_ENFORCED` reads 0 and the self-test that consumes it is
not registered; X5 selects the capability. X3 listed the x2APIC register path here as still owed; it
is not, both legs having been exercised by naming a processor model that reports the feature.

**X5. The aspace family on a single root.** The kernel half replicated into every table, the root
switch performed from code mapped identically on both sides of it, and the invalidate primitives.
*Expected:* T4's four transitions and T6's process witness both pass, with no change to either arm.
Reusing the arms unmodified is the point: an arm that needed rewording per arch would be describing
the backend rather than the contract. **And the two arms that carry the contracts most likely to have
been written A64-shaped come with it:** the driver handoff readback, since same-frame same-address
mapping on a single-root architecture is a different implementation of the same promise; and T7's
equal-virtual-address copy, since the owner-carrying helpers are where an A64-only author would most
plausibly have leaned on two table registers without noticing.

*Landed at X5.* `arch/x86/x86_64/aspace_x86_64.cc` for the map editor,
`arch/x86/x86_64/include/kickos/arch/aspace.h` for the boot handover and the port's own kernel
range, and one call in `arch/x86/chip/q35/chip_q35.cc`'s `arch_init` after `ring3_init`.
`arch/x86/x86_64/probe5_x86_64.cc` plus `.S` and `tools/run-qemu-x86_64-x5.sh` are the witness:
**163 arms**, each printing its own name and outcome, a LIVE figure re-derived by running it on
2026-08-29. Sixteen things a reader would otherwise re-derive. **The figure was 141 until the
external audit below added ten, and 151 until the attribute-table redesign below traded two arms
for fourteen**, which is what every measurement recorded above it and at X6 and X8 was taken at, so
a 141 or a 151 elsewhere in this document is a dated record and not a disagreement.

**THE ATTRIBUTE-TABLE WITNESS WAS ITSELF UNSAFE, AND WHAT REPLACED IT PROVES LESS ABOUT ONE LEG.**
A re-review raised it and the manual confirms it. `memtype_follows_a_reprogrammed_attribute_table`
wrote `IA32_PAT` with a permuted layout, reloaded `CR3` and put it back, to show the backend's
memory-type selection searching the LIVE table rather than reproducing the power-up one. Intel SDM
Vol 3 section 14.12.4 makes the writer responsible for the translation caches AND, in its step 2,
for the PROCESSOR caches wherever a live mapping's type moves in a way self-snooping cannot cover,
naming a line going from write-back to uncacheable for memory-mapped I/O as the case. The witness
did step 1 and never step 2, and its step 1 was itself conditional on `CR4.PGE` and `CR4.PCIDE`
being clear, which it assumed rather than read. This port ADOPTS the firmware's regime, so the
mappings at risk are the ones it never composed: the low identity map and the local-APIC window.
*Measured on this bench before the change, and the measurement is why the ruling is not "safe":*
`CR4` reads `0x68`, so `PGE` and `PCIDE` are both clear and the `CR3` reload was the right branch
here; a walk of every present leaf under the installed root found **527865 leaves and all of them at
attribute-table index 0**, so the permutation of fields 1 through 3 retyped nothing live. That is a
fact about OVMF on q35 today and not about the architecture, and the arm ships.
*The redesign.* The decode is now a pure function of the table,
`kickos::x86_64::aspace_memtype_bits(pat, type, out)`, and the map path is its only caller that
supplies the live one. The witness drives it over SIX synthetic tables it never installs: the
power-up layout, one placing all three types in fields 4 through 6 so every answer needs the PAT
bit, one with write-back in two fields for the first-match rule, and three each missing one of the
three types so the refusal has a subject.
*What is UNWITNESSED on purpose, and it is a loss.* Nothing now proves the decode is fed the LIVE
register rather than a constant. The two arms that tie it to the map path,
`attribute_table_in_use_is_the_live_msr` and `leaf_bits_match_the_decode_on_the_live_table`, are
the strongest safe form and they are VACUOUS on this firmware, which leaves `IA32_PAT` at the
power-up value the report line prints (`0x0007040600070406`); a backend handed that constant
composes the same leaf. Only a machine whose live table differs separates the two, and making one
differ here is the write section 14.12.4 forbids. **The removed arm DID catch that**, verified by
mutation: feeding the constant turns the old arm red and leaves all 163 new ones green. So the
trade is a decode witnessed over tables no firmware here provides, against a feed no longer
witnessed at all, and it is taken because the alternative retypes live mappings.

**THE FAMILY FITTED UNCHANGED, which is the third data point F8 asked for.** Every signature is
arch.h's as it stands, `arch_aspace_frame_at` included: this backend IMPLEMENTS that member and
adds nothing beside it, so the diff this step contributes is empty and the milestone's headline
stays one added record.
*Two sentences that stood here were false against the tree and are corrected rather than deleted,
the conclusion above surviving both.* The member is M6.3's R2.2 addition and R2.2 is an ANCESTOR of
this branch (`git log --oneline` shows one linear stack, `480767f1 R2.2` below this tip), so
"this branch's base predates that commit" was wrong; and the member is declared in the SEAM header
`arch/include/kickos/arch/arch.h`, with no declaration of it anywhere under `arch/x86/`, so
"declared in the backend's own header" was wrong too. What follows from the corrected pair is the
opposite reading of the instrument: `sh tests/static/check_aspace_sigdiff.sh` run from THIS tree
reports DIFF and exits 2, 36 candidate records against 35 baseline, the one added record being
`FUNC arch_aspace_frame_at`. That exit 2 IS the result and the baseline does not move. This tree
holds both halves, so the figure it prints is the UNION's and it is M6.3's verdict arriving
unchanged, not a contribution from this backend.

**THIS STEP'S EXPECTED RESULT WAS WRONG IN THE SAME WAY X3's AND X4's WERE, and by now that is a
pattern rather than three accidents.** "T4's four transitions and T6's process witness both pass"
names TAP arms of `user/apps/common/selftest`, and TWO independent things hold that layer back
here, neither of them the seam: the chip selects no memory family, so every kernel source compiled
under the address-space axis is absent from the link; and the image is a PE32+ application, which
no `add_executable` through the compiler driver can write. What X5 owes is therefore the
PROPERTIES at the seam, and the two arms the stanza singles out are among them: the handoff
readback is `frame_at` agreeing across two spaces at one address, and the equal-virtual-address
copy is one virtual address answering two different frames under two roots.

**THE CHIP STILL SELECTS NEITHER FAMILY, AND THAT IS A DECISION RATHER THAN AN OMISSION.** X4's
note that "X5 selects the capability" was written before the cost was measured. Selecting
`HAS_ASPACE` on this chip turns on `KICKOS_MEMORY_ENFORCED`, and with it `kernel/grant`, a changed
`KICKOS_MAX_DOMAINS`, the `klib` translation units and the runtime privatisation that rewrites
their names, and it REGISTERS `kernel_runtime` and `kernel_got` as ctest cases on a board whose
kernel archive X4 already recorded as carrying eight global-offset-table relocations. It also
needs `__kickos_frame_pool_start`, `_end` and `_delta`, which `kernel/mem/frame_pool.cc` takes as
strong LINK symbols and which this board has no KickOS linker script to carve; the honest answer
there is a page-aligned reservation inside the image with a delta of zero, identity being what
makes that truthful, and it is a few lines rather than a hazard. None of that is witnessable while
the application ladder is off, and declaring memory enforced while the flat over-grant of X4
stands would be the same false statement `arch/Kconfig` gives as the reason RV64's own select was
deferred. So `arch_mpu_probe_addr` also stays at 0 and its comment keeps the knob as the reason.

*Two of those five costs are gone, recorded here because a later step would otherwise argue from a
premise that stopped holding.* The eight global-offset-table relocations are FIXED and the guard
reads the archives at every link (X4b above), so `kernel_got` would register against a clean
subject rather than eight known hits. And the application ladder is ON, at TEN image arms
including a 102-arm self-test, so "none of that is witnessable" no longer holds either. The
frame-pool carve is cheaper than it reads too: `arch/x86/x86_64/pe_image.ld` already states six
linker windows, so `__kickos_frame_pool_start`, `_end` and `_delta` are more of the same. WHAT
STILL STANDS IS THE ONE THAT MATTERS: declaring memory enforced while X4's flat over-grant stands
is the false statement, and the app leaving the kernel's half is what retires it.

*And the editor being in every link is now paid for once.* `arch_init` adopts the live regime, so
`aspace_x86_64.cc` reaches the linker in every image carrying the chip archive, while
`kernel/mem/frame_pool.cc` is compiled only under `KICKOS_HAVE_ASPACE`. The kernel-free images
declined `kickos_frame_alloc` and `kickos_frame_free` themselves; an image with the KERNEL and no
pool takes them from `arch/x86/x86_64/nopool_x86_64.cc`, on the application-image link line in one
place so a new app cannot forget it. Both bodies REFUSE on the wire and end through
`kfault_terminate` rather than answering 0: 0 is what arch.h calls exhaustion, so a build with no
pool at all would read to every caller as a full one that happened to be empty.

**THE FIRMWARE'S OWN TOP-LEVEL CHILD TABLE IS FULL, and that single measurement decided the shape
of the port's kernel range.** The first design hung a kernel window one level below the boot
root's first present entry, on the reasoning that a table below a COPIED entry is shared by every
space while a top-level entry added later reaches nobody. It refused at boot: the table under that
entry holds 512 present entries, every one of them a 1 GiB leaf identity mapping the low range, so
there is no free entry anywhere inside the adopted kernel half to hang anything from. What is left
is a slot of the root itself, and taking one is sound for exactly one reason: it happens in
`aspace_init`, before the first create, so every space that will ever exist copies it. The figure
is printed with the witness (`first_child_entries=512`) because it is the premise, and a firmware
that left room would make a different design correct.

**SO THE KERNEL HALF IS TWO THINGS HERE, AND F1's "FIXED HIGH RANGE" IS THE SECOND CLAIM THIS
BACKEND FALSIFIES.** F8 already records that F1's "the kernel half is untouched by a switch" fails
on a single root register. On an ADOPTED identity regime F1's other half fails too: the kernel's
mappings are the firmware's low identity map, and the image is one flat link inside it, so the
kernel lives LOW and not high. This port's own range does sit high, in the top-level slot
`aspace_init` claims, which is where F1 wants it; the adopted part cannot be moved there without
rebuilding the regime the milestone chose this backend to adopt. Measured: three top-level slots
present after the install, two the firmware's and one this port's, and the range a space may map
is the low-half slots the boot root left ABSENT, which is a measurement rather than a constant.

**ACQUIRE IS AN ADDITION AND THE DELTA IS ZERO, and the reason is not A64's.** A64's addition comes
from a high-half map of all physical RAM that the port itself built; RV64 has no offset to add at
all, its physical space being wider than its virtual, and ships a six-slot transient window. Here
the adopted regime identity maps the conventional run the frame pool is carved from, so a frame's
bytes are at its own address and no window is needed or built. F8 warns in as many words that this
is a claim about the MAP and not about the machine, so the body does not assume it: acquire walks
the BOOT space for the frame's own address and answers only where that walk comes back with the
frame, and `aspace_init` refuses at boot if the regime does not map this image's own tables at
their own addresses. The check is unwitnessable on this firmware, deleting it changing no arm,
which is stated rather than hidden.
*And the consequence for `frame_at` is worth keeping.* With a delta of zero, naming a frame by
subtracting two acquire pointers WORKS on this backend and on A64, and fails silently on Sv39,
where a handful of window slots hand back the same small number for every frame. Two of three
backends make the shortcut look right. That is the argument for the member M6.3 added rather than
against it.

**THE LEVEL COUNT IS A RUNTIME FIGURE HERE where it is a constant on both siblings**, and F7's
"ask the hardware" is a live rule rather than a slogan: 4-level paging translates 48 linear bits
and 5-level 57, the mode is a control-register bit, and every walk is written over that figure and
the leaf's level. The witness reads the bit itself and holds the backend's answer to it rather
than taking the backend's word. What the figure does NOT vary is the granule: one granule and
three mapping sizes, exactly as F7 reads it, so the model word reports a granule set of one
whichever depth is running.
**AND THE WALK OWES A LARGE-LEAF LEG THAT NEITHER SIBLING NEEDS.** The boot space is built out of
2 MiB and 1 GiB leaves, so a walk that refused one, as both siblings' leaf lookups do, would
answer "not mapped" for most of the kernel half and `frame_at` on any adopted address would return
0. The read-only walk therefore resolves a large leaf and adds the offset inside it; the editable
lookup still refuses one, this editor having no business taking one apart. Deleting the leg does
not merely redden an arm: `aspace_init`'s own identity check goes through the same walk, so the
image refuses at boot before any arm prints.

**THE INVALIDATION RULES ARE THE SEAM'S BUT NOT A64's, and all three differences are in the
manual.** Writing the root register IS the sweep: with no tag enabled it drops every translation
for identifier 0 except the GLOBAL ones and every paging-structure-cache entry for it (Intel SDM
Vol 3 section 5.10.4.1), so activate needs no maintenance call beside it, and nothing this file
writes sets the global bit, which is what keeps the range sweep honest. `invlpg` invalidates every
paging-structure-cache entry for the current identifier whatever address it corresponds to, so a
table entry this editor installs owes no invalidate of its own, a conclusion A64 reaches by a
different route. And break-before-make is CONDITIONAL here where A64's is unconditional: the
architecture demands the clear-invalidate-write sequence only when a write changes the page SIZE
(section 5.10.4.2), which this editor never does. It is done anyway, and what it buys is stated:
without it an access between the two writes may take the old frame with the new permissions,
invalidation otherwise being free to be delayed (section 5.10.4.4).
**AND THE ONE THE SEAM'S OWN COMMENT GETS WRONG FOR THIS ARCHITECTURE IS THE FRESH MAP.** arch.h
says a map into a slot that was empty needs an invalidate too, "architectures caching negative
translations". x86_64 does not: no TLB or paging-structure-cache entry is ever created from an
entry whose present bit is clear, so clearing to present needs no invalidation at all (section
5.10.4.3). It is issued anyway, and NOT out of caution: that section's own footnote makes the
exemption conditional on every EARLIER clearing of the same slot having been invalidated, which is
a property of the slot's whole history rather than of the call in front of you. The seam's comment
is right about the obligation and wrong about the reason on one of its three backends.

**THE KERNEL'S "VIRTUAL ADDRESS IS THE PHYSICAL ADDRESS" CONVENTION DOES NOT SURVIVE AN ADOPTED
IDENTITY REGIME, and this is X5's finding above the seam.** Every virtual address the kernel picks
today is a physical one: `ustack_alloc` maps a run at its own output address, `aspace_reserve`
returns the run itself, and the handoff maps it into a second space at the same address, which is
what makes a reservation a globally unique name. That works on both siblings because their kernels
live in a high half and low physical DRAM is therefore a legal per-space address. Here the low
identity map IS the kernel half, so a reservation named by its own physical address lands inside
the shared top-level slots and a space cannot be given it without editing tables every other space
shares. The fix is above the seam and costs no signature: bias a reservation into the per-space
window the way `pool_delta` already biases the other direction. It is recorded here rather than
done, the kernel side of the axis being off on this board, and it is the single thing most likely
to surprise whoever turns it on.

**WHAT THE A64 BACKEND NEEDS AND THIS ONE DOES NOT.** `kickos_armv8a_ttbr0_to_boot` exists because
every device that chip's fault reporter touches is a low address, so a fault under a user space
would print nothing. There is no analogue here and it is not an oversight: this console is PORT
I/O, which translation does not reach at all, and the local APIC window is inside the copied
top-level entries, so a reporter runs unchanged whichever root is installed.

**DESTROY'S BORROWER RULE BIT THE WITNESS BEFORE IT BIT A CALLER, which is the useful way round.**
arch.h says a space must not still map a frame it does not own when destroy runs, and the first
version of the witness destroyed two spaces that still mapped the four frames its arms plant their
values in. Destroy reclaimed them, the allocator then handed one back as a root table and the copy
of the boot root overwrote a planted value: FIVE unrelated-looking arms went red in three
different groups, and the cause was one contract. The witness now unmaps what it lent before
either space dies, and carries a separate arm for what destroy DOES reclaim, without which the
pair above would read as "destroy frees nothing".

**THE MUTATION MATRIX, and it separates three outcomes rather than two.** A mutation can be killed
by an arm going red or by the image not finishing at all, and only the third case is a survivor;
reporting the first two as one hides which arm holds the property. Every verdict is taken in a
WIPED build directory over an rsynced copy of the tree, and the rig refuses when the build log
does not name the mutated translation unit, a skipped build reading exactly like a harmless
mutation. Two controls first: the pristine tree passes 141 arms, and a comment-only edit to the
same file also passes 141, so the rig is not reporting a rebuild as a kill.
KILLED BY A NAMED ARM: the root write (`activate_a_wrote_the_root`, and 66 of 141 arms reached
before the image faulted reading an address only the new root maps); create copying ONE LEVEL
DEEPER (`kernel_edit_seen_from_a`); create copying nothing (`kernel_half_in_a`); the
break-before-make clear and its invalidate (`replacing_a_live_page_issues_two`, one red arm out of
141); the fresh-map invalidate (`fresh_map_issues_one`); the not-installed elision
(`map_into_a_space_this_core_is_not_on_elides`); the map unwind
(`partway_refusal_left_no_partial_mapping`); the write-and-execute refusal
(`refuse_write_and_execute`, 11 red).
KILLED BY THE IMAGE NOT FINISHING: destroy's keep guard, which walks into the shared kernel half
and hands the pool the firmware's own tables; the large-leaf leg of the walk, which the boot-time
identity check catches before a single arm prints; the kernel range's sign extension, which makes
a high-half slot index a non-canonical address; and the execute-disable enable turned into an
unconditional DISABLE, which makes that bit reserved in every leaf this editor writes.
SURVIVED, AND EACH FOR A STATED REASON: acquire's identity check, every frame on this firmware
being identity mapped so the check can only ever agree; activate's interrupt mask, one core and
nothing else running; destroy's invalidate moved AFTER the frees, the emulator not reusing a freed
table's contents inside the window; and the execute-disable enable as written, which is DEAD CODE
on this firmware because firmware sets the bit itself, so the conditional never runs. That last
one is why the mutation had to be turned into an unconditional clear to say anything at all.

**WHAT HAS NO WITNESS HERE, separated by why.** NOT PRODUCIBLE ON THIS MACHINE: the address-space
identifier and the instruction that invalidates by it, neither reported on any of five processor
models the emulator offers, which is F8's own measurement corrected above and which X6 re-took across
TEN models with the same answer; 5-level paging, which X6 corrects: the emulator DOES offer models
whose parts support it and they still report four levels, the mode being a control-register bit this
port adopts rather than sets and the firmware leaving it clear, so the runtime level count is
exercised at one of its two values for a reason no model can change; a physical frame
outside what the memory map identity maps, so acquire's refusal leg is unreached; and a second
core, which is what the elision is compiled out above. NOT PRODUCIBLE BY CONSTRUCTION:
`ARCH_ASPACE_ECAPACITY`, all three M6 backends being radix, and the witness counts every result
class and reports the capacity count as 0 rather than assuming it; and the per-SLOT leg of the
range test, which needs a firmware leaving a present top-level slot between two absent ones, this
one leaving its two present slots at the bottom. NOT IMPLEMENTED: the kernel side of the axis, per
the decision above.

**AN EXTERNAL AUDIT OF THIS BACKEND (2026-08-29) RAISED THREE FINDINGS AND ALL THREE HELD.** Their
reach is latent rather than live: `arch/Kconfig` selects no memory family on this chip, so nothing
above the seam calls this editor and the defects bite the day the board selects `HAS_ASPACE`. They
are fixed in the code that already exists rather than carried as a port obligation, and the ten
arms that hold them take the witness from 141 to 151.

- **THE SHARED-SLOT TEST READ HARDWARE-UPDATED BITS, which is the critical one.** `free_subtree`
  and `prune_empty` recognised a slot as the kernel half's by comparing the copy's WHOLE
  descriptor against the boot root's. The processor sets the accessed flag in every
  paging-structure entry a translation walks and the dirty flag in every entry that maps a page
  (Intel SDM Vol 3 section 5.8), and a space's copy is walked independently of the boot root, so
  the two descriptors of one shared slot drift apart on their own. Destroying that space then
  walked into tables every other space still points at. Measured on this firmware: the boot root
  leaves slot 1 present with the accessed flag CLEAR, so the divergence needs one ordinary
  translation to appear.
  *The fix makes the authority TOTAL rather than adding a record beside it*, which is this
  project's rule against a second truth: a top-level slot the boot root HAS is the kernel half,
  full stop, because `range_ok` offers a space only the slots the boot root left absent, so no
  space can own a table under one. Presence is the whole test and no mutable bit enters it.
  Comparing the physical target instead, which the audit offered as the alternative, would have
  been a narrower version of the same mistake: it still reads the copied descriptor, and it frees
  the shared table on any mismatch where presence leaks it instead.
  *The witness is two arms and the kill is physical.* `a_shared_slot_diverged_by_an_accessed_bit`
  SEARCHES for a present top-level slot the boot root has not been walked through and sets the
  flag in the copy, so a firmware leaving none reddens that arm rather than making the next one
  vacuous; `the_diverged_slot_kept_its_table` reads the slot and its child table back after the
  destroy. Restoring the whole-descriptor comparison kills the image at the destroy with a
  page fault at `CR2=0x1fa04000`, inside the firmware's own paging structures, write=1 present=1
  -- the firmware write-protects them, which is the only reason this backend's teardown announced
  itself here instead of corrupting quietly. The port's OWN window tables are ordinary writable
  `.bss` and would have gone silently.

- **THE PHYSICAL RANGE WAS CHECKED AT ITS START AND AGAINST A CONSTANT.** `arch_aspace_map` tested
  the first output address against the entry's 52-bit address field and nothing else, and
  `aspace_kernel_map` tested only granule alignment. Bits 51:MAXPHYADDR are reserved in EVERY
  paging-structure entry (SDM Vol 3 section 5.1.4 and Table 6-8), so a leaf above the width the
  part reports faults with the reserved-bit code instead of translating; and a run walking off the
  top of the 52-bit field comes back masked, which reports success for an alias onto a low frame.
  MAXPHYADDR is CPUID 0x80000008 EAX bits 7:0, capped at 52, and 36 where the leaf is absent.
  *The whole extent is validated UP FRONT*, before the first table is edited, which is what the
  seam's total-or-fail promise costs on this path; `arch_aspace_model` reads the same helper, so
  the figure it reports and the figure the map path enforces cannot drift.
  *RV64 answers the same finding differently and the two do not contradict.* Sv39's physical space
  is WIDER than its virtual, so its endpoint check is arithmetic the seam's own extent helper
  already does; here the ceiling is a runtime CPUID answer and the reserved-bit fault is the
  consequence, which is why this backend reads a register and that one does not.

- **THE MEMORY TYPES ASSUMED THE FIRMWARE'S ATTRIBUTE TABLE.** `memtype_bits` wrote down the
  PWT/PCD combinations that name write-back, UC- and UC under the POWER-UP layout of `IA32_PAT`
  (SDM Vol 3 Table 14-12) without reading it. Any field of that table may hold any of the seven
  types (Table 14-10), so a conforming firmware that programmed a layout of its own would have a
  device page composed here come out cacheable. The MTRRs cannot do that: Table 14-7 gives UC and
  UC- as uncacheable under every MTRR type, so the attribute table is the whole hazard.
  *The fix READS the table and refuses what it cannot find, and deliberately does not write one.*
  Establishing a layout of this port's own is the wrong move for a backend that ADOPTS a live
  regime: every mapping the firmware installed was composed against the fields it programmed,
  the local APIC window inside the copied top-level entries among them, so a new layout retypes
  what this port inherited and section 14.12.4 puts the cache and TLB consequences on whoever
  writes it. So the three index bits are searched out of the running table -- all eight fields,
  bit 7 being the index's high bit in a granule leaf (section 14.12.3) -- and a type no field
  encodes is refused through `arch_aspace_memtype_support`, which arch.h says exists for exactly
  that. On the power-up layout the bits are byte-identical to the ones that were written down, so
  nothing about this firmware changes.
  *The witness used to PERMUTE THE LIVE TABLE and no longer does, because the same reasoning that
  forbids the port a layout of its own forbids the witness one.*
  `memtype_follows_a_reprogrammed_attribute_table` wrote a permuted layout, reloaded `CR3` and put
  it back; section 14.12.4 wants the processor caches flushed as well wherever a live mapping's
  type moves, and that half was never done. The decode is a pure function of the table now,
  `aspace_memtype_bits(pat, type, out)`, and the witness drives it over synthetic tables it never
  installs. Restoring the hardcoded combinations turns eight of those arms red. What went with the
  old arm is the FEED: nothing proves the running table reaches the decode. The X5 record above
  states that gap and why it cannot be closed from here.

**THE HYGIENE PAIR IN `pe_image.ld` IS FIXED AND NO TRACKED GATE COVERS EITHER.** A space before a
tab and a blank line at end of file, both reported by `git diff --check` and by nothing in
`tests/static/`; `check_ascii.sh` reads character sets and not layout, and the repository sets no
`core.whitespace` and installs no hook. So the class is caught by a reviewer running one command
and by no instrument the tree owns.

**X6. The verdict.**
*Expected:* an EMPTY signature diff on the aspace family across the whole port. This is the
sub-milestone's actual deliverable and the only auditable form of "the implementation is not coupled
to one architecture". A non-empty diff is not a failure and must not be written up as one -- it is the
finding F8 predicted, and it lands in the seam.

**THIS STEP'S EXPECTED RESULT NAMES THE WRONG FAMILY, which is the fourth time in this
sub-milestone and by now a property of how the stanza was written rather than four accidents.** F8
gives each backend one half of the seam to falsify: RV64 takes the ASPACE family and x86_64 takes
the ENTRY AND BOOT paths, "a different seam from the one above". The aspace verdict is therefore
M6.3's, it is already taken, and it is not empty: exactly one added member, `arch_aspace_frame_at`,
forced by a windowed backend, which X5 measured fitting x86_64 with no signature change of its own.
Running the aspace differ here and calling its answer this backend's verdict would report on a family
this port contributed nothing to. So X6's subject is the entry and boot paths, and the expected
result above is corrected to that.

*Landed at X6.* `tests/static/check_entry_sigdiff.sh` with
`tests/static/entry_seam_family.awk` beside it for the instrument, and one measurement landed in
`arch/x86/x86_64/entry_x86_64.cc` with its assertion in `tools/run-qemu-x86_64.sh`.
*The "nothing else moved" evidence this record first cited was false and is replaced by what is
true.* `git diff 7ee94a94 -- arch/include/ arch/riscv/ arch/arm64/ arch/arm/` was never empty at
this commit: measured, it is 20 files changed with 3295 insertions and 304 deletions, which is
M6.3's whole ISA half sitting under this branch. What holds still is the ENTRY family and not the
directories: the differ reports 54 records against 54 with no parameter rename either, and every
record that DID move belongs to the aspace family, which is M6.3's verdict and is measured by the
other instrument.

**THE ENTRY AND BOOT PATH DIFF IS EMPTY, AND THE FORM IT IS EMPTY IN IS THE FAMILY'S AND NOT THE
FILE'S.** Fifty four signature records against fifty four, at the frozen baseline `f0360d3a` the
aspace verdict uses, and no parameter RENAME either, so the instrument's own non-signature channel is
silent as well. *This record also claimed a stronger form, that `arch/include/kickos/arch/` is
BYTE-IDENTICAL between that baseline and this branch, and that was false when it was written:*
`arch/include/kickos/arch/arch.h` stands at 83 insertions and 5 deletions against `f0360d3a`, all of
it M6.3's aspace work under this branch. So the header WAS opened, by the other half of the stack,
and the entry half's claim is the one the extractor makes: of the 54 entry-family records not one
moved. A hundred and fifteen arms across the X3 and X4 witnesses exercise that family on a booting
image, the two properties the roadmap picked this architecture for among them. (Re-derived
2026-08-29 on `aea89358`: X3 prints 21 and X4 prints 94. This line has now been wrong twice the
same way, reading ninety five when X4 was 74 and ninety six when X4 was 75, each time because a
later step moved X4's count and the sum was not re-added. It is 21 plus whatever X4 prints; run
`ninja x3-run x4-run` rather than quoting it.)

**AN INSTRUMENT IS WORTH BUILDING HERE, AND THE DATE THE ARGUMENT TURNS ON HAD ALREADY PASSED.**
The argument as written was that a byte comparison of the seam headers gives today's verdict on its
own, so an extractor adds nothing to THIS reading, and that what settles it the other way is the byte
comparison stopping working on a known date: M6.3's R2.2 adds `arch_aspace_frame_at` to the same
header, so once that work stands under this branch `arch/include/` is no longer identical to the
baseline and a byte diff can no longer certify that the ENTRY half took nothing. The conclusion is
right and the tense was wrong. R2.2 was ALREADY an ancestor when this stanza was written, and
`arch.h` already stood 83 insertions from the baseline, so the byte comparison was not going to
stop working, it had stopped: this verdict could never have been taken by eye and the extractor is
the only thing that takes it. From here the entry verdict either has an extractor restricted to its
own family or it collapses to the eye, which is exactly what R5's instrument exists to avoid, on a
subject half again the size of R5's, 54 records against 35. The second reason is F8's own
doctrine: the litmus is a future port held against the seam, and the entry path is the half
every port implements first, so this is the differ the next backend runs.
*What it reuses, and that is the design.* `tests/static/aspace_seam.awk` VERBATIM. awk runs BEGIN
blocks in the order its `-f` programs are named, so a second program carrying nothing but the family
PREFIX replaces the family and leaves every extraction rule alone. The rules that decide what a
signature IS are therefore R5's own, byte for byte, and the two verdicts cannot come to disagree
about it; a copy of the extractor with one line changed is how they would have drifted. The baseline
is R5's file, READ and never written, for the same reason: one commit, one record.
*And the rule R5 states applies here unchanged:* the instrument's exit IS the verdict, 0 for no
difference and 2 for a difference, and 1 for a comparison it could not make, which is UNKNOWN and
not clean. A difference is the finding F8 predicts and lands in the seam with the other backends
updated. Do NOT move the baseline to quiet one, which would delete the result this step exists to
produce.
*And it floors per GROUP as well as per kind,* which R5's does not need. This family has ten groups
and a total floor of forty five would pass an extraction that lost the six console edges whole,
because losing them on BOTH sides is a clean empty diff. Each group carries its own floor, and a
member the family admits and the group table does not classify is a REFUSAL rather than an unfloored
record.

**THE INSTRUMENT'S MUTATION MATRIX, fifteen arms, every one in an rsynced copy so the tree never
holds a mutation.** Two controls first: the pristine copy PASSES, and a comment rewrite plus a
whitespace change inside a declaration also PASSES, so the rig is not reporting a copy or a reflow as
a kill. Then a changed return type, a changed parameter type, a member added and a member removed all
exit DIFF and name the member; a parameter renamed exits PASS and prints the NOTE, which is the rule
this instrument inherits rather than a hole. The five that matter are the ones a differ fails
silently: a family PREFIX matching nothing refuses on the floors instead of reporting fifty four
records against zero as no difference; ONE group lost from the family on both sides refuses on that
group's floor, which no total floor catches; a member the family admits and no group claims refuses;
a corpus built from a path with no seam header refuses on the anchor; and a seam header the extractor
cannot walk refuses as UNKNOWN rather than clean. The family file DELETED refuses too, which is the
one that would otherwise have been a false PASS with real numbers in it: the extractor would fall
back to its own aspace prefix and this script would print M6.3's verdict under this name.

**WHAT WAS RE-TAKEN AND WHAT IS CARRIED FORWARD, because a verdict that trusts five step reports is
not a verdict.** RE-TAKEN, on this tree and this build: the three instructions that run on the
caller's stack pointer, read out of `switch.S` and still `swapgs`, a store of `rsp` through the gs
base, and a load of the kernel stack from the same block, with the parked value going on to the FRAME
and the per-core slot holding it only across those three; the flag mask, still naming the interrupt
flag with a `static_assert` refusing a mask that does not, and the enable of the fast pair still
written LAST so no SYSCALL can be taken before the mask and the target exist; the root replacement
performed from code the writing space maps at the same address, read out of `arch_aspace_activate`;
and the whole ladder, thirty one of thirty one and the five witnesses, on QEMU 11.0.3 TCG (a DATED
X6 figure; the board reads 38 now).
*And one claim had NO witness at all in the tree, which is now fixed rather than reported.* F8's
handover contract was a citation: the port's first instruction clears the interrupt flag, so nothing
afterwards can read what the flag WAS, and `if_after_cli=0` measures the port and not the firmware.
The entry now reads the flags, `CR0`, `CR3` and `EFER` BEFORE that instruction and prints the six
figures, and `tools/run-qemu-x86_64.sh` asserts the four the specification mandates. Measured under
both firmware legs: interrupts ENABLED, paging ON, long mode ACTIVE, a root LIVE. Mutation tested by
moving the read to after the `cli`, which turns the first figure to 0 and fails the arm, with the
paired control of deleting the assertion, which passes it: the assertion and nothing else holds the
property. The two remaining figures, write protection and execute disable, are printed and matched
LOOSELY on purpose, being this firmware's choices rather than the architecture's, and they are what
X4's table edit and X5's dead execute-disable enable both rest on.
*CARRIED FORWARD, and named so a later reader does not think otherwise:* every figure that needs an
instruction this emulator does not raise, which is the non-maskable interrupt and the machine check;
the descriptor-ordering rule's PROVENANCE, since arms report the two selector-base fields and reach
ring 3 through SYSRET so the resulting layout is witnessed, while a layout FITTED to make those arms
pass would look identical and only the manual separates the two; and the timer and timestamp
frequencies, measured once at X3 against the i8254.

**FIVE CORRECTIONS X1 THROUGH X5 MADE TO F8 WERE RECORDED IN A STEP RECORD AND NOT IN F8, and a
freeze that carries its corrections only in the steps that found them is a freeze the next reader
misreads.** F8 now carries all five. The flag mask covers neither the non-maskable interrupt nor the
machine check, so its own sentence understated the window by two vector classes. SYSRET loads no
stack pointer either, so the property is not a property of entry. Adopting a live regime does not
hand you ring 3, every entry down to the leaf covering the loaded image having the user bit clear and
the tables being write protected against their own kernel. One root serves BOTH privilege levels
here where M6.3 needed two, because the supervisor fetch RISC-V forbids outright is forbidden on x86
only under a feature this port refuses, which is a root-count-per-privilege-level disagreement no
A64-shaped reading predicts. And F1's "fixed high range" fails on an adopted identity regime because
the kernel lives LOW, with the sharper half above the seam: the kernel's convention that a virtual
address it picks IS the physical address does not survive that regime.
*The sixth was already there,* the identifier and its invalidation instruction both being absent, and
X6 re-took it by machine and widened it: TEN emulated processor models rather than five, `max`
included and both of the emulator's 5-level-capable server models included, all reporting neither
feature and all running 141 of 141 arms green. The reason is the emulator's TCG feature set rather
than the model list, which is worth knowing because a KVM run would probably report the identifier
and this bench has no access to one.

**WHAT THIS VERDICT SAYS THAT A SIGNATURE DIFF CANNOT SHOW.** Five things, all of them mechanism, and
F8 says mechanism is the shape a good seam produces. The fifth came out of the ten-angle review of
the closed milestones and is about this verdict's own PASS.
  - **A three-instruction window on the caller's stack pointer, and what covers it.** Privileged
    execution begins on a stack pointer ring 3 chose. None of the three instructions that run before
    the kernel stack is loaded dereferences it: the swap takes no operand and the two moves address
    a fixed kernel object rather than anything the caller chose. That block is itself reachable and
    writable from ring 3 on this posture, so the argument covers the caller's stack pointer and not
    the block's contents. Interrupts are already masked when the first of
    them executes. The two classes the flag does not hold off have interrupt-stack-table slots of
    their own, so even those build their frame on a stack of their own rather than on the caller's
    pointer. That argument, not a signature, is what makes the window safe, and no diff carries it.
  - **A descriptor ORDERING dictated by two instructions.** SYSCALL takes the stack selector from the
    code selector field plus 8; SYSRET takes the stack selector from its own field plus 8 and the
    code selector from that field plus 16. So the kernel data descriptor must sit one entry ABOVE the
    kernel code descriptor and the user data descriptor one entry BELOW the user code descriptor.
    Nothing above the seam names a descriptor table, so this constrains a backend's private layout
    and never a signature, and it moved two hard-coded figures in an earlier step's witness when it
    was discovered.
  - **Five frame fields rewritten by a killed thread's redirect, against three on RV64 and two on
    A64.** An interrupt return reloads the code selector, the stack selector, the flags and the stack
    pointer along with the instruction pointer, so each is a decision rather than a copy, and the
    stack selector is not optional: the instruction refuses a return whose stack selector does not
    carry the privilege the code selector names. One seam call, three backends, two three and five
    fields.
  - **A reservation-naming convention that does not survive an adopted regime.** Every virtual
    address the kernel picks today is a physical one, which is what makes a reservation a globally
    unique name, and it works on both siblings because their kernels live in a high half. Here the
    low identity map IS the kernel half, so a reservation named by its own physical address lands in
    the slots every space shares. The fix is above the seam and costs no signature, a bias into the
    per-space window. X5 recorded it as the thing most likely to surprise whoever turns the memory
    axis on, and it is the one item on this list that is a debt rather than a description.
  - **A MEMBER OF THIS VERY FAMILY GAINED A MANDATORY CALLER PRECONDITION, AND THE DIFFER REPORTED
    PASS, CORRECTLY.** `arch_switch` says at R6 that it MUST be called either with interrupts masked
    or from ISR context: publishing the incoming context and saving the outgoing one are two steps in
    every backend, so an interrupt between them is delivered against a half-applied switch and saves
    the wrong frame as the incoming thread's saved context. That obligation went from unstated to
    mandatory over the same stack this verdict measures, and it changes what every caller must
    already hold. Fifty four records against fifty four is exactly right by the instrument's own rule,
    which reports types, counts, order and values: a precondition is none of those. So "the entry seam
    did not move" is a statement about SIGNATURES, and the reading it licenses is that no caller has
    to be re-typed, not that no caller has to be re-audited. A new backend implementing this family
    reads the contract and not the record count.

**WHAT M6.4 HAS NOT WITNESSED, verified rather than transcribed.** Every item below was re-measured
at X6 unless it says otherwise.
  - **NO RUN ON REAL HARDWARE, and no path to one.** `docs/reference/boards.md` names no x86 part and
    no flash tool in `tools/` names this arch, so the backend has only ever run under emulation. This
    is the one item on the list with no cheap remedy.
  - **ONE FIRMWARE IMPLEMENTATION, not two.** X1's text asks for a second firmware, a hypervisor's
    own EFI under hardware virtualisation, on the reasoning that two implementations prove the
    contract was read. What exists is one EDK II build in two PACKAGINGS, the split code-plus-variable
    pflash pair and the combined image through `-bios`, both from the same distribution package. Both
    legs were re-run at X6 and both report the same six handover figures. The bench has no reachable
    `/dev/kvm`, and acceleration would not supply the missing half anyway: the requirement is a
    different IMPLEMENTATION, and OVMF under KVM is still OVMF. This substitution had been written
    down nowhere until X1's landed record went in with the same commit as this list.
  - **THE NON-MASKABLE INTERRUPT AND MACHINE-CHECK STACKS ARE NEVER ENTERED**, this emulator raising
    neither, so the one window the flag mask cannot close is closed by argument. Their gates and
    interrupt-stack-table slots are installed and unexercised.
  - **SUPERVISOR-MODE EXECUTION AND ACCESS PREVENTION ARE REFUSED, and both bits are clear on TEN
    processor models**, `max` included, where earlier steps' arms ran on the default model only.
    Re-measured at X8 by booting the X4 image under each of the ten: 75 of 75 arms green on all ten,
    and the arm that reads the two bits reports them clear on all ten. The ten are named at X8, which
    is also where the knob that selects a model is; naming them is what makes the sweep repeatable.
  - **FIVE-LEVEL PAGING IS NEVER ENTERED, and the reason is the FIRMWARE and not the model.** X5
    recorded it as "no model enabling it", which the ten-model sweep corrects: the emulator does offer
    two models whose parts support it, and both still report four levels, because the mode is a
    control-register bit this port ADOPTS rather than sets and OVMF leaves it clear. So the runtime
    level count is exercised at one of its two values for a reason no CPU model can change.
  - **NO SECOND CORE**, which is what the not-installed elision in the map editor is compiled out
    above.
  - **`ARCH_ASPACE_ECAPACITY` IS UNPRODUCIBLE**, all three M6 backends being radix, and the witness
    counts every result class and reports that one as 0 rather than assuming it. Re-read at X6: 15
    OK, 2 ENOMEM, 0 ECAPACITY, 16 EINVAL.
  - **THE BASE-RELOCATION DELTA IS ALWAYS ZERO IN AN ORDINARY RUN, AND LOAD-BASE INDEPENDENCE IS NOW
    WITNESSED ANYWAY.** Firmware loads at `0x400000`, which is the image's own `ImageBase`, so the
    seven `DIR64` entries a full image carries are normally applied with no displacement, and the
    witness `image=` line says so. That is a fact about the DEFAULT link, not a limit: relinking the
    full kernel at `0xff000000` made OVMF relocate it by a delta of `-0xE146D000`, and it booted and
    ran, with X5's full arm set green at five different link bases. So the item on this list is the
    delta being zero by DEFAULT, and no longer the independence being unproven.
  - **THE SELECTOR DECODE REACHES ONE OF ITS THREE TABLE LEGS, and X2's prediction that the other two
    become producible at X4 did not come true.** Only `tbl=gdt` is ever printed. A complete 256-gate
    table refuses nothing on delivery, so the interrupt-table leg has no producible fault, and this
    port installs no local descriptor table at all, so that leg cannot happen by construction rather
    than by scope. X4 landing ring 3 changed neither.
  - **AND THE PAGE-FAULT DECODE STILL EXERCISES TWO BITS OF SEVEN**, write and instruction fetch,
    with present, user, reserved, protection-key and shadow-stack all zero across every arm on the
    tree. The `user` bit staying zero is the one that changed meaning: ring 3 exists now, and it does
    not take page faults, because X4's flat over-grant leaves it nothing to fault ON. Its denial arms
    are general-protection faults instead. So that bit becomes producible when the app leaves the
    kernel's half, which is the same work that retires the over-grant.

**TWO ARM COUNTS IN THIS PLAN HAD DRIFTED, and they are re-measured on the final tree:** X3's witness
prints 20 arms where its record says nineteen, and X4's prints 74 where its record says 72. Both
records are corrected. The witnesses assert per ARM NAME rather than on a count, which is why neither
drift broke anything, and is also why neither was noticed.

**X7. THE FULL-BYTE EXIT PATH, and the arm the lossy channel was keeping off the ladder.** Added
after X6 rather than planned, because X4b's own record named the fix and declined to take it. There
was never anything wrong with `fault` on this board: the image prints
`=== THREAD FAULT === thread 'root' killed` exactly once and it always did. What failed was the
STATUS. `isa-debug-exit` reports `(status << 1) | 1` into an 8-bit process exit code, so only 0
through 127 round-trip, and `KOS_EXIT_FAULT` = 139 arrived as 11. Restating 11 in the app's
`CMakeLists.txt` was the tempting fix and it is the wrong one: it turns a contract into a
description of one backend's exit device.

*What landed.* `arch/x86/chip/q35/chip_q35.cc` prints the image's own status on the console as one
line, and `tests/lib/gate.sh`'s `boot_status` reads it. **The whole mechanism is gated on
`KICKOS_BOOT=uefi-pe`, and the gate is load-bearing rather than tidy.** As first written the line
EXTRACTION was ungated and only the device recovery and the cross-check were on the posture, so on
arm64, armv7m, riscv and sim any line matching the pattern in ordinary output replaced the real exit
status with nothing checking it. Below the gate those boards take the raw status untouched, which is
the behaviour they had before any of this existed. On the gated posture the two channels must AGREE:
the printed value is used only where the device corroborated it, and a printed line with no device
report is refused rather than believed. That matters here specifically because the console is polled
and an unprivileged thread on this board can write kernel memory (X4's over-grant), so a printed line
is a claim an application could make about itself, while the device's code comes from the emulator.

- **THE LINE IS EMITTED IN `arch_shutdown` AND NOT IN `kickos_terminate`, and that is the whole
  point of the choice.** `kickos_terminate` is the chokepoint for every ORDERED terminal path, which
  is not the same set as every terminal path: `kfault_terminate` on this chip calls `arch_shutdown`
  directly and never passes through it, and so do the kernel-free X3, X4 and X5 probe images.
  `arch_shutdown` is the last instruction stream on the board either way, and it already drains the
  UART, so the line is emitted where nothing can leave without it. The cost of the choice is that
  the emission is per chip rather than fleet-wide, which is exactly why the READER is not: a board
  that prints no line keeps the behaviour it had, and any future backend with a lossy exit channel
  buys the fix by printing the same line.
- **THE TWO CHANNELS ARE CROSS-CHECKED rather than one replacing the other.** Making the printed
  line simply win would have silently retired what `sched_exit`'s own comment says it is for: it is
  the arm that asserts the status CROSSES the exit device. `boot_status` requires the device's
  recovered value to equal the printed status modulo 128 and fails the gate on a disagreement, and it
  refuses a printed line the device did not corroborate at all rather than trusting it. The timeout code is never overridden either: an image killed for making no
  progress must read as killed, and a status line printed before the kill is precisely what would
  hide that.
- **THE LINE IS DELIBERATELY NOT BANNER-SHAPED.** `tests/static/check_panic_banners.sh` reads every
  `\n=== x ===` string literal in the tree as a fault reporter's dump marker and requires
  `tests/lib/panic.ere` to match it. Spelling an ordinary exit that way is a real failure, measured:
  the gate refused the first wording, and adding the banner to the ERE would have made every
  `assert_no_panic` on this board fail on a clean exit. `KICKOS-EXIT status <n>` carries no `===`.
- **THE ARM IS NON-VACUOUS, held against four mutations in a throwaway copy of the tree, each with
  the build log checked for the mutated translation unit and each reverted before the next.** The
  status line deleted: the gate falls back to the device and reddens with `expected exit 139, got
  11`. The status line emitting `status + 1`: the cross-check fires, `printed status 140 but the exit
  device reported 11`. The thread-fault dump deleted: `fault-dump marker 'THREAD FAULT' missing`.
  And the killed thread carrying 7 instead of `KOS_EXIT_FAULT`: `expected exit 139, got 7`, which is
  the one that matters most, because both channels AGREED on 7 and the arm still caught it. Two
  controls stayed green, an unmutated run and a comment-only edit to the same function.
- **NO OTHER GATE WAS EXCLUDED FOR THIS REASON, checked by going through the list rather than
  assuming.** FOUR arms in the tree assert a status of 128 or more: `fault_dump`, `kernelhalf` and
  `stackguard` at 139, and `aspacefault` at 132. None of the other three is a lossy-channel
  exclusion: all three of `kernelhalf`, `aspacefault` and `stackguard` need `HAS_ASPACE` and do not
  build on this board at all, and `aspacefault`'s registration is additionally keyed on the
  `ARMV8A EXCEPTION` banner. The other side of the check is the images that DO build here and carry
  no ctest arm, and each is a real absence: `specfault` is a silicon one-shot against a wrapped
  external aperture no emulator models, `stress` has a sim-only gate because the sim is the target
  with no virtual clock, and `hello_c` is gated on no board in the fleet. What the fix does buy is
  forward: when `HAS_ASPACE` reaches this board, `kernelhalf` and a page-fault `aspacefault` can be
  registered without a per-arch status.

**X8. THE ADVERSARIAL REVIEW, and the ten things a green suite could not show.** Run deliberately
before the external audit, against `c7ab8e75` with 32 of 32 ctest green and all five `ninja`
witnesses passing throughout. That count is DATED by the commit it names and is not the board's
today, which the fleet paragraph in the ladder section carries. That is the point of the step rather than a caveat on it: none of the
ten was visible from the suite, and two of them were live wedges.

*The two that made it urgent, and both are re-measured below.*

- **`EFER.NXE` WAS WRITTEN WITH NO CAPABILITY CHECK, and the board died on any part without the
  bit.** The only `CPUID 0x80000001` read in the tree tested bit 11, SYSCALL, and nothing tested bit
  20. Measured under `-cpu qemu64,nx=off`: X1 through X4 stayed green and X5 ran 43 of its 141 arms
  and took a vector-14 fault with `rsvd=1` and `CR2` equal to the port's own kernel-window base,
  bit 63 being RESERVED while `EFER.NXE` is clear. **THE DECISION IS TO REFUSE AT BOOT, not to omit
  the bit,** and the reason is what the seam would otherwise return. Every leaf of the port's kernel
  window carries `PTE_XD`, and `leaf_attrs` expresses "readable, not executable" with that bit and
  with nothing else this architecture offers, so on a part without it a caller asking for a
  non-executable page would be handed an executable one together with a success code. That is the
  silent widening the isolation rule forbids, and omitting the bit would have bought a boot at the
  cost of making `arch_aspace_map` lie on exactly the parts an attacker would choose. Refusing also
  covers the WRMSR, which the SDM makes a general-protection fault on a part reporting no bit 20, so
  there was no quiet option in either direction. `aspace_init` reads the bit and refuses by name.
- **AND THE ARM THAT SHOULD HAVE CAUGHT IT MATCHED TOO LOOSELY.** X1's handover line was asserted as
  `wp=[01] nx=[01]`, and the looseness is right for both of those: they are the FIRMWARE's choices
  and a firmware leaving either clear conforms. The capability is not the firmware's, so the entry
  now prints `nxcap=` from CPUID and the runner pins it to 1. Measured: under `nx=off` the X1 arm
  fails on that field with `nxcap=0` in the log, so the ladder stops at its first step instead of at
  its fifth.
- **THE SHUTDOWN FALLBACK WAS A NO-OP ON THIS PLATFORM, so every fault path wedged without
  `isa-debug-exit`.** `pm1a_soft_off` wrote sleep type 5, which is the value real firmware most often
  publishes for S5. QEMU's ICH9 power-management block decodes 0 as soft power off and sends 5 to its
  default branch, where nothing happens, so control reached `while (true) hlt` with interrupts
  masked. Measured before the fix: X3 without the device printed its status line and then hung to
  rc=124 at the timeout. The comment claiming soft-off "stops the machine" was false and had never
  been run. **Sleep type 0 now, and the comment says which figures are hardcoded and what real ACPI
  would require:** the S5 type comes from the DSDT's `\_S5` object and the register address from the
  FADT, and nothing in `arch/x86/` reads either, so both are this emulated chip's rather than the
  platform's. The residual halt loop is now reachable only on a machine with neither channel, and it
  is documented as a wedge, which is the honest report for a machine that cannot stop itself.

*The other eight.*

- **X6's OWN INSTRUMENT LOST ITS DISTINGUISHING FEATURE UNDER bash AND STILL EXITED PASS.** The group
  table in `check_entry_sigdiff.sh` was held in a variable named `GROUPS`, which is a bash SPECIAL
  holding the caller's group ids: an assignment to it does not take, so under bash the table expanded
  to the single number `1000`, every per-group floor went empty, two integer comparisons errored into
  `/dev/null`'s neighbour and the script printed one bogus `group 1000 54/54 (floor )` line and
  exited 0. The floors are the ONLY thing that instrument adds over the aspace differ, so bash
  silently downgraded it to a rerun of M6.3's verdict. Fixed by renaming the table, parsing it ONCE
  into a validated file through a here-document rather than a pipeline (so a refusal is not confined
  to a subshell), and checking the parsed row count against a declared figure so a shell that mangles
  the text fails loudly. Verified under both shells with identical output, and mutation-tested: a copy
  with the variable renamed back to `GROUPS` passes under dash and fails under bash with
  `group table row "1000" is not <name> <name-regex> <floor>`.
- **AND IT IS NOW REGISTERED, unlike its aspace sibling, because the two expect opposite outcomes.**
  R5 deliberately left `check_aspace_sigdiff.sh` off the ladder: its job is to REPORT a diff for a
  milestone that was changing the seam, so its exit 2 would have failed the ladder that reads the
  report. This one asserts the entry seam did NOT move under a second backend, so PASS is its
  expected verdict and a diff is a regression that must not land quietly. It reads the tree through
  git, opens no build directory, and takes about a second, so it registers as a `host` gate on every
  board. What retires it is a milestone that deliberately changes the entry seam, which has to move
  the baseline ref anyway.
- **THE TEN FAULT-CLASS IMAGES COULD SHIP STALE, which is the milestone's own documented hazard
  recurring in the one place it was not applied.** `cmake/x86_64_boot.cmake`'s per-class `DEPENDS`
  omitted `kickos_x86_64_nokernel`, which is on both that command's no-GOT line and its `ld` line,
  and a `DEPENDS` on an object library alone is order-only. Fixed with `$<TARGET_OBJECTS:>` as the
  other commands already do. **Every other custom command in that file was then checked rather than
  only the reported one:** the X3, X4 and X5 links and the application link each already list every
  object on their own command lines, and the ESP command depends on the image it packages, so the
  per-class one was the only omission.
- **`KICKOS-EXIT` OVERRODE THE EXIT STATUS ON EVERY BOARD IN THE FLEET.** X7 gated the device
  recovery and the cross-check on `KICKOS_BOOT=uefi-pe` and left the line EXTRACTION ungated, then
  let the printed value win unconditionally, so on arm64, armv7m, riscv and sim any line matching
  `^KICKOS-EXIT status <n>$` in ordinary output replaced the real status with nothing checking it.
  Fixed by gating the whole mechanism on the posture, and by requiring corroboration on that posture:
  a printed line the device did not confirm is refused rather than believed. Witnessed by driving
  `boot_status` directly under both shells: with a forged line injected into the output, a
  `kernel`-posture run and a posture-unset run both still report the raw status 139, while on
  `uefi-pe` a forged 0 against a device reporting 139 is refused and a printed line with no device
  report is refused. That closes the forge as well as the accident, because on this board the console
  is polled and X4's over-grant lets an unprivileged thread write kernel memory.
- **THE RAM SEAM TOOK ONE CONVENTIONAL RUN AND DISCARDED THE REST, and one of its three defects made
  an empty arena pass every arm.** The pick was the largest single descriptor at or above the legacy
  floor, unaligned, unbounded and skip-on-straddle. Three fixes, each cheap: the page count is
  bounded before it is multiplied, so a descriptor near the top of the range cannot wrap into a small
  run at a huge address; adjacent runs are MERGED, without assuming the map is sorted, which UEFI
  does not promise; and a run straddling the floor is CLAMPED to it rather than discarded. The base
  is then rounded up to the map editor's large-leaf size, `arch_ram_alloc` aligning a region to its
  own size. Measured with a two-arm control at `-m 512`, the floor raised to `0x2000000` so the
  straddle case is the one under test: the old skip falls to an arena of 8616 pages at an unaligned
  `0x1bb8c000`, the clamp gives 105324 pages at `0x2000000`. At the real floor the arena is 107372
  pages at `0x1800000`, against the old code's 107500 at the unaligned `0x1780000`, the 128-page
  difference being exactly the alignment round-up. **AND AN EMPTY ARENA NOW FAILS.** The entry prints
  the arena and its page count and the X1 runner pins the count non-zero; the entry refuses a map
  naming no usable run; and `arch_init` refuses a published size of zero, which is what turns X3's
  arms from reporting `ok=1` against nothing into a named refusal.
  *DELIBERATELY LEFT: the arena is still ONE span.* At `-m 8G` the largest span is the one above
  4 GiB, so 511,373 pages of the 2,084,237 the map names go unused, 24.5% of conventional memory,
  and all of it below 4 GiB; at `-m 32G` the same 511,373 pages are lost out of 8,375,693, 6.1%. A
  multi-run arena is kernel-side work and is recorded in `TODO.md` rather than taken here.
- **ENABLING THE MEMORY AXIS WOULD MAKE EVERY PAGE TABLE RING-3 WRITABLE.** Recorded with F1's
  reservation-naming item above, where whoever turns the axis on will meet it, and NOT fixed here:
  the fix is the link-time separation of the halves that X5's own record already names. The axis is
  not selected.
- **THE GRANT IS LEAF-GRANULAR AND THE EXPOSURE CHECK WAS BYTE-GRANULAR.** `g_tables_exposed`
  compared a table's address against the range the CALLER asked for, and the unit this hardware can
  grant is a leaf: ring 3 was measured reading `0x460000`, past the image's requested end of
  `0x455000`, inside the same 2 MiB leaf. So the instrument's zero was true by where this firmware
  put its tables rather than because the check asked the right question, and on a firmware using
  1 GiB leaves the over-cover would be up to 1 GiB per end. **The check now asks the hardware.** The
  grant walk RECORDS the distinct tables it goes through, by physical address, and a census after
  every grant walks the live regime for each one and applies the processor's own rule, the user bit
  ANDing down from the root. A table exposed by a large leaf whose own walk never touched it is
  therefore counted. Overflow of the record is refused rather than dropped, an incomplete census
  being one that under-reports. And the DENOMINATOR is reported and asserted: `tables_walked` joins
  `tables_exposed` on the witness line and a new arm requires it non-zero, so a census over an empty
  record cannot read as a clean one. Measured: `tables_exposed=0 tables_walked=3`, which is what
  takes X4 from 74 arms to 75. What a zero establishes is now stated exactly in `ring3.h`: none of
  the tables THIS PORT's grant walked is reachable from ring 3 at the end of `ring3_init`. It does
  not establish that no table anywhere in the regime is reachable.
- **AND THE THING IT DOES NOT ESTABLISH IS FALSE, MEASURED.** Two blindnesses, either of them
  sufficient on its own: the corpus is the THREE tables the grant walked, against every one in the
  live tree, and the census runs inside `ring3_init`, BEFORE `aspace_init` installs the kernel
  window. **THE HIERARCHY'S OWN SIZE IS POSTURE-DEPENDENT AND THE EXPOSURE IS NOT**: 1035 tables on
  the default processor model against 13 under `-cpu max`, where the firmware uses 1 GiB leaves, and
  every reachability figure below is identical across the two.
  The table `aspace_init` installs, `g_kwin_table[2]`, is reachable and WRITABLE at CPL3, and so is
  `g_cpu`, the per-core block `IA32_GS_BASE` names, with `kernel_sp` at offset 0; a ring-3 read
  returned the live kernel-block top. Both are `.bss` statics of the one flat link, so they lie
  inside the 2 MiB image leaf the grant opens. **The device clause HOLDS and is now measured rather
  than argued**: zero reachable leaves in the APIC band, zero in low legacy, zero outside the image
  and the arena, identical under `-bios` and under `-cpu max`. **NOT the firmware-sibling mechanism**
  an external reviewer proposed: pre-grant the user bit is set NOWHERE in the hierarchy, so that
  hazard is UNFIRED here and UNESTABLISHED in general. And the exposure did not GROW -- once kernel
  RAM is writable, writing a table or `kernel_sp` grants nothing the scheduler state and the
  capability table in the same leaf did not already grant. What was wrong is two bounds that made
  the exposure describable. The instrument is `arch/x86/x86_64/probe4_x86_64.cc`'s whole-hierarchy
  census, which runs after `aspace_init`, asserts the device clause and PINS the two exposed pages
  by role.
- **THE KERNEL RAN FOREVER ON THE FIRMWARE'S STACK, and it now runs on its own.** Live RSP was inside
  `EfiBootServicesData`, exactly the 128 KiB UEFI 2.11 sets as the minimum, with type-7 Conventional
  memory directly below it. `landed_kernel_x86_64.cc` now switches to a 128 KiB `alignas(16)` array
  in the image's own `.bss`, which the PE loader placed in memory the image owns outright, and it
  switches AFTER the constructors, which still need the firmware stack, with `call` rather than an
  indirect jump so the callee sees the alignment the psABI gives an ordinary call and so the target
  is a direct relative branch: taking the address of the entry function instead would be a
  global-offset-table load on this `-fpie` build and `check-x86_64-no-got.sh` refuses one. The size
  matches what firmware supplied, so the switch cannot be a regression on depth.
  *DELIBERATELY LEFT:* nothing measures the boot path's depth or guards the low end, and the adopted
  translation root is itself `BootServicesData`, memory UEFI says an OS may reclaim. This port never
  reclaims boot-services memory, which makes the root safe by a premise rather than by enforcement.
  Both are in `TODO.md`.
- **ELEVEN FALSE DOC CLAIMS, plus drift in five sibling documents.** Corrected in place above, each
  against a measurement: three gates name an interrupt-stack-table slot rather than one, and the two
  beyond the double fault are the whole argument that closes the syscall entry's caller-stack window;
  X1's landed record exists; `SizeOfHeaders` is `0x600`; ten image arms; `fault_dump` asserts a status
  beside `sched_exit`; four arms assert 128 or more, `stackguard` being the fourth; five `ninja`
  witnesses; this board's capability table has TWO chunks, configure printing
  `2 chunks of 8`, which corrects which SIDE of the TCB measurement it is on without changing the
  measurement; and the APIC and timestamp-counter figures are a per-run calibration whose digits
  below the first do not reproduce. `ctest --preset qemu-x86_64` was documented and `x86.json` had no
  `testPresets` at all, so the documented command errored; the preset is added and the command ran
  33 of 33 at this step, a DATED record: more gates have been registered since and the board
  reads 38 now, per the fleet paragraph in the ladder section above. Status text is out of
  `x86.json`'s `displayName` and out of the board's `defconfig`, both of which `boards.md` forbids
  by its own rule.

*THREE RECORDS WERE STALE IN THE PORT'S FAVOUR, and an understated record is still a false one.*

- **LOAD-BASE INDEPENDENCE IS WITNESSED.** Relinking the full kernel at `0xff000000` made OVMF
  relocate it by a delta of `-0xE146D000`, and it booted and ran; X5's arms passed at five different
  link bases. The not-witnessed list said the independence was unproven; what is actually unexercised
  is a NON-ZERO delta in an ORDINARY run, the default link base being the one firmware honours.
- **BOTH LOCAL-APIC LEGS ARE EXERCISED.** `-cpu max` and `-cpu Skylake-Server` report x2APIC, the
  backend takes the MSR path and the arms pass, so "the x2APIC path is present and unwitnessed" was
  wrong. The default model takes the memory path, which is why the map-dependence caution stands.
- **THE USER HALF IS DERIVED, not a figure this port picked.** `aspace_user_lo` comes out of which
  low top-level slots the boot root leaves free, and the ten-model sweep gives it two distinct
  values: `0x8000000000` (512 GiB) on Nehalem, Haswell, both Skylake models, Cascadelake-Server,
  Icelake-Server and SapphireRapids, and `0x10000000000` (1 TiB) on qemu64, EPYC-Milan and `max`.
  Green at both.

*THE MODEL SWEEP IS NOW REPEATABLE, which it was not.* `KICKOS_X86_64_CPU` selects a `-cpu` model in
all five witness runners and passes nothing when unset, so the sweeps above are taken through the
supported path rather than by hand. The ten models are `qemu64`, `Nehalem`, `Haswell`,
`Skylake-Client`, `Skylake-Server`, `Cascadelake-Server`, `Icelake-Server`, `SapphireRapids`,
`EPYC-Milan` and `max`. X4 reports 75 of 75 on every one and X5 reports 141 of 141 on every one.
(A DATED X8 record, and BOTH of its figures have since moved, so the ten-model sweep stands only
as evidence that the models agree with one another. X5 went to 151 arms when the audit at that
step added ten and to 163 when the attribute-table redesign recorded above traded two arms for
fourteen; X4 went to 94 when the CPL3 reachability census landed. Re-derived 2026-08-29 on
`aea89358` with no `-cpu` model selected: X4 94 of 94, X5 163 of 163. The sweep has not been
re-taken across the ten models at either figure.)

*TWO HYPOTHESES WERE FIXED BLIND because they are cheap and unmeasurable here.* The measured timer
and timestamp-counter frequencies were `uint32_t`, so any part above 4.295 GHz wrapped and a 4.5 GHz
processor would have reported a clock about 22 times fast; TCG ignores `tsc-frequency`, so nothing on
this bench can produce one. Both fields are 64 bits now, and the two places that must narrow,
`arch_cpu_clock_hz` and `SystemCoreClock`, CLAMP rather than truncate: a rate low by whatever the part
exceeds the width is wrong in a direction a caller can reason about, where a wrapped one is not. The
seam's own 32-bit width is the residual and is in `TODO.md`. And the firmware vendor string's loop
tested `vendor[n] != 0` before `n < 64`, so it read element 64 before deciding it was out of range;
the bound goes first.

*THE REST ARE RECORDED RATHER THAN FIXED, because fixing on speculation is how a port acquires code
no measurement asked for.*

- **The translation-tag arm is MODEL-DEPENDENT, and this bench cannot produce the model.**
  `arch_aspace_model` sets `ARCH_ASPACE_MODEL_ASID` when the part reports NO identifier, and probe5
  asserts the bit, so on a part reporting one the arm goes red. The review predicted that for any
  Haswell-or-later outside TCG. Measured across the ten models: every one of them, `max` and
  SapphireRapids included, reports `tag_bits=0`, so the prediction is UNPRODUCIBLE here and the
  hazard is the arm's SHAPE rather than a failure anyone can currently see. A KVM run would settle
  it, and this bench has no reachable `/dev/kvm`.
- **The i8254 is mandatory on this board and its rate is hardcoded**, being the only reference the
  APIC timer can be measured against.
- **The xAPIC register window is dereferenced without being mapped or checked by this port**, and it
  is the DEFAULT leg. It is reachable through the map OVMF built, which is the map-dependence caution
  X3 already states, applied to a window rather than to a frame.
- **`swapgs` in the trap path keys on the interrupted frame's code selector**, so a non-maskable
  interrupt or machine check taken in either window runs on the USER `gs` base. Harmless only because
  no handler dereferences `gs`, which is an invariant stated nowhere and exactly what SMP per-CPU data
  in the trap path would break.
- **TSC invariance is never checked.**

**AN EXTERNAL AUDIT OF THE ENTRY, SWITCH AND INTERRUPT PATHS (2026-08-29) RAISED SIX FINDINGS,
FOUR HELD, ONE DID NOT, AND ONE WAS A CONTRACT DEFECT RATHER THAN A BACKEND ONE.** Each was checked
against the manual before it was fixed, which is what separated them.

- **The interrupt entry called C with a caller-chosen direction flag, and it HELD.** Delivery
  through a gate clears TF, NT and RF, and an interrupt gate also clears IF; the direction flag is
  on neither vendor's list (Intel SDM Vol 3 section 7.12.1.3, AMD APM Vol 2 section 8.9.2), so it
  arrives holding what the interrupted code left in it, and the instruction that sets it is
  unprivileged. `trap_x86_64.S` clears it ahead of the first call. The SYSCALL leg was already
  covered and that was verified rather than assumed: bit 10 is in the `IA32_FMASK` value
  `ring3_x86_64.cc` programs, and SYSCALL clears every flag the mask names. **WHAT MAKES THIS MORE
  THAN A CONVENTION HERE:** the built kernel archive carries ten `rep stos`, the compiler's own
  lowering of a zero fill. None of the ten is on the interrupt entry's call graph today, so the
  hazard is LATENT rather than live, and it is latent by a property of this month's code generation.
  `tests/static/check_x86_64_entry_cld.sh` reads the assembled object rather than the source text
  and goes red on the instruction's deletion; no runtime arm in this tree witnesses the hazard,
  nothing on this board setting the flag from ring 3.
- **A hostile user stack pointer turning the syscall return into a kernel fault DOES NOT HOLD**, and
  the manual is what says so. The only frame field a ring 3 caller supplies through SYSCALL is the
  stack pointer, and IRETQ performs NO canonicality check on the popped RSP: the 64-bit
  outer-privilege return does `RSP := tempRSP` with no test, and the instruction's own 64-bit
  exception list names a non-canonical address only for the return instruction pointer and for
  popping off the OLD stack (Intel SDM Vol 2A, IRET/IRETD/IRETQ). So a malformed value is loaded and
  faults on the user's first stack access at ring 3, where the kill rule contains it. Every field
  IRETQ DOES check is kernel stamped: the return instruction pointer comes from `rcx`, which SYSCALL
  writes, or from a hardware-pushed interrupt frame; the two selectors are immediates in `switch.S`;
  the flags are masked, the I/O privilege level among them. Nothing was added. **THE RESIDUAL IS
  X4's OVER-GRANT AND NOT THE ENTRY:** while a ring 3 thread can write kernel memory it can write a
  parked frame directly, and a canonicality test on the way out would be a guard in front of an open
  door.
- **Enabled vector and x87 state neither saved nor trapped, and it HELD, measured rather than
  argued.** The X1 line reports what this firmware hands over: `em=0 ts=0 mp=1 osfxsr=1
  osxmmexcpt=1 osxsave=0`, which is SSE fully enabled at both privilege levels with nothing in the
  port saving an XMM register. TRAPPING was chosen over eager save, on the isolation principle's
  refuse-rather-than-mask tiebreaker and because the port supports none of the state.
  `entry_x86_64.cc` sets `CR0.EM`, `CR0.TS` and `CR0.MP` and clears `CR4.OSFXSR`, `CR4.OSXMMEXCPT`
  and `CR4.OSXSAVE` as soon as ExitBootServices returns, which is the FIRST CR4 write anywhere under
  `arch/x86`. **FIVE CLASSES AND FIVE DIFFERENT BITS, which is why it is not one bit:** `CR0.EM`
  raises #NM on x87 and #UD on MMX and legacy SSE, and reaches NO VEX-encoded instruction and no
  member of the XSAVE family (Intel SDM Vol 2A Table 2-21 lists `CR0.EM` under the legacy-SSE row
  alone), so `CR4.OSXSAVE` clear is what takes those; `CR0.TS` is a second net across all of them;
  `CR0.MP` with it takes WAIT and FWAIT. The two remaining CR4 bits are cleared because this
  operating system manages neither that state nor the SIMD floating-point exception.
  `tools/run-qemu-x86_64.sh` pins the installed line and matches the found one loosely, the second
  being the firmware's choice and the first not.
- **Unowned interrupts contained as user faults with no end-of-interrupt, and it HELD.**
  `arch_fault_is_user_thread` read the pushed code selector for every vector, so an external
  delivery at ring 3 was attributed to whichever thread it interrupted. It now refuses a vector at
  or above 32, the processor's own boundary between the exceptions an instruction raises and an
  external delivery (Intel SDM Vol 3 Table 6-1), and `kickos_x86_64_isr` writes the end-of-interrupt
  register for such a vector before returning it to the report (section 13.8.5). Measured both ways
  by pointing the doorbell at an unowned vector: before, the self-test dies on `=== THREAD FAULT ===
  thread 'irqI' killed`; after, the fault report and a halt. **AND `apic_eoi` IS TOTAL NOW**, because
  `desc_init` loads the interrupt table ahead of `apic_init`: with no register block resolved the
  write would have landed at offset `0xb0` of the low identity map.
- **Different pending lines collapsing into one doorbell payload: TRUE OF THE CODE, and the finding
  named it as this backend's defect when it is the SEAM's.** `arch.h`'s single-doorbell contract
  states exactly that coalescing and the caller obligation that makes it sound, and the xtensa
  backend carries the same one-cell shape. What was actually wrong is that the contract named the
  backends it applied to as a HAND-MAINTAINED LIST, and x86_64 was not on it, so this port took the
  weaker reading of an obligation it was silently under. The backend now carries a pending BITMAP
  its doorbell handler drains, which is strictly stronger than the floor and changes nothing a
  portable caller may assume; the contract text now states the floor, names which backend carries
  which form, and says that a backend absent from both lists is promised more than it may deliver.
  **KEYING THE CONTRACT ON A DECLARED PROPERTY rather than on a list is the real fix and was NOT
  taken**: it needs every backend to declare the form it implements, which is six ports and outside
  this pass. `irq_two_lines_one_region` in the X3 witness is the arm; reverted to one shared cell it
  reports `delivered=1 seen=8 want=136`, line 7 lost.
- **The firmware interrupt table staying live after ExitBootServices, and it HELD.** The
  constructors and the stack transition run before `desc_init`, and `cli` masks neither the
  non-maskable interrupt nor the machine check, so anything raised in that window vectored through
  handlers and storage whose lifetime ended with the call. `entry_x86_64.cc` builds and loads a
  32-gate table of its own the instant ExitBootServices returns, every gate naming one body in the
  same translation unit that reports and halts; `desc_init` replaces it. THIRTY TWO because those
  are the vectors the processor itself raises and a delivery above them takes a general-protection
  fault against the limit, which gate 13 reports. The gate selector is the live `cs`, the descriptor
  this code is already executing under. Witnessed as a measurement rather than as an arm, the
  emulator raising neither of the two classes the finding is about: a `ud2` planted in the window
  reports `FAIL trap before the descriptor tables were installed`, and the same `ud2` with the
  `lidt` alone defeated produces NOTHING on the wire.

**Stage result:** three MMU backends behind one seam, and a boot path that has been read rather than
fitted to one firmware.

### M6.5 -- frame-level capabilities

Scoped by F3 and deliberately last, and now designed against three backends rather than one. C1
through C3 were stated coarsely on purpose: the shape of C2 depends on what T5 and T6 actually cost,
and pinning it before them would have been a guess wearing a plan's clothes. C0 is what replaced the
guess with measurement, and the four findings under it are what the three steps below now reason
from.

**"AGAINST THREE BACKENDS" IS A DESIGN CLAIM AND NOT A WITNESS COUNT, and the evidence sentence has
to say so.** `CHIP_Q35` selects no memory family (`arch/Kconfig`), so the x86_64 board builds none of
the kernel-side aspace family and runs none of these arms; F8's own record forbids selecting the axis
there before the link-time separation of the halves, every table the port allocates being ring-3
writable until it lands. So M6.5 is HELD AGAINST three backends and RUN on two, which is the RX72M
method F8 endorses rather than a shortfall against it. A step that reports "three backends" without
that clause overstates by one.

**C0. The baseline, before the first object kind. LANDED.** F8's verdict is a diff against a frozen
API and 3.4b requires the API to exist before the falsifier starts; C1's expected result is a diff
too, and it had nothing to diff against. `tests/static/check_cap_sigdiff.sh` declares the capability
ABI family over `user/include/kickos/sys/abi.h` and `system/include/kickos/sys/cap_index.h`, and
`tests/static/cap_seam_records.txt` freezes it. Unlike the aspace differ it is ON the ctest ladder,
because its expected verdict is PASS for the whole milestone: where the aspace differ reports a diff
for a seam that was moving, this one asserts the capability ABI did not gain an addressing concept.
*Expected, and observed at C0:* PASS, 22 records.

Four findings, each of which the steps below cite rather than rediscover.

  - **The entry's type field is exactly full after this milestone.** `KCAP_TYPE_BITS` is 3 and
    `CAP_IRQ` is 5, so a frame kind and a page-table kind take the last two values. A THIRD kind
    needs a repartition, and the reply call sequence packed beside the type and the rights is what a
    repartition spends.
  - **Both budget guards were blind to exactly that growth, and are not now.** Each was keyed on the
    last member by NAME, so `CAP_IRQ` and the three rights bits bounded themselves and nothing added
    past them. Measured at C0: a kind at 8 and a fourth rights bit both compiled clean while an
    independent probe over the same tree failed on the same expression. `CapType` carries a
    `CAP_KIND_MAX` sentinel the assert reads, and the rights mask is `CAP_RIGHTS_ALL`. This is the
    milestone's own recurring class one level below where M6.2 through M6.4 kept meeting it: not an
    instrument whose corpus can go empty, but a guard whose SUBJECT does not grow with the thing it
    guards.
  - **A capability may not name one frame, and the numbers are not close.** The frame pool is 8 MiB
    on both translating chips, so 2048 frames of the frozen granule, against a root capability table
    of 10 slots, a spawned child's 7, and object pools of 16, 8, 4 and 8. Two to three orders of
    magnitude on both axes. So the unit a frame capability names is a RUN, which is what the frame
    pool already allocates and what a reservation already is; a per-frame object is not a pool-sizing
    question that a bigger number answers.
  - **The kernel-side cap layer is outside C0's instrument and says so in its own file.** The
    extractor reads a C header and `kernel/include/kickos/cap.h` is C++, yielding twelve records of
    any name, so a corpus built over it would have compared two near-empty sets and reported clean.
    What holds that half is the static_assert set in the header, which is why the second finding
    above was worth fixing rather than noting.

**Two rulings C0 owed, taken here so C1 does not discover them.**

*Map and unmap take their permission from the AUTHORITY WORD and not from a new right.* The rights
field is full, and widening it spends the reply sequence, breaks the frozen 8-byte entry and the
packing arm that pins it, to buy a per-capability right where the tree already answers this question
another way: `KOS_SYS_MEM_SELF_GRANT` is gated on `AUTH_MEMORY` today. Possession of the object
carries the object; the authority word carries the permission to map it. This also leaves the last
two type values for a real third kind rather than spending one on a right.

*A frame capability names its run by HANDLE, and the physical identity stays in the object below the
resolve chokepoint.* Today a reservation's virtual address IS its physical run base (`aspace_reserve`,
landed at T5), which is what lets the handoff reproduce a donor's address in a second space, and C3
retires that identity by letting a holder choose the address. Keeping the number above the chokepoint
would put an address in the cap layer, which is the one thing C1 exists to show does not happen. F10
already demoted system-wide uniqueness from a freeze to a policy for a wide-address backend; the
same-address rule demotes the same way, from a mechanism to what a caller may still ask for.

**C1. Frame and page-table objects in the capability layer**, typed like every other object and named
by handle.
*Expected:* the object pools and the resolve chokepoint carry them with no new addressing concept
anywhere in the cap layer, which is the address-space-agnostic property the spike's QW-5 asked be
preserved and the one thing M6.5 could plausibly break.

**THE SECOND KIND IS AN ADDRESS SPACE AND NOT A PAGE TABLE, and F3's own wording is what this
corrects.** F3 says "page-table and frame capability types", which is the vocabulary of a kernel that
holds page tables. This one does not: 3.4b freezes `map` as the owner of the entry encoding, F1 keeps
the identifier below the seam, and the kernel's handle on a space is the opaque `struct arch_aspace`
whose tables are allocated under the seam and never named above it. A capability called a page table
would therefore name a mechanism the seam exists to hide, and it would do it inside the one layer C1
is meant to prove stayed free of them. So the pair is a frame RUN and an ADDRESS SPACE, which are the
two things the kernel actually holds. F3's phrase came from the lineage the spike was read against
and was never a decision taken here; per section 5's opening rule it is an observation being removed
rather than a decision being revised, and nothing downstream rests on it.

**C2. Map and unmap as capability operations**, inside the single lock F4 keeps.
*Expected:* a mapping installed and revoked through the cap layer with the resolve-to-use span no
wider than it is today.
*Landed at C2, and three things are worth the record.* The permission comes from the AUTHORITY word
as C0 ruled, so no rights bit was spent and the last two type values stay available. ONE lock spans
BOTH resolves and the edit they drive, so the F4 span is one lock wider than nothing and no window
opens between resolving the frame and resolving the space. And the range the map records is
BORROWED, which is not a new lifetime rule but the one F10 already has: the frames belong to the
capability, so the space that maps them frees nothing at teardown and the last holder is what
returns them.
*The ADDRESS is an argument and never a field.* Both syscalls take it positionally, so no structure
in the capability ABI carries one and the two members that entered C0's baseline are plain
enumerators. That is what makes C1's claim survive a step that necessarily names addresses: the cap
LAYER stays free of them while the OPERATION over it takes one.
**AN EXTERNAL AUDIT (2026-08-30) RAISED TWO HIGH FINDINGS AGAINST THIS STEP AND BOTH HELD. They are
ONE missing thing: a mapping did not record or pin the run it named.** The revoke matched a SHAPE,
`VR_BORROWED` plus a page count, which the image (`VR_IMAGE | VR_BORROWED`) and every F10 handoff
also satisfy, so a frame capability of the right length could revoke a mapping it never placed, its
own image text included; and the map took no reference, so the last CAPABILITY's drop freed frames a
live leaf still pointed at. C3's own lesson is why neither showed: a freed frame stays readable
through a leaf nobody tore down, and the tests unmapped before closing.

*The fix is identity, and it is asked of the HARDWARE rather than stored.* A `VR_FRAMECAP` flag says
this range came from `aspace_cap_map`, and WHICH run is settled by comparing
`arch_aspace_frame_at(space, va)` against the named run's base. A field on the range would have been
the obvious answer and it costs eight bytes in every range of every domain: measured, a `run` field
defaulted to -1 also made `VirtualRange` non-zero-initialised and moved the whole domain array out
of `.bss` into `.data`, 32 KiB of image and boot-time copy, and even zero-defaulted its 5 KiB of
`.bss` shrank the arena enough to break `mem_self_grant`, which sat on ONE range slot of margin.
The final shape costs sixteen bytes.

*And a MAPPING IS A HOLDER.* `aspace_cap_map` takes a reference on the run and `aspace_cap_unmap`
surrenders it; a space that dies still mapping surrenders it in the teardown sweep, which reads the
frame before the unmap clears it and releases by base. Witnessed by `cap_map_pins_run`: a child maps
the delegated run, drops its own capability, root drops the last one, and the holder count reads ONE
with no capability anywhere naming it.

*One arm's limit, measured rather than assumed.* "A second unmap is refused" does NOT witness that
the range came back: with the release deleted, the backend refuses an already-unmapped range on its
own and that check still passed. What witnesses the release is RE-MAPPING the same address, which
goes red exactly there. The weaker check was written first and kept only because the stronger one
sits beside it.

**C3. Sharing GENERALISED, not restored.** One frame mapped into two processes by an explicit grant,
at addresses the holders choose -- where M6.2 had exactly one hard-wired case of this, the handoff
F10 contracts at handover time, whether that handover is an explicit task create or a grant-carrying
spawn. T5 removed the dedup, never the sharing.
*Expected:* two processes reading one frame through separate address spaces, with T8's denial arms
still failing closed for everything not granted.
*Landed at C3.* The frame reaches the second process as a DELEGATED CAPABILITY and nothing else: its
task is created with a null block, so F10's handoff never runs, and the child names its own space
through a capability of its own. The delegation mask is `CAP_TRANSFER` alone, neither new kind using
a rights bit at all.
*The authority ruling gained its negative control here, and it was found rather than designed.* A
spawn grants NO authority by default, so the first attempt at this arm had the child refused at the
map and reporting nothing; passing `KOS_AUTH_MEMORY` explicitly is what made it work. An identical
child spawned without it now stands beside the positive case, so possession of the frame is
witnessed NOT to be permission to map it, which is the gap C2 left open.

**C3'S LIFETIME RULE IS THE FRAME RUN'S OWN REFCOUNT, AND IT DOES NOT REPLACE THE DONOR EDGE.**
This paragraph said it did, before the step was built; that was a prediction and it was wrong, so it
is corrected here rather than left for the next reader. T4 refused a frame-level refcount on the
ground that one would duplicate the ownership M6.5 would express properly, and T10 completed the
surviving rule with a reference on the DONOR's domain, carried by `Domain::borrowed_from`.

What C3 actually adds is a SECOND ownership, and the two coexist because they own frames
differently. Under F10's handoff the frames come out of the DONOR'S RESERVATION, which its range
list owns, so the donor edge is what stops a donor's teardown freeing them under a borrower; that
path is unchanged and still needs it. Under C3 the frames belong to the RUN OBJECT, refcounted by
the capabilities naming it, and the space that maps them owns nothing: the range is recorded
BORROWED and its teardown frees nothing. A donor dying first therefore takes no frames with it
because it never held them.

**And the cycle the old paragraph feared is not expressible on the new path, for a reason the edge
never had.** `borrowed_from` is a domain-to-domain edge, which is why its acyclicity had to be
argued from being written once. A capability reference points from a THREAD'S TABLE at an OBJECT,
and objects name no domains, so there is no domain-to-domain edge to close a cycle with however many
holders there are. The write-once argument is not weakened by N holders; it is simply not the thing
carrying C3.

*Witnessed at C3:* one run mapped into two spaces at two DIFFERENT addresses with the bytes crossing
both ways, the borrower's task killed while the donor still maps it, and the pool asserted to still
be one run short at that moment -- reading the page does NOT test this, a freed frame staying
readable through a leaf nobody tore down. Measured: freeing the run at the first drop instead of the
last turns exactly that assertion red.

## 6. What M7 inherits

**Multicore inherits a live TLB obligation rather than a deferred one, and that reverses what its
own spike says.** `docs/design-m7-smp.md` was written when SMP came first, and it argued that
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
