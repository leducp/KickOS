/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RX72M startup: option-setting memory, the two vector tables, and _start. The fixed
 * table sits at 0xFFFFFF80 and its last word (0xFFFFFFFC) is the reset vector (UM sec.2.6.1,
 * sec.5.5.6).
 *
 * (RX72M Group User's Manual: Hardware, r01uh0804ej0120, Rev.1.20.)
 */

#include <kickos/chip_limits.h>   /* KICKOS_RX_INTB_ENTRIES: the INTB table size */

    .text

/* ------------------------------------------------------------------------- */
/* Option-setting memory (UM sec.7). All-ones = little-endian (MDE sec.7.2.5), WDT       */
/* stopped after reset (OFS0 sec.7.2.3), LVD/HOCO off (OFS1 sec.7.2.4). Emitted so the   */
/* boot config does not depend on erased-flash values.                                   */
/* ------------------------------------------------------------------------- */
    .section .ofsm, "a"
    .balign 4
    .word 0xFFFFFFFF            /* 0xFE7F5D00 MDE:  little-endian, linear bank */
    .word 0xFFFFFFFF            /* 0xFE7F5D04 OFS0: WDT/IWDT stopped after reset */
    .word 0xFFFFFFFF            /* 0xFE7F5D08 OFS1: voltage-detect + HOCO off */
    .word 0xFFFFFFFF            /* 0xFE7F5D0C reserved (write 1) */

/* ------------------------------------------------------------------------- */
/* Relocatable interrupt vector table (INTB). Slot n -> handler for INT/IRQ n. Slots     */
/* 0..15 carry no peripheral source. Slot 87 (TXI6) serves the console drain, or a        */
/* userspace TX driver after the console handover. Every other device slot goes to the    */
/* shared default entry, which asks the chip to demux: that serves the group vectors      */
/* (106..113).                                                                            */
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
    .rept (86 - 31)
    .word _kickos_rx_default_irq      /* 31..85 */
    .endr
    .word _kickos_rx_sci6_rxi_isr     /* 86: SCI6 RXI6 -> tier-1 line 86 */
    .word _kickos_rx_console_txi_isr  /* 87: SCI6 TXI6 -> console drain, or line 87 */
    .rept (KICKOS_RX_INTB_ENTRIES - 88)
    .word _kickos_rx_default_irq      /* 88..255, incl. the group vectors 106..113 */
    .endr

/* ------------------------------------------------------------------------- */
/* Fixed exception vector table @ 0xFFFFFF80 (EXTB reset). Offsets from ISA UM sec.5.5.  */
/* ------------------------------------------------------------------------- */
    .section .fvectors, "a"
    .balign 4
    .global _g_fvectors
_g_fvectors:
    .rept 20                          /* 0x00..0x4C: reserved */
    .word _rx_trap
    .endr
    .word _rx_ex_priv                 /* 0x50 privileged instruction */
    .word _rx_ex_access               /* 0x54 access exception (MPU) */
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
/* _start: reset entry. Supervisor mode, ISP undefined.                        */
/* ------------------------------------------------------------------------- */
    .text
    .global _start
_start:
    mov.l   #_istack, r0          /* U=0 at reset: R0 = ISP */
    mov.l   #_g_rvectors, r5
    mvtc    r5, intb
    bra.a   _rx_reset_handler

/* Exception entry shims: r1 = cause (the fixed-vector offset), r2 = the [PC][PSW] pair on   */
/* the ISP. bsr, not bra: the reporter RETURNS when it redirected the fault to the           */
/* thread-exit stub by rewriting the pair, and the RTE below is what lands there. bsr's own   */
/* push sits BELOW the pair, so r2 stays the frame address across the call.                   */
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
    mov.l   r0, r2
    bsr.a   _kickos_rx_fault_report
    rte                           /* -> kickos_thread_fault_exit, supervisor-on-USP */
