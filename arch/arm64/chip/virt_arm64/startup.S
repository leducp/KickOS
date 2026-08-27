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
 * 4 GiB reach. x9 carries the offset back to the physical address, and the last instruction
 * of the section is the branch that stops needing it.
 */

/* Level-1 block descriptor (4 KiB granule, so one block spans 1 GiB). */
#define BLOCK           (1 << 0)        /* valid, and a block rather than a table */
#define AF              (1 << 10)       /* access flag: 0 faults on first touch */
#define SH_INNER        (3 << 8)        /* inner shareable, for Normal memory */
#define ATTR_NORMAL     (0 << 2)        /* MAIR_EL1 Attr0 */
#define ATTR_DEVICE     (1 << 2)        /* MAIR_EL1 Attr1 */
#define TABLE           (3 << 0)        /* valid, and a table rather than a block */
#define UXN             (1 << 54)       /* never executable at EL0 */
#define PXN             (1 << 53)       /* never executable at EL1 */
#define AP_RW_EL0       (1 << 6)        /* AP=0b01: read/write at EL1 and at EL0 */
#define AP_RO_EL0       (3 << 6)        /* AP=0b11: read-only at EL1 and at EL0 */

/* THE CODE/DATA SPLIT IS FORCED. A region writable at EL0 is not executable at EL1, so
 * giving all of DRAM AP=0b01 for EL0 stacks makes the kernel's next instruction fetch a
 * permission fault (EC 0x21, IFSC 0x0D). One flat block cannot serve both levels, which
 * makes this a W^X split by arithmetic rather than by policy.
 *
 * 2 MiB level-2 blocks are the granularity, so the linker pads the code to that boundary
 * and ASSERTS the fit; a 4 KiB boundary would need a third level filled at runtime.
 */
#define L2_BLOCK_SIZE   0x200000
#define RAM_L2_BLOCKS   32              /* 64 MiB of DRAM in 2 MiB blocks */

/* MAIR_EL1: Attr0 Normal write-back read/write-allocate, Attr1 Device-nGnRnE, Attr2 Normal
 * outer and inner non-cacheable (DDI 0487 M.b section D24.2.126: oooo and iiii both 0b0100,
 * and neither half may be 0b0000, which is what would make the attribute Device instead).
 * All three slots are filled here so arch_aspace_memtype_support can honour every type
 * rather than refusing one; a quietly downgraded type is a cacheable view of a DMA buffer.
 */
#define MAIR_VALUE      0x4400FF

/* TCR_EL1. T0SZ and T1SZ are both 25, so each half is a 39-bit VA whose walk starts at
 * level 1 and reaches the blocks below. IPS 0b010 is the A53's 40-bit output (F7).
 *
 * TG1's 4 KiB encoding is 0b10 and TG0's is 0b00 (DDI 0487 M.b, TCR_EL1): the two granule
 * fields do NOT share an encoding, so TG0's constant is the wrong value here.
 */
#define TCR_T0SZ        25
#define TCR_IRGN0_WB    (1 << 8)
#define TCR_ORGN0_WB    (1 << 10)
#define TCR_SH0_INNER   (3 << 12)
#define TCR_TG0_4K      (0 << 14)
#define TCR_T1SZ        (25 << 16)
#define TCR_IRGN1_WB    (1 << 24)
#define TCR_ORGN1_WB    (1 << 26)
#define TCR_SH1_INNER   (3 << 28)
#define TCR_TG1_4K      (2 << 30)
#define TCR_IPS_40BIT   (2 << 32)
#define TCR_VALUE       (TCR_T0SZ | TCR_IRGN0_WB | TCR_ORGN0_WB | TCR_SH0_INNER | \
                         TCR_TG0_4K | TCR_T1SZ | TCR_IRGN1_WB | TCR_ORGN1_WB | \
                         TCR_SH1_INNER | TCR_TG1_4K | TCR_IPS_40BIT)

#define SCTLR_M         (1 << 0)
#define SCTLR_C         (1 << 2)
#define SCTLR_I         (1 << 12)

    .section .text.init, "ax"
    .global _start
_start:
    ldr     x9, =__kickos_arm64_va_base

    /* SP first, so the refusal path below can call C. */
    ldr     x0, =_estack            /* boot/exception stack: top of RAM, 16-byte aligned */
    sub     x0, x0, x9
    mov     sp, x0

    /* Stated rather than inherited: the vector table's first four slots are unreachable
       only while SPSel is 1, and a reset value nothing writes is an assumption. */
    msr     SPSel, #1
    isb

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

    /* The level stays in x0 as the argument. The MMU is still off here, so the callee runs
       at its physical alias and must touch no unaligned address. */
.Lbad_el:
    ldr     x1, =kickos_arm64_bad_el /* noreturn (chip_virt_arm64.cc) */
    sub     x1, x1, x9
    br      x1

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
    /* A table rather than a block, for the split above. */
    .quad   kickos_arm64_ram_table + TABLE
    .fill   510, 8, 0

/* The high half, VA = PA + __kickos_arm64_va_base. One set of descriptors is both the
 * kernel window and the map of all physical RAM, so the level-1 indices are the ones above.
 * A SECOND ROOT and not the same page: TTBR0's becomes per-address-space once a domain owns
 * one, while this one is the half every space shares.
 *
 * EL0 permissions match TTBR0's: the identity map grants EL0 the same frames until T6
 * replaces it with a per-process root.
 */
    .balign 4096
    .global kickos_arm64_kernel_table
kickos_arm64_kernel_table:
    .quad   BLOCK + AF + ATTR_DEVICE + UXN + PXN
    .quad   kickos_arm64_ram_table + TABLE
    .fill   510, 8, 0

/* Level 2 for 0x4000_0000..0x8000_0000, one entry per 2 MiB, reached from both roots. */
    .balign 4096
    .global kickos_arm64_ram_table
kickos_arm64_ram_table:
    /* Code and rodata. UXN stays clear: an EL0 thread runs its own text out of here. */
    .quad   __kickos_dram_base + BLOCK + AF + ATTR_NORMAL + SH_INNER + AP_RO_EL0
    /* Writable, executable at neither level. */
    .set    _blk, 1
    .rept   RAM_L2_BLOCKS - 1
    .quad   __kickos_dram_base + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NORMAL + \
            SH_INNER + AP_RW_EL0 + UXN + PXN
    .set    _blk, _blk + 1
    .endr
    /* Past the end of DRAM: invalid, so a stray access faults rather than aliasing. */
    .fill   512 - RAM_L2_BLOCKS, 8, 0

    .end
