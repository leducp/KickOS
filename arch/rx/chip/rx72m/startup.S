/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RX72M startup: option-setting memory, the two vector tables, and _start.
 *   - .ofsm     : MDE/OFS0/OFS1 option words (little-endian, WDT/LVD off).
 *   - .rvectors : the 256-entry relocatable interrupt table (INTB); the syscall
 *                 trap sits at slot 1, the CMTW0 compare-match ISR at slot 30.
 *   - .fvectors : the fixed exception table at 0xFFFFFF80 whose last word
 *                 (0xFFFFFFFC) is the reset vector (UM sec.2.6.1, sec.5.5.6).
 *   - _start    : set the ISP + INTB, then hand off to the C runtime init.
 *
 * (RX72M Group User's Manual: Hardware, r01uh0804ej0120, Rev.1.20.)
 */

#include <kickos/board_config.h>   /* KICKOS_MAX_IRQ: the INTB table size */

    .text

/* ------------------------------------------------------------------------- */
/* Option-setting memory (UM sec.7). All-ones = little-endian (MDE sec.7.2.5), WDT   */
/* stopped after reset (OFS0 sec.7.2.3), LVD/HOCO off (OFS1 sec.7.2.4). Emitted so    */
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
/* slot 30 = CMTW0 compare match (CMWI0), slot 87 = SCI6 TXI6 (buffered console  */
/* drain). Slot 26 = SWINT2, the software-inject doorbell: arch_irq_inject latches  */
/* the logical line + pends SWINT2, and kickos_rx_swint2 dispatches it.            */
/* ------------------------------------------------------------------------- */
    .section .rvectors, "a"
    .balign 4
    .global _g_rvectors
_g_rvectors:
    .word _rx_trap                    /* 0: BRK (unused) */
    .word _kickos_rx_syscall_trap     /* 1: syscall trap (INT #1) */
    .rept 24
    .word _kickos_rx_default_irq      /* 2..25 */
    .endr
    .word _kickos_rx_swint2           /* 26: SWINT2 -> software-inject doorbell */
    .word _kickos_rx_pendsw           /* 27: SWINT -> deferred context switch */
    .word _kickos_rx_default_irq      /* 28 */
    .word _kickos_rx_default_irq      /* 29 */
    .word _kickos_rx_timer_isr        /* 30: CMTW0 compare match (CMWI0) */
    .rept (87 - 31)
    .word _kickos_rx_default_irq      /* 31..86 */
    .endr
    .word _kickos_rx_console_txi_isr  /* 87: SCI6 TXI6 -> buffered console drain */
    .rept (KICKOS_MAX_IRQ - 88)
    .word _kickos_rx_default_irq      /* 88..255 */
    .endr

/* ------------------------------------------------------------------------- */
/* Fixed exception vector table @ 0xFFFFFF80 (EXTB reset). Offsets from ISA UM  */
/* sec.5.5: privileged +0x50, access +0x54, undefined +0x5C, address +0x60, FP     */
/* +0x64, NMI +0x78, reset +0x7C (= 0xFFFFFFFC).                                */
/* ------------------------------------------------------------------------- */
    .section .fvectors, "a"
    .balign 4
    .global _g_fvectors
_g_fvectors:
    .rept 20                          /* 0x00..0x4C: reserved */
    .word _rx_trap
    .endr
    .word _rx_ex_priv                 /* 0x50 privileged instruction */
    .word _rx_ex_access               /* 0x54 access exception (MPU: M2) */
    .word _rx_trap                    /* 0x58 reserved */
    .word _rx_ex_undef                /* 0x5C undefined instruction */
    .word _rx_ex_addr                 /* 0x60 address exception */
    .word _rx_ex_fp                   /* 0x64 floating-point */
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

/* Exception entry shims (fixed vector table). On an RX exception the CPU pushes  */
/* PC then PSW to the ISP (so [R0]=PC, 4[R0]=PSW) and clears U (R0 = ISP). Each    */
/* shim tags the cause (its fixed-vector offset) in r1, loads PC/PSW into r2/r3,   */
/* and tail-branches to the C reporter (dumps context, then never returns).       */
    .global _rx_ex_priv
_rx_ex_priv:
    mov     #0x50, r1
    bra     1f
    .global _rx_ex_access
_rx_ex_access:
    mov     #0x54, r1
    bra     1f
    .global _rx_ex_undef
_rx_ex_undef:
    mov     #0x5C, r1
    bra     1f
    .global _rx_ex_addr
_rx_ex_addr:
    mov     #0x60, r1
    bra     1f
    .global _rx_ex_fp
_rx_ex_fp:
    mov     #0x64, r1
    bra     1f
/* Catch-all for the reserved slots, BRK, and NMI: cause 0 ("trap"). */
    .global _rx_trap
_rx_trap:
    mov     #0, r1
1:
    mov.l   [r0], r2              /* stacked PC  */
    mov.l   4[r0], r3             /* stacked PSW */
    bra.a   _kickos_rx_fault_report
