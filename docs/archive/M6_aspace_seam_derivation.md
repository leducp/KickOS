<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# M6 address-space seam derivation

Extracted from F8 of `docs/design-m6-mmu.md`. This is the dated cross-backend argument; the live F8 states the contract.

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
originate it. **And the baseline does NOT move:** the aspace seam differ measured
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
