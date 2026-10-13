/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * i.MX 8M Plus EVK (quad Cortex-A53) startup. The image enters at the ELF entry point with the
 * MMU and caches off. A64 loads no SP from a vector table, so SP is set here; .data and .bss
 * are Reset_Handler's.
 *
 * The part hands over at EL3 with no BL31 under the image (QEMU's imx8mp-evk sets no PSCI
 * conduit), so the EL3 block programs what a lower EL cannot reach and erets to EL1: the kernel
 * is an EL1 kernel, EL3 having no TTBR1. EL1 entry is also accepted (a board booted through ATF
 * and U-Boot), skipping that block. EL2 is refused by name.
 *
 * The MMU comes up before any C: with SCTLR_EL1.M clear every access is Device-nGnRnE and
 * natural alignment is mandatory, while GCC and newlib stage wide copies through unaligned
 * q-register `ldur`/`stur`, so compiled code alignment-faults (EC 0x25, DFSC 0x21).
 *
 * Everything in .text.init is reached untranslated: an absolute literal holds a high virtual
 * address no walk can resolve yet and `adrp` to a high symbol is out of reach. x9 carries the
 * offset back to the physical address. The EL refusal reporter is linked into this section for
 * the same reason (chip_imx8mp.cc).
 */

#include <vmsa.h>
#include <boot_el1.inc>

#include <chip_layout.h>

#if ((KICKOS_LAYOUT_DRAM_SIZE) % L2_BLOCK_SIZE) != 0 || ((KICKOS_LAYOUT_DRAM_SIZE) / L2_BLOCK_SIZE) > L2_ENTRIES
#error "KICKOS_LAYOUT_DRAM_SIZE is not a whole number of L2_BLOCK_SIZE blocks within one level-2 table"
#endif
#define RAM_L2_BLOCKS   ((KICKOS_LAYOUT_DRAM_SIZE) / L2_BLOCK_SIZE)

/* The EL3 handover set (DDI 0487 M.b). SCR_EL3 bits [5:4] are RES1. NS moves the lower levels
 * to Non-secure, where this part's GIC-500 reports a single Security state and semihosting is
 * admitted. */
#define SCR_EL3_NS      (1 << 0)
#define SCR_EL3_RES1    (3 << 4)
#define SCR_EL3_RW      (1 << 10)       /* the next lower level is AArch64 */
#define SCR_EL3_VALUE   (SCR_EL3_NS | SCR_EL3_RES1 | SCR_EL3_RW)

/* SPSR for the eret: EL1h with DAIF masked. */
#define SPSR_EL3_EL1H   0x3c5

#define HCR_EL2_RW      (1 << 31)
#define CPTR_EL2_RES1   0x33ff
#define CNTHCTL_EL1PCTEN (1 << 0)
#define CNTHCTL_EL1PCEN  (1 << 1)

/* SCTLR_EL1's RES1 bits with M, C and I clear: the MMU comes up below, not here. */
#define SCTLR_EL1_RES1  0x30d00800

/* The system counter feeding CNTPCT_EL0, written into CNTFRQ_EL0 at EL3: the 24 MHz base
 * divided by three, which CNTFID0 itself reads (IMX8MPRM rev 3 section 4.11.4.1.1). It divides
 * a second exactly, keeping the ns-per-tick conversion lossless. */
#define KICKOS_IMX8MP_SYSCNT_HZ 8000000

    .section .text.init, "ax"
    .global _start
_start:
    KICKOS_ARM64_BOOT_ENTRY
    cmp     x0, #1
    b.eq    .Lat_el1
    cmp     x0, #3
    b.ne    .Lbad_el                /* EL2: refused by name, x0 still carries the level */

    /* --- EL3: what a lower EL cannot program for itself --------------------------- */

    msr     cptr_el3, xzr           /* CPTR_EL3.TFP resets UNKNOWN: it would trap the FP below */
    msr     mdcr_el3, xzr

    /* EL1's access to the ICC_* registers is gated at both higher levels: without
       ICC_SRE_EL2.Enable the first ICC_* write traps to an EL2 vector nothing has written. */
    mov     x0, #9                  /* SRE | Enable */
    msr     ICC_SRE_EL3, x0
    isb
    msr     ICC_SRE_EL2, x0
    isb

    ldr     x0, =HCR_EL2_RW         /* EL1 is AArch64; without it the eret targets AArch32 */
    msr     HCR_EL2, x0
    ldr     x0, =CPTR_EL2_RES1      /* the FP traps EL2 would otherwise impose on EL1 */
    msr     CPTR_EL2, x0

    /* CNTHCTL_EL2's two enables reset clear, sending every CNTPCT_EL0 and CNTP_* access at EL1
       to that same unwritten EL2 vector. */
    mov     x0, #(CNTHCTL_EL1PCTEN | CNTHCTL_EL1PCEN)
    msr     CNTHCTL_EL2, x0
    msr     CNTVOFF_EL2, xzr

    /* CNTFRQ_EL0 is writable at EL3 only and reads UNKNOWN out of reset. */
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
    /* SP_EL1 is UNKNOWN across the eret; the SP written at _start was the entry level's bank. */
    ldr     x0, =_estack
    sub     x0, x0, x9
    mov     sp, x0

    KICKOS_ARM64_BOOT_EL1

#include <boot_tables.inc>

    .end
