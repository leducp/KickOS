<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
# 10. The same walk with an MMU: `qemu-arm64`

This chapter runs the journey again on `qemu-arm64`: a Cortex-A53 on QEMU's `virt` machine.
It is told as a difference from the reference walk on the Cortex-M4F. The stops are the same:
power-on ([chapter 1](01-power-on.md)), the first syscall ([chapter 2](02-first-syscall.md)) and
shared memory ([chapter 6](06-shared-memory.md)). Here we look at what changes when the part has
an MMU instead of an MPU. By the end you will have seen:

- the boot at EL1 with the MMU off, the boot page tables, and the moment translation turns on;
- the split between a kernel half that every address space shares and a user half that belongs
  to one task;
- one address space per task: the frames it is built from, the page-table walker that edits it,
  and what one map and one unmap do to it;
- the exception vector and the SVC path at EL1;
- how the kernel copies to and from user memory without touching the user's addresses;
- the context switch writing a new root (`TTBR0_EL1`) tagged with an ASID;
- the TLB maintenance that keeps the translation caches honest.

One thread runs through all of it. KickOS has ONE address-space abstraction over two kinds of
protection hardware. A region MPU draws protection in ranges, and page tables draw it in
translation. The kernel above the arch layer makes the same calls on both. At several stops below
we put one kernel call side by side and show where it lands on each backend.

The concept is in the Book: [The machine underneath](../book/the-machine-underneath.md) explains
what an MMU is and why most of the fleet has an MPU instead, and
[Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md)
ties the protection state to the context switch. The contract for a memory domain is in the
Reference, [Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit).

## Words this chapter uses

- **Exception level (EL).** AArch64's privilege levels. EL0 is user code, EL1 is the kernel,
  EL2 a hypervisor, EL3 the secure monitor. KickOS runs its kernel at EL1 and every task at EL0.
- **Virtual address (VA), physical address (PA).** With the MMU on, every address the CPU issues
  is virtual. The MMU translates it to the physical address on the bus.
- **Page table, descriptor, granule.** A page table is an array of 512 eight-byte
  **descriptors**. Each descriptor either points to the next table, or maps a block of memory
  (a **leaf**), or is invalid. KickOS uses a 4 KiB **granule**: the smallest page is 4 KiB.
- **Walk.** The hardware reads the tables, one level after another, to translate a VA. With a
  39-bit address space the walk starts at level 1 (each entry covers 1 GiB), goes through level 2
  (2 MiB per entry) and ends at level 3 (4 KiB per entry).
- **TTBR0_EL1, TTBR1_EL1.** The two root registers. TTBR0 translates the low half of the
  address space and TTBR1 the high half.
- **TLB.** The translation lookaside buffer: a cache of recent walks. When the kernel changes a
  table it must also drop stale TLB entries.
- **ASID.** An address-space identifier: a tag on TLB entries, so entries of two spaces can
  sit in the TLB at once without being confused.

## The images we use

Two images from the `qemu-arm64` build:

- **`hello`** (`user/apps/common/hello/main.cc`). Two threads, `ping` and `pong`, play
  ping-pong. It is the simplest boot to userspace, and we use it for the boot, the first
  address space, the first EL0 instruction and the first syscall.
- **`selftest`** (`user/apps/common/selftest/`). Its arms map and unmap frames and create and
  destroy tasks. We use it for the map, unmap, ASID and TLB stops.

Launch one stopped, as [Setup](00-setup.md) explains, and attach gdb:

```
qemu-system-aarch64 -M virt -cpu cortex-a53 -nic none -display none -serial stdio \
    -semihosting -kernel build/qemu-arm64/user/apps/common/hello/hello \
    -gdb tcp::1234 -S &
gdb-multiarch build/qemu-arm64/user/apps/common/hello/hello \
    -ex 'target remote :1234'
```

Both `-cpu cortex-a53` and `-nic none` are needed; [boards.md](../reference/boards.md) says why.
QEMU's gdb stub shows the system registers by name, so `info registers TTBR0_EL1 SCTLR_EL1` works.
There is no `CurrentEL` register in gdb, but the current level is bits [3:2] of `$cpsr`.

The addresses in the outputs below come from one build. Yours may differ by a few pages; the
shapes will not.

One helper makes the page-table stops readable. Paste it into gdb (or a file you `source`). It
walks the CURRENT user root, the one in `TTBR0_EL1`, for one virtual address. It assumes the
root is a task's own, which holds only tables and 4 KiB pages:

```
define uwalk
  set $kva = 0xffffff8000000000
  set $tbl = ($TTBR0_EL1 & 0x0000FFFFFFFFF000) + $kva
  set $va = (unsigned long long)$arg0
  set $lvl = 1
  set $shift = 30
  while $lvl <= 3
    set $idx = ($va >> $shift) & 511
    set $desc = *(unsigned long long*)($tbl + $idx * 8)
    printf "L%d[%d] = 0x%016llx\n", $lvl, (int)$idx, $desc
    if ($desc & 1) == 0
      set $lvl = 4
    else
      set $tbl = ($desc & 0x0000FFFFFFFFF000) + $kva
      set $lvl = $lvl + 1
      set $shift = $shift - 9
    end
  end
end
```

A trap for gdb users on AArch64: do not name a convenience variable `$d1`, `$s0`, `$q2` and
so on. Those are the SIMD registers, and gdb will print a register instead of your value.

## The map: same stop, what changes

| Reference walk (Cortex-M4F, MPU) | This walk (Cortex-A53, MMU) |
|---|---|
| Reset loads SP and PC from the vector table | `_start` sets SP itself; the level it entered at is checked |
| One flat physical address space | Two halves: kernel high (TTBR1), user low (TTBR0, per task) |
| The kernel reaches all RAM through the privileged background region | The kernel reaches all RAM through a direct map in the high half |
| A thread's region set, encoded and stashed | A task's page-table root |
| The region set is committed in `PendSV_Handler` | `TTBR0_EL1` is written in `switch_book` |
| `SVC_Handler` and `svc_trampoline` | Vector slot 0x400, `kickos_armv8a_el0_sync_entry` |
| The kernel copies with a plain `memcpy` | The kernel copies through `arch_aspace_acquire`, a kernel alias |
| A self-grant adds a region for the calling thread | A self-grant maps pages for the whole task |

### Stop 1: entered at EL1, the MMU off
**Where:** `arch/arm64/chip/virt_arm64/startup.S`, `_start`; `arch/arm64/common/boot_el1.inc`,
`KICKOS_ARM64_BOOT_ENTRY`; the chip's `kickos_arm64_bad_el`

QEMU's `-kernel` loads the ELF into DRAM at `0x4000_0000` and jumps to its entry point with the
MMU and the caches off. On the Cortex-M the hardware read the first two words of the vector table
into SP and PC. AArch64 does not do that: there is no SP in any table, so the first instructions
set SP by hand.

The first instruction loads `__kickos_arm64_va_base` into `x9`. That is the offset between a
kernel virtual address and its physical address (the kernel is linked high, see Stop 3). Until
translation is on, every address the code computes from a kernel symbol must have `x9`
subtracted, or it points into a half that does not exist yet. That is why the stack pointer is
loaded as `_estack - x9`.

Then it reads `CurrentEL`. Which level QEMU hands over at is a property of the QEMU command line:
plain `-M virt` hands over at EL1, `virtualization=on` at EL2, `secure=on` at EL3. This port is an
EL1 kernel and refuses the other two by name, through `kickos_arm64_bad_el`. The silicon twin
`imx8mp` (`arch/arm64/chip/imx8mp/startup.S`) is where the chip layer differs: that part hands
over at EL3, so its `_start` programs the EL3 and EL2 registers a lower level cannot reach and
then `eret`s down to EL1. Both chips meet at `KICKOS_ARM64_BOOT_EL1`.

**Try it:** you are already stopped at `_start`. `info registers pc cpsr SCTLR_EL1`, then
`p/x ($cpsr >> 2) & 3`, then `stepi` and `info registers x9`. You should see:

```
pc             0x40000000          0x40000000 <_start>
cpsr           0x400003c5          [ SP EL=1 F I A D BTYPE=0 Z ]
SCTLR_EL1      0xc50838            12912696
$1 = 0x1
x9             0xffffff8000000000  -549755813888
```

EL is 1, and bit 0 of `SCTLR_EL1` (the M bit, MMU enable) is clear. To see the refusal, run the
image without gdb under `-M virt,virtualization=on`: it prints
`KickOS: qemu-arm64 entered at EL2, and this port implements EL1 only` and exits.

**Why, and the contract:** [Book: Interrupts and traps](../book/interrupts-and-traps.md),
[Reference: boards.md](../reference/boards.md) (the `qemu-arm64` entry)

### Stop 2: the boot page tables
**Where:** `arch/arm64/common/boot_tables.inc`, `kickos_arm64_ident_table`,
`kickos_arm64_kernel_table`, `kickos_arm64_ram_table`, `kickos_arm64_kernel_ram_table`,
the `L2_PARTITION_TABLE` macro; `arch/arm64/common/vmsa.h`

The MMU has to come on before any C runs. The reason is in the comment at the top of
`startup.S`: with the MMU off, every access is Device memory, which demands natural alignment,
and the compiler copies structs with unaligned 16-byte loads. So C would fault on its first wide
copy. The first tables therefore cannot be built at run time by C. They are assembled as
constants, two roots and two level-2 tables, and linked at their physical addresses.

Each root has two valid level-1 entries. Entry 0 maps the first gigabyte (the GIC and the UART)
as one Device block. Entry 1 points to a level-2 table for the DRAM gigabyte. Every other entry is
zero, so a stray access outside those two gigabytes faults instead of reading something.

```
kickos_arm64_ident_table:
    /* EL1-only: every device touch on this port goes through a syscall. */
    .quad   BLOCK + AF + ATTR_DEVICE + UXN + PXN    /* 0x0000_0000..0x4000_0000 */
    /* A table rather than a block, for the split above. DRAM_L1_ENTRY names this slot. */
    .quad   kickos_arm64_ram_table + TABLE
    .fill   L2_ENTRIES - DRAM_L1_ENTRY - 1, 8, 0
```

The level-2 table maps DRAM in 2 MiB blocks. Block 0, where the kernel is fetched from, is
read-only and executable at EL1. The other blocks are read-write and never executable. None of
them grants anything to EL0: the AP field says "EL1 only" in every entry.

Why two roots with the same descriptors? Because they will stop being the same. The identity
root goes into `TTBR0`, where it lets the code that turns the MMU on keep fetching at the address
it is running at. It is replaced by a per-task root as soon as a task runs. The kernel root goes
into `TTBR1` and never changes: it is the half every address space shares.

**Try it:** still at `_start`, the tables sit at their physical addresses and gdb can read them:
`x/2gx &kickos_arm64_ident_table`, `x/3gx &kickos_arm64_ram_table`,
`x/3gx &kickos_arm64_kernel_ram_table`. You should see:

```
0x40001000 <kickos_arm64_ident_table>:	0x0060000000000405	0x0000000040003003
0x40003000 <kickos_arm64_ram_table>:	0x0040000040000781	0x0060000040200701
0x40003010 <kickos_arm64_ram_table+16>:	0x0060000040400701
0x40004000 <kickos_arm64_kernel_ram_table>:	0x0040000040000781	0x0060000040200701
0x40004010 <kickos_arm64_kernel_ram_table+16>:	0x0060000040400701
```

Decode `0x0040000040000781` with `vmsa.h` open: bit 0 is `BLOCK`, `0x80` is `AP_RO_EL1`,
`0x300` is `SH_INNER`, `0x400` is `AF`, bit 54 is `UXN`, and the output address is
`0x4000_0000`. The next one, `...0701`, has `AP_RW_EL1` (AP is 0) and both `UXN` and `PXN`.

**Why, and the contract:** [Book: The CPU reads ahead](../book/memory-types-and-speculative-access.md)
(why a Device type for everything not RAM), [Reference: boards.md](../reference/boards.md)

### Stop 3: turning translation on, and the jump to the high half
**Where:** `arch/arm64/common/boot_el1.inc`, `KICKOS_ARM64_BOOT_EL1`, `kickos_arm64_boot_high`;
`arch/arm64/common/vmsa.h`, `MAIR_VALUE`, `TCR_VALUE`; `arch/arm64/chip/virt_arm64/chip_virt_arm64.cc`,
`Reset_Handler`

The macro writes the translation registers in order. `MAIR_EL1` defines three memory types
(Normal cacheable, Device, Normal non-cacheable), which descriptors name by index. `TCR_EL1` sets
the shape: `T0SZ = T1SZ = 25` gives each half 39 bits, so TTBR0 covers `0` up to
`0x7F_FFFF_FFFF` and TTBR1 covers `0xFFFF_FF80_0000_0000` up to the top. It also asks for 16-bit
ASIDs. Then the two roots go into `TTBR0_EL1` and `TTBR1_EL1`, a `tlbi vmalle1` drops anything
cached, and `VBAR_EL1` gets the PHYSICAL address of the vector table, so an exception during boot
already has somewhere to go.

Then the switch itself:

```
    mrs     x0, SCTLR_EL1
    orr     x0, x0, #SCTLR_M
    orr     x0, x0, #SCTLR_C
    orr     x0, x0, #SCTLR_I
    msr     SCTLR_EL1, x0
    isb                             /* the next instruction is fetched translated */

    /* TTBR0's identity map is what keeps this section fetching across the write above.
       Through a register: the displacement to the high half exceeds every branch's. */
    ldr     x0, =kickos_arm64_boot_high
    br      x0
```

Notice the order. The instruction right after the `isb` is fetched through the MMU, at the same
address as before. That only works because TTBR0 holds an identity map (VA = PA) of this code.
The `br` then lands in the high half, at `PA + 0xFFFF_FF80_0000_0000`, through TTBR1. From
`kickos_arm64_boot_high` on, the kernel runs at its linked addresses. That code reseats SP and
`VBAR_EL1` at their high addresses and calls `Reset_Handler`, which runs the C runtime and
`kmain`.

This is the first big difference from the Cortex-M walk. There, the kernel and the app live in
one physical address space, and the kernel reaches all of it through the MPU's privileged
background region. Here, the kernel lives in its own half. That half also maps all of DRAM at
`PA + __kickos_arm64_va_base`, so the kernel can reach any physical frame by adding a constant.
This **direct map** is what later stops lean on.

Later, `kickos_armv8a_percore_init` calls `kickos_armv8a_mmu_check`, which panics if the CPU
lacks the 4 KiB granule or cannot output as many physical address bits as `TCR` asks for.

**Try it:** `break kickos_arm64_boot_high`, `continue`,
`info registers pc sp SCTLR_EL1 TTBR0_EL1 TTBR1_EL1 TCR_EL1 VBAR_EL1`. Then `break Reset_Handler`,
`continue`, `info registers sp VBAR_EL1`. You should see:

```
pc             0xffffff8040019788  0xffffff8040019788 <kickos_arm64_boot_high>
sp             0x44000000          0x44000000
SCTLR_EL1      0xc5183d            12916797
TTBR0_EL1      0x40001000          1073745920
TTBR1_EL1      0x40002000          1073750016
TCR_EL1        0x12b5193519        80347739417
VBAR_EL1       0x40017000          1073836032
...
sp             0xffffff8044000000  0xffffff8044000000
VBAR_EL1       0xffffff8040017000  -548681977856
```

At the first stop the PC is already high but SP and `VBAR_EL1` are still physical. At
`Reset_Handler` both have moved up.

**Why, and the contract:** [Book: The machine underneath](../book/the-machine-underneath.md),
[Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit)

### Stop 4: frames, the raw material of an address space
**Where:** `kernel/init/kmain.cc`, `kmain`; `kernel/mem/frame_pool.cc`, `frame_pool_init`,
`kickos_frame_alloc`, `kickos_frame_free`; `kernel/mem/frame.cc`, `FrameAllocator`

A **frame** is one granule (4 KiB) of physical RAM. Page tables are made of frames, and so is
every page a task maps that does not come from the linked image: its stack, a reservation, a
shared buffer. The kernel keeps them in one pool: a bitmap allocator (`FrameAllocator`) over a
range the linker carves (the `.framepool` section). The arch layer says where that range is,
through `arch_frame_pool_bounds`.

`kmain` calls `frame_pool_init` before it builds root, because root's stack comes out of the
pool. On the Cortex-M walk this line does not exist: under `#if KICKOS_HAVE_ASPACE` it replaces
the boot-arena stack the region build allocates. That is the pattern of this whole chapter. The
kernel knows which kind of protection hardware it has, and compiles in only the subsystems that
mean something on it.

`kickos_frame_alloc` answers a PHYSICAL address. The page-table code turns it into a pointer
it can write through by adding the direct-map offset (`table_at` in `aspace_armv8a.cc`).

**Try it:** you will see the pool hand out frames at the next stop. There, after
`break arch_aspace_map` and `continue`, set `break kickos_frame_alloc`, then `continue` and
`finish`, twice. You should see the two table frames the first mapping needs:

```
Value returned is $1 = 1077043200
...
Value returned is $2 = 1077047296
```

That is `0x40326000` and `0x40327000`: the level-2 and level-3 tables for root's app text.

**Why, and the contract:** [Book: Where your RAM goes](../book/where-your-ram-goes-full-cxx-memory-floor-and-the-linker-split.md),
[Reference: porting.md](../reference/porting.md) (the translating ports' frame pool)

### Stop 5: one address space per task
**Where:** `kernel/domain/domain.cc`, `claim_slot`; `arch/arm64/armv8a/aspace_armv8a.cc`,
`arch_aspace_create`; `kernel/mem/aspace.cc`, `aspace_image_seed`; `kernel/mem/ustack.cc`,
`ustack_alloc`

On the Cortex-M walk a task's domain carries a region set. Here it carries a page-table root.
`kmain` asks for root's task through `task_for`, which reaches `claim_slot`, and on a translating
build `claim_slot` does three things:

1. `arch_aspace_create` takes one frame from the pool, zeroes it, and returns it as the new
   root. It also opens a row for this root in the ASID record `g_residency` (Stop 12). Nothing
   from the kernel is copied in: the kernel lives in TTBR1, so a user root starts empty.
2. `aspace_image_seed` maps the app's code and data. The app text is mapped read and execute,
   pointing at the frames the loader put it in, so every task shares one physical copy. The app
   data is mapped read and write. The first space, root's, maps the image's own data frames;
   every later space gets a private copy.
3. Back in `kmain`, `ustack_alloc` takes a run of frames for root's stack and maps all but the
   lowest one. That lowest frame is the **guard page**: it is never mapped, so a store just
   below the stack has no translation and faults.

The app window is identity-linked on this board: a task reaches a frame at its own physical
address. `arch_aspace_user_offset` answers 0 here. So in the outputs below VA and PA are equal.

**Try it:** `break arch_aspace_create`, `continue`, `bt 5`, `finish`. Then `delete`,
`break arch_aspace_map`, and `continue` three times, printing `p/x va`, `p pages` and
`p/x rights` each time. You should see:

```
#1  ... in kickos::(anonymous namespace)::claim_slot (caller=caller@entry=0, ...) at .../kernel/domain/domain.cc:90
#2  ... in kickos::domain_for (...)
#3  ... in kickos::task_for (...)
#4  ... in kickos::kmain (argc=argc@entry=0, argv=argv@entry=0x0) at .../kernel/init/kmain.cc:336
Value returned is $1 = (arch_aspace *) 0xffffff8040325000

arch_aspace_map (space=0xffffff8040325000, va=1074810880, pa=1074810880, pages=15, rights=5, ...)
#1  ... in kickos::aspace_image_seed (...)
arch_aspace_map (space=0xffffff8040325000, va=1075838976, pa=1075838976, pages=19, rights=3, ...)
#1  ... in kickos::aspace_image_seed (...)
arch_aspace_map (space=0xffffff8040325000, va=1077137408, pa=1077137408, pages=5, rights=3, ...)
#1  ... in kickos::ustack_alloc (d=<optimized out>, want=want@entry=20480) at .../kernel/mem/ustack.cc:53
```

The space handle `0xffffff8040325000` is the root frame `0x40325000` seen through the direct map.
`rights=5` is `ARCH_MAP_R | ARCH_MAP_X` (app text at `0x40105000`), `rights=3` is
`ARCH_MAP_R | ARCH_MAP_W` (app data at `0x40200000`, then the stack at `0x4033d000`).

**Why, and the contract:** [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit),
[Reference: invariants.md](../reference/invariants.md) (`space-data-starts-from-the-image`)

### Stop 6: the page-table walker
**Where:** `arch/common/aspace_walk.h`, `walk_map`, `map_into`, `walk_leaf`;
`arch/arm64/armv8a/aspace_armv8a.cc`, `arch_aspace_map`, `leaf_attrs`, `link_table`, `edits_done`

`arch_aspace_map` is short. It refuses a range that is misaligned or reaches into the kernel
half (`range_ok`), and an output address wider than the hardware can produce (`phys_range_ok`).
Then `leaf_attrs` builds the bits every leaf of this mapping carries:

```
        // Tag user entries by ASID; TTBR1 kernel entries remain global.
        uint64_t desc = DESC_VALID | DESC_BIT1 | DESC_AF | DESC_NG | DESC_AP_EL0 | attr;
        if ((rights & ARCH_MAP_W) == 0)
        {
            desc |= DESC_AP_RO;
        }
        if ((rights & ARCH_MAP_X) == 0)
        {
            desc |= DESC_UXN;
        }
        // Never allow privileged execution of user pages.
        desc |= DESC_PXN;
```

Three things to notice. Every user leaf is `nG` (not global), so its TLB entries carry the
space's ASID. Every user leaf is `PXN`, so the kernel can never execute user code by mistake.
And a request for write plus execute is refused just above this excerpt: user pages are W^X.

The walk itself is not in the arm64 file. `aspace_walk.h` is shared by every translating
backend, which spells its own descriptor format through a few hooks (`entry_table`, `leaf_desc`,
`link_table`, `invalidate_page`, `edits_done`). `walk_map` first checks the whole range: it
refuses one that is partly mapped, so a failed map can be undone by removing exactly what it
added. Then `map_into` descends:

```
            if (height == 0)
            {
                if ((table[idx] & ENTRY_VALID) != 0)
                {
                    // ...
                    // Break before make.
                    table[idx] = 0;
                    invalidate_page_if(va, e.resident);
                }
                table[idx] = leaf_desc(attrs, pa);
                invalidate_page_if(va, e.resident);
                // ...
            }

            if ((table[idx] & ENTRY_VALID) == 0)
            {
                arch_phys_addr_t const frame = kickos_frame_alloc();
                if (frame == 0)
                {
                    return ARCH_ASPACE_ENOMEM;
                }
                zero_table(table_at(frame));
                link_table(&table[idx], frame, e);
            }
```

A missing intermediate table is made on demand from a fresh frame. On arm64 `link_table` puts a
`dsb ishst` before the store, so the walker can never see the new entry before it sees the zeroes
of the table it points to. When a leaf replaces a valid one, the old one is cleared and
invalidated first: the architecture calls this **break-before-make**, and skipping it can leave
two translations for one address in the TLB.

**Try it:** `break kickos_root_entry` (it is the first user instruction, Stop 8), `continue`,
then use the helper: `uwalk 0x40105000`, `uwalk 0x40200000`, `uwalk 0x4033d000`,
`uwalk 0x4033c000`, `uwalk 0x09000000`. You should see:

```
L1[1] = 0x0000000040326003
L2[0] = 0x0000000040327003
L3[261] = 0x0020000040105fc3
...
L3[0] = 0x0060000040200f43
...
L3[317] = 0x006000004033df43
...
L3[316] = 0x0000000000000000
L1[0] = 0x0000000000000000
```

App text ends in `fc3`: valid page, `AP` = read-only at EL0, `nG` set, and only `PXN` in the top
bits, so EL0 may execute it. Data and stack end in `f43` with both `UXN` and `PXN`: read-write,
never executable. The guard page below the stack is zero. And the UART at `0x0900_0000` is not
in this task's space at all: level-1 entry 0 is empty. Compare with the boot root of Stop 2,
whose entry 0 mapped the whole device gigabyte.

**Why, and the contract:** [Reference: porting.md](../reference/porting.md) (the `arch_aspace_*`
seam), [Book: Privilege is three axes](../book/privilege-is-three-axes-not-one-bit.md)

### Stop 7: installing the space at the first switch
**Where:** `kernel/sched/sched.cc`, `sched::start`, `switch_book`; `kernel/mem/aspace.cc`,
`aspace_activate_for`, `seat`; `arch/arm64/armv8a/aspace_armv8a.cc`, `arch_aspace_activate`,
`write_ttbr0`, `capture_boot`

Here is the place where the ONE abstraction is easiest to see. The scheduler installs what the
incoming thread may touch with two lines, the same two lines on every board:

```
            // What the incoming thread may touch is installed here and nowhere else.
            [[maybe_unused]] struct arch_aspace* const rspace = aspace_activate_for(next);
            next->mpu.apply();
```

`sched::start` has the same pair for the first thread. On the Cortex-M build,
`aspace_activate_for` is an inline that returns null, and `next->mpu.apply()` reaches
`arch_mpu_apply`, which stashes the thread's encoded region set. The hardware is programmed later,
in `PendSV_Handler`, by `kickos_arch_mpu_commit`: at the physical switch, not at the decision.
On this build it is the other way round. `MpuSet::apply` compiles to nothing, because
`KICKOS_HAVE_MPU` is 0 (the arch links `arch/common/arch_mpu_none.cc`), and
`aspace_activate_for` does the work.

`aspace_activate_for` finds the thread's task space. `seat` compares it with the space this core
last installed (`g_current`) and returns at once when they are the same, which is the common case:
two threads of one task share one root. Otherwise it calls `arch_aspace_activate`, which builds
the TTBR0 value, root frame plus ASID in bits [63:48], masks interrupts and calls
`write_ttbr0`:

```
    void write_ttbr0(uint64_t ttbr)
    {
        uint64_t const leaving = kickos_armv8a_read_ttbr0_el1();
        kickos_armv8a_write_ttbr0_el1(ttbr);
        kickos_armv8a_isb();
        if (asid_of(leaving) == 0)
        {
            kickos_armv8a_tlbi_all_local();
            kickos_armv8a_dsb_ish();
            kickos_armv8a_isb();
        }
        g_residency.note(ttbr & DESC_OA_MASK, arch_cpu_id());
    }
```

Why flush only when LEAVING ASID 0? The boot root has ASID 0 and its entries are global (no `nG`
bit). A global TLB entry matches every ASID, so after a switch away from the boot root its
entries could still answer for the new space. Switching between two tagged roots needs no flush
at all: their entries carry different ASIDs and cannot be confused. Switching INTO the boot root
needs none either, because the task entries left behind are tagged and the boot root's lookups
use ASID 0.

`capture_boot` records the boot root on the first activation. The fault reporter and
`aspace_release` switch back to it later.

Why is it safe to write TTBR0 at the decision, here, when the Cortex-M build waits for the
physical switch? Between `switch_book` and the return to EL0, only EL1 kernel code runs, and the
kernel runs from TTBR1. It never dereferences a user address through TTBR0 on that path (Stop 10
shows how it reaches user memory instead). The outgoing thread's own user code cannot run again
before the switch completes.

**Try it:** `break arch_aspace_activate`, `continue`, `bt 6`, `info registers TTBR0_EL1`,
`finish`, `info registers TTBR0_EL1`. You should see:

```
#4  kickos::aspace_activate_for (t=t@entry=0xffffff8040313c50 <kickos::detail::g_instance+8544>) at .../kernel/mem/aspace.cc:1447
#5  0xffffff8040016040 in kickos::sched::start () at .../kernel/sched/sched.cc:726
TTBR0_EL1      0x40001000          1073745920
...
TTBR0_EL1      0x1000040325000     281476053749760
```

Before: the boot identity root, ASID 0. After: root's own table `0x40325000`, ASID 1. To see the
flush rule, run `disassemble '(anonymous namespace)::write_ttbr0(uint64_t)'`, find its
`tlbi vmalle1`, and put a breakpoint on that instruction's address. On `selftest` it fires only on an
activation whose previous `TTBR0_EL1` had ASID 0 (`0x40001000`), never between two tasks.

**Why, and the contract:** [Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Reference: invariants.md](../reference/invariants.md) (`mpu-apply-on-every-switch-in`)

### Stop 8: the first EL0 instruction
**Where:** `arch/arm64/armv8a/arch_armv8a.cc`, `arch_context_init`, `arch_start`;
`arch/arm64/armv8a/switch.S`, `kickos_armv8a_start`, `RESTORE_FRAME_AND_ERET`;
`user/src/root_entry.cc`, `kickos_root_entry`

On the Cortex-M walk root starts with a fabricated exception frame and an exception return that
drops to unprivileged Thread mode. AArch64 does the same with `eret`. `arch_context_init` builds a
frame whose saved `SPSR` says EL0t (EL0, using `SP_EL0`) with interrupts on, whose saved `ELR` is
the entry point, and whose saved `SP_EL0` is the top of the user stack. Where it puts that frame
matters. For an EL0 thread the frame goes on the thread's **kernel block**, not on its user stack.
The user stack is mapped task-wide, so another thread of the same task could write it and choose the `SPSR` of
your first `eret`. The kernel block is in the kernel half, which EL0 cannot reach.

`arch_start` hands the frame to `kickos_armv8a_start`, which sets SP and runs
`RESTORE_FRAME_AND_ERET`. After the `eret`, `SP_EL1` sits at the top of the kernel block. Every
later trap from this thread lands on that block (Stop 9).

**Try it:** `break kickos_root_entry`, `continue`,
`info registers pc cpsr sp SP_EL0 SP_EL1 TTBR0_EL1`, `p/x ($cpsr >> 2) & 3`. You should see:

```
pc             0x401095a4          0x401095a4 <kickos_root_entry(void*)>
cpsr           0x200               [ EL=0 D BTYPE=0 ]
sp             0x40342000          0x40342000
SP_EL0         0x40342000          1077157888
SP_EL1         0xffffff8040301080  -548678922112
TTBR0_EL1      0x1000040325000     281476053749760
$1 = 0x0
```

EL is 0. The user SP is the top of the stack mapped at Stop 5 (`0x4033d000` plus 5 pages).
`SP_EL1` is the top of a kernel block, in the high half.

**Why, and the contract:** [Book: Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md),
[Book: Whoever stacks the trap frame owns the bounds check](../book/whoever-stacks-the-trap-frame-owns-the-bounds-check.md),
[Reference: invariants.md](../reference/invariants.md) (`root-unprivileged-idle-alone-privileged`)

### Stop 9: the exception vector and the SVC path at EL1
**Where:** `arch/arm64/armv8a/switch.S`, `arch_syscall`, `kickos_armv8a_el0_sync_entry`,
`ENTER_FROM_EL0`; `arch/arm64/armv8a/vectors.S`, `kickos_armv8a_vectors`;
`kernel/syscall/syscall.cc`, `syscall_dispatch`

The user side is the same idea as on the Cortex-M: `arch_syscall` puts the call number and the
arguments in registers and executes `svc #0`. It sits in the app's own text (section `.apptrap`),
because EL0 cannot execute anything in the kernel half.

The hardware side differs. AArch64 has one vector table of sixteen slots, 128 bytes each, at
`VBAR_EL1`. The slot is chosen by WHERE the exception came from (same EL or lower EL) and WHAT it
is (synchronous, IRQ, FIQ, SError). An `svc` from EL0 lands in slot 0x400, "lower EL,
synchronous", which branches to `kickos_armv8a_el0_sync_entry`. Every other slot is filled too,
with a reporter: an empty slot would run whatever bytes follow the table.

Slot 0x400 receives every synchronous exception from EL0, faults included. So the entry must
first ask which one it is, from the exception class in `ESR_EL1`:

```
kickos_armv8a_el0_sync_entry:
    ENTER_FROM_EL0
    mrs     x0, esr_el1
    lsr     x0, x0, #26                     /* ESR_EL1.EC */
    cmp     x0, #0x15                       /* SVC instruction executed in AArch64 state */
    b.ne    .Lel0_fault
    // ...
    ldp     x0, x1, [sp, #0]
    ldp     x2, x3, [sp, #16]
    ldr     x4, [sp, #32]
    bl      syscall_dispatch
    /* uint64_t returns in x0 at this width, so the arch_syscall64 pair collapses. */
    str     x0, [sp, #0]
    RESTORE_FRAME_AND_ERET
```

Compare with the Cortex-M. There the hardware stacks eight registers on the THREAD's stack, and
`SVC_Handler` rewrites the stacked PC so the exception return lands in `svc_trampoline`, which
calls the dispatcher in privileged thread mode. Here nothing is stacked by hardware: the return
address is in `ELR_EL1` and the old state in `SPSR_EL1`. `ENTER_FROM_EL0` saves all registers,
those two, and `SP_EL0` into a frame on the thread's kernel block, where `SP_EL1` already
points. `syscall_dispatch` then runs at EL1 on that block, still in the calling thread's context,
so it can block like any kernel code. The result goes back into the saved `x0` and
`RESTORE_FRAME_AND_ERET` returns to EL0. A class other than 0x15 goes to `.Lel0_fault` and
`kickos_fault_kill_thread` ([chapter 9](09-fault-and-kill.md)).

**Try it:** `break kickos_root_entry`, `continue`, `delete`, `break arch_syscall`, `continue`,
`info registers x0`, `x/2i $pc`. Then `break kickos_armv8a_el0_sync_entry`, `continue`,
`info registers pc sp cpsr ELR_EL1 SPSR_EL1 ESR_EL1`, `p/x $ESR_EL1 >> 26`. Then
`break syscall_dispatch`, `continue`, `bt 3`, `info registers sp`. You should see:

```
x0             0x47                71
=> 0x40105040 <arch_syscall64>:	svc	#0x0
   0x40105044 <arch_syscall64+4>:	ret
...
pc             0xffffff8040017918  0xffffff8040017918 <kickos_armv8a_el0_sync_entry>
sp             0xffffff8040301080  0xffffff8040301080 <kickos::(anonymous namespace)::g_kstacks+4096>
cpsr           0x3c5               [ SP EL=1 F I A D BTYPE=0 ]
ELR_EL1        0x40105044          1074810948
SPSR_EL1       0x80000200          2147484160
ESR_EL1        0x56000000          1442840576
$1 = 0x15
...
#0  syscall_dispatch (nr=71, a0=0, a1=0, a2=0, a3=0) at .../kernel/syscall/syscall.cc:371
#1  0xffffff80400179dc in kickos_armv8a_el0_sync_entry () at .../arch/arm64/armv8a/switch.S:250
#2  0x00000000401071dc in kos_thread_self () at .../user/src/syscall_stubs.cc:256
sp             0xffffff8040300d60  0xffffff8040300d60 <kickos::(anonymous namespace)::g_kstacks+3296>
```

Call 71 is `KOS_SYS_THREAD_SELF`. `ELR_EL1` is the instruction after the `svc`, and `SPSR_EL1`'s
low four bits are 0 (EL0t). SP at entry is the top of the kernel block, and the dispatcher runs
800 bytes lower: that is the frame `ENTER_FROM_EL0` pushed (31 general registers, the saved
state, and all 32 vector registers). gdb even unwinds through the trap into the user caller.

**Why, and the contract:** [Book: The syscall path](../book/the-syscall-path-trap-dispatch-return.md),
[Reference: User/kernel separation](../reference/architecture.md#userkernel-separation),
[Reference: invariants.md](../reference/invariants.md) ("Syscall, privilege & the user/kernel boundary")

### Stop 10: copying to and from user memory
**Where:** `kernel/syscall/syscall_mem.cc`, `access_copy`, `kaccess_from_user`,
`kaccess_to_user`, `kaccess_word_to_user`; `arch/arm64/armv8a/aspace_armv8a.cc`,
`arch_aspace_acquire`

On the Cortex-M the kernel and the caller share one physical address space. Once the syscall has
checked that a user pointer lies in memory the caller owns, `access_copy` is a plain `kmemcpy`.

Here a user pointer is a VIRTUAL address in some task's space. The kernel does not use it as a
pointer. For each granule, `access_copy` asks `arch_aspace_acquire` for a kernel pointer to the
same bytes:

```
    arch_irq_state_t const s = arch_irq_save();
    uint64_t const* const entry = leaf_entry(root_of(space), page);
    uint64_t desc = 0;
    if (entry != nullptr)
    {
        desc = *entry;
    }
    arch_irq_restore(s);
    if (entry == nullptr)
    {
        return nullptr;
    }
    arch_phys_addr_t const out = static_cast<arch_phys_addr_t>(desc & DESC_OA_MASK);
    // ...
    return reinterpret_cast<void*>(static_cast<uintptr_t>(out) + va_base() +
                                   (va & (GRANULE - 1)));
```

The acquire walks the space's tables in software, takes the physical frame from the leaf, and
answers that frame's address in the direct map. This buys three things:

- **The right space.** The space is named explicitly, so the kernel can copy between two tasks
  (an IPC send, `ep_copy`) even though at most one of them is in `TTBR0`.
- **No kernel fault on a bad pointer.** An unmapped page answers null, and the copy fails with an
  error instead of a data abort at EL1. A page unmapped by another thread between the check and the
  copy is refused the same way.
- **No dependence on the hardware.** The Cortex-A53 implements ARMv8.0, which has no PAN bit to
  stop EL1 from reading user pages through TTBR0. Going through the acquire is the kernel's rule
  for every user access, not a fallback.

The copy goes granule by granule because two neighbouring user pages need not be neighbours in
physical memory.

**Try it:** on `hello`, `break kickos_root_entry`, `continue`, `delete`,
`break arch_aspace_acquire`, `continue`, `bt 5`, `p/x va`, `finish`. You should see:

```
#1  ... in kickos::(anonymous namespace)::access_copy (dspace=0xffffff8040325000, dst=dst@entry=1077157704, sspace=sspace@entry=0x0, ...)
#2  ... in kickos::kaccess_to_user (dspace=<optimized out>, udst=udst@entry=1077157704, ...)
#3  ... in kickos::cap_out_deliver (out=1077157704, ...)
#4  ... in (anonymous namespace)::syscall_body (nr=11, a0=1077157704, ...)
$1 = 0x40341f48
Value returned is $2 = (void *) 0xffffff8040341f48
```

Call 11 (`KOS_SYS_NOTIFY_CREATE`) returns a new handle through an out-pointer on root's stack,
`0x40341f48`. The kernel writes it at `0xffffff8040341f48`: the same physical byte, through
TTBR1.

**Why, and the contract:** [Reference: The map editor's acquire pair](../reference/porting.md#the-map-editors-acquire-pair-arch_aspace_acquire-arch_aspace_release),
[Reference: invariants.md](../reference/invariants.md) (`user-args-validated-at-boundary`,
`kernel-alias-maintained-over-uncached-memory`), [Book: Alignment across the syscall boundary](../book/alignment-across-the-syscall-boundary.md)

### Stop 11: one kernel call, two backends
**Where:** `kernel/syscall/syscall.cc`, the `KOS_SYS_MEM_SELF_GRANT` case;
`kernel/mem/aspace.cc`, `aspace_self_grant`; `kernel/thread/thread.cc`, `thread_self_grant`;
`user/include/kickos/sys.h`, `kos_ram_alloc`, `kos_mem_self_grant`

[Chapter 6](06-shared-memory.md) follows memory from a reservation to a grant on the Cortex-M.
The user API is the same here: `kos_ram_alloc` reserves page-aligned RAM for the task, and
`kos_mem_self_grant` asks for access to it. Inside the kernel the one syscall splits on the
backend:

```
#if KICKOS_HAVE_ASPACE
            // ...
            int const grc = aspace_self_grant(domain_space(thread_domain(c)),
                                              domain_ranges_mut(thread_domain(c)), base, size,
                                              ARCH_MAP_R | ARCH_MAP_W, map_memtype_of(attr));
            // ...
            return static_cast<uint64_t>(grc);
#else
            // ...
            return static_cast<uint64_t>(thread_self_grant(c, base, size, attr));
#endif
```

On the region backend, `thread_self_grant` adds one region to the CALLING THREAD's set and loads
the set at once with `MpuSet::apply_now` (not the stash of Stop 7: the caller's next instruction
may touch the memory). On this backend, `aspace_self_grant` checks that the range is one this
task reserved, then calls `arch_aspace_map` on the TASK's space, which edits the page tables of
Stop 6. The mapping is task-wide, so every thread of the task reaches it. The Reference states
the portable floor: a grant is guaranteed to reach its holder, and a region backend is stricter
than that floor.

The frame calls go further and exist only here. `KOS_SYS_FRAME_CREATE`, `KOS_SYS_FRAME_MAP`
and `KOS_SYS_FRAME_UNMAP` (`kos_frame_map` in user code) name a run of frames by capability and
map it into a space named by another capability, through `aspace_cap_map`. A region build does
not compile them, because there is no translation to map into.

**Try it:** run the same `selftest` call on both builds. On `qemu-arm64`:
`break arch_aspace_map if pages == 1`, `continue`, `bt 4`, `finish`, then `uwalk base`. On the reference `qemu` build (launched as in [Setup](00-setup.md)):
`break kickos::thread_self_grant`, `continue`, `bt 4`. You should see, on arm64:

```
arch_aspace_map (space=0xffffff8040325000, va=1077141504, pa=1077141504, pages=1, rights=3, type=ARCH_MAP_NORMAL)
#1  ... in kickos::aspace_self_grant (..., base=base@entry=1077141504, size=size@entry=64, rights=rights@entry=3, ...)
#2  ... in (anonymous namespace)::syscall_body (nr=37, a0=1077141504, a1=64, a2=0, a3=0)
#3  syscall_dispatch (nr=37, a0=1077141504, a1=64, a2=0, a3=0)
...
L3[318] = 0x006000004033ef43
```

and on the Cortex-M4F:

```
kickos::thread_self_grant (c=c@entry=0x20004d40 <kickos::detail::g_instance+1424>, base=base@entry=537018368, size=size@entry=64, attr=attr@entry=3)
#1  ... in (anonymous namespace)::syscall_body (nr=<optimized out>, a0=537018368, a1=64, ...)
#2  syscall_dispatch (nr=<optimized out>, a0=537018368, a1=64, ...)
#3  0x00018246 in svc_trampoline () at .../arch/arm/armv7m/switch.S:496
```

Same call number (37), same 64-byte grant. On one build it becomes a page-table leaf for a task,
on the other a region for a thread.

**Why, and the contract:** [Reference: Memory domains](../reference/architecture.md#memory-domains-the-isolation-unit),
[Book: The machine underneath](../book/the-machine-underneath.md)

### Stop 12: the context switch changes the address space
**Where:** `kernel/sched/sched.cc`, `switch_book`; `kernel/mem/aspace.cc`, `seat`;
`arch/arm64/armv8a/aspace_armv8a.cc`, `arch_aspace_activate`, `asid_capacity`, `g_residency`;
`arch/include/kickos/arch/aspace_residency.h`, `Residency`;
`arch/arm64/armv8a/arch_armv8a.cc`, `arch_switch`

The register swap itself is [chapter 3](03-sleep.md)'s story with AArch64 spelling. In thread
context `arch_switch` calls `kickos_armv8a_switch_now`, which parks the outgoing frame and
`eret`s off the incoming one. From an interrupt, `arch_switch` only records the target and the
IRQ exit path (`kickos_armv8a_irq_entry`, `kickos_armv8a_el0_irq_entry`) swaps the frames. What is
new here is the address space, and it changes in `switch_book`, before either path.

When the incoming thread belongs to another task, `seat` writes a new `TTBR0_EL1`. The value
carries an ASID, so the outgoing task's TLB entries can stay cached. If the scheduler comes back
to that task soon, its translations may still be there. Without ASIDs every root write would have
to flush the whole TLB.

ASIDs are a small resource, and `Residency` hands them out. `arch_aspace_create` opens a row keyed
by the root's physical address and takes the lowest free ASID, bounded by what the hardware
supports (`asid_capacity`, 8 or 16 bits) and by the number of domain slots. Each row also records
which cores have used that root. ASID 0 is never handed out: it marks the boot root, whose
entries are global. An ASID is given back only after its TLB entries are gone (Stop 13).

**Try it:** on `selftest`, set a breakpoint that logs and continues:

```
break arch_aspace_activate
commands
silent
printf "activate space=%p  TTBR0 before=0x%llx\n", space, $TTBR0_EL1
continue
end
break arch_aspace_destroy
commands
silent
printf "destroy space=%p\n", space
continue
end
continue
```

Let it run for some seconds and interrupt. You should see lines like:

```
activate space=0xffffff8040325000  TTBR0 before=0x40001000
activate space=0xffffff8040353000  TTBR0 before=0x1000040325000
activate space=0xffffff80403c0000  TTBR0 before=0x2000040353000
activate space=0xffffff8040353000  TTBR0 before=0x30000403c0000
activate space=0xffffff80403c0000  TTBR0 before=0x2000040353000
activate space=0xffffff8040001000  TTBR0 before=0x30000403c0000
destroy space=0xffffff80403c0000
activate space=0xffffff8040353000  TTBR0 before=0x40001000
...
activate space=0xffffff80403c4000  TTBR0 before=0x2000040353000
activate space=0xffffff8040353000  TTBR0 before=0x30000403c4000
```

Read the top 16 bits of each "before" value: ASID 1 is root, ASID 2 the selftest's main task,
ASID 3 a child task. When the child exits, `aspace_release` moves this core to the boot root
(`0xffffff8040001000`, ASID 0) before it frees the child's tables. The next child, a different
root at `0x403c4000`, gets ASID 3 again, because it was given back.

Add a third logging breakpoint on `kickos::aspace_activate_for` and count. In one run of about 25
seconds it was reached 2159 times and `arch_aspace_activate` 251 times: `seat` skips every switch
that stays inside one task.

**Why, and the contract:** [Book: Protection follows the CPU](../book/protection-follows-the-cpu-not-the-schedulers-intent.md),
[Book: Context switching and the silicon contract](../book/context-switching-and-the-silicon-contract.md),
[Reference: porting.md](../reference/porting.md)

### Stop 13: unmap and TLB maintenance
**Where:** `arch/common/aspace_walk.h`, `walk_unmap`, `remove_run`, `detach_empty`;
`arch/arm64/armv8a/aspace_armv8a.cc`, `arch_aspace_unmap`, `invalidate_page`, `invalidate_all`,
`edits_done`, `arch_aspace_destroy`; `kernel/mem/ustack.cc`, `ustack_free`

On the Cortex-M, taking memory away from a thread means rebuilding its region set; the next
commit replaces the whole set in the MPU. A page table needs two more steps, because the TLB and
the walk caches remember. After the descriptor store, the stale translation must be dropped:

```
    void invalidate_page(uintptr_t va)
    {
        kickos_armv8a_dsb_ishst();
        // The IS form reaches all PEs in the Inner Shareable domain; the local form
        // reaches this PE. DSB ISH completes either (DDI 0487 M.b, D8.17.5 and B2.6.9.1).
#if KICKOS_KERNEL_CORES > 1
        kickos_armv8a_tlbi_page_is(va >> GRANULE_SHIFT);
#else
        kickos_armv8a_tlbi_page_local(va >> GRANULE_SHIFT);
#endif
        kickos_armv8a_dsb_ish();
        kickos_armv8a_isb();
    }
```

Each barrier has a job. `dsb ishst` makes the store of the zeroed descriptor visible to the
walker before the invalidate. `tlbi vaae1` drops the entry for that page (the second "a" means
"for all ASIDs", so the code does not need to know which ASID the space has). `dsb ish` waits for
the invalidate to finish, and `isb` makes the following instructions use the new state. On a
multicore build the `is` form reaches every core; [chapter 11](11-multicore.md) picks that up.

`remove_run` clears each leaf and invalidates it. Then `detach_empty` takes out of the tree any
table the unmap left empty. Such a table cannot go back to the pool yet: the hardware may hold a
partial walk through it. So `edits_done` runs `invalidate_all` (a `tlbi vmalle1`, which drops walk
caches too) before `remove_run` frees the detached tables.

Destroying a space follows the same order at a larger scale. `arch_aspace_destroy` invalidates
everything, THEN gives the ASID back with `g_residency.close`, THEN frees the tables and the root.
Free the ASID first, and a new space could reuse it while old entries tagged with it still sit in
the TLB.

**Try it:** on `selftest`, `break arch_aspace_unmap`, `continue`, `bt 3`, `p/x va`,
`set $u = va`, `uwalk $u`, then `finish` and `uwalk $u` again. Then `delete`,
`break invalidate_page`, `continue`, `bt 3`, `x/5i $pc` to see the maintenance on the next unmap.
You should see:

```
arch_aspace_unmap (space=0xffffff8040353000, va=1077616640, pages=3)
#1  ... in kickos::ustack_free (..., base=1077616640, ...)
#2  ... in kickos::sched::exit_current (code=0, ...)
$1 = 0x403b2000
L1[1] = 0x0000000040354003
L2[1] = 0x0000000040356003
L3[434] = 0x00600000403b2f43
Value returned is $3 = ARCH_ASPACE_OK
L1[1] = 0x0000000040354003
L2[1] = 0x0000000040356003
L3[434] = 0x0000000000000000
...
(anonymous namespace)::invalidate_page (va=1077633024)
#2  ... in (anonymous namespace)::remove_run (root=root@entry=0xffffff8040353000, va=va@entry=1077633024, pages=pages@entry=3, e=...)
=> ...:	dsb	ishst
   ...:	lsr	x0, x0, #12
   ...:	tlbi	vaae1, x0
   ...:	dsb	ish
   ...:	isb
```

A thread of the selftest's main task (`TTBR0` with ASID 2) exits, and `ustack_free` unmaps its
three stack pages. The leaf that pointed at `0x403b2000` is zero after the unmap. `uwalk` reads
`TTBR0_EL1`, so it only shows the right space while that space is installed, as it is here.

**Why, and the contract:** [Reference: porting.md](../reference/porting.md) (the `arch_aspace_*`
seam and the cross-core maintenance), [Reference: invariants.md](../reference/invariants.md)

## Where to next

- [Chapter 6, shared memory](06-shared-memory.md): the same reservation and grant on the region
  backend, in full.
- [Chapter 9, fault and kill](09-fault-and-kill.md): what `.Lel0_fault` does with a data abort
  from EL0, and why the fault exit keeps the dying task's `TTBR0` installed.
- [Chapter 11, multicore](11-multicore.md): the `is` forms of the TLB maintenance, the residency
  mask, and why a core that runs a thread with no space installs the boot root.
- [Chapter 15, x86_64](15-x86_64.md) and [chapter 14, RISC-V](14-riscv.md): the other
  translating backends, which share `aspace_walk.h` and differ in their hooks.

## Exercises

1. **Where is the UART?** Stop at `kickos_root_entry` and run `uwalk 0x09000000`. Predict the
   result from Stop 2 and Stop 5 before you look. (Level-1 entry 0 is empty: the task has no
   device mapping at all, while the boot root had one.)
2. **Count the root writes in `hello`.** Before running, predict how many times
   `arch_aspace_activate` fires while `ping` and `pong` play for ten seconds. Then log it with a
   `commands` block as in Stop 12. (Twice: root's space with ASID 1, where the init runs, then
   the space of the task that runs `main`, with ASID 2. `ping` and `pong` are threads of that
   task, so every switch between them is skipped by `seat`. Check with `break kickos_init_entry`
   and a breakpoint on `ping`, reading `TTBR0_EL1` at each.)
3. **When does the full flush fire?** Break on the `tlbi vmalle1` inside `write_ttbr0` and log
   `TTBR0_EL1` at each `arch_aspace_activate`. Predict which activations reach the flush. (Only
   those whose previous `TTBR0` had ASID 0: the first one, and the first after a task's release
   moved the core to the boot root.)
