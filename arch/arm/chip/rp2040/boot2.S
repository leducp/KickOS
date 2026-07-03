/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * RP2040 second-stage bootloader (boot2). Clean-room from the RP2040 datasheet
 * (§2.8.1 boot sequence, §4.10 SSI); no vendor sources consulted.
 *
 * Context: the bootrom copies the first 256 bytes of flash into SRAM5, verifies
 * the CRC-32/MPEG-2 over bytes 0..251 (checksum tool: cmake/rp2040_checksum.py),
 * then JUMPS into this payload running FROM SRAM. At that point the bootrom has
 * already muxed the QSPI pins and left the Synopsys SSI enabled in a plain 1-bit
 * SPI mode used for the 03h reads that loaded us. Our only job (§2.8.1.3) is to
 * reconfigure the SSI for execute-in-place so the CPU can fetch directly from the
 * 0x1000_0000 XIP window, then hand control to the application vector table.
 *
 * XIP mode here is the near-universal "03h serial read per access" (§4.10.3):
 * each XIP access issues an 8-bit 0x03 command + 24-bit address, then clocks in
 * data on a single line. Slow but compatible with any SPI flash -- correct for
 * first bring-up. All other setup (PLLs, regulator, faster QSPI modes) is the
 * platform's job over XIP, per the datasheet.
 *
 * This code is position-independent (runs from SRAM at a bootrom-chosen address):
 * only PC-relative branches and literal-pool loads; the sole absolute code target
 * is the app vector table, a data constant.
 */

    .syntax unified
    .cpu cortex-m0plus
    .thumb

    /* The SSI register block (§4.10.13) and its byte offsets. */
    .equ XIP_SSI_BASE,   0x18000000
    .equ SSI_CTRLR0,     0x00
    .equ SSI_CTRLR1,     0x04
    .equ SSI_SSIENR,     0x08
    .equ SSI_BAUDR,      0x14
    .equ SSI_SPI_CTRLR0, 0xf4

    /* CTRLR0 = 0x001f0300: SPI_FRF=STD(0), DFS_32=31 (32-bit data frames),
     * TMOD=EEPROM_READ(3) -- transmit the cmd/addr control frame, then receive. */
    .equ CTRLR0_XIP,     0x001f0300
    /* SPI_CTRLR0 = 0x03000218: XIP_CMD=0x03, INST_L=8b(2<<8), ADDR_L=24b(6<<2),
     * TRANS_TYPE=1C1A(0) -- command+address both in standard 1-bit SPI. */
    .equ SPI_CTRLR0_XIP, 0x03000218
    /* SCK = clk_sys / BAUDR. clk_sys is the ~6.5 MHz ROSC at boot (and 12 MHz XOSC
     * after chip init); /4 keeps SCK comfortably within any flash's 03h limit. */
    .equ BAUDR_DIV,      4

    .equ M0PLUS_VTOR,    0xe000ed08
    .equ XIP_VECTORS,    0x10000100      /* app vector table (after 256B boot2) */

    .section .text
    .align 2
    .global _rp2040_boot2_entry
    .thumb_func
_rp2040_boot2_entry:
    ldr     r3, =XIP_SSI_BASE

    /* The SSI cannot be reconfigured while enabled (§4.10.9): disable it. */
    movs    r1, #0
    str     r1, [r3, #SSI_SSIENR]

    movs    r1, #BAUDR_DIV
    str     r1, [r3, #SSI_BAUDR]

    ldr     r1, =CTRLR0_XIP
    str     r1, [r3, #SSI_CTRLR0]

    movs    r1, #0
    str     r1, [r3, #SSI_CTRLR1]        /* NDF unused in XIP; keep deterministic */

    ldr     r1, =SPI_CTRLR0_XIP
    movs    r2, #SSI_SPI_CTRLR0          /* offset 0xf4 > 5-bit imm range */
    str     r1, [r3, r2]

    movs    r1, #1
    str     r1, [r3, #SSI_SSIENR]        /* re-enable: XIP window is now live */

    /* Hand off to the application. Point VTOR at the flash vector table, load the
     * initial MSP from vector[0], and branch to the reset handler at vector[1]. */
    ldr     r0, =XIP_VECTORS
    ldr     r1, =M0PLUS_VTOR
    str     r0, [r1]
    ldr     r1, [r0, #0]
    msr     msp, r1
    ldr     r0, [r0, #4]
    bx      r0

    .align 2
    .pool
