/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RX72M startup: option-setting memory, the two vector tables, and _start.
 *   - .ofsm     : MDE/OFS0/OFS1 option words (little-endian, WDT/LVD off).
 *   - .rvectors : the 256-entry relocatable interrupt table (INTB); the syscall
 *                 trap sits at slot 1, the CMTW0 compare-match ISR at slot 30.
 *   - .fvectors : the fixed exception table at 0xFFFFFF80 whose last word
 *                 (0xFFFFFFFC) is the reset vector (UM §2.6.1, §5.5.6).
 *   - _start    : set the ISP + INTB, then hand off to the C runtime init.
 *
 * (RX72M Group User's Manual: Hardware, r01uh0804ej0120, Rev.1.20.)
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the INTB table size */

    .text

/* ------------------------------------------------------------------------- */
/* Option-setting memory (UM §7). All-ones = little-endian (MDE §7.2.5), WDT   */
/* stopped after reset (OFS0 §7.2.3), LVD/HOCO off (OFS1 §7.2.4). Emitted so    */
/* the boot config is deterministic, not dependent on erased-flash values.      */
/* ------------------------------------------------------------------------- */
    .section .ofsm, "a"
    .balign 4
    .word 0xFFFFFFFF            /* 0xFE7F5D00 MDE:  little-endian, linear bank */
    .word 0xFFFFFFFF            /* 0xFE7F5D04 OFS0: WDT/IWDT stopped after reset */
    .word 0xFFFFFFFF            /* 0xFE7F5D08 OFS1: voltage-detect + HOCO off */
    .word 0xFFFFFFFF            /* 0xFE7F5D0C reserved (write 1) */

/* ------------------------------------------------------------------------- */
/* Relocatable interrupt vector table (INTB). Slot n -> handler for INT/IRQ n.  */
/* Slots 0..15 carry no peripheral source; slot 1 = syscall trap (regs.h        */
/* SYSCALL_VECTOR), slot 27 = SWINT (the deferred context switch, PendSV analog),*/
/* slot 30 = CMTW0 compare match (CMWI0). Slot 26 (SWINT2) stays the default     */
/* handler -- it is the test-injection line (arch_irq_inject).                   */
/* ------------------------------------------------------------------------- */
    .section .rvectors, "a"
    .balign 4
    .global _g_rvectors
_g_rvectors:
    .word _rx_trap                    /* 0: BRK (unused) */
    .word _kickos_rx_syscall_trap     /* 1: syscall trap (INT #1) */
    .rept 25
    .word _kickos_rx_default_irq      /* 2..26 (26 = SWINT2, test injection) */
    .endr
    .word _kickos_rx_pendsw           /* 27: SWINT -> deferred context switch */
    .word _kickos_rx_default_irq      /* 28 */
    .word _kickos_rx_default_irq      /* 29 */
    .word _kickos_rx_timer_isr        /* 30: CMTW0 compare match (CMWI0) */
    .rept (KICKOS_MAX_IRQ - 31)
    .word _kickos_rx_default_irq      /* 31..255 */
    .endr

/* ------------------------------------------------------------------------- */
/* Fixed exception vector table @ 0xFFFFFF80 (EXTB reset). Offsets from ISA UM  */
/* §5.5: privileged +0x50, access +0x54, undefined +0x5C, address +0x60, FP     */
/* +0x64, NMI +0x78, reset +0x7C (= 0xFFFFFFFC).                                */
/* ------------------------------------------------------------------------- */
    .section .fvectors, "a"
    .balign 4
    .global _g_fvectors
_g_fvectors:
    .rept 20                          /* 0x00..0x4C: reserved */
    .word _rx_trap
    .endr
    .word _rx_trap                    /* 0x50 privileged instruction */
    .word _rx_trap                    /* 0x54 access exception (MPU: M2) */
    .word _rx_trap                    /* 0x58 reserved */
    .word _rx_trap                    /* 0x5C undefined instruction */
    .word _rx_trap                    /* 0x60 address exception */
    .word _rx_trap                    /* 0x64 floating-point */
    .word _rx_trap                    /* 0x68 reserved */
    .word _rx_trap                    /* 0x6C reserved */
    .word _rx_trap                    /* 0x70 reserved */
    .word _rx_trap                    /* 0x74 reserved */
    .word _rx_trap                    /* 0x78 non-maskable interrupt */
    .word _start                      /* 0x7C reset (0xFFFFFFFC) */

/* ------------------------------------------------------------------------- */
/* _start: reset entry. Supervisor mode, ISP undefined. Set the ISP and the    */
/* INTB, then enter the C runtime init (never returns).                        */
/* ------------------------------------------------------------------------- */
    .text
    .global _start
_start:
    mov.l   #_istack, r0          /* ISP = top of SRAM (U=0 at reset -> R0 = ISP) */
    mov.l   #_g_rvectors, r5
    mvtc    r5, intb              /* relocatable interrupt table base */
    bra.a   _rx_reset_handler     /* C runtime init + kmain (noreturn) */

/* A catch-all trap for exceptions not yet handled (M1.x): halt. */
    .global _rx_trap
_rx_trap:
    bra     _rx_trap
