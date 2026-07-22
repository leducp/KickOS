<!-- SPDX-License-Identifier: CECILL-C -->
# EXPLORATORY: the MMU / new-platform horizon (post-M6)

> Status: EXPLORATION ONLY. This is a research + design-thinking spike, NOT a
> contract and NOT a milestone plan. Nothing here is implemented; nothing here
> licenses a code change. The only actionable output is section 5 (QUICK WINS),
> and even those are PROPOSALS to be scheduled, never done from this doc.
>
> Scope: what it would take to grow KickOS from an MPU RTOS (one physical
> address space, per-thread region sets) into (a) an MMU OS with real virtual
> address spaces -- concretely an x86_64 PC target -- and (b) a heterogeneous
> AMP system (MMU cores + MPU cores in one product, i.MX8MP). Grounded in the
> live code so the seam analysis is real, not generic.

Conventions in the code sketches: ASCII only, spelled `and`/`or`/`not`, no
ternary, traditional include guards. Sketches are illustrative, not buildable.

---

## 1. The current model + its single-physical-space assumptions

KickOS today has exactly ONE physical address space. Every address a thread
names is a physical address; isolation is achieved by an MPU withholding access
to physical ranges the thread was not granted, NOT by giving each thread its own
view of memory. This one fact is baked into five places:

- **Domain == an MPU region set.** `struct Domain`
  (`kernel/include/kickos/domain.h:30-40`) is literally
  `arch_mpu_region regions[KICKOS_MPU_MAX_REGIONS]` + `region_count` + a
  `privileged` posture + refcount. A Domain is "the set of physical windows the
  threads in it may touch". `domain_for` (`kernel/domain/domain.cc:72-135`)
  dedups domains by matching `regions[0].base == base` -- i.e. by PHYSICAL base
  address. Two threads granted the same physical block share a domain; that is
  only meaningful because base addresses are global.

- **`arch_mpu_apply` replaces the active region set on switch-in.**
  (`arch/include/kickos/arch/arch.h:122`.) The switch path composes
  static-code/data regions + domain regions + the private stack region and hands
  the whole list to the MPU (`thread.cc`). There is no address translation --
  the region descriptors carry physical bases straight to the hardware
  (ARM PMSA RBAR, RISC-V PMP pmpaddr, K64F SYSMPU). The MPU answers one question
  ("may this core touch physical address X now?"), never "what physical address
  does virtual X map to?".

- **`arch_ram_alloc` is a physical bump allocator over one arena.**
  (`arch.h:191-193`.) `arch_ram_base()`/`arch_ram_size()` define a single
  contiguous MPU-governed pool; `arch_ram_alloc` hands out naturally-aligned,
  power-of-two-sized blocks so each is coverable by ONE MPU descriptor
  (`arch_ram_region_size`, `arch.h:149-171`). The pow2-size + natural-alignment
  discipline exists ONLY because an MPU region must be so shaped -- it is a
  physical-allocator constraint that a paging allocator would not have.

- **Pointer validation is a physical range containment test.**
  `user_range_ok` (`kernel/syscall/syscall.cc:127-161`) walks the current
  thread's `arch_mpu_region` set and accepts `[ptr, ptr+len)` iff it lies inside
  one granted region with the needed rights. `user_readable_ok`
  (`syscall.cc:172-179`) adds `arch_user_text_readable` for app code/rodata the
  backend does not model as a region. Both treat the user pointer as a PHYSICAL
  address the kernel can dereference directly once the range check passes -- no
  per-address-space translation, no copy_from_user walk.

- **The object namespace is per-task handle tables, backing per-KERNEL object
  pools.** (`kernel/syscall/cap.cc`, book `handles-and-the-resolve-chokepoint`.)
  Nuance worth stating plainly: caps do NOT resolve "against the Domain" in the
  code -- a handle is an index into the calling THREAD's table, resolving to an
  object in the single per-`Kernel` SlotPool (`instance.h`: `sems`, `mutexes`,
  `threads`, `domains`). This layer is address-space-agnostic ALREADY: an
  integer handle -> object slot has nothing to do with physical vs virtual. That
  is the good news for section 2.

Plus the AMP groundwork, which is the OTHER axis: `KICKOS_MULTI_INSTANCE`
(`instance.h:89-96`) already lets several `Kernel` structs co-reside, each
reached through a per-thread pointer. Today that is the sim's multi-slave trick;
it is also the hook a per-core-cluster kernel would hang on (sections 4, 5).

Baseline in one line: **isolation = withhold physical ranges; naming = physical
address for memory, integer handle for objects.** The MMU era keeps the second
half and rewrites the first.

---

## 2. What an MMU / VMSA changes

An MMU inserts a translation between the address a thread names (virtual) and the
address that hits the bus (physical), via per-address-space page tables walked by
hardware (ARMv8-A VMSA; x86_64 4-level paging). Two consequences dominate:

1. Each address space is a SEPARATE page-table tree. The same virtual address in
   two spaces maps to different physical frames (or to nothing). Isolation stops
   being "withhold ranges from a shared space" and becomes "you simply cannot
   name what is not in your table".
2. Allocation decouples from placement. Virtual layout is chosen freely; physical
   frames are scattered 4K pages stitched under the chosen virtual addresses. The
   pow2/natural-alignment MPU discipline evaporates.

Mapped onto the KickOS seams:

**Domain becomes a page-table root instead of a region array.** This is the
cleanest mapping and the reason the Domain seam is the whole spike's fulcrum. A
Domain already means "the memory a set of threads may touch, refcounted, joined
by threads, freed at last exit". Under an MMU it means exactly that -- the field
`arch_mpu_region regions[]` is replaced by an opaque `arch_aspace* aspace` (an
`arch_context`-style opaque handle owned by the arch, holding the top-level page
table physical address to load into TTBR0 / CR3). Everything ABOVE the seam --
`domain_ref`/`domain_release` lifecycle, "threads sharing memory share a domain",
the cap layer resolving handles to objects -- is untouched, because none of it
reads `regions[]` except `domain_for`'s dedup and the switch-time compose. This
is the seL4 shape: a VSpace is a first-class object a task holds a capability to;
mapping a frame is a cap operation on it (seL4 manual; docs.sel4.systems Mapping
tutorial). KickOS's Domain is a coarse VSpace-analog already.

**What genuinely must change BELOW the seam:**

- `arch_mpu_apply(regions, n)` -> `arch_aspace_activate(aspace)`: switch-in loads
  a page-table root (write TTBR0_EL1 + ASID / CR3) instead of reprogramming N
  region descriptors. Cheaper per switch (one CR3 write vs N RBAR writes) but now
  carries a TLB-shootdown obligation on SMP.
- `arch_ram_alloc` (one pow2 physical block) -> a two-layer allocator: a physical
  FRAME allocator (4K/2M frames, no alignment cleverness) + per-aspace VIRTUAL
  range bookkeeping + a `map(aspace, va, pa, rights)` page-table editor. This is
  the single biggest new subsystem. The frozen `arch_ram_region_size` /
  `arch_ram_region_align` helpers become no-ops (page granularity is uniform).
- `user_range_ok` -> a page-table walk in the target aspace (the `copy_from_user`
  problem): the kernel can no longer just dereference a validated user pointer,
  because a user VA is not valid in the kernel's aspace. Either the kernel maps
  itself into every aspace at a fixed high half (the Linux/x86_64 model, keeps
  kernel pointers usable) or it walks the user page table to translate then
  accesses via the physical/kernel window. `arch_user_text_readable` folds into
  the same walk.
- `arch_context_init` grows an aspace association; the initial frame must start in
  the right translation regime (x86 ring3 with CR3 already the task's; ARM EL0
  with TTBR0 set).

**What does NOT map cleanly (be honest):**

- **The pow2/aligned arena assumption is load-bearing in more than the
  allocator.** `domain_for` dedups on `(base,size)` with `size == rounded MPU
  size`; MMIO grants assert exact encodability (`domain.cc:122-134`). Under
  paging, "the same block" is a set of frames + a VA, not a base/size pair --
  the dedup key changes shape, and the MMIO-never-rounded invariant becomes
  "map exactly these device pages", a different check.
- **`arch_mpu_region` is a FROZEN seam** (arch.h comment: "must fit with no
  signature changes", RX72M litmus test). An MMU does NOT fit the region seam --
  it needs a translation seam alongside it. So the honest verdict is: the arch
  seam GROWS a parallel aspace API; it does not reinterpret the MPU one. An MPU
  backend keeps `arch_mpu_apply`; an MMU backend implements `arch_aspace_*` and
  stubs `arch_mpu_apply` (or the two are one `#if`-selected family). Trying to
  make one call serve both is the trap.
- **`user_range_ok`'s "privileged caller bypasses" shortcut** (`syscall.cc:134`)
  assumes kernel and user share a space. With a split high-half kernel it still
  holds; with fully separate spaces it needs a translate-first path even for the
  kernel. Minor but real.
- **Wild-pointer semantics differ.** Today an out-of-region access FAULTS the
  physical MPU; under paging an unmapped VA faults the page-fault handler, which
  a real OS might service (demand paging, COW). KickOS would almost certainly
  keep fault == kill (no swap, no overcommit) -- worth stating so nobody assumes
  demand paging comes for free.

Net: the object/cap/IPC layer and the Domain lifecycle survive intact; the
region-set -> page-table swap, the physical-bump -> frame+map allocator, and the
pointer-validation -> translation walk are the three genuine rewrites, all
BELOW the arch seam.

---

## 3. x86_64 PC target -- "KickOS as a real PC OS"

Feasibility: plausible and a good MMU proving ground, because x86_64 is the
best-documented paging target and QEMU makes silicon-free iteration trivial.
This would be KickOS's FIRST non-embedded, first-MMU, first-ring-based arch.

**QEMU-first path.** `qemu-system-x86_64 -serial stdio -m ...` with a multiboot2
or a small hand-rolled stage; console is COM1 (already the "minimal debug
console" seam, `arch_console_write*`). No new hardware to buy; the sim-first
discipline KickOS already lives by carries over. Boot a multiboot2 ELF via QEMU's
`-kernel` to skip writing a disk bootloader initially.

**Boot model (the genuinely new arch surface).** Coming up on x86_64 is a
sequence KickOS has never needed:
- Land in 32-bit protected mode (multiboot2 hands you this), build a minimal GDT
  with a 64-bit code + data segment (x86 needs a GDT even without segmentation).
- Build initial page tables: identity-map low physical for the transition, plus
  the kernel high-half, using 2M pages to keep the boot tables tiny (PML4 ->
  PDPT -> PD). Set CR4.PAE, EFER.LME (MSR 0xC000_0080), then enable CR0.PG+PE to
  activate long mode (OSDev "Setting Up Long Mode"; phil-opp "Entering Long
  Mode"; ringzeroandlower x86-64 boot).
- Load a real IDT (a zero-length IDT triple-faults on any interrupt -- the OSDev
  note). This is the first arch where the exception/interrupt VECTOR TABLE is a
  rich per-vector structure, not KickOS's thin mask/unmask/raise triad.

**ring0/3 vs privileged/unprivileged.** Maps almost 1:1 onto the existing
posture bit: `arch_context_init(..., int privileged)` already selects
kernel-vs-user posture; on x86_64 privileged==ring0, unprivileged==ring3. The
syscall trap seam (`arch_syscall` / `syscall_dispatch`, arch.h:225-244) maps to
the `syscall`/`sysret` instruction pair + MSR_STAR/LSTAR setup. The CONTRACT
that dispatch runs in privileged THREAD context (not handler context) is
naturally satisfied by `syscall` (it does not switch stacks by itself the way an
interrupt gate does; the kernel sets up the stack) -- a good fit for KickOS's
"blocking syscall blocks by ordinary context switch" rule.

**Paging as the MMU backend.** This is where section 2's `arch_aspace_*` lands
first: CR3 == the aspace root, `map()` edits the 4-level table, TLB invalidation
via `invlpg` / CR3 reload. A Domain -> one PML4.

**APIC/interrupt model vs the arch IRQ seam.** The `arch_irq_mask/unmask/inject`
triad (arch.h:246-266) is deliberately thin, which HELPS: the Local APIC + IOAPIC
+ (optionally) the APIC timer back onto it. `arch_timer_arm` -> APIC timer
one-shot (TSC-deadline mode is the clean tickless fit). `arch_clock_now` -> TSC
(with the same monotonic-clock discipline KickOS already fought for on other
arches; see the DWT/CCOUNT clock history). The thin seam was designed for exactly
this ("earned per-chip at M1 against real silicon") -- the APIC's priority/EOI
richness stays inside arch/x86_64, invisible above.

**New arch surface vs already-abstracted.** Already covered by the seam: switch,
crit-section (CLI/STI or just IF via `arch_irq_save`), timer, syscall, console,
IRQ triad, idle (HLT). GENUINELY new: GDT/IDT/TSS setup, long-mode boot, the
`arch_aspace_*` translation family (shared with any MMU arch), `syscall`/`sysret`
MSR plumbing, and per-CPU state via GS-base (the x86 analog of the per-core
globals design-multicore.md calls out).

**Rough effort shape.** Medium-large but well-trodden. Order of magnitude: boot
+ long mode + GDT/IDT (small, copy-the-wiki), thin IRQ/timer/console/switch
(small, it is the existing seam), the aspace/paging allocator (LARGE -- the
section-2 rewrite, shared with all future MMU targets so it pays for itself).
A single-address-space "flat" x86_64 port (physical == virtual, identity map,
MPU-emulated-via-page-permissions) is a much smaller stepping stone that proves
boot/switch/IRQ without the allocator rewrite -- worth considering as the true
first milestone.

---

## 4. i.MX8MP heterogeneous AMP -- MMU KickOS (A53) + MPU KickOS (M7)

The i.MX8MP is 4x Cortex-A53 (ARMv8-A, VMSA/MMU) + 1x Cortex-M7 (ARMv7-M, MPU) in
one SoC. The vision: an MMU KickOS instance on the A53 cluster alongside an MPU
KickOS instance on the M7, one kernel per core-cluster, talking over cross-core
IPC. This is the culmination of BOTH axes -- the MMU work (section 2/3) AND the
AMP work (design-multicore / design-multicore-ipc) -- meeting on one board.

**It extends the M4 SPSC-ring + doorbell model from homogeneous to
heterogeneous.** The M4 design (design-multicore-ipc) is: two SPSC rings per
channel (one per direction) in a shared-SRAM window, DMB-ordered index publish,
a doorbell interrupt to wake the peer's local `recv`. That entire contract
survives the A53/M7 asymmetry with three added concerns:

- **Shared-SRAM window, now across a translation boundary.** On RP2040 both cores
  see the ring at the same physical address. On i.MX8MP the M7 sees the OCRAM/SRAM
  window at a physical address; the A53 sees it through its MMU at whatever VA the
  A53 kernel maps it to. So the ring's cross-core pointers CANNOT be virtual --
  the shared region must be described in PHYSICAL terms and each side maps/knows
  it locally. The ring struct must hold offsets or physical addresses, never a VA
  from one side's aspace. This is a NEW invariant the homogeneous design did not
  need (IPC-4 "shared window is the only cross-writable memory" now also means
  "shared window is the only address both sides agree on").
- **Cache coherence is not automatic across the A53/M7 boundary.** The A53s are
  cache-coherent with each other; the M7 is generally NOT in the same coherency
  domain for the shared window. So the DMB-orders-publish rule (IPC-1) must be
  reinforced with explicit cache maintenance (clean before publish, invalidate
  before read) OR the shared window must be mapped non-cacheable on at least one
  side. This is the single biggest heterogeneous delta vs the homogeneous
  M0+/M33 rings. (NXP/OpenAMP put the vrings in non-cached SRAM for exactly this
  reason.)
- **Doorbell == the Messaging Unit (MU), not the SIO FIFO.** i.MX8MP's cross-core
  notify is the MU peripheral's interrupt (or a shared GIC SPI), the direct analog
  of RP2040's SIO FIFO doorbell. It backs onto the SAME `arch_irq_*` seam +
  `kickos_isr_irq` callback the homogeneous design uses (IPC-3: recv parks on its
  OWN core's queue, woken by its OWN core's ISR). Only the peripheral behind the
  seam changes.

**Who-boots-whom.** Standard i.MX8MP flow: the boot ROM + U-Boot (or the A53 SPL)
starts, and the A53 side releases the M7 from reset (loads the M7 image to TCM,
kicks it). So the A53 KickOS is the "master" that brings up the M7 KickOS -- the
inverse is possible (M7-first for fast boot) but A53-first is the grain of the
silicon. This is a new bring-up sequence but a well-documented one (Toradex/NXP
community; Embedded Artists heterogeneous notes).

**RPMsg comparison.** The industry-standard here is RPMsg/OpenAMP: VirtIO vrings
in shared memory + MU doorbell (rt-rk; NXP community). KickOS's own SPSC+doorbell
IS a lean RPMsg-shaped design without the VirtIO/VirtIO-vring/name-service
overhead. The strategic call: KickOS can either (a) keep its own minimal ring
contract on BOTH sides (full control, both ends are KickOS -- the likely choice),
or (b) speak RPMsg on the wire so a KickOS M7 could talk to a Linux A53 (interop,
but drags in VirtIO). Worth a decision doc when it is real; for a
KickOS-on-both-sides product, (a) is the grain of the existing M4 design.

**The shared IPC contract across the asymmetry** is therefore: a physically-
addressed, cache-maintenance-disciplined, MU-doorbelled SPSC ring pair -- the M4
contract with (1) physical (not virtual) shared addressing, (2) explicit cache
ops or non-cacheable mapping, (3) MU instead of SIO. The API/syscall seam above
it (send/recv on an endpoint handle) is IDENTICAL on both sides -- which is the
payoff of keeping the cap/IPC layer address-space-agnostic (section 1's good
news).

---

## 5. QUICK WINS -- FLAG ONLY (proposals to schedule during M3/M4, NOT done here)

These are cheap seam/groundwork changes worth making WHILE M3/M4 code is being
written, so the MMU era does not force a breaking rewrite. Each is a PROPOSAL;
none is implemented in this spike. Ordered by leverage.

**QW-1. Give `Domain` an opaque backend field instead of a bare region array --
or at least route ALL region access through accessors.**
What: today `struct Domain` exposes `arch_mpu_region regions[]` directly, and
callers (`domain_for` dedup, `thread.cc` compose, and any future reader) touch it
as a raw array. The reshape: keep it MPU-only in BEHAVIOR, but funnel every read
through a small accessor surface (`domain_regions(d, &n)` / a `domain_backend(d)`
opaque) so the field can later become `arch_aspace*` without touching callers.
Why cheap now / expensive later: there are only a handful of readers today (M3).
Once caps, IPC endpoints, MMIO grants, and the console-handover work all read
`regions[]` directly, swapping the representation touches every one of them under
pressure. A one-file accessor now contains the blast radius of section 2's single
biggest below-seam change.
PROPOSAL -- schedule, do not implement here.

**QW-2. Name the "physical == the address you dereference" assumption in
`user_range_ok` / `user_readable_ok` with a single choke helper.**
What: the two validators (`syscall.cc:127-179`) both assume a validated user
pointer is directly dereferenceable by the kernel. Introduce (design-only) the
notion that "validate" and "access" are two steps, even if today they collapse
(a `kaccess_from_user(ptr, ...)` that is currently an identity pass-through). Do
NOT build translation -- just stop scattering raw `reinterpret_cast<T*>(user_ptr)`
dereferences through the syscall handlers, routing them through one helper.
Why cheap now / expensive later: the copy_from_user rewrite (section 2) is the
nastiest MMU change because a user VA stops being kernel-dereferenceable. If every
kernel-side user-pointer dereference already goes through ONE helper, that becomes
one function to implement per arch; if not, it is a hunt across every syscall.
PROPOSAL -- schedule, do not implement here.

**QW-3. Keep the shared-IPC ring contract PHYSICALLY addressed from day one.**
What: when the M5 IPC ring lands (design-multicore-ipc), specify that ring
control words and slot references are offsets / physical addresses, NEVER a
pointer valid in one core's space -- even though on RP2040 (homogeneous, one
physical space) a raw pointer would work fine.
Why cheap now / expensive later: it costs nothing on RP2040 (physical == virtual)
but is the exact property section 4 needs for A53/M7. Baking a VA into the ring on
the homogeneous prototype would silently work until the first MMU peer, then break
the wire format. Getting the invariant into the M5 design text is free; retrofitting
it after apps depend on the layout is not.
PROPOSAL -- schedule (fold into the M4 IPC design), do not implement here.

**QW-4. Isolate the pow2/natural-alignment MPU shaping so a page allocator can
sit beside the bump allocator.**
What: `arch_ram_region_size`/`arch_ram_region_align` (arch.h:149-184) encode
MPU-descriptor geometry into the ALLOCATOR. Flag (design-only) that these are
"MPU shaping" and belong behind the same arch family switch that would later
select frame allocation. No behavior change -- just do not let new M3/M4 callers
assume "allocation size is always the MPU-rounded size" outside the allocator.
Why cheap now / expensive later: today the pow2 assumption leaks (`domain_for`
dedups on the rounded size). Each new leak is another site the frame allocator
must reconcile. Containing the assumption to the allocator now keeps section 2's
allocator rewrite local.
PROPOSAL -- schedule, do not implement here.

**QW-5. Confirm the cap/handle layer stays address-space-agnostic -- and keep it
that way.**
What: this is a "do no harm" flag, not a change. The per-task handle -> per-kernel
object-pool model (cap.cc) is ALREADY MMU-clean (section 1). The quick win is a
review rule: no cap/endpoint code introduced in M3 should start keying on a
physical address or a region base (the way `domain_for` does). Keep object naming
purely by handle/slot, never by address.
Why cheap now / expensive later: free (it is the current design). But endpoint IPC
(M3 #4) is exactly the kind of code tempted to stash a shared-buffer physical
address in a cap -- which would drag address-space assumptions into the one layer
section 2 relies on being clean.
PROPOSAL -- adopt as an M3 review rule, do not implement here.

**QW-6. Reserve an `arch_aspace`-shaped hole in the arch seam doc (not the code).**
What: the arch.h header prose already frames the seam as "concepts, never
mechanisms" and freezes `arch_mpu_region`. Add a design NOTE (doc-only) that the
MMU era introduces a PARALLEL `arch_aspace_*` family rather than reinterpreting
the MPU seam (section 2's honest verdict), so a future porter does not try to
overload `arch_mpu_apply` to mean "load a page table".
Why cheap now / expensive later: a sentence of foresight prevents a wrong-shaped
first MMU port. Free now.
PROPOSAL -- fold into arch.h prose / the Book when convenient, do not implement here.

---

## 6. Open questions + what to research deeper before any implementation milestone

- **Kernel address model on MMU: high-half shared vs fully-separate spaces?**
  High-half (kernel mapped into every aspace, Linux/x86_64 style) keeps kernel
  pointers usable and makes copy_from_user a same-CR3 walk -- but weakens
  isolation (Meltdown-class). Fully-separate is stronger but forces a translate
  step on every user access. This choice drives QW-2's shape. Research: how seL4
  (fully separate, no kernel heap in user spaces) vs a monolith differ, and which
  fits KickOS's "fault == kill, no swap" posture.

- **Does the Domain -> VSpace mapping stay 1:1, or does an MMU want finer objects?**
  seL4 splits VSpace into page-directory/page-table/frame CAPS. KickOS's Domain is
  coarser (one region set / one aspace). Is coarse enough for the target products,
  or does the cap layer eventually need frame-level caps? Decide before the aspace
  API freezes.

- **TLB shootdown on SMP-MMU.** Section 2 notes `arch_aspace_activate` carries a
  shootdown obligation the MPU never had. This collides with the design-multicore
  BKL/SMP work. Research the interaction: is AMP-only (per-core-cluster kernels,
  no shared aspace) enough to dodge shootdown entirely for the first MMU product?
  (i.MX8MP AMP suggests yes.)

- **Cache coherence contract for heterogeneous IPC (section 4).** Non-cacheable
  shared window vs explicit clean/invalidate: measure the latency cost on real
  i.MX8MP silicon before committing. Needs the arch to expose a cache-maintenance
  seam KickOS does not have today.

- **x86_64 first milestone granularity.** Flat single-address-space x86_64
  (identity map, no per-process spaces) as a boot/switch/IRQ proving stepping
  stone, THEN the aspace allocator -- or go straight to paged multi-space? The
  former de-risks the boot/long-mode/GDT/IDT novelty separately from the
  allocator rewrite.

- **RPMsg interop: needed, or a distraction?** If a KickOS-on-both-sides product
  is the only target, the lean SPSC ring wins (section 4). If a KickOS M7 must
  ever talk to a Linux A53, RPMsg/VirtIO becomes mandatory. Pin the product
  requirement before designing the wire format.

- **Boot chain ownership on i.MX8MP.** A53-first (U-Boot releases M7) vs M7-first
  (fast RTOS boot, A53 later). Affects who owns the shared-SRAM window init and
  the doorbell setup order. Research the NXP boot ROM contract in detail.

---

### Sources (external grounding, briefly)

- seL4 VSpace / capability memory model, MPU-vs-MMU distinction: seL4 Reference
  Manual (sel4.systems/Info/Docs/seL4-manual-latest.pdf), docs.sel4.systems
  Mapping tutorial; "From MMU to MPU" Pip adaptation (arxiv 2301.04546).
- x86_64 long mode boot / paging / GDT / IDT: OSDev "Setting Up Long Mode" and
  "Entering Long Mode Directly"; phil-opp "Entering Long Mode"; ringzeroandlower
  "Booting from Grub2 to x86 long mode".
- i.MX8MP heterogeneous AMP, RPMsg/OpenAMP, MU doorbell, shared SRAM: rt-rk RPMsg
  inter-core communication; NXP community i.MX8MP RPMsg / shared-memory threads;
  Embedded Artists heterogeneous multi-core; Kynetics AMP notes.
- Internal: `docs/design-multicore.md`, `docs/design-multicore-ipc.md`,
  `arch/include/kickos/arch/arch.h`, `kernel/domain/*`, `kernel/syscall/*`,
  `docs/book/handles-and-the-resolve-chokepoint.md`.
