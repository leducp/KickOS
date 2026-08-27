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
it, and T5 made the self-grant a refusal by name rather than a silent widening. **Its unfinished half
is the pair of static-data hooks, and section 3.3 carries it**: those are still keyed on
`KICKOS_HAVE_MPU`, so on this board they take the non-enforcing arm and admit app globals out of a
linker whitelist. **T8 SUPPLIED THE HOSTILE WITNESS** (`grant_kernel_word_refused`): an authorized unprivileged
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
A64 alone produced three claims it falsifies, and each is corrected where it was made:

  - **F1's "the kernel half is untouched by a switch"** holds on A64's two-register split and fails
    on a single root register, where the kernel's mappings must be replicated per table.
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
  - **F1's address-space-identifier note** reasons from 16 bits always being there. The second
    backend's identifiers are narrower and feature-gated, so the CONCLUSION survives -- thousands of
    identifiers against a domain pool of tens -- while the premise does not. Do not let the width
    into the design.

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
dies, leaving exactly one space holding the block; there is no refcount, because a refcount here would
duplicate the ownership M6.5 is going to express properly and would have to be unpicked again. T4
makes a violation loud rather than absorbed: the frame pool counts refused frees and an arm requires
that count to be zero, so a double free fails a test instead of being swallowed by the allocator's own
guard. T5 and T6 honour the rule or T8b replaces it with something better.

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
`arch_mpu_probe_addr` stays keyed on `KICKOS_HAVE_MPU`, that one being a question about descriptors.

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
    `-mcmodel=large` on the three archives holding kernel text (`kickos_apply_split_image`): it emits
    no GOT reference at all, which leaves the one `.got` entirely to the app, AND it materialises
    every external address as a 64-bit literal, which retires all four kernel-to-app data crossings
    with no source change. Measured cost on the `selftest` link: kernel text 45,561 to 53,000 bytes,
    in a 1 MiB region. `-mno-direct-extern-access` does not exist on this GCC.
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
Skips stay empty. The ctest total for the board went 31 to 32, the extra one being
`qemu_arm64_faultsurvive`: the fleet's own survive arm became available the moment the arch joined
`KICKOS_FAULT_ISOLATION`, and registering it was a one-line board-list edit.

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

*Ordered invalidation was ESTABLISHED BY NEGATIVE CONTROL, not asserted, and the result splits.* Two
of the four orderings have a witness on this bench and two do not, and which is which was measured
by removing each one and running the board.
  - `arch_aspace_unmap`'s per-page invalidate IS witnessed: without it `aspacefault` fails, the
    running translation still answering for a page the editor removed.
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
  - **The data-cache clean and invalidate seam,** `arch_dcache_clean` and `arch_dcache_invalidate`,
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

### M6.3 -- the RV64 Sv39 backend, and the aspace-seam verdict

Per F8 this is the LITMUS, so a step here is judged by what it proves about the seam and not by how
much of a platform it lights up. QEMU first; the silicon witness is a single-core RV64 Sv39 part.

**R1. The ISA half, reusing what exists.** The RISC-V trap vocabulary and CSR shapes are already in
the tree, but this port runs in S-mode against a page-table root where the existing backend runs in
M-mode against PMP, so the privilege model is new work rather than reuse.
*Expected:* boot to a console byte and a running switch under `qemu-system-riscv64`, with the memory
model still flat. Do not carry over an assumption from the M-mode backend without re-deriving it.

**R2. The aspace family on a single root, with PA WIDER THAN VA.** Build, activate, map, unmap, and
the page-window pair of section 3.3.
*Expected:* the acquire/release window is exercised on a target that CANNOT direct-map its whole
physical address space, which is the arm that fails if any helper still assumes a fixed offset. This
is the single most valuable expected result in the milestone, because it is the one A64 and x86_64
cannot produce between them.

**R3. Level count and paging mode as backend facts.** The mode is selectable on one part, so the
level count is not a constant.
*Expected:* the granule query of F7 answers, nothing above the seam names a level count, and switching
the configured mode changes no kernel code.

**R4. No address-space identifier, legitimately.** The architecture permits the identifier length to
be hardwired to zero.
*Expected:* the port works with no identifier at all, which is F1's claim that identifiers stay below
the seam, tested rather than asserted.

**R5. The verdict.**
*Expected:* an EMPTY signature diff on the aspace family. A non-empty diff is the finding F8 predicts,
and it lands in the seam with A64 updated to match.

**Stage result:** the aspace seam proven against a target chosen for unlikeness rather than
convenience, and the T-step arms reused unmodified.

### M6.4 -- the x86_64 backend, and the entry-path verdict

Per F8. The purpose is falsification, so a step here is judged by what it proves about the seam and
not by how much of a PC it lights up.

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

**X2. Descriptor tables and the fault report.** The segment descriptors x86 needs even without
segmentation, the interrupt table, and a task-state segment carrying the kernel stack.
*Expected:* a deliberate fault reports and halts with its vector and error code. Note that an absent
or malformed interrupt table triple-faults, which presents as the EMULATOR RESETTING rather than as a
fault report -- so an early fault that reboots the machine is this, not a bad handler.

**X3. Console, timer, interrupts, switch, idle.** A polled COM1, the local APIC timer as the tickless
one-shot, the monotonic clock, the switch, and the halt instruction for idle.
*Expected:* the S5 and S6 arms pass on this arch with no change to the arms. The polled console is a
deliberate scope cut: it keeps the IO-APIC and the legacy interrupt controller out of the milestone
entirely and leaves the APIC timer as the only interrupt source. Mask the legacy controller anyway --
an unmasked one delivers spurious vectors into an interrupt table that has just started working.

**X4. Ring 3 and the syscall entry.** The fast syscall pair, with the kernel stack loaded EXPLICITLY
because F8 shows the instruction loads none, the flag mask programmed so the interrupt flag is clear
on entry, and the per-core pointer through the segment base x86 uses for exactly that.
*Expected:* S7's arm passes, and the kernel stack is demonstrably loaded BY the entry rather than
inherited from the caller. This is the step where M5.2.1's claim is re-earned by hand on a second
arch, which is itself worth knowing.

**X5. The aspace family on a single root.** The kernel half replicated into every table, the root
switch performed from code mapped identically on both sides of it, and the invalidate primitives.
*Expected:* T4's four transitions and T6's process witness both pass, with no change to either arm.
Reusing the arms unmodified is the point: an arm that needed rewording per arch would be describing
the backend rather than the contract. **And the two arms that carry the contracts most likely to have
been written A64-shaped come with it:** the driver handoff readback, since same-frame same-address
mapping on a single-root architecture is a different implementation of the same promise; and T7's
equal-virtual-address copy, since the owner-carrying helpers are where an A64-only author would most
plausibly have leaned on two table registers without noticing.

**X6. The verdict.**
*Expected:* an EMPTY signature diff on the aspace family across the whole port. This is the
sub-milestone's actual deliverable and the only auditable form of "the implementation is not coupled
to one architecture". A non-empty diff is not a failure and must not be written up as one -- it is the
finding F8 predicted, and it lands in the seam.

**Stage result:** three MMU backends behind one seam, and a boot path that has been read rather than
fitted to one firmware.

### M6.5 -- frame-level capabilities

Scoped by F3 and deliberately last, and now designed against three backends rather than one. These three are stated coarsely on purpose: the shape of C2
depends on what T5 and T6 actually cost, and pinning it now would be a guess wearing a plan's clothes.

**C1. Frame and page-table objects in the capability layer**, typed like every other object and named
by handle.
*Expected:* the object pools and the resolve chokepoint carry them with no new addressing concept
anywhere in the cap layer, which is the address-space-agnostic property the spike's QW-5 asked be
preserved and the one thing M6.5 could plausibly break.

**C2. Map and unmap as capability operations**, inside the single lock F4 keeps.
*Expected:* a mapping installed and revoked through the cap layer with the resolve-to-use span no
wider than it is today.

**C3. Sharing GENERALISED, not restored.** One frame mapped into two processes by an explicit grant,
at addresses the holders choose -- where M6.2 had exactly one hard-wired case of this, the handoff
F10 contracts at handover time, whether that handover is an explicit task create or a grant-carrying
spawn. T5 removed the dedup, never the sharing.
*Expected:* two processes reading one frame through separate address spaces, with T8's denial arms
still failing closed for everything not granted.

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
