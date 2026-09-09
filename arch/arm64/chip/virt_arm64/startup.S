/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * QEMU `virt` (AArch64, Cortex-A53) startup. `-kernel <elf>` loads the image into DRAM at
 * 0x4000_0000 and enters at the ELF entry point with the MMU and caches off. A64 loads no SP
 * from a vector table, so SP is set here; .data and .bss are Reset_Handler's, as on
 * virt_rv32.
 *
 * The entry exception level is a property of the QEMU INVOCATION: `-M virt` hands over at
 * EL1, `virtualization=on` at EL2, `secure=on` at EL3. This port implements EL1 and refuses
 * the others by name.
 *
 * THE MMU COMES UP BEFORE ANY C. While SCTLR_EL1.M is 0 every access is Device-nGnRnE, which
 * mandates natural alignment whatever SCTLR_EL1.A says, and AArch64 GCC stages array and
 * struct copies through unaligned `ldur`/`stur` of q registers, as does newlib inside memcpy.
 * So compiled code alignment-faults (EC 0x25, DFSC 0x21) on its first wide copy, and the
 * tables below are what buy Normal memory and with it the caches. Link-time constants.
 *
 * EVERYTHING IN .text.init IS REACHED UNTRANSLATED, so an absolute literal here holds a high
 * virtual address that no walk can resolve yet and `adrp` to a high symbol is out of its
 * 4 GiB reach. x9 carries the offset back to the physical address, and the branch into the
 * high half is what stops needing it. The EL refusal reporter is linked into this section
 * for the same reason (chip_virt_arm64.cc).
 */

#include <vmsa.h>

/* 64 MiB of DRAM in 2 MiB blocks. Must be KICKOS_ARM64_DRAM_SIZE / L2_BLOCK_SIZE from the
 * linker script; nothing checks the pair. */
#define RAM_L2_BLOCKS   32

#define L2_ENTRIES      512             /* one 4 KiB table of 8-byte descriptors */
/* One level-1 entry spans a whole level-2 table, and the DRAM entry is index 1 in both roots
 * below, so this table's slot 0 describes the VA one level-1 entry up from zero. */
#define L1_BLOCK_SIZE   (L2_ENTRIES * L2_BLOCK_SIZE)
#define DRAM_L1_ENTRY   1

/* The level-2 table describes the PARTITION and not this image.
 *
 * A slot's index is its own VA, so the output has to be MAP_BASE + index * block for the map
 * to be an identity: an output biased by this image's base would leave the image unreachable
 * from its own entry point.
 *
 * Each node maps ITS OWN share and the shared region, which sits above every node's share, so
 * a count describing one share leaves that region outside the table and every write to it
 * faults. A peer's memory is left invalid. Nothing on this machine ENFORCES the boundary.
 */
#if KICKOS_AMP_OWN_IMAGE
#include <amp_partition.ld.h>

/* Every derivation below is an integer division that TRUNCATES rather than failing, and an
 * index whose meaning is fixed by the level-1 entry this table hangs off, so a partition
 * breaking any of the three links here would link cleanly and fault at the first touch of
 * memory no descriptor ever covered.
 */
#if (KICKOS_AMP_PARTITION_BASE) != (DRAM_L1_ENTRY * L1_BLOCK_SIZE)
#error "KICKOS_AMP_PARTITION_BASE is not the base of the DRAM level-1 entry this table hangs \
off. A descriptor's slot index IS its virtual address here, so slot 0 describes DRAM_L1_ENTRY \
* L1_BLOCK_SIZE whatever output it names: a partition based anywhere else maps every node at \
an offset from where it linked, and this image is unreachable from its own entry point. Base \
the partition at that address, or add the level-1 entry that covers the one it states."
#endif

#if ((KICKOS_AMP_NODE_SHARE) % L2_BLOCK_SIZE) != 0
#error "KICKOS_AMP_NODE_SHARE is not a whole number of L2_BLOCK_SIZE blocks. This table maps 2 \
MiB blocks and nothing finer, so OWN_L2_LO truncates a node's base DOWN into its predecessor's \
share and OWN_L2_HI truncates its top DOWN inside its own: node 0 keeps a mapping it does not \
own and every node loses the tail of its RAM, with a clean link. Round the share up to a \
multiple of 2 MiB."
#endif

#if ((KICKOS_AMP_SHARED_SIZE) % L2_BLOCK_SIZE) != 0
#error "KICKOS_AMP_SHARED_SIZE is not a whole number of L2_BLOCK_SIZE blocks. SHARED_L2_HI \
truncates DOWN, so a region narrower than one 2 MiB block is described by NO descriptor at all \
and every write to the region every node writes takes a level-2 translation fault, while a \
wider unaligned one loses its tail. This is the ONE region both nodes write, so it fails as a \
partition that boots and never converses. Round it up to a multiple of 2 MiB."
#endif

#define MAP_BASE        KICKOS_AMP_PARTITION_BASE
#define MAP_L2_BLOCKS   ((KICKOS_AMP_PARTITION_END - KICKOS_AMP_PARTITION_BASE) / L2_BLOCK_SIZE)

#if (MAP_L2_BLOCKS) > (L2_ENTRIES)
#error "The AMP partition spans more than L2_ENTRIES blocks of L2_BLOCK_SIZE, one L1_BLOCK_SIZE \
(1 GiB) in total, and this table has one level-2 page for the whole partition. The .fill below \
would run with a negative count, which the assembler only WARNS about, so the table ends short \
of the partition and a slot past it aliases the next table in memory instead of faulting. Shrink \
KICKOS_AMP_NODE_SHARE, KICKOS_AMP_NODES or KICKOS_AMP_SHARED_SIZE below 1 GiB, or add the second \
level-2 table and level-1 entry a wider partition needs."
#endif

#define OWN_L2_LO       ((KICKOS_AMP_IMAGE_BASE - KICKOS_AMP_PARTITION_BASE) / L2_BLOCK_SIZE)
#define OWN_L2_HI       (OWN_L2_LO + KICKOS_AMP_NODE_SHARE / L2_BLOCK_SIZE)
#define SHARED_L2_LO    ((KICKOS_AMP_SHARED_BASE - KICKOS_AMP_PARTITION_BASE) / L2_BLOCK_SIZE)
#define SHARED_L2_HI    (SHARED_L2_LO + KICKOS_AMP_SHARED_SIZE / L2_BLOCK_SIZE)
#else
#define MAP_BASE        __kickos_dram_base
#define MAP_L2_BLOCKS   RAM_L2_BLOCKS
#define OWN_L2_LO       0
#define OWN_L2_HI       RAM_L2_BLOCKS
#define SHARED_L2_LO    0
#define SHARED_L2_HI    0
#endif

/* The one block the kernel is fetched from, read-only and executable at EL1. */
#define CODE_L2_BLOCK   OWN_L2_LO

/* One level-2 table for 0x4000_0000..0x8000_0000. Emitted twice, into the two roots, which
   differ in their comments and not in their descriptors. */
.macro  L2_PARTITION_TABLE
    .set    _blk, 0
    .rept   MAP_L2_BLOCKS
    .if     _blk == CODE_L2_BLOCK
    .quad   MAP_BASE + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NORMAL + SH_INNER + \
            AP_RO_EL1 + UXN
    .elseif (_blk >= OWN_L2_LO && _blk < OWN_L2_HI) || \
            (_blk >= SHARED_L2_LO && _blk < SHARED_L2_HI)
    .quad   MAP_BASE + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NORMAL + SH_INNER + \
            AP_RW_EL1 + UXN + PXN
    .else
    .quad   0
    .endif
    .set    _blk, _blk + 1
    .endr
    .fill   L2_ENTRIES - MAP_L2_BLOCKS, 8, 0
.endm



    .section .text.init, "ax"
    .global _start
_start:
    ldr     x9, =__kickos_arm64_va_base

    /* SPSel AHEAD OF THE FIRST STACK WRITE, and stated rather than inherited: entered with
       SPSel 0 the `mov sp` below loads SP_EL0 and leaves SP_ELx as it was, so selecting SP_ELx
       afterwards hands the refusal path an uninitialised stack. The vector table's first four
       slots are also unreachable only while SPSel is 1. */
    msr     SPSel, #1
    isb

    /* SP before the EL check, so the refusal path below can call C. */
    ldr     x0, =_estack            /* boot/exception stack: top of RAM, 16-byte aligned */
    sub     x0, x0, x9
    mov     sp, x0

    mrs     x0, CurrentEL           /* CurrentEL.EL is bits [3:2]; EL1 == 0b01 */
    lsr     x0, x0, #2
    and     x0, x0, #3
    cmp     x0, #1
    b.ne    .Lbad_el

    /* CPACR_EL1.FPEN resets to TRAPPING, so the q-register copies above would trap. */
    mov     x0, #(3 << 20)          /* FPEN=0b11: no trapping at either level */
    msr     CPACR_EL1, x0
    isb

    ldr     x0, =MAIR_VALUE
    msr     MAIR_EL1, x0
    ldr     x0, =TCR_VALUE
    msr     TCR_EL1, x0
    /* Both roots are identity-linked, so these literals are already physical. */
    ldr     x0, =kickos_arm64_ident_table
    msr     TTBR0_EL1, x0
    ldr     x0, =kickos_arm64_kernel_table
    msr     TTBR1_EL1, x0

    /* The tables have to be visible to the walker before the walker exists. */
    dsb     sy
    isb
    tlbi    vmalle1
    dsb     sy
    isb

    /* Before the MMU and before the C runtime, so the whole boot path is covered rather
       than the part after arch_init where the RISC-V chip installs mtvec: an exception with
       no vector base runs whatever bytes VBAR points at. The reporter it reaches touches
       only the console and .rodata, both live here. Reseated at its high address past the
       branch below, where the physical alias stops being the one the kernel runs at. */
    ldr     x0, =kickos_armv8a_vectors
    sub     x0, x0, x9
    msr     VBAR_EL1, x0
    isb

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

    /* The level stays in x0 as the argument. A direct branch, the callee being linked into
       this same section: no literal to unbias, and nothing it reads sits in the high half. */
.Lbad_el:
    b       kickos_arm64_bad_el     /* noreturn (chip_virt_arm64.cc) */

/* Reached through TTBR1. SP and VBAR_EL1 still hold the physical aliases .text.init needed
 * before the walk existed.
 */
    .section .text.boot_high, "ax"
    .global kickos_arm64_boot_high
kickos_arm64_boot_high:
    ldr     x0, =_estack
    mov     sp, x0
    ldr     x0, =kickos_armv8a_vectors
    msr     VBAR_EL1, x0
    isb

    bl      Reset_Handler           /* C runtime + kmain (noreturn) */
.Lhang:
    wfi
    b       .Lhang

/* THE TABLES ARE IDENTITY-LINKED (virt_arm64.ld places .rodata.mmu_ident in .text.init):
 * a descriptor names its output by physical address, so every expression here has to be one.
 *
 * Whole and constant, both roots: entry 0 covers the device window that holds the GIC and
 * the PL011, entry 1 the DRAM this image links into. Every other entry is invalid, so a
 * stray access outside the two faults rather than reading a neighbour. TTBR0's is replaced by
 * a per-task root once a domain owns an address space; TTBR1's stays a constant.
 */
    .section .rodata.mmu_ident, "a"
    .balign 4096
    .global kickos_arm64_ident_table
kickos_arm64_ident_table:
    /* EL1-only: every device touch on this port goes through a syscall. */
    .quad   BLOCK + AF + ATTR_DEVICE + UXN + PXN    /* 0x0000_0000..0x4000_0000 */
    /* A table rather than a block, for the split above. DRAM_L1_ENTRY names this slot, and
       the refusals above rest on it being this one. */
    .quad   kickos_arm64_ram_table + TABLE
    .fill   L2_ENTRIES - DRAM_L1_ENTRY - 1, 8, 0

/* The high half, VA = PA + __kickos_arm64_va_base. One set of descriptors is both the
 * kernel window and the map of all physical RAM, so the level-1 indices are the ones above.
 * A SECOND ROOT and not the same page: TTBR0's becomes per-address-space once a domain owns
 * one, while this one is the half every space shares.
 */
    .balign 4096
    .global kickos_arm64_kernel_table
kickos_arm64_kernel_table:
    .quad   BLOCK + AF + ATTR_DEVICE + UXN + PXN
    .quad   kickos_arm64_kernel_ram_table + TABLE
    .fill   L2_ENTRIES - DRAM_L1_ENTRY - 1, 8, 0

/* Level 2 for 0x4000_0000..0x8000_0000 through TTBR0's boot root, one entry per 2 MiB.
 * The EL0 grants here are the identity map's own: .text.init runs untranslated against
 * these addresses, and nothing unprivileged is scheduled before its task's root replaces
 * this one.
 */
    .balign 4096
    .global kickos_arm64_ram_table
kickos_arm64_ram_table:
    /* EL1-ONLY, and nothing runs at EL0 under this root: a task's own root is installed
       before its first instruction, and the three other holders of this space (the fault
       reporter, aspace release, and the boot answer itself) all run privileged. PXN stays
       clear because kbanner reaches the app's build stamp at EL1 before any space exists. */
    /* Past the mapped span: invalid, so a stray access faults rather than aliasing. */
    L2_PARTITION_TABLE

/* The same DRAM through TTBR1, and the whole point of the second page: AP names no EL0
 * access and UXN forbids the fetch, so the half every address space shares grants an
 * unprivileged thread nothing. PXN stays clear on the code block alone, the kernel
 * executing out of it.
 */
    .balign 4096
    .global kickos_arm64_kernel_ram_table
kickos_arm64_kernel_ram_table:
    L2_PARTITION_TABLE

    .end
