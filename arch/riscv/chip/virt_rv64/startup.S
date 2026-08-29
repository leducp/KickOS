/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * QEMU `virt` (RISC-V RV64IMAC) startup. QEMU with `-bios none -kernel <elf>` loads each
 * PT_LOAD segment at its p_paddr and enters at the LOWEST of them, so _start runs at
 * 0x8000_0000 in MACHINE mode while the image's VMAs are high (virt_rv64.ld).
 *
 * EVERYTHING IN .text.init IS REACHED UNTRANSLATED. medany addressing is PC-relative, so `la`
 * here yields the PHYSICAL alias of its target, which is exactly what satp and a
 * descriptor's output address need. A high address is therefore reached the one way that does
 * not go through the PC: a link-time absolute word.
 *
 * THE REGIME AND THE PRIVILEGE CHANGE TOGETHER. satp is written in machine mode, where it
 * governs no access, and takes effect on the mret; mepc holds the landing pad's VIRTUAL
 * address. So no instruction has to be valid on both sides of the switch, and the only claim
 * the ordering rests on is that the mret target is mapped. The PMP grant and the delegation
 * come first: an entry that does not permit the walk denies every S-mode access, and an
 * undelegated page fault goes to an mtvec nothing wrote.
 *
 * THE PMP GRANT IS OWED BY A PORT WITH NO REGION BACKEND AT ALL. Once any PMP entry is
 * implemented, every S-mode and U-mode access fails unless an entry permits it. Entry 0 below
 * is the one machine-mode write this translating port keeps.
 *
 * THE TIMER GRANT IS MACHINE-MODE WORK TOO, which is why the timebase's bring-up is split
 * across this file and the chip's arch_timer_arm: menvcfg.STCE and mcounteren.TM are machine
 * CSRs, and without them an S-mode write of stimecmp traps and an S-mode read of time traps.
 *
 * ONE ROOT, NOT ONE PER PRIVILEGE LEVEL (docs/design-m6-mmu.md R2.2). The pair existed because
 * a flat link put kernel text and app text on the same page, and the ISA gives a page one U
 * bit. The app now links into a level-2 slot of its own, so the U bit follows the half and one
 * root serves both privilege levels: satp is written at each space ACTIVATION and not at each
 * privilege transition.
 *
 * THE KERNEL WINDOW IS A LEVEL-1 TABLE AND NOT A 1 GiB LEAF (docs/design-m6-mmu.md R2.3), which
 * is what lets the kernel's own mapping stop being writable and executable at once: one 2 MiB
 * leaf carries the KTEXT region read-execute and every leaf above it is read-write. The
 * boundary is the region boundary virt_rv64.ld already draws, so no finer level is needed, and
 * the transient window's leaf table is the one page the kernel writes after boot, which is why
 * .mmu_leaves lives in the writable half while .mmu_boot does not.
 *
 * THE LEVEL COUNT IS NOT WRITTEN HERE (docs/design-m6-mmu.md R3). It is derived from the
 * configured paging mode in <kickos/arch/rv64_paging.h>, which the map editor derives its own
 * walk from, so the two sides of the chip<->arch seam cannot disagree about a depth. What a
 * mode with more levels costs is one table page per added level and one non-leaf entry per
 * added level for the loop below to fill; the tables at level 2 and under it, the addresses
 * the linker script uses and everything else in this file are the same in every mode.
 *
 * BOTH VECTORS ARE WRITTEN HERE AND NEITHER HAS AN ARCHITECTURAL RESET VALUE. mtvec goes in
 * FIRST, before any other machine CSR, which is what covers the case no readback can reach: a
 * hart that does not IMPLEMENT one of the machine CSRs below takes an illegal instruction on
 * the write, and with mtvec already installed that lands in a named refusal instead of an
 * arbitrary address. The readbacks after each write cover the other case, a CSR that exists
 * and refused the VALUE (WARL). kickos_rv64_boot_high installs a provisional stvec of the same
 * shape before it calls Reset_Handler, whose own .data copy and app .bss zero would otherwise
 * take a page fault to an stvec of zero; kickos_rv64_init replaces it with the real entry.
 *
 * gp IS SEATED PAST THE SWITCH, at its high value. Nothing between _start and the landing pad
 * may make a gp-relative access, and norelax is what keeps the linker from turning one of the
 * loads below into one.
 */

#include <kickos/arch/rv64_paging.h>

#include "boot_layout.ld.h"

/* Page-table entry bits (RISC-V Privileged ISA, "Sv39 Page Table Entry"; the format is the
 * same in every mode on this ladder). */
#define PTE_V   (1 << 0)
#define PTE_R   (1 << 1)
#define PTE_W   (1 << 2)
#define PTE_X   (1 << 3)
#define PTE_U   (1 << 4)
#define PTE_G   (1 << 5)
#define PTE_A   (1 << 6)
#define PTE_D   (1 << 7)

/* A level-2 leaf spans 1 GiB and a level-1 leaf 2 MiB; the unused low PPN fields of each must
 * be zero, so an output address is aligned to the span it names. A larger mapping is not a
 * second granule (docs/design-m6-mmu.md F7): the granule stays the 4 KiB
 * arch_aspace_granule reports.
 */
#define GIGAPAGE 0x40000000
#define MEGAPAGE 0x200000
#define PTE_LEAF(pa, rights) \
    ((((pa) >> 12) << 10) + PTE_V + PTE_A + PTE_D + PTE_G + (rights))

#define VPN2(va)    KICKOS_RV64_VPN(va, 2)
#define VPN1(va)    KICKOS_RV64_VPN(va, 1)
#define VPN0(va)    KICKOS_RV64_VPN(va, 0)
#define MMIO_VPN2   VPN2(KICKOS_RV64_VA_BASE)
#define KERNEL_VPN2 VPN2(KICKOS_RV64_VA_BASE + KICKOS_RV64_DRAM_BASE)
#define WINDOW_VPN2 VPN2(KICKOS_RV64_WINDOW_VA)
#define WINDOW_VPN1 VPN1(KICKOS_RV64_WINDOW_VA)
#define WINDOW_VPN0 VPN0(KICKOS_RV64_WINDOW_VA)

/* THE TABLES ABOVE LEVEL 2, one per level the mode adds over Sv39. Every kernel-half address
 * takes the SAME index at every one of them, KICKOS_RV64_VA_BASE having every bit above 32
 * set, so ONE slot number serves the whole chain and the guard below holds the three addresses
 * to it. That is why the slot is read at level 3 and not per level: rv64_paging.h admits at
 * most four levels, so level 3 is the only one above 2 there is.
 */
#define UPPER_TABLES (KICKOS_RV64_PAGE_LEVELS - 3)
#define UPPER_SLOT   KICKOS_RV64_VPN(KICKOS_RV64_VA_BASE, 3)

/* The slot of the ROOT table an address takes, which is level 2 at Sv39 and deeper above it. */
#define ROOT_SLOT(va) KICKOS_RV64_VPN(va, KICKOS_RV64_LEVEL_ROOT)

/* A kernel-half address must be canonical in the configured mode: every bit above the mode's
 * virtual width equals the top translated bit. The mode is what decides where that boundary
 * falls, so an address canonical under one mode is not automatically canonical under the next.
 */
#define HIGH_CANONICAL(va) \
    (((va) >> (KICKOS_RV64_VA_BITS - 1)) == ((1 << (65 - KICKOS_RV64_VA_BITS)) - 1))

/* The read-execute half is a whole number of level-1 leaves of the kernel window. */
#define KTEXT_MEGAPAGES (KICKOS_RV64_KTEXT_SIZE / MEGAPAGE)

/* Written and read back at the top word of the DRAM the layout claims. Both halves differ, so
 * a bus that answers with the address or with a stuck value fails the compare. */
#define RAM_PROBE_PATTERN 0x5A5AA5A5

.if (KICKOS_RV64_DRAM_BASE & (GIGAPAGE - 1)) != 0
.error "KICKOS_RV64_DRAM_BASE is not 1 GiB aligned: no level-2 leaf can name it as an output"
.endif
.if (KICKOS_RV64_VA_BASE & (GIGAPAGE - 1)) != 0
.error "KICKOS_RV64_VA_BASE is not a multiple of 1 GiB: the kernel window would straddle two level-2 entries"
.endif
.if KICKOS_RV64_DRAM_SIZE > GIGAPAGE
.error "KICKOS_RV64_DRAM_SIZE outgrows the one level-2 slot spent on it"
.endif
.if ROOT_SLOT(KICKOS_RV64_APP_VA_BASE) == ROOT_SLOT(KICKOS_RV64_VA_BASE) || ROOT_SLOT(KICKOS_RV64_APP_VA_BASE) == ROOT_SLOT(KICKOS_RV64_VA_BASE + KICKOS_RV64_DRAM_BASE) || ROOT_SLOT(KICKOS_RV64_APP_VA_BASE) == ROOT_SLOT(KICKOS_RV64_WINDOW_VA)
.error "KICKOS_RV64_APP_VA_BASE lands in a root slot the boot root already fills: a per-space mapping there would edit a table every space shares"
.endif
.if !HIGH_CANONICAL(KICKOS_RV64_VA_BASE) || !HIGH_CANONICAL(KICKOS_RV64_WINDOW_VA)
.error "the kernel half is not a canonical address in the configured paging mode: the walk faults on the sign bits before it reaches a table"
.endif
.if UPPER_TABLES != 0 && (UPPER_SLOT != KICKOS_RV64_VPN(KICKOS_RV64_VA_BASE + KICKOS_RV64_DRAM_BASE, 3) || UPPER_SLOT != KICKOS_RV64_VPN(KICKOS_RV64_WINDOW_VA, 3))
.error "the devices, the kernel window and the transient window take different slots above level 2: the one chain _start fills reaches only one of them"
.endif
.if KICKOS_RV64_KERNEL_WINDOW_SIZE != GIGAPAGE
.error "KICKOS_RV64_KERNEL_WINDOW_SIZE is not the span of the kernel-window level-1 table"
.endif
.if (KICKOS_RV64_KTEXT_SIZE % MEGAPAGE) != 0 || KTEXT_MEGAPAGES == 0
.error "KICKOS_RV64_KTEXT_SIZE is not a whole number of 2 MiB level-1 leaves: the read-execute half cannot be described"
.endif
.if KTEXT_MEGAPAGES >= 512
.error "KICKOS_RV64_KTEXT_SIZE fills the kernel window, leaving no read-write leaf for kernel data"
.endif
.if WINDOW_VPN2 == MMIO_VPN2 || WINDOW_VPN2 == KERNEL_VPN2
.error "KICKOS_RV64_WINDOW_VA lands in a level-2 slot the boot tables already fill"
.endif
.if WINDOW_VPN1 != 0 || WINDOW_VPN0 != 0
.error "KICKOS_RV64_WINDOW_VA is not level-1 aligned: the window's two tables are entered at index 0"
.endif

    .section .text.init, "ax"
    .global _start
_start:
    .option push
    .option norelax

    /* a1 carries the DTB the loader passed; nothing reads it, and parsing it is the mechanism
       that would replace the boot_layout.ld.h constants this file derives every figure from. */

    /* mtvec BEFORE THE FIRST OTHER MACHINE-CSR WRITE, and that ordering is the point. RISC-V
       defines no reset value for it, so until this instruction runs every machine-mode trap
       goes to an arbitrary address, which is 0 on this emulator and a silent trap loop. With
       it installed first, a hart that does not IMPLEMENT one of the CSRs below takes its
       illegal instruction into .Lmtrap instead: that is the case no readback can reach, since
       the readback is downstream of the write that traps. DIRECT mode, bits 1:0 = 00, and
       .Lmtrap is 4-byte aligned. */
    la      t0, .Lmtrap
    csrw    mtvec, t0

    /* PMP entry 0: NAPOT over the whole address space, read/write/execute. */
    li      t0, -1
    srli    t0, t0, 10
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    /* THE WARL CASE, which mtvec does not cover: a CSR that exists and refused the VALUE. The
       cfg byte is WARL per entry, and an accepted pmpaddr0 beside a refused pmpcfg0 leaves
       entry 0 unconfigured, which denies every S-mode access with nothing to read back later.
       BOTH reading back zero is PMP unimplemented, where every access is permitted and there
       is no grant to make. */
    csrr    t0, pmpcfg0
    csrr    t1, pmpaddr0
    bnez    t0, 1f
    beqz    t1, 1f
    la      a0, .Lpmp_refused
    la      a1, .Llbl_pmpcfg0
    mv      a2, t0
    la      a3, .Llbl_pmpaddr0
    mv      a4, t1
    j       .Lm_refuse
1:

    /* medeleg is WARL per bit: this hart refuses bit 11 and bit 14, storing 0xb7ff of an
       all-sixteen 0xffff.
       BIT 9 IS UNDELEGATED, so the value written is not that 0xffff. The ecall stub is linked into the app's half and every per-space
       leaf carries U, and the ISA forbids S-mode EXECUTING a U=1 page irrespective of SUM, so
       ECALL-from-S has no caller and cannot acquire one while the stub lives there. Delegating
       it would also foreclose the SBI route: on a hart whose firmware provides SBI, an ecall
       from S-mode is how the kernel reaches machine-mode services, and a delegated bit 9 would
       trap the kernel's own SBI calls into stvec. Undelegated, an ECALL-from-S that does occur
       is a kernel bug and goes to the mtvec installed above, which names it in machine mode
       instead of dispatching it as a syscall on a frame that means nothing. */
    li      t0, 0xffff & ~(1 << 9)  /* every exception to S-mode but ECALL-from-S */
    csrw    medeleg, t0
    csrr    t0, medeleg
    andi    t1, t0, 1 << 9
    beqz    t1, 1f
    la      a0, .Lmedeleg_refused
    la      a1, .Llbl_medeleg
    mv      a2, t0
    li      a3, 0
    j       .Lm_refuse
1:
    li      t0, 0x222               /* SSIP, STIP, SEIP to S-mode */
    csrw    mideleg, t0

    /* menvcfg.STCE at bit 63: S-mode writes stimecmp without trapping and the timer
       interrupt is delivered to S-mode, so no machine-mode timer shim stays resident. Read
       back, because a hart that keeps the bit clear traps every S-mode stimecmp write and the
       timer never arms. */
    li      t0, 1
    slli    t0, t0, 63
    csrs    menvcfg, t0
    csrr    t1, menvcfg
    bltz    t1, 1f                  /* bit 63 at XLEN 64 is the sign bit */
    la      a0, .Lstce_refused
    la      a1, .Llbl_menvcfg
    mv      a2, t1
    li      a3, 0
    j       .Lm_refuse
1:

    /* mcounteren.TM: S-mode may read the time CSR, which is arch_clock_now's source. Read
       back, because a hart that keeps the bit clear traps that read and the clock has no
       source at all. */
    li      t0, 2
    csrs    mcounteren, t0
    csrr    t1, mcounteren
    andi    t2, t1, 2
    bnez    t2, 1f
    la      a0, .Ltm_refused
    la      a1, .Llbl_mcounteren
    mv      a2, t1
    li      a3, 0
    j       .Lm_refuse
1:

    /* THE DRAM THE LAYOUT CLAIMS, PROBED WHILE A FAULT STILL HAS A VECTOR. _estack is
       ORIGIN(RAM) + LENGTH(RAM), which is the kernel alias of the address below, and the
       landing pad's first instruction after the drop stores through it; on a machine with less
       DRAM than KICKOS_RV64_DRAM_SIZE that store faults in SUPERVISOR mode, where the cause is
       delegated to an stvec still zero, so mtvec alone does not cover it. Machine mode over
       physical addresses here, so this is the physical top word, and a fault ON the probe
       lands in .Lmtrap. */
    li      t0, KICKOS_RV64_DRAM_BASE + KICKOS_RV64_DRAM_SIZE - 8
    li      t1, RAM_PROBE_PATTERN
    sd      t1, 0(t0)
    ld      t2, 0(t0)
    beq     t1, t2, 1f
    la      a0, .Lram_refused
    la      a1, .Llbl_probe_read
    mv      a2, t2
    la      a3, .Llbl_probe_at
    mv      a4, t0
    j       .Lm_refuse
1:

    /* The transient window's two non-leaf entries, filled here rather than assembled: a
       non-leaf entry holds the next table's PHYSICAL frame number, and a shift of a
       relocatable symbol is not a relocation any object format carries. Untranslated, so `la`
       answers with the physical address the entry needs. A level-1 table under the level-2
       table's WINDOW_VPN2 slot, and a level-0 table under that; the map editor writes the
       level-0 leaves at run time. */
    la      t0, kickos_rv64_window_l0
    srli    t0, t0, 12
    slli    t0, t0, 10
    ori     t0, t0, PTE_V
    la      t1, kickos_rv64_window_l1
    sd      t0, WINDOW_VPN1 * 8(t1)

    la      t0, kickos_rv64_window_l1
    srli    t0, t0, 12
    slli    t0, t0, 10
    ori     t0, t0, PTE_V
    la      t1, kickos_rv64_high_l2
    li      t2, WINDOW_VPN2 * 8
    add     t1, t1, t2
    sd      t0, 0(t1)

    /* The kernel window's non-leaf entry, for the same reason: its level-1 table is a
       link-time constant but the frame number of one is not expressible in an object file. */
    la      t0, kickos_rv64_kernel_l1
    srli    t0, t0, 12
    slli    t0, t0, 10
    ori     t0, t0, PTE_V
    la      t1, kickos_rv64_high_l2
    li      t2, KERNEL_VPN2 * 8
    add     t1, t1, t2
    sd      t0, 0(t1)

    /* THE CHAIN THE MODE ADDS, one non-leaf entry per level above 2, each naming the next table
       down: the tables are contiguous and one page apart by construction below, and the slot is
       the same at every level. The walk STOPS AT THE LEVEL-2 TABLE'S OWN SYMBOL, so how many
       entries it fills is the layout's answer rather than a second statement of the depth, and
       the guard at the end of this file holds that layout to the derived level count. Zero
       iterations at Sv39, where the root IS the level-2 table, so both modes run this same code
       (docs/design-m6-mmu.md R3). */
    la      t1, kickos_rv64_root
    la      t5, kickos_rv64_high_l2
    li      t4, UPPER_SLOT * 8
    li      t6, 4096                /* one table page, past an addi's immediate */
1:  beq     t1, t5, 2f
    add     t0, t1, t6
    srli    t2, t0, 12
    slli    t2, t2, 10
    ori     t2, t2, PTE_V
    add     t3, t1, t4
    sd      t2, 0(t3)
    mv      t1, t0
    j       1b
2:

    /* satp := the configured mode over the one root, at the table's physical address. */
    la      t0, kickos_rv64_root
    srli    t0, t0, 12
    li      t1, KICKOS_RV64_SATP_MODE
    slli    t1, t1, KICKOS_RV64_SATP_MODE_SHIFT
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma zero, zero

    /* satp.MODE IS WARL, so a hart that does not implement the configured mode keeps the field
       it had and translation is simply not on. Read it back and name that here: the mret below
       jumps to a virtual address, so the alternative is an instruction fetch fault with no
       stvec written yet, which is a silent spin (docs/design-m6-mmu.md R3). */
    csrr    t1, satp
    srli    t1, t1, KICKOS_RV64_SATP_MODE_SHIFT
    andi    t1, t1, 0xf
    li      t2, KICKOS_RV64_SATP_MODE
    beq     t1, t2, .Lmode_stuck

    la      a0, .Lmode_refused
    la      a1, .Llbl_satp
    csrr    a2, satp
    li      a3, 0
    j       .Lm_refuse

.Lmode_stuck:

    li      t0, 0x1800              /* mstatus.MPP */
    csrc    mstatus, t0
    li      t0, 0x800               /* MPP = supervisor */
    csrs    mstatus, t0

    ld      t0, .Lboot_high_va      /* the landing pad's high address, link-time absolute */
    csrw    mepc, t0
    mret

    .balign 8
.Lboot_high_va:
    .quad   kickos_rv64_boot_high

/* THE MACHINE-MODE TRAP HANDLER mtvec points at, which is every machine trap and every S-mode
 * exception medeleg does not cover: bit 11, bit 14, code 9 and every code at or above 16. The
 * cause and the faulting PC are both reported, so a hart refusing one specific CSR write is
 * identifiable from the console alone.
 */
    .balign 4
.Lmtrap:
    csrr    a2, mcause
    csrr    a4, mepc
    la      a0, .Lmtrap_msg
    la      a1, .Llbl_mcause
    la      a3, .Llbl_mepc
    j       .Lm_refuse

/* THE SHARED MACHINE-MODE REFUSAL, reached by `j` and never returning: a0 names what happened,
 * a1 labels a2 and a3 labels a4, a zero label skipping its value. It emits all of that, writes
 * the finisher's FAIL word and spins.
 *
 * NO MEMORY IS TOUCHED FOR CONTROL FLOW, there being no stack until the landing pad seats one:
 * ra links this to the two emitters and t6 links those to the byte writer, so the deepest chain
 * is two explicit link registers and no push exists to spill.
 */
.Lm_refuse:
    mv      s0, a1
    mv      s1, a2
    mv      s2, a3
    mv      s3, a4
    jal     ra, .Lm_puts
    beqz    s0, 1f
    mv      a0, s0
    jal     ra, .Lm_puts
    mv      a0, s1
    jal     ra, .Lm_puthex
1:  beqz    s2, 2f
    mv      a0, s2
    jal     ra, .Lm_puts
    mv      a0, s3
    jal     ra, .Lm_puthex
2:  la      a0, .Lm_eol
    jal     ra, .Lm_puts
    li      t0, KICKOS_RV64_TEST_FINISHER_PA
    li      t1, KICKOS_RV64_FINISHER_FAIL + (KICKOS_RV64_FATAL_STATUS << 16)
    sw      t1, 0(t0)
3:  j       3b                      /* the finisher answered nothing */

/* One byte, a0, to the console the chip drives, at its PHYSICAL address. Entered with `jal t6`
 * and returned with `jr t6`, so the two callers keep ra for their own return. Clobbers t0-t2. */
.Lm_putc:
    li      t0, KICKOS_RV64_UART_POLL_BOUND
1:  li      t1, KICKOS_RV64_UART0_PA + 5
    lbu     t2, 0(t1)
    andi    t2, t2, 0x20            /* LSR.THRE: the holding register is empty */
    bnez    t2, 2f
    addi    t0, t0, -1
    bnez    t0, 1b
2:  li      t1, KICKOS_RV64_UART0_PA
    sb      a0, 0(t1)
    jr      t6

/* a0 = a NUL-terminated string, at the physical alias `la` yields in this section. Clobbers
 * a0, t0-t3 and t6. */
.Lm_puts:
    mv      t3, a0
1:  lbu     a0, 0(t3)
    beqz    a0, 2f
    jal     t6, .Lm_putc
    addi    t3, t3, 1
    j       1b
2:  ret

/* a0 = a doubleword, 16 hex digits, most significant first. Clobbers a0, t0-t2 and t4-t6. */
.Lm_puthex:
    mv      t4, a0
    li      t5, 60
1:  srl     a0, t4, t5
    andi    a0, a0, 0xf
    li      t0, 10
    blt     a0, t0, 2f
    addi    a0, a0, 'a' - '0' - 10
2:  addi    a0, a0, '0'
    jal     t6, .Lm_putc
    addi    t5, t5, -4
    bgez    t5, 1b
    ret

/* Read PC-relatively, so these are in this section and reached untranslated like everything
 * else before the switch. */
.Lmode_refused:
    .asciz  "\r\nKickOS: satp.MODE did not stick: this hart does not implement the configured paging mode"
.Lmtrap_msg:
    .asciz  "\r\nKickOS: machine-mode trap before the drop: mcause 2 is a CSR this hart does not implement, 5 or 7 a physical address this layout claims and the machine has not"
.Lpmp_refused:
    .asciz  "\r\nKickOS: pmpcfg0 refused entry 0 while pmpaddr0 took it: every supervisor access would be denied"
.Lmedeleg_refused:
    .asciz  "\r\nKickOS: medeleg bit 9 read back set: this hart delegates ECALL-from-S and this port requires it undelegated"
.Lstce_refused:
    .asciz  "\r\nKickOS: menvcfg.STCE did not stick: a supervisor write of stimecmp traps and the timer never arms"
.Ltm_refused:
    .asciz  "\r\nKickOS: mcounteren.TM did not stick: a supervisor read of the time CSR traps and the clock has no source"
.Lram_refused:
    .asciz  "\r\nKickOS: the top word of the DRAM this layout claims does not read back: the machine has less than KICKOS_RV64_DRAM_SIZE"
.Llbl_satp:
    .asciz  " satp=0x"
.Llbl_mcause:
    .asciz  " mcause=0x"
.Llbl_mepc:
    .asciz  " mepc=0x"
.Llbl_pmpcfg0:
    .asciz  " pmpcfg0=0x"
.Llbl_pmpaddr0:
    .asciz  " pmpaddr0=0x"
.Llbl_medeleg:
    .asciz  " medeleg=0x"
.Llbl_menvcfg:
    .asciz  " menvcfg=0x"
.Llbl_mcounteren:
    .asciz  " mcounteren=0x"
.Llbl_probe_read:
    .asciz  " read=0x"
.Llbl_probe_at:
    .asciz  " at=0x"
.Lm_eol:
    .asciz  "\r\n"

    .option pop

/* Reached translated, so the PC is the kernel's own virtual address from here on and `la`
 * yields high values.
 */
    .section .text.privtrap, "ax"
    .global kickos_rv64_boot_high
kickos_rv64_boot_high:
    /* A PROVISIONAL stvec BEFORE ANY SUPERVISOR ACCESS, and FIRST because the gp load below is
       itself one. kickos_rv64_init installs the real entry from arch_init, which is after
       Reset_Handler's .data copy and app .bss zero; a page fault in either would otherwise go
       to an stvec of zero and spin. DIRECT mode, and .Lboot_trap is 4-byte aligned. */
    la      t0, .Lboot_trap
    csrw    stvec, t0

    /* gp := __global_pointer$, out of a link-time word rather than a `la`: the anchor is in
       the APP's half and a PC-relative materialisation of it does not reach. This seats the
       register and dereferences nothing, no root mapping that half yet. -msmall-data-limit=0 on
       the KickOS-owned archives keeps kernel DATA out of small-data; it does not by itself stop
       an access resolving through gp, which is check_riscv_kernel_gp.sh's job. */
    .option push
    .option norelax
    ld      gp, .Lgp_anchor
    .option pop

    la      sp, _estack             /* boot/trap stack: top of RAM */
    li      tp, 0                   /* no TLS: thread pointer parked at 0 */

    call    Reset_Handler           /* C runtime + kmain (noreturn) */
1:  j       1b                      /* Reset_Handler never returns */

    .balign 4
.Lboot_trap:
    la      a0, .Lboot_trap_msg
    j       kickos_rv64_supervisor_refuse

/* THE SUPERVISOR-MODE REFUSAL, a0 naming what happened. Translation is on here, so the console
 * and the finisher are reached at the kernel's own alias of their physical addresses, the same
 * delta the kernel window uses. NO STACK IS TOUCHED: one caller is a boot path that has not
 * seated one and the other (switch.S) has just found the trap stack unusable. Never returns.
 */
    .global kickos_rv64_supervisor_refuse
kickos_rv64_supervisor_refuse:
    li      t5, KICKOS_RV64_VA_BASE + KICKOS_RV64_UART0_PA
    mv      t3, a0
1:  lbu     t4, 0(t3)
    beqz    t4, 4f
    li      t0, KICKOS_RV64_UART_POLL_BOUND
2:  lbu     t2, 5(t5)
    andi    t2, t2, 0x20            /* LSR.THRE: the holding register is empty */
    bnez    t2, 3f
    addi    t0, t0, -1
    bnez    t0, 2b
3:  sb      t4, 0(t5)
    addi    t3, t3, 1
    j       1b
4:  li      t0, KICKOS_RV64_VA_BASE + KICKOS_RV64_TEST_FINISHER_PA
    li      t1, KICKOS_RV64_FINISHER_FAIL + (KICKOS_RV64_FATAL_STATUS << 16)
    sw      t1, 0(t0)
5:  j       5b                      /* the finisher answered nothing */

.Lboot_trap_msg:
    .asciz  "\r\nKickOS: supervisor trap before the trap entry was installed: the boot path faulted\r\n"

    .balign 8
.Lgp_anchor:
    .quad   __global_pointer$

/* THE TABLES BELOW ARE LINK-TIME CONSTANTS AND _start FILLS THREE NON-LEAF ENTRIES AMONG THEM,
 * PLUS ONE PER LEVEL THE MODE ADDS: the two the transient window's pair needs, the level-2
 * table's entry for the kernel window's level-1 table, and the chain from the root down to that
 * level-2 table. Nothing here is written after boot, which is why all of them sit in the
 * read-execute leaf; the transient window's LEVEL-0 table is written by the map editor and is
 * the one page in the writable half.
 *
 * ONE ROOT, and what retired the pair is the LINK rather than a page-table trick. "Irrespective
 * of SUM, the supervisor may not execute code on pages with U=1" (RISC-V Privileged ISA, the U
 * bit), so while kernel text and app text shared a page no single root could serve both
 * privilege levels. They no longer share one: virt_rv64.ld puts the app in a level-2 slot of
 * its own, this root leaves that slot EMPTY, and a per-space root fills it with U-accessible
 * leaves of its own. Everything this root does name is U-clear, so the same root is correct in
 * both modes and satp changes only when the SPACE does.
 *
 * MMIO_VPN2 maps the low gigabyte at KICKOS_RV64_VA_BASE, which is where chip_virt_rv64.cc
 * names the UART, the CLINT and the test finisher: the SAME delta as the kernel window beside
 * it, so one rule turns any physical address below 4 GiB into the kernel address that reaches
 * it. NOT identity, and that is what keeps the whole low half free: every space copies the
 * ROOT's entries and shares every table under them, and a device leaf in a low slot would
 * refuse a per-space mapping anywhere in the first gigabyte. KERNEL_VPN2 is the kernel window
 * and the map of physical RAM together, a level-1 table rather than a leaf so the
 * write-execute boundary below fits inside it. Every other entry is invalid, so an access
 * outside them faults rather than reading a neighbour, the low RAM alias .text.init ran at
 * included.
 */
    .section .mmu_boot, "a"
    .balign 4096
    .global kickos_rv64_root
kickos_rv64_root:
    /* ONE PAGE PER LEVEL THE MODE ADDS OVER Sv39, all zero: _start's chain writes one entry in
       each, naming the page after it. At Sv39 this reserves nothing and the root IS the level-2
       table below, which is what makes the two symbols one address there. */
    .fill   UPPER_TABLES * 512, 8, 0

    .global kickos_rv64_high_l2
kickos_rv64_high_l2:
    .fill   MMIO_VPN2, 8, 0
    .quad   PTE_LEAF(0, PTE_R + PTE_W)
    .fill   KERNEL_VPN2 - MMIO_VPN2 - 1, 8, 0
    .quad   0                       /* the kernel window's level-1 table: _start fills it */
    .fill   512 - KERNEL_VPN2 - 1, 8, 0

/* THE KERNEL WINDOW, ONE 2 MiB LEAF PER ENTRY, AND THE WRITE-EXECUTE BOUNDARY IS WHERE
 * virt_rv64.ld ALREADY PUTS IT (docs/design-m6-mmu.md R2.3). The first KTEXT_MEGAPAGES leaves
 * cover the KTEXT region, kernel text and rodata and these tables, read-execute; every leaf
 * above covers the app's load window and the kernel's writable state, read-write. No leaf
 * carries both, and none carries U: the whole window is the privileged level's.
 *
 * The whole gigabyte stays described, as the single leaf it replaces did, so a frame anywhere
 * in KICKOS_RV64_KERNEL_WINDOW_SIZE is still reached by adding the window's delta and
 * arch_aspace_acquire's offset route is unchanged.
 */
    .balign 4096
    .global kickos_rv64_kernel_l1
kickos_rv64_kernel_l1:
    .set    _kleaf, 0
    .rept   KTEXT_MEGAPAGES
    .quad   PTE_LEAF(KICKOS_RV64_DRAM_BASE + _kleaf * MEGAPAGE, PTE_R + PTE_X)
    .set    _kleaf, _kleaf + 1
    .endr
    .rept   512 - KTEXT_MEGAPAGES
    .quad   PTE_LEAF(KICKOS_RV64_DRAM_BASE + _kleaf * MEGAPAGE, PTE_R + PTE_W)
    .set    _kleaf, _kleaf + 1
    .endr

/* The transient window's level-1 table. A slot mapped under it is kernel data: no U and no X,
 * so it is reachable in supervisor mode and by no unprivileged thread. In the kernel half, so
 * an unprivileged thread cannot write it either.
 */
    .balign 4096
    .global kickos_rv64_window_l1
kickos_rv64_window_l1:
    .fill   512, 8, 0

/* THE ONE TABLE PAGE THE KERNEL WRITES AFTER BOOT, so it is the one that cannot sit in the
 * read-execute leaf: arch_aspace_acquire and arch_aspace_release write these leaves. Its own
 * section, homed in the writable half by virt_rv64.ld, and LOADED rather than NOLOAD because
 * _start fills the level-1 entry above it before anything zeroes .bss.
 */
    .section .mmu_leaves, "aw"
    .balign 4096
    .global kickos_rv64_window_l0
kickos_rv64_window_l0:
    .fill   512, 8, 0

/* THE CHAIN IS AS LONG AS THE MODE SAYS: one page between the root and the level-2 table per
 * level above 2. Checked here and not above because a difference of two labels is a constant
 * only once both are defined, and it is the one place a level count stated twice is refused
 * rather than walked (docs/design-m6-mmu.md R3).
 */
.if (kickos_rv64_high_l2 - kickos_rv64_root) != UPPER_TABLES * 4096
.error "the boot tables reserve a different number of pages above level 2 than the configured paging mode names"
.endif

    .end
