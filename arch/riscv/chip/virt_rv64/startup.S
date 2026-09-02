/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * QEMU `virt` (RISC-V RV64IMAC) startup. QEMU with `-bios none -kernel <elf>` loads each
 * PT_LOAD segment at its p_paddr and enters at the LOWEST of them, so _start runs at
 * 0x8000_0000 in MACHINE mode while the image's VMAs are high (virt_rv64.ld).
 *
 * EVERYTHING IN .text.init IS REACHED UNTRANSLATED, and medany makes `la` here yield the
 * PHYSICAL alias of its target. A high address is reached out of a link-time absolute word.
 *
 * satp is written in machine mode, where it governs no access, and takes effect on the mret,
 * whose mepc holds the landing pad's VIRTUAL address. The PMP grant and the delegation come
 * first: an entry that does not permit the walk denies every S-mode access, and an undelegated
 * page fault goes to an mtvec nothing wrote.
 *
 * mtvec goes in FIRST, before any other machine CSR: a hart that does not IMPLEMENT one of the
 * CSRs below takes an illegal instruction on the write, which is the case no readback can
 * reach. Every mtvec write is then read back exactly, mtvec being WARL and permitted to hold a
 * read-only value.
 *
 * gp IS SEATED PAST THE SWITCH, at its high value. Nothing between _start and the landing pad
 * may make a gp-relative access, and norelax is what keeps the linker from turning one of the
 * loads below into one.
 */

#include <kickos/arch/rv64_frame.h>
#include <kickos/arch/rv64_paging.h>

#include "boot_layout.ld.h"

/* Undefined, `#if KICKOS_NUM_CORES > 1` would take the one-core arm silently and every hart
 * the machine has would race the boot-hart-only work below. */
#ifndef KICKOS_NUM_CORES
#error "KICKOS_NUM_CORES is not defined: the hart park cannot know how many harts this image drives"
#endif

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
 * be zero, so an output address is aligned to the span it names.
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

/* The tables above level 2, one per level the mode adds over Sv39. Every kernel-half address
 * takes the same index at every one of them, KICKOS_RV64_VA_BASE having every bit above 32
 * set, so one slot number serves the whole chain and the guard below holds the three addresses
 * to it.
 */
#define UPPER_TABLES (KICKOS_RV64_PAGE_LEVELS - 3)
#define UPPER_SLOT   KICKOS_RV64_VPN(KICKOS_RV64_VA_BASE, 3)

/* The slot of the ROOT table an address takes, which is level 2 at Sv39 and deeper above it. */
#define ROOT_SLOT(va) KICKOS_RV64_VPN(va, KICKOS_RV64_LEVEL_ROOT)

/* A kernel-half address must be canonical in the configured mode: every bit above the mode's
 * virtual width equals the top translated bit. Where that boundary falls moves with the mode.
 */
#define HIGH_CANONICAL(va) \
    (((va) >> (KICKOS_RV64_VA_BITS - 1)) == ((1 << (65 - KICKOS_RV64_VA_BITS)) - 1))

/* The read-execute half is a whole number of level-1 leaves of the kernel window. */
#define KTEXT_MEGAPAGES (KICKOS_RV64_KTEXT_SIZE / MEGAPAGE)

/* Written and read back at the top word of the DRAM the layout claims. Both halves differ, so
 * a bus that answers with the address or with a stuck value fails the compare. */
#define RAM_PROBE_PATTERN 0x5A5AA5A5

/* The delegation this port DEPENDS on. Both registers are WARL and an implementation may
 * subset the delegatable traps (Privileged ISA 3.1.8), so the readbacks below are held against
 * these and not against the values written.
 *
 * medeleg holds every exception a U-mode thread can raise with instructions it may execute: 8
 * is the syscall ABI, 12/13/15 the translating backend's own faults, 2 the illegal instruction
 * a slain thread reaches the fault reporter with, 3 the breakpoint EBREAK raises. 4 and 6 are
 * per-region behaviour, so the probe further down MEASURES what this hart raises and holds a
 * second readback against that.
 *
 * mideleg holds the two sources sie enables; bit 9 is asked for and not required.
 */
#define MEDELEG_REQUIRED \
    ((1 << 2) + (1 << 3) + (1 << 8) + (1 << 12) + (1 << 13) + (1 << 15))
#define MIDELEG_REQUIRED ((1 << 1) + (1 << 5))

/* Every mtvec write is read back exactly: a write that does not take leaves the PREVIOUS vector
 * live. Clobbers t0 and t1 and refuses before the vector it failed to seat is depended on.
 */
.macro SEAT_MTVEC target
    la      t0, \target
    csrw    mtvec, t0
    csrr    t1, mtvec
    beq     t0, t1, 8f
    la      a0, .Lmtvec_refused
    la      a1, .Llbl_mtvec
    mv      a2, t1
    la      a3, .Llbl_mtvec_want
    mv      a4, t0
    j       .Lm_refuse
8:
.endm

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

    /* mtvec BEFORE THE FIRST OTHER MACHINE-CSR WRITE. RISC-V defines no reset value for it, so
       until this instruction runs every machine-mode trap goes to an arbitrary address. DIRECT
       mode, bits 1:0 = 00, and .Lmtrap is 4-byte aligned. Its own refusal reaches the console
       without a vector: nothing on the way to it can trap. */
    SEAT_MTVEC .Lmtrap

#if KICKOS_NUM_CORES > 1
    /* THE PARK IS AHEAD OF THE PMP WRITE AND EVERY PROBE. There is no firmware, so every hart
       the machine has arrives here, and the boot-hart-only work below is written for one
       writer: the DRAM probe, the boot page tables and the refusals they can reach.
       mtvec alone precedes it, a hart that traps before one is seated landing nowhere.

       s11 CARRIES THE HART INDEX FROM HERE TO THE mret and nothing between writes it.

       mhartid IS NOT THE INDEX: it is an integrator-chosen value (the openc906 core hardwires
       it to zero and its integrator customises it per instance), so the range test below is
       what makes it one, and the claim is what refuses two harts answering the same. */
    csrr    s11, mhartid
    li      t0, KICKOS_NUM_CORES
    bltu    s11, t0, .Lhart_in_range
.Lhart_unclaimed:
    /* No row, so no trap stack and no console turn: a hart this image drives nothing for. */
    wfi
    j       .Lhart_unclaimed

.Lhart_in_range:
    la      t0, kickos_rv64_hart_claim
    slli    t1, s11, 2
    add     t0, t0, t1
    li      t1, 1
    amoswap.w t1, t1, (t0)
    beqz    t1, .Lhart_claimed
    la      a0, .Lhartid_dup
    la      a1, .Llbl_mhartid
    mv      a2, s11
    li      a3, 0
    j       .Lm_refuse

.Lhart_claimed:
    beqz    s11, .Lhart_prologue

    /* WFI'S WAKEUP IS GATED BY mie ALONE, not by mstatus.MIE, so a hart parked with mie at its
       reset zero can never leave: no interrupt is enabled, nothing can make one pending, and
       the release word alone is a store no hart is watching for. The release therefore rings
       this hart's own msip word as the EDGE beside the word as the STATE, and this enable is
       what lets the edge land. The interrupt is never TAKEN here: mstatus.MIE is still zero,
       so it only ends the wait. */
    li      t0, 8                   /* mie.MSIE */
    csrs    mie, t0
    la      s10, kickos_rv64_hart_release
    slli    t1, s11, 3
    add     s10, s10, t1
.Lhart_park:
    wfi
    /* Drop the edge before reading the state: msip is level-sensitive, so a word left set
       would carry into supervisor mode as a doorbell no peer asked for. */
    slli    t0, s11, 2
    li      t1, KICKOS_RV64_CLINT_PA
    add     t0, t0, t1
    sw      zero, 0(t0)
    ld      t0, 0(s10)
    beqz    t0, .Lhart_park

.Lhart_prologue:
#endif

    /* PMP entry 0: NAPOT over the whole address space, read/write/execute. */
    li      t0, -1
    srli    t0, t0, 10
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    /* Every PMP CSR field is WARL and may be read-only zero (Privileged ISA 3.7.1), so the
       entry that stands is DECODED: an A field the hart narrowed, or an R/W/X it dropped,
       leaves a non-zero pmpcfg0 that denies the S-mode accesses this port then makes.

       pmpcfg0 and pmpaddr0 both reading zero carries no answer at all, no entry implemented
       and every entry OFF reading the same, so that case is passed to the effective probe after
       the DRAM probe below. Only once EVERY configuration byte reads zero, though: a higher
       entry can otherwise permit exactly the word that probe measures and deny everything else.
       The eight even-numbered CSRs are the whole set on RV64.

       ACCEPTED: L clear, R and W and X set, and A either NAPOT or TOR. Coverage comes off
       pmpaddr0's READBACK, not the value written: not all of bits 55:2 need be implemented, so
       what says "the whole space" is t1 + 1 being a power of two with t1 non-zero. */
    csrr    t0, pmpcfg0
    csrr    t1, pmpaddr0
    andi    t2, t0, 0xff
    bnez    t2, 2f
    bnez    t1, 3f
    csrr    t3, pmpcfg2             /* entry 0 is OFF: no other entry may be configured */
    or      t3, t3, t0
    csrr    t4, pmpcfg4
    or      t3, t3, t4
    csrr    t4, pmpcfg6
    or      t3, t3, t4
    csrr    t4, pmpcfg8
    or      t3, t3, t4
    csrr    t4, pmpcfg10
    or      t3, t3, t4
    csrr    t4, pmpcfg12
    or      t3, t3, t4
    csrr    t4, pmpcfg14
    or      t3, t3, t4
    beqz    t3, 1f                  /* all zero: the effective probe below decides */
    la      a0, .Lpmp_uncharted
    la      a1, .Llbl_pmpcfg_rest
    mv      a2, t3
    li      a3, 0
    j       .Lm_refuse
2:  andi    t3, t2, 0x87            /* L, and the three permission bits */
    li      t4, 0x07
    bne     t3, t4, 3f
    srli    t3, t2, 3
    andi    t3, t3, 0x03            /* A */
    li      t4, 3                   /* NAPOT */
    beq     t3, t4, 4f
    li      t4, 1                   /* TOR */
    bne     t3, t4, 3f
4:  beqz    t1, 3f
    addi    t3, t1, 1
    and     t3, t3, t1              /* zero iff pmpaddr0 is a run of ones from bit 0 */
    beqz    t3, 1f
3:  la      a0, .Lpmp_refused
    la      a1, .Llbl_pmpcfg0
    mv      a2, t0
    la      a3, .Llbl_pmpaddr0
    mv      a4, t1
    j       .Lm_refuse
1:

    /* BIT 9 MUST STAY UNDELEGATED: an ECALL-from-S is a kernel bug here, and delegated it would
       be dispatched as a syscall on a frame that means nothing, and would swallow the kernel's
       own SBI calls on a hart whose firmware provides them. */
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

    /* A hart may hold any delegation bit read-only zero, and the exceptions this port depends
       on would then terminate in machine mode with no task killed and no syscall answered.
       Only MEDELEG_REQUIRED is named. */
1:  li      t1, MEDELEG_REQUIRED
    and     t2, t0, t1
    beq     t2, t1, 1f
    la      a0, .Lmedeleg_short
    la      a1, .Llbl_medeleg
    mv      a2, t0
    la      a3, .Llbl_required
    mv      a4, t1
    j       .Lm_refuse
1:
    /* mideleg is read back as a SUBSET: bits a hart holds read-only one are permitted for
       lower-level interrupts (Privileged ISA 3.1.8), so an exact compare refuses a conforming
       hart. Undelegated, sip and sie hold the corresponding bits read-only zero, so the failure
       is silent rather than a trap. */
    li      t0, 0x222               /* SSIP, STIP, SEIP to S-mode */
    csrw    mideleg, t0
    csrr    t0, mideleg
    li      t1, MIDELEG_REQUIRED
    and     t2, t0, t1
    beq     t2, t1, 1f
    la      a0, .Lmideleg_short
    la      a1, .Llbl_mideleg
    mv      a2, t0
    la      a3, .Llbl_required
    mv      a4, t1
    j       .Lm_refuse
1:

    /* menvcfg.STCE at bit 63: S-mode writes stimecmp without trapping and the timer interrupt
       is delivered to S-mode. Read back: a hart that keeps the bit clear traps every S-mode
       stimecmp write and the timer never arms. */
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

    /* mcounteren.TM: S-mode may read the time CSR, which is arch_clock_now's source. Read back:
       a hart that keeps the bit clear traps that read and the clock has no source. */
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

    /* The DRAM the layout claims, probed while a fault still has a vector: the landing pad
       stores through _estack, and on a machine with less DRAM that store faults in SUPERVISOR
       mode with stvec still zero. Machine mode over physical addresses here.

       BOOT HART ONLY: it is a property of the machine, and a secondary repeating it would
       write the top word under a boot hart already standing on the stack below it. */
#if KICKOS_NUM_CORES > 1
    bnez    s11, .Lram_probed
#endif
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
#if KICKOS_NUM_CORES > 1
.Lram_probed:
#endif

    /* What this hart does with a misaligned access, MEASURED: whether one raises an exception
       at all is a per-region property (Privileged ISA 3.6.2), so no register answers it.

       s4 collects 1 << mcause for each probe that traps and stays zero for each that does not,
       so the delegation this hart is held to is the one its own behaviour needs. The vector
       RESUMES at s6, so a hart that resolves misalignment in hardware boots straight through.

       s7 IS THE HART'S OWN WORD, never a shared one: the store puts back the four bytes
       already at s7 + 1, so a hart that completes it changes nothing, but two harts probing
       one word would interleave a load between another's store and its restore. */
    li      s4, 0
#if KICKOS_NUM_CORES > 1
    la      s7, kickos_rv64_hart_probe
    slli    t0, s11, 3
    add     s7, s7, t0
#else
    li      s7, KICKOS_RV64_DRAM_BASE + KICKOS_RV64_DRAM_SIZE - 8
#endif
    ld      s8, 0(s7)
    SEAT_MTVEC .Lmisaligned_probe
    la      s6, 1f
    lw      t1, 1(s7)
1:  la      s6, 1f
    srli    t1, s8, 8
    sw      t1, 1(s7)
1:  SEAT_MTVEC .Lmtrap

    /* The delegation that measurement adds. medeleg is unwritten since the check above, so the
       readback is re-taken and a bit this hart never raises is never named in the refusal. */
    beqz    s4, 1f
    csrr    t0, medeleg
    and     t2, t0, s4
    beq     t2, s4, 1f
    la      a0, .Lmedeleg_misaligned
    la      a1, .Llbl_medeleg
    mv      a2, t0
    la      a3, .Llbl_required
    mv      a4, s4
    j       .Lm_refuse
1:

    /* The grant's effective permission, which no PMP readback carries. mstatus.MPRV with
       MPP = S makes a machine-mode load or store translated and protected as a supervisor one,
       so this MEASURES the permission the decode above can only describe.

       One address stands for every address because the decode above accepts only all-or-nothing
       across the space. The word is the one the misaligned probe above just read and wrote in
       machine mode, so a fault here is the PMP entry alone; mtvec moves to a vector of this
       probe's own for the two instructions.

       THE REGIME IS ESTABLISHED HERE AND NOT INHERITED. Under MPRV the access is translated as
       well as protected, and RISC-V defines a reset value for satp nowhere, so satp is written
       Bare and read back and MPV is cleared and read back.

       THE FENCE IS THE PMP WRITE'S: a hart may cache PMP settings alongside a translation, so
       M-mode must issue SFENCE.VMA rs1 = rs2 = x0 after writing the PMP CSRs (Privileged ISA
       3.7.1) or the probe answers off a verdict taken before the grant existed. */
    li      t0, 0x1800              /* mstatus.MPP */
    csrc    mstatus, t0
    li      t0, 0x800               /* MPP = supervisor */
    csrs    mstatus, t0
    csrr    t1, mstatus
    srli    t2, t1, 11
    andi    t2, t2, 3
    li      t3, 1
    beq     t2, t3, 1f
    la      a0, .Lmpp_refused
    la      a1, .Llbl_mstatus
    mv      a2, t1
    li      a3, 0
    j       .Lm_refuse
1:  li      t0, 1
    slli    t0, t0, 39              /* mstatus.MPV */
    csrc    mstatus, t0
    csrr    t1, mstatus
    and     t2, t1, t0
    beqz    t2, 1f
    la      a0, .Lmpv_refused
    la      a1, .Llbl_mstatus
    mv      a2, t1
    li      a3, 0
    j       .Lm_refuse
1:  csrw    satp, zero
    csrr    t0, satp
    beqz    t0, 1f
    la      a0, .Lbare_refused
    la      a1, .Llbl_satp
    mv      a2, t0
    li      a3, 0
    j       .Lm_refuse
1:  sfence.vma zero, zero
    SEAT_MTVEC .Lpmp_probe_trap
    li      t0, 0x20000             /* mstatus.MPRV */
    csrs    mstatus, t0
#if KICKOS_NUM_CORES > 1
    mv      t0, s7
#else
    li      t0, KICKOS_RV64_DRAM_BASE + KICKOS_RV64_DRAM_SIZE - 8
#endif
    ld      t1, 0(t0)
    sd      t1, 0(t0)
    li      t0, 0x20000
    csrc    mstatus, t0
    SEAT_MTVEC .Lmtrap

    /* The transient window's two non-leaf entries. A non-leaf entry holds the next table's
       PHYSICAL frame number, and a shift of a relocatable symbol is not a relocation any object
       format carries, so they are filled here. The map editor writes the level-0 leaves at run
       time.

       BOOT HART ONLY, through the end of the chain below: one root serves every hart, so a
       secondary re-writing these entries would edit tables a walker is already reading. */
#if KICKOS_NUM_CORES > 1
    bnez    s11, .Lboot_tables_done
#endif
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

    /* The kernel window's non-leaf entry, for the same reason. */
    la      t0, kickos_rv64_kernel_l1
    srli    t0, t0, 12
    slli    t0, t0, 10
    ori     t0, t0, PTE_V
    la      t1, kickos_rv64_high_l2
    li      t2, KERNEL_VPN2 * 8
    add     t1, t1, t2
    sd      t0, 0(t1)

    /* The chain the mode adds, one non-leaf entry per level above 2, each naming the next table
       down. The walk stops at the level-2 table's own symbol, and the guard at the end of this
       file holds that layout to the derived level count. Zero iterations at Sv39. */
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
#if KICKOS_NUM_CORES > 1
.Lboot_tables_done:
#endif

    /* satp := the configured mode over the one root, at the table's physical address. */
    la      t0, kickos_rv64_root
    srli    t0, t0, 12
    li      t1, KICKOS_RV64_SATP_MODE
    slli    t1, t1, KICKOS_RV64_SATP_MODE_SHIFT
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma zero, zero

    /* satp.MODE is WARL, so a hart that does not implement the configured mode keeps the field
       it had and translation is simply not on. The mret below jumps to a virtual address, so an
       unread-back failure is an instruction fetch fault with no stvec, i.e. a silent spin. */
    csrr    t1, satp
    srli    t1, t1, KICKOS_RV64_SATP_MODE_SHIFT
    andi    t1, t1, 0xf
    li      t2, KICKOS_RV64_SATP_MODE
    beq     t1, t2, .Lmode_took

    la      a0, .Lmode_refused
    la      a1, .Llbl_satp
    csrr    a2, satp
    li      a3, 0
    j       .Lm_refuse

.Lmode_took:

    li      t0, 0x1800              /* mstatus.MPP */
    csrc    mstatus, t0
    li      t0, 0x800               /* MPP = supervisor */
    csrs    mstatus, t0

#if KICKOS_NUM_CORES > 1
    /* sscratch := this hart's per-CPU block, which is also its trap-stack top (percpu.h).
       MACHINE MODE IS THE ONLY PLACE THIS CAN BE DONE: mhartid is not readable in supervisor
       mode, so a hart that reached S-mode with sscratch unseated could not find its own row.
       The HIGH address, out of a link-time absolute word: S-mode runs translated, and `la`
       here yields the physical alias. */
    ld      t0, .Lpercpu_high_va
    li      t1, KICKOS_RV64_PERCPU_ROW_SIZE
    mul     t2, s11, t1
    add     t0, t0, t2
    li      t1, KICKOS_RV64_TRAP_STACK_SIZE
    add     t0, t0, t1
    csrw    sscratch, t0

    /* mscratch := this hart's machine-mode save area, PHYSICAL: the trampoline runs
       untranslated. Its word 0 is this hart's own CLINT msip address, precomputed because the
       trampoline has two registers and no stack. */
    la      t0, kickos_rv64_hart_mtrap
    slli    t1, s11, 4
    add     t0, t0, t1
    csrw    mscratch, t0
    slli    t1, s11, 2
    li      t2, KICKOS_RV64_CLINT_PA
    add     t1, t1, t2
    sd      t1, 0(t0)

    /* The doorbell's machine-level enable. While the hart runs in supervisor mode a machine
       interrupt is enabled regardless of mstatus.MIE, so this bit alone gates delivery. */
    li      t0, 8                   /* mie.MSIE */
    csrs    mie, t0

    /* A SECONDARY LANDS SOMEWHERE ELSE: the boot pad runs the C runtime's data copy and bss
       zero and calls kmain, all of which are the boot hart's once. */
    beqz    s11, .Lboot_mret
    ld      t0, .Lsecondary_high_va
    csrw    mepc, t0
    mret
.Lboot_mret:
#endif
    ld      t0, .Lboot_high_va      /* the landing pad's high address, link-time absolute */
    csrw    mepc, t0
    mret

    .balign 8
.Lboot_high_va:
    .quad   kickos_rv64_boot_high
#if KICKOS_NUM_CORES > 1
.Lsecondary_high_va:
    .quad   kickos_rv64_secondary_high
.Lpercpu_high_va:
    .quad   kickos_rv64_percpu
#endif

/* The machine-mode trap handler mtvec points at: every machine trap and every S-mode exception
 * medeleg does not cover. Reports the cause and the faulting PC.
 */
    .balign 4
.Lmtrap:
#if KICKOS_NUM_CORES > 1
    /* THE CROSS-HART DOORBELL'S MACHINE-MODE LEG. A peer's CLINT msip write raises a MACHINE
       software interrupt, cause 3, and mideleg holds bit 3 read-only zero (Privileged ISA
       3.1.8 forbids only read-only ONE there), so it cannot be delegated and lands here. This
       lowers it to mip.SSIP, where the existing supervisor arm of kickos_rv64_isr_dispatch
       already demuxes it.

       IT RUNS UNDER A LIVE SUPERVISOR KERNEL and must leave everything as it found it: two
       registers through mscratch, no stack, no mstatus write. The mret clears mstatus.MPRV as
       the architecture requires of a return below machine mode, which is the value the boot
       path already left.

       CLEARING MSIP IS THIS HART'S OWN and must precede the mret: msip is level-sensitive, so
       a word left set re-enters here forever. */
    csrrw   t0, mscratch, t0        /* t0 = &save area; mscratch = t0's original */
    sd      t1, 8(t0)
    csrr    t1, mcause
    bgez    t1, .Lmtrap_hard        /* an exception: the sign bit is what marks an interrupt */
    addi    t1, t1, -3
    slli    t1, t1, 1               /* the interrupt bit alone survives iff the code was 3 */
    bnez    t1, .Lmtrap_hard
    ld      t1, 0(t0)               /* this hart's CLINT msip word, precomputed at bring-up */
    sw      zero, 0(t1)
    li      t1, 2                   /* mip.SSIP */
    csrs    mip, t1
    ld      t1, 8(t0)
    csrrw   t0, mscratch, t0
    mret

.Lmtrap_hard:
    ld      t1, 8(t0)
    csrrw   t0, mscratch, t0
#endif
    csrr    a2, mcause
    csrr    a4, mepc
    la      a0, .Lmtrap_msg
    la      a1, .Llbl_mcause
    la      a3, .Llbl_mepc
    j       .Lm_refuse

/* The misalignment probe's own vector, and the one vector in this file that RETURNS. s6 holds
 * the resume address so no instruction length is assumed: the C extension makes mepc + 4 the
 * wrong answer for half the encodings. Clobbers t0 and t1 and accumulates into s4.
 */
    .balign 4
.Lmisaligned_probe:
    csrr    t0, mcause
    li      t1, 1
    sll     t1, t1, t0
    or      s4, s4, t1
    csrw    mepc, s6
    mret

/* The PMP probe's own vector, live across the MPRV load and store alone: the word was read and
 * written in machine mode a moment earlier, so a trap there is the PMP entry and nothing else.
 */
    .balign 4
.Lpmp_probe_trap:
    csrr    a2, mcause
    csrr    a4, mepc
    la      a0, .Lpmp_denied
    la      a1, .Llbl_mcause
    la      a3, .Llbl_mepc
    j       .Lm_refuse

/* The shared machine-mode refusal, reached by `j` and never returning: a0 names what happened,
 * a1 labels a2 and a3 labels a4, a zero label skipping its value.
 *
 * NO MEMORY IS TOUCHED FOR CONTROL FLOW, there being no stack until the landing pad seats one:
 * ra links this to the two emitters and t6 links those to the byte writer.
 *
 * mstatus.MPRV is cleared first, and this is the one place it is cleared, so the console is
 * reachable from the PMP probe's vector, the generic vector and every direct jump alike.
 *
 * ONE HART TAKES THE CONSOLE. Every refusal here writes a byte at a time to a UART no lock
 * covers and then stops the machine at the finisher, so a second refusing hart would interleave
 * its message into the first's and could stop the machine mid-line. The claim decides which
 * refusal is reported; the harts that lose it stop without emitting and without answering the
 * finisher, leaving the whole diagnostic and the exit status to the one that won.
 */
.Lm_refuse:
    li      t0, 0x20000             /* mstatus.MPRV */
    csrc    mstatus, t0
#if KICKOS_NUM_CORES > 1
    la      t0, .Lrefuse_claim
    li      t1, 1
    amoswap.w t1, t1, (t0)
    beqz    t1, .Lm_refuse_owned
.Lm_refuse_lost:
    wfi
    j       .Lm_refuse_lost
.Lm_refuse_owned:
#endif
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
    li      t1, KICKOS_RV64_FINISHER_FAIL + (KICKOS_FATAL_STATUS << 16)
    sw      t1, 0(t0)
3:  j       3b                      /* the finisher answered nothing */

/* One byte, a0, to the console at its PHYSICAL address. Entered with `jal t6` and returned with
 * `jr t6`, so ra is left for a caller's own return. Clobbers t0-t2. */
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

/* Read PC-relatively, so these must stay in this section and reached untranslated. */
.Lmode_refused:
    .asciz  "\r\nKickOS: satp.MODE did not stick: this hart does not implement the configured paging mode"
.Lmtrap_msg:
    .asciz  "\r\nKickOS: machine-mode trap before the drop: mcause 2 is a CSR this hart does not implement, 5 or 7 a physical address this layout claims and the machine has not"
.Lpmp_refused:
    .asciz  "\r\nKickOS: PMP entry 0 did not stand as written: cfg byte 0 must read L clear, RWX set and A NAPOT or TOR, and pmpaddr0 a run of ones covering every implemented bit, or supervisor accesses this port makes are denied"
.Lpmp_denied:
    .asciz  "\r\nKickOS: PMP denies the supervisor accesses this port makes: a load and store under mstatus.MPRV with MPP=S faulted on DRAM this machine has, so the entries that stand grant supervisor mode nothing"
.Lmpp_refused:
    .asciz  "\r\nKickOS: mstatus.MPP did not take supervisor: this hart implements no supervisor mode, so neither the grant probe nor the mret below can name one"
.Lmedeleg_refused:
    .asciz  "\r\nKickOS: medeleg bit 9 read back set: this hart delegates ECALL-from-S and this port requires it undelegated"
.Lmedeleg_short:
    .asciz  "\r\nKickOS: medeleg dropped an exception this port depends on: bits 2, 3, 8, 12, 13 and 15 must reach S-mode or an illegal instruction, an EBREAK, a user syscall and a page fault all terminate in machine mode instead of killing the thread"
.Lmedeleg_misaligned:
    .asciz  "\r\nKickOS: this hart raises an exception for a misaligned access and medeleg does not delegate it: an unprivileged misaligned load or store terminates in machine mode instead of killing the thread"
.Lpmp_uncharted:
    .asciz  "\r\nKickOS: PMP entry 0 reads OFF while another entry is configured: what the entries that stand permit is not the one word the probe below measures, and this port cannot tell a grant over its image from a grant over that word"
.Lmpv_refused:
    .asciz  "\r\nKickOS: mstatus.MPV did not clear: the grant probe and the supervisor this port drops into would both run as a guest under a second stage of translation this port never wrote"
.Lbare_refused:
    .asciz  "\r\nKickOS: satp did not read back Bare: the grant probe's supervisor access would translate through a root nothing here wrote, and a page fault would be reported as the PMP entry denying it"
.Lmideleg_short:
    .asciz  "\r\nKickOS: mideleg dropped an interrupt this port depends on: bits 1 and 5 must reach S-mode or sip and sie hold both read-only zero, so the injected-IRQ doorbell never rings and the timer never fires"
.Lmtvec_refused:
    .asciz  "\r\nKickOS: an mtvec write did not stick: mtvec is WARL and may hold a read-only value, and the vector still live is one written for a different trap"
.Lstce_refused:
    .asciz  "\r\nKickOS: menvcfg.STCE did not stick: a supervisor write of stimecmp traps and the timer never arms"
.Ltm_refused:
    .asciz  "\r\nKickOS: mcounteren.TM did not stick: a supervisor read of the time CSR traps and the clock has no source"
.Lram_refused:
    .asciz  "\r\nKickOS: the top word of the DRAM this layout claims does not read back: the machine has less than KICKOS_RV64_DRAM_SIZE"
#if KICKOS_NUM_CORES > 1
.Lhartid_dup:
    .asciz  "\r\nKickOS: two harts report the same mhartid: mhartid is integrator-chosen rather than an index, and a second hart taking this row would stand on a trap stack another hart is already on"
.Llbl_mhartid:
    .asciz  " mhartid=0x"
#endif
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
.Llbl_pmpcfg_rest:
    .asciz  " other pmpcfg=0x"
.Llbl_mstatus:
    .asciz  " mstatus=0x"
.Llbl_medeleg:
    .asciz  " medeleg=0x"
.Llbl_mideleg:
    .asciz  " mideleg=0x"
.Llbl_required:
    .asciz  " required=0x"
.Llbl_mtvec:
    .asciz  " mtvec=0x"
.Llbl_mtvec_want:
    .asciz  " want=0x"
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

/* Reached translated, so `la` yields high values from here on. */
    .section .text.privtrap, "ax"
    .global kickos_rv64_boot_high
kickos_rv64_boot_high:
    /* A PROVISIONAL stvec BEFORE ANY SUPERVISOR ACCESS, and first because the gp load below is
       itself one. kickos_rv64_init installs the real entry from arch_init, which is after
       Reset_Handler's .data copy and app .bss zero. DIRECT mode, .Lboot_trap 4-byte aligned. */
    la      t0, .Lboot_trap
    csrw    stvec, t0

    /* gp := __global_pointer$, out of a link-time word: the anchor is in the APP's half and a
       PC-relative materialisation of it does not reach. Seats the register and dereferences
       nothing, no root mapping that half yet. check_riscv_kernel_gp.sh is what holds kernel
       accesses off gp. */
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

/* The supervisor-mode refusal, a0 naming what happened. Translation is on, so the console and
 * the finisher are reached at the kernel's alias of their physical addresses. NO STACK IS
 * TOUCHED: no usable one may be assumed on entry.
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
    li      t1, KICKOS_RV64_FINISHER_FAIL + (KICKOS_FATAL_STATUS << 16)
    sw      t1, 0(t0)
5:  j       5b                      /* the finisher answered nothing */

#if KICKOS_NUM_CORES > 1
/* Where a released secondary lands in supervisor mode, translated, with sscratch already
 * naming its row. gp is the APP's anchor and is seated the same way the boot pad seats it;
 * nothing here may make a gp-relative access before that.
 *
 * ITS STACK IS ITS OWN SLICE of the boot reservation: hart N's top is _estack minus N strides,
 * and it carries this hart's early C and the frame of any interrupt taken while it is parked.
 */
    .global kickos_rv64_secondary_high
kickos_rv64_secondary_high:
    la      t0, .Lboot_trap
    csrw    stvec, t0

    .option push
    .option norelax
    ld      gp, .Lgp_anchor
    .option pop

    la      sp, _estack
    li      t0, KICKOS_RV64_HART_STACK_STRIDE
    mul     t0, t0, s11
    sub     sp, sp, t0
    li      tp, 0                   /* no TLS: thread pointer parked at 0 */

    call    kickos_rv64_secondary_entry
1:  j       1b                      /* kickos_rv64_secondary_entry never returns */
#endif

.Lboot_trap_msg:
    .asciz  "\r\nKickOS: supervisor trap before the trap entry was installed: the boot path faulted\r\n"

    .balign 8
.Lgp_anchor:
    .quad   __global_pointer$

/* The tables below are link-time constants; _start fills three non-leaf entries among them plus
 * one per level the mode adds. Nothing here is written after boot, which is why all of them sit
 * in the read-execute leaf.
 *
 * Every leaf this root names is U-clear, and the app's level-2 slot is left EMPTY for a
 * per-space root to fill, so one root serves both privilege levels and satp changes only when
 * the SPACE does.
 *
 * MMIO_VPN2 maps the low gigabyte at KICKOS_RV64_VA_BASE, where chip_virt_rv64.cc names the
 * UART, the CLINT and the test finisher: the same delta as the kernel window beside it. Every
 * space copies the ROOT's entries and shares every table under them, so a device leaf in a LOW
 * slot would refuse a per-space mapping in the first gigabyte. KERNEL_VPN2 is a level-1 table
 * so the write-execute boundary below fits inside it. Every other entry is invalid.
 */
    .section .mmu_boot, "a"
    .balign 4096
    .global kickos_rv64_root
kickos_rv64_root:
    /* One page per level the mode adds over Sv39, all zero: _start's chain writes one entry in
       each. At Sv39 this reserves nothing and the root IS the level-2 table below. */
    .fill   UPPER_TABLES * 512, 8, 0

    .global kickos_rv64_high_l2
kickos_rv64_high_l2:
    .fill   MMIO_VPN2, 8, 0
    .quad   PTE_LEAF(0, PTE_R + PTE_W)
    .fill   KERNEL_VPN2 - MMIO_VPN2 - 1, 8, 0
    .quad   0                       /* the kernel window's level-1 table: _start fills it */
    .fill   512 - KERNEL_VPN2 - 1, 8, 0

/* The kernel window, one 2 MiB leaf per entry, with the write-execute boundary where
 * virt_rv64.ld already puts it. The first KTEXT_MEGAPAGES leaves cover kernel text, rodata and
 * these tables read-execute; every leaf above is read-write. No leaf carries both, and none
 * carries U. The whole gigabyte stays described, so a frame anywhere in
 * KICKOS_RV64_KERNEL_WINDOW_SIZE is still reached by adding the window's delta.
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

/* The transient window's level-1 table. A slot mapped under it is kernel data: no U and no X. */
    .balign 4096
    .global kickos_rv64_window_l1
kickos_rv64_window_l1:
    .fill   512, 8, 0

/* The one table page the kernel writes after boot (arch_aspace_acquire and _release), so it
 * cannot sit in the read-execute leaf. Its own section, homed in the writable half by
 * virt_rv64.ld, and LOADED: _start fills the level-1 entry above it before .bss is zeroed.
 */
    .section .mmu_leaves, "aw"
    .balign 4096
    .global kickos_rv64_window_l0
kickos_rv64_window_l0:
    .fill   512, 8, 0

#if KICKOS_NUM_CORES > 1
/* The boot handshake, reached UNTRANSLATED by every hart. LOADED and outside _sbss.._ebss for
 * the same reason .mmu_leaves is: a hart claims its row and parks before the boot hart has
 * reached the C runtime, and .bss zeroing would take the claims back under it.
 *
 * The release words are what a hart parks on and nothing writes them yet.
 */
    .section .smpboot, "aw", @progbits
    .balign 8
    .global kickos_rv64_hart_release
kickos_rv64_hart_release:
    .fill   KICKOS_NUM_CORES, 8, 0

/* One doubleword per hart for the misaligned-access and PMP-grant probes, so no two harts
 * write one word. */
    .global kickos_rv64_hart_probe
kickos_rv64_hart_probe:
    .fill   KICKOS_NUM_CORES, 8, 0

/* Two doublewords per hart for the machine-mode trampoline: its own CLINT msip address, then
 * the one register it cannot hold in mscratch. */
    .balign 8
    .global kickos_rv64_hart_mtrap
kickos_rv64_hart_mtrap:
    .fill   KICKOS_NUM_CORES * 2, 8, 0

/* Naturally aligned: amoswap.w takes a 4-byte-aligned address. */
    .global kickos_rv64_hart_claim
kickos_rv64_hart_claim:
    .fill   KICKOS_NUM_CORES, 4, 0
    .balign 4
.Lrefuse_claim:
    .word   0
#endif

/* One page between the root and the level-2 table per level above 2. Checked here because a
 * difference of two labels is a constant only once both are defined.
 */
.if (kickos_rv64_high_l2 - kickos_rv64_root) != UPPER_TABLES * 4096
.error "the boot tables reserve a different number of pages above level 2 than the configured paging mode names"
.endif

    .end
