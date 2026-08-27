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
  handover time, whether that is an explicit create or an implicit spawn -- because the driver bring-up idiom requires exactly that and would otherwise break.
  What M6.5 adds is a general way to express sharing; it does not introduce sharing.
  **The existing `domain_share` arm needs a decision, and calling it a sibling test would be wrong.**
  It is two IMPLICIT spawns carrying the same reserved range, and resolving a spawn's task always
  spends a fresh task slot, so those two were never siblings: the dedup is what made them share a
  domain. After F2 they are two tasks, two spaces and two handoffs of one range -- and the arm STILL
  PASSES, because F10 maps those frames at the same address in both. So it is KEPT and re-described as
  the two-handoff witness it becomes, which tests F10 better than it tested F2. A genuine
  sibling-sharing witness is a NEW arm -- one task create with two members, not a rewrite of this one
  -- and it belongs at T6, beside the process witness, since sibling visibility is only observable
  once two members have a mapped image to share.
  **The harder half is the singleton BEFORE that dedup.** Every unprivileged task with no explicit
  grant is returned the one immortal default-user domain, which is a shared identity by construction
  and by comment. That is the COMMON case, not an edge: two ordinary tasks would still share one
  root after the dedup arm is gone, and F2 would still be false. On a translating build the
  default-user state becomes a TEMPLATE that each task instantiates, not an identity that each task
  joins. T5 therefore owes a no-grant two-task witness beside its same-grant one, and the no-grant
  case is the one that fails today.

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
an explicit task create and an implicit spawn carrying it, and above that freeze the distinction does
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

### F5. A translation fault kills the thread. There is no demand paging

An unmapped access is a fault the kernel reports and contains, exactly as an MPU denial is today:
no swap, no overcommit, no copy-on-write, and no BACKING STORE decision in a fault -- the kernel
never allocates memory, performs I/O, or extends a mapping there. This keeps the
M4.7.9 fault-isolation machinery -- `arch_fault_is_user_thread` and `arch_fault_redirect_to_exit`
(`arch/include/kickos/arch/arch.h`) -- as the whole of the answer, with the syndrome register
decoded into the existing report. Stating it matters because "the MMU arrived" is otherwise read as
"demand paging arrived for free".

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
dangerous.** `KICKOS_HAVE_MPU` currently carries two meanings at once -- "this build has MPU
descriptors" and "memory protection is live" -- and an MMU board sets it to 0 on the first meaning
while needing the second. With it at 0 today, grant admission degrades to the memory-type check
alone, losing arena confinement and the reserved-block refusal, and the region encoder reports every
region as enforced unconditionally. A self-grant can then add an arbitrary range that `user_range_ok`
trusts and the access helpers dereference PRIVILEGED. On an MPU-less board that is merely honest,
there being no protection to breach; on a TRANSLATING board with a high-half kernel it is a
user-reachable privileged access to a kernel address.

So this freeze has a second half: **the two meanings are split before any MMU board configures.**
Enforcement-live becomes its own fact that the MMU arm sets, Rule 7 and reserved-block admission stay
active under it, and the self-grant path is either a real map operation or refused by name. T5 does
not land before that split, and T8 owes a hostile witness: an authorized unprivileged caller
self-granting a high-half kernel address must be REFUSED, not merely fail to fault.

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

The reason it is generic is worth knowing, because it decides the fix. Only the M-profile backends are
FORCED to derive the thread pointer by masking the stack pointer, that ISA offering no per-thread
register. The three arches that DO have one currently mask anyway, deriving the pointer from the
stack rather than seating it from the thread's own block. That is a chosen uniformity, not a
requirement -- and it means A64, which has a thread-pointer register, escapes the tax only if it
SEATS that register from the thread control block instead of copying the masking idiom. Doing so is
what makes the fragmentation win real; copying the idiom would keep power-of-two stacks and let T6
pass while the claimed target state quietly did not hold. The universal statement in the Reference
gets a per-mechanism qualification, and the non-power-of-two stack witness lands at T6,
which is where a stack stops being an arena block and becomes frames with a guard page.

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
break under task-wide mapping, and the reason is precise: possession is currently DERIVED from the
caller's region set, so widening the mapping silently widens possession with it. **So possession must
become explicit thread-local authority rather than a walk over what the caller can reach.** That is a
real change, it is a precondition of T5 rather than a consequence of it, and once made it preserves
both the ABI promise and the two arms unchanged on every backend. Conflating authority with
reachability is the actual defect here; the MMU only exposes it.

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
The explicit path hands a reserved range to a task create. The IMPLICIT path is older and is the one
the allocation header actually names -- a reserved range handed to a plain spawn as that thread's
domain data region -- and resolving a spawn's task always spends a fresh task slot, so an implicit
spawn is a new task too. Today the dedup is what let two such spawns land on one domain; once F2
deletes it, an implicit spawn carrying a reserved range IS this handoff, arriving through a different
syscall. Contracting only the create call would leave every implicit-task spawn without a mapping,
which is most of the spawns in the tree. Two live mappings of one block is not a new state: the task-create contract already warns that
mismatched memory-type flags leave exactly that, and that rule becomes the coherence rule for the
pair.

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
  - **The region-budget refusal evaporates.** The bound is a count of hardware descriptors, and
    section 3.1 has already established that ceiling has no analogue under paging, so exhaustion
    becomes a frame question or does not occur.

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

**`TPIDR_EL1` is the per-core pointer M7 wants, and this port has ALREADY SPENT IT**, which the
tri-arch spike caught and which `roadmap.md` still states as a plan. `ENTER_FROM_EL0`
(`arch/arm64/armv8a/switch.S`) uses it as the entry scratch, and its own comment names the collision
while creating it. The two intentions are circular rather than merely conflicting: the scratch is
needed only because `SP_EL1` cannot be trusted on entry, and `SP_EL1` cannot be trusted only because
`thread_create` seats `ctx.kernel_sp` AFTER `arch_context_init` returns, so a new thread's first trap
arrives with `SP_EL1` still on its user stack.

So the resolution is to seat the kernel stack pointer inside `arch_context_init`, which makes
`SP_EL1` trustworthy on entry and frees the register. **That is the same edit T6 already owes** for a
different reason: T6 must stop building a thread's privileged return state on the thread's own stack,
and both changes are about `arch_context_init` establishing trusted state before the first trap
rather than after it. Doing them together is cheaper than doing either alone, and neither is M7 work.
One further M7 note while it is in view: `kickos_armv8a_kernel_sp` is a single global, so it becomes
per-core at the same time the pointer does.

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

### 3.1 The region array becomes an opaque address space

`domain_region_count` / `domain_region_at` (`kernel/domain/domain.cc`) are already the only
sanctioned readers, and `kernel/thread/thread.cc` composes a thread's set through them. That was
the spike's QW-1 accessor half and it is what makes the representation swap local.

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

**Nor does a domain currently get destroyed, which F2's "freed at the last release" quietly assumes.**
Release decrements a refcount and does nothing else; there is no destroy path anywhere in the tree,
and a slot is reused by scanning for a zero refcount and reinitialising it in place. Under an MPU
that is complete, the region set being pure description. Under paging the same slot reuse strands a
page-table root, its tables, and every data and stack frame the process held -- so a build can pass
the mapping and process witnesses while leaking every process that exits. Destruction is therefore
part of this rewrite and not an afterthought: an aspace destroy contract, ordered invalidation before
the root or identifier is reused, and a complete unwind on every failure path.

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

**And there is a public ABI in the way.** The RAM-allocation syscall returns the arena bump pointer verbatim and grants nothing; the
self-grant syscall then takes that same number as a PHYSICAL base and admits it by rounded size and
natural alignment. The number is load-bearing in THREE places, not one: the self-grant, the domain
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
`kaccess_from_user`, `kaccess_to_user`, and `ep_copy` -- which are identity copies because a
validated user address is directly kernel-dereferenceable. That funnelling was the spike's QW-2 and
it is what turns this from a hunt into three functions.

Two things change, and the second is the one the spike misses.

**Validation should read a range list, not walk a table.** A per-address-space list of granted
virtual ranges keeps validation at the cost it has today and leaves the page tables as the
enforcement rather than the oracle. Walking the tables on every syscall argument would put a
multi-level memory traversal on the entry path to buy an answer the kernel already knows.

**And that list must be SEEDED with the process image, or ordinary code stops working.** Today the two
static-data hooks are what admit a buffer or an out-pointer living in app globals, and under an
enforcing backend they already return false because the region set is the whole oracle. A translating
backend keeps them false -- their surviving branch compares against link-time PHYSICAL extents, which
name nothing in a process's low half -- so what admits app globals has to be the range list itself,
seeded at T6 with the mapped text as read-execute and the copied static data as read-write. Miss that
and every syscall taking a pointer into a global fails while the memory is perfectly valid and
mapped, which reads as a validation bug and is a seeding bug. T6 therefore owes two more arms: a
syscall whose INPUT buffer is a global, and one whose OUT-POINTER is a global.

Worth noting where the linker window comes from, because it is the same conflation again: the
app-data window is carved under the descriptors-exist gate, so F6's split is what makes it available
to a translating build at all.

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
that is not true today. Two endpoint sites hand a PARKED peer's address to the kernel/user helpers,
which resolve against the RUNNING space -- the payload delivery and the receive-info write -- and
both already have the peer thread in hand, so the owner is available and simply not passed. One
console syscall dereferences a user pointer directly, outside the funnel entirely, after validating
it. Under one physical space all three are correct; under F2 the first two silently read or write the
wrong process and the third has no seam to fix. Closing the leak is a precondition of T7 and not a
tidy-up after it.

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
not simply provide, and the amount of it is knowable at build time from the same window the MPU
backends already carve for app data.

**Thread stacks stop being arena blocks and become mappings.** Today an unprivileged thread's stack
is an `arch_ram_alloc` block shaped so one descriptor covers it, and it is composed into the
thread's region set at switch-in. In a process it is frames mapped at a chosen virtual address in
a DISTINCT virtual range in its task's shared address space per F9, with no power-of-two size and no
natural alignment -- and, since placement
is free, with an unmapped page below it, so a stack overrun faults instead of reaching a neighbour.
That guard page is free here and is worth taking on the first pass rather than the second.

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
switch -- and saying so narrows this seam usefully: activate has two callers, map has three, and
conflating them would have had the self-grant reloading a root it never left.

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
armv7m, also non-enforcing, also reports zero skips over the same 101 arms that board ran at M6.1. On a flat board the
enforcement arms are NOT REGISTERED rather than registered-and-skipped, so there is no list to keep
and nothing is passing vacuously. What T8 is therefore scored against is the pair of facts below it,
not a skip list:
- **one PARTIAL**, `periph_reg_write_unheld`, declared in the selftest's expected set: this chip
  names no MMIO window a driver could be granted, so the arm's free-window half finds nothing to
  take. A real coverage gap, and T8 closing it means minting a window.
- **the arms that do not register at all**, which on this board are `bus_device_slots`,
  `uart_service`, `reboot_priv` and `call_reg_fastpath`. The first three want a driver, a service
  list or `arch_reboot`; the fourth wants the IPC fastpath this arch does not implement.

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
  - **`ARCH_ASPACE_ECAPACITY` is unreachable on all three M6 backends,** every one of them being
    radix. It stays, because the caller shape it forces is the reason to distinguish it and a
    hashed-table backend cannot be added later without it, but it is untestable rather than untested
    and no coverage gate may demand an arm for it.

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

**T5. The domain backend field, the dedup deleted, and possession made explicit.** The region array
becomes the opaque handle behind the accessors, the reuse arm in `domain_for` goes and the
default-user singleton becomes a template (F2), and possession stops being a walk over the caller's
reachable regions and becomes thread-local authority (F9). That last one is a PRECONDITION of the
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
none is a silent claim. **T6 owns F10's allocator and is where the refusal becomes a map**, and T8
owns the hostile witness.

*Also owed here:* the driver-service bring-up flow CONFIGURES and runs, and the `domain_share` arm is
re-described per F2 as the two-handoff witness it becomes. **The same-address READBACK is not owed
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

*Plus the sibling-sharing witness F2 assigns here*, one task create with two members, which is the
arm `domain_share` was mistaken for and which only becomes observable once two members share a mapped
image.

*Plus the two arms section 3.3 assigns here*, which pin the granted-range list being SEEDED with the
image rather than inheriting an oracle that no longer answers: a syscall whose input buffer is an app
global, and one whose out-pointer is. Both are ordinary code today, and both fail on a translating
backend if the seeding is missed -- presenting as a validation bug when it is a seeding bug.

**T7. The access seams, page-split, owner-carrying, and cross-address-space.** The two kernel-user
helpers and the endpoint copy, per section 3.3, plus the three sites that bypass or mis-scope the
funnel today.
*Expected:* three arms, and the second is the one no existing arm can stand in for. A validated range
spanning two non-adjacent frames copies correctly, which catches a helper still written as one
`memcpy` over a translated base. Two processes at the SAME virtual addresses exchange a message and
its receive-info correctly, which catches both the dead overlap assert and any site still resolving a
parked peer against the running space. And an endpoint call and reply crosses two processes.

**T8. The third memory-model arm, the enforcement split, and the denial witnesses.** This is where F5
is discharged -- the existing fault-isolation path decodes a translation fault and contains it, and
nothing populates a mapping in a handler. **F6's split is NOT what lands here** -- it is a dependency
of T5, per F6 itself, since T5 must not configure an MMU board down the permissive path. What T8 owes
is the WITNESS that the split holds, which is a different thing from the split.
*Expected:* two denials, and the second is the one a permissive gate would let through. An
unprivileged thread touching a virtual address its space does not map is reported and killed, and the
system survives. And an authorized unprivileged caller self-granting a HIGH-HALF kernel address is
REFUSED by name rather than merely failing to fault -- which is the arm that fails if the MMU board
inherited the no-protection admission path. Then diff the skip list against S9's: the arms that MOVED are the
enforcement this stage bought, and any arm still skipping owes a reason in the record.

**T8b. Teardown, and the churn that proves it.** The aspace destroy contract of section 3.1: free the
root, its tables and every frame the process held, invalidate before the slot or identifier is
reused, and unwind completely on every failure path.
*Expected:* repeated create-and-exit and forced-failure churn returns the frame and root counts to
where they started. A build with no destroy path at all passes every earlier T-step, so nothing before
this one can stand in for it.

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
  - **OPEN, and the one thing in this family still unfrozen.** The rendezvous needs to know which
    cores have the space active, which is knowledge the seam does not currently carry. Either
    `unmap` gains a core-set parameter, which is a signature change and puts a multicore concept in
    every backend's face at one core, or the opaque address space carries an active-core set the
    kernel can read, which keeps the signature but weakens the handle's opacity. The spike
    recommends the second. Nothing in M6 needs the answer, `arch_aspace` being opaque, so adding
    the field later costs nothing above the seam; the parameter would not be free later, which is
    why the choice is named here rather than left to M7 to discover.

*Also corrected by that spike, in `docs/design-m7-smp.md` rather than here:* two claims that do not
hold. The blocking all-core rendezvous is not owed by the milestone that adds the second core on
A64, the data-side rendezvous being the hardware's; what IS owed there is narrower and was unnamed,
`ISB` not being broadcast, so a change to an EXECUTABLE mapping still needs the far side poked. And
`sbi_remote_sfence_vma` is not promised synchronous by its specification, where success means the
request was sent; one implementation happening to block is not a contract. **Plus the data-cache clean and invalidate seam, which `roadmap.md` lists as an M6
deliverable.** The seam lands here; what stays open is who calls it, which is section 7's entry.
*Expected:* `tests/static/check_cpu_id_fold.sh` passes with `KICKOS_NUM_CORES` at 1, so the core
identity folds to a literal and there is no fallback path to drift out of date.

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
F10 contracts at handover time, whether that handover is an explicit task create or an implicit
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
  image sits at addresses shared across processes, and a reservation's address is system-wide unique
  so the handoff can reproduce it. What is still open is the layout of everything else, and whether
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
