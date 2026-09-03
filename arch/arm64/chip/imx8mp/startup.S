/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * i.MX 8M Plus EVK (quad Cortex-A53) startup. `-kernel <elf>` loads the image into DDR at
 * 0x4000_0000 and enters at the ELF entry point with the MMU and caches off. A64 loads no SP
 * from a vector table, so SP is set here; .data and .bss are Reset_Handler's.
 *
 * THIS PART HANDS OVER AT EL3 AND THIS FILE STANDS IN FOR THE FIRMWARE THAT WOULD NOT.
 * `qemu-system-aarch64 -M imx8mp-evk` sets no PSCI conduit and leaves EL3 implemented, so an
 * image loaded with `-kernel` is the first thing to run on the part and there is no BL31 under
 * it. The kernel itself is an EL1 kernel and cannot be anything else: EL3 has no TTBR1, and
 * the whole port rests on the kernel owning the high half while a process owns the low one.
 * So the block below programs what a lower EL cannot reach for itself and erets to EL1.
 *
 * EL1 ENTRY IS ALSO ACCEPTED, which is what a board booted through ATF and U-Boot arrives at;
 * the EL3 block is then skipped and its work has already been done. EL2 is refused by name.
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
 * for the same reason (chip_imx8mp.cc).
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
#define AP_RW_EL1       (0 << 6)        /* AP=0b00: read/write at EL1, no access at EL0 */
#define AP_RO_EL1       (2 << 6)        /* AP=0b10: read-only at EL1, no access at EL0 */

/* THE TWO ROOTS DESCRIBE THE SAME DRAM WITH DIFFERENT PERMISSIONS, so each carries its own
 * level-2 table. TTBR1's grants EL0 nothing: an unprivileged thread reaches what its own
 * TTBR0 root maps and nothing else, which is what makes the high half the kernel's alone.
 * TTBR0's boot table keeps the EL0 grants the identity map was built with; a per-process
 * root replaces it before any unprivileged thread runs.
 *
 * THE CODE/DATA SPLIT IS FORCED. A region writable at EL0 is not executable at EL1, so
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
 * level 1 and reaches the blocks below. IPS 0b010 is the A53's 40-bit output.
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

/* The EL3 handover set. DDI 0487 M.b names every field; what is written and why is at the
 * block that uses them.
 *
 * SCR_EL3 bits [5:4] are RES1, so a value without them is not the register's reset shape.
 * NS moves the lower levels to Non-secure, which is where this part's GIC-500 reports a
 * single Security state and where semihosting is admitted. */
#define SCR_EL3_NS      (1 << 0)
#define SCR_EL3_RES1    (3 << 4)
#define SCR_EL3_RW      (1 << 10)       /* the next lower level is AArch64 */
#define SCR_EL3_VALUE   (SCR_EL3_NS | SCR_EL3_RES1 | SCR_EL3_RW)

/* SPSR for the eret: EL1h, so SP_EL1 rather than SP_EL0, with DAIF masked. Interrupts first
 * reach the core through the initial thread's SPSR, exactly as on a part whose firmware
 * hands over masked. */
#define SPSR_EL3_EL1H   0x3c5

#define HCR_EL2_RW      (1 << 31)
#define CPTR_EL2_RES1   0x33ff
#define CNTHCTL_EL1PCTEN (1 << 0)
#define CNTHCTL_EL1PCEN  (1 << 1)

/* SCTLR_EL1's RES1 bits with M, C and I clear: the MMU comes up below, not here. */
#define SCTLR_EL1_RES1  0x30d00800

/* The system counter feeding CNTPCT_EL0, written into CNTFRQ_EL0 at EL3: a 24 MHz base
 * divided by three, which is the value CNTFID0 itself reads (IMX8MPRM rev 3 section
 * 4.11.4.1.1, reset 0x007A_1200). A second divides exactly by it, which is what keeps the
 * ns-per-tick conversion lossless. */
#define KICKOS_IMX8MP_SYSCNT_HZ 8000000

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
    b.eq    .Lat_el1
    cmp     x0, #3
    b.ne    .Lbad_el                /* EL2: refused by name, x0 still carries the level */

    /* --- EL3: what a lower EL cannot program for itself ---------------------------
       Every register here is written because EL1 either cannot reach it or would trap
       through it. x9 and the literals survive the eret; general registers are not
       cleared by it. */

    msr     cptr_el3, xzr           /* CPTR_EL3.TFP resets UNKNOWN: it would trap the FP below */
    msr     mdcr_el3, xzr

    /* The GICv3 CPU interface is a set of system registers, and EL1's access to them is
       gated at BOTH higher levels: ICC_SRE_EL3.Enable, and ICC_SRE_EL2.Enable because this
       part implements EL2 whether or not anything executes there. Without the second, the
       backend's first ICC_* write traps to an EL2 vector nothing has written. */
    mov     x0, #9                  /* SRE | Enable */
    msr     ICC_SRE_EL3, x0
    isb
    msr     ICC_SRE_EL2, x0
    isb

    ldr     x0, =HCR_EL2_RW         /* EL1 is AArch64; without it the eret targets AArch32 */
    msr     HCR_EL2, x0
    ldr     x0, =CPTR_EL2_RES1      /* the FP traps EL2 would otherwise impose on EL1 */
    msr     CPTR_EL2, x0

    /* THE GENERIC TIMER IS THE KERNEL'S WHOLE TIMEBASE AND EL1 CANNOT REACH IT BY DEFAULT:
       CNTHCTL_EL2's two enables reset clear, which sends every CNTPCT_EL0 and CNTP_* access
       at EL1 to that same unwritten EL2 vector. */
    mov     x0, #(CNTHCTL_EL1PCTEN | CNTHCTL_EL1PCEN)
    msr     CNTHCTL_EL2, x0
    msr     CNTVOFF_EL2, xzr

    /* CNTFRQ_EL0 is WRITABLE AT EL3 ONLY and reads UNKNOWN out of reset, so programming it
       is the firmware role this block is standing in for. arch_init reads it back and
       refuses a value that does not divide a second exactly. */
    ldr     x0, =KICKOS_IMX8MP_SYSCNT_HZ
    msr     CNTFRQ_EL0, x0

    ldr     x0, =SCTLR_EL1_RES1     /* MMU and caches off, RES1 bits set: EL1's reset is ours */
    msr     SCTLR_EL1, x0

    ldr     x0, =SCR_EL3_VALUE
    msr     SCR_EL3, x0
    ldr     x0, =SPSR_EL3_EL1H
    msr     SPSR_EL3, x0
    adr     x0, .Lat_el1
    msr     ELR_EL3, x0
    isb
    eret

.Lat_el1:
    /* SP_EL1 IS UNKNOWN ACROSS THE ERET, and the SP written at _start was the entry level's
       own bank. Re-seated rather than assumed, and idempotent on the EL1 entry path. */
    ldr     x0, =_estack
    sub     x0, x0, x9
    mov     sp, x0

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
    b       kickos_arm64_bad_el     /* noreturn (chip_imx8mp.cc) */

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

/* THE TABLES ARE IDENTITY-LINKED (imx8mp.ld places .rodata.mmu_ident in .text.init):
 * a descriptor names its output by physical address, so every expression here has to be one.
 *
 * Whole and constant, both roots: entry 0 covers the device gigabyte that holds this part's
 * GIC-500 at 0x3880_0000 and its UART1 at 0x3086_0000, entry 1 the DDR this image links into.
 * Every other entry is invalid, so a stray access outside the two faults rather than reading
 * a neighbour. TTBR0's is replaced by
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
 */
    .balign 4096
    .global kickos_arm64_kernel_table
kickos_arm64_kernel_table:
    .quad   BLOCK + AF + ATTR_DEVICE + UXN + PXN
    .quad   kickos_arm64_kernel_ram_table + TABLE
    .fill   510, 8, 0

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
    .quad   __kickos_dram_base + BLOCK + AF + ATTR_NORMAL + SH_INNER + AP_RO_EL1 + UXN
    .set    _blk, 1
    .rept   RAM_L2_BLOCKS - 1
    .quad   __kickos_dram_base + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NORMAL + \
            SH_INNER + AP_RW_EL1 + UXN + PXN
    .set    _blk, _blk + 1
    .endr
    /* Past the end of DRAM: invalid, so a stray access faults rather than aliasing. */
    .fill   512 - RAM_L2_BLOCKS, 8, 0

/* The same DRAM through TTBR1, and the whole point of the second page: AP names no EL0
 * access and UXN forbids the fetch, so the half every address space shares grants an
 * unprivileged thread nothing. PXN stays clear on the code block alone, the kernel
 * executing out of it.
 */
    .balign 4096
    .global kickos_arm64_kernel_ram_table
kickos_arm64_kernel_ram_table:
    .quad   __kickos_dram_base + BLOCK + AF + ATTR_NORMAL + SH_INNER + AP_RO_EL1 + UXN
    .set    _blk, 1
    .rept   RAM_L2_BLOCKS - 1
    .quad   __kickos_dram_base + (_blk * L2_BLOCK_SIZE) + BLOCK + AF + ATTR_NORMAL + \
            SH_INNER + AP_RW_EL1 + UXN + PXN
    .set    _blk, _blk + 1
    .endr
    .fill   512 - RAM_L2_BLOCKS, 8, 0

    .end
