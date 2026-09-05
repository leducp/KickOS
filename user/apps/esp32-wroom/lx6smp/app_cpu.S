/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * lx6smp APP_CPU side. Entered by the ESP32 ROM, which jumps to the address the
 * PRO_CPU left in DPORT_APPCPU_CTRL_REG_D_REG (TRM v5.8 Register 12.8, 0x3FF00038).
 *
 * CALL0 ONLY, NO STACK, NO CALL, NO ENTRY/RETW, NO wsr.vecbase: a1 holds whatever the
 * ROM left, and nothing here writes memory through it.
 *
 * THIS CORE MUST NOT TOUCH THE UART: one UART, two cores, and the console interleaves at
 * byte granularity. Results go into the shared cells and PRO_CPU reports.
 *
 * A fault here reaches the ROM vectors and stops with no report, so the last checkpoint
 * written localises the death:
 *   ck1 only            -> died at rsr.prid
 *   ck1 + prid + ck2    -> died in the l32ai / s32c1i loop
 * Outside the measured loop only s32i / l32i / memw are used: Core Architecture, so they
 * cannot be the missing option under test.
 */

/* Offsets into struct cells (main.cc), pinned there by static_assert. */
    .set CELL_COUNTER, 0
    .set CELL_GO,      4
    .set CELL_CK1,     8
    .set CELL_PRID,    12
    .set CELL_CK2,     16
    .set CELL_CK3,     20
    .set CELL_OK,      24
    .set CELL_RETRY,   28
    .set CELL_ROUNDS,  32

    .set CK1_MAGIC,    0xA11E0001
    .set CK2_MAGIC,    0xA11E0002
    .set CK3_MAGIC,    0xA11E0003

    .text

    .align 4
    .global kickos_lx6smp_app_entry
    .type kickos_lx6smp_app_entry, @function
kickos_lx6smp_app_entry:
    movi    a2, g_lx6smp_cells

    movi    a3, CK1_MAGIC
    s32i    a3, a2, CELL_CK1
    memw

    /* RISKY 1: PRID is Special Register 235 (ISA summary 6.4, Table 120). RSR of a
       register number >= 64 raises PrivilegedCause if CRING != 0, and an unconfigured
       register generally raises IllegalInstructionCause (ISA summary 8.3.276, p.585).
       Neither is reportable from this core. */
    rsr.prid a4
    s32i    a4, a2, CELL_PRID
    memw

    movi    a3, CK2_MAGIC
    s32i    a3, a2, CELL_CK2
    memw

    /* Wait for PRO_CPU to open the contended phase. Plain l32i, not l32ai: l32ai needs
       the Multiprocessor Synchronization Option, which is one of the things under test. */
.Lwait_go:
    memw
    l32i    a3, a2, CELL_GO
    beqz    a3, .Lwait_go

    l32i    a5, a2, CELL_ROUNDS         /* rounds remaining */
    movi    a6, 0                       /* successful stores */
    movi    a7, 0                       /* failed compares */

.Lround:
    beqz    a5, .Ldone

    /* RISKY 2 and 3: the canonical S32C1I increment of ISA summary 4.3.13.2 (p.119).
       s32c1i writes the value READ from the location into its data register, so the
       store succeeded exactly when that value equals the loaded old value. */
.Lretry:
    l32ai   a8, a2, CELL_COUNTER
    wsr.scompare1 a8
    addi    a9, a8, 1
    s32c1i  a9, a2, CELL_COUNTER
    beq     a9, a8, .Lstored
    addi    a7, a7, 1
    j       .Lretry

.Lstored:
    addi    a6, a6, 1
    addi    a5, a5, -1
    j       .Lround

.Ldone:
    s32i    a6, a2, CELL_OK
    s32i    a7, a2, CELL_RETRY
    memw
    movi    a3, CK3_MAGIC
    s32i    a3, a2, CELL_CK3
    memw

    /* waiti is privileged, and comes after the last checkpoint so a refusal costs no result. */
.Lpark:
    waiti   0
    j       .Lpark
    .size kickos_lx6smp_app_entry, .-kickos_lx6smp_app_entry
