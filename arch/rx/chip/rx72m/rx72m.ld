/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the Renesas RX72M (RX72M Group, RXv3 core).
 * (RX72M Group User's Manual: Hardware, r01uh0804ej0120, Rev.1.20.)
 *
 *   Code flash : 4 MiB  @ 0xFFC0_0000 .. 0xFFFF_FFFF (UM memory map)
 *   SRAM       : 512 KiB @ 0x0000_0000 (main on-chip RAM; expansion RAM omitted)
 *   OFSM       : option-setting memory @ 0xFE7F_5D00 (MDE/OFS0/OFS1) -- data in
 *                the image, not code (UM sec.7). Determines endian + WDT/LVD state.
 *
 * Two vector tables (UM sec.2.6):
 *   - Fixed exception table at 0xFFFF_FF80 (EXTB reset value). Its last word,
 *     0xFFFF_FFFC, is the RESET vector (fetched by hardware at power-on).
 *   - Relocatable interrupt table (INTB), 256 words in ROM; startup sets INTB.
 *
 * RX section names: GCC emits .text/.rodata/.data/.bss for KickOS TUs, but the
 * GNU RX libc/libgcc use the RX-native short names P (code), C/C_1/C_2/W/L
 * (const), D/D_1/D_2 (data), B/B_1/B_2 (bss) -- all matched below.
 *
 * RX symbol names: the RX psABI user-label-prefix is '_', so a C `extern _sidata`
 * resolves to `__sidata` and `extern __kickos_ram_start` to `___kickos_ram_start`.
 * The link-time symbols below carry that extra leading underscore to match; asm
 * symbols (e.g. _istack) are used verbatim and take no extra prefix.
 */

ENTRY(_start)

MEMORY
{
    RAM   (rwx) : ORIGIN = 0x00000000, LENGTH = 512K
    OFSM  (r)   : ORIGIN = 0xFE7F5D00, LENGTH = 0x10
    ROM   (rx)  : ORIGIN = 0xFFC00000, LENGTH = 0x003FFF80  /* up to 0xFFFFFF80 */
    FVECT (rx)  : ORIGIN = 0xFFFFFF80, LENGTH = 0x80        /* fixed vectors + reset */
}

_istack = ORIGIN(RAM) + LENGTH(RAM);   /* boot/interrupt stack (ISP): top of SRAM */
_kernel_stack_size = 8K;               /* ISP (boot/ISR) stack reservation */

/* App-owned globals get their own MPU region so an unprivileged thread reaches its
   own .data/.bss but not the kernel's. The RX MPU takes arbitrary 16-byte-page
   bounds, but the shared alloc/linker path shapes regions as a power of two (so the
   PMSA/NAPOT backends share the block), so the app data is padded up to this pow2
   size; a real app whose globals exceed it trips the ASSERT below. Keep it pow2.
   Gated on KICKOS_HAVE_MPU (the script is cpp-preprocessed): a non-enforcement
   build reserves no app-data block and separates nothing (zero overhead). */
#if KICKOS_HAVE_MPU
_appdata_size = 4K;
#endif

SECTIONS
{
    /* Option-setting memory: little-endian + WDT/LVD off (all-ones defaults;
       UM sec.7.2.5 MDE little-endian = 0xFFFFFFFF, sec.7.2.3/4 OFS0/OFS1). */
    .ofsm : { KEEP(*(.ofsm)) } > OFSM

    .rvectors : ALIGN(4)
    {
        KEEP(*(.rvectors))     /* 256-word relocatable interrupt table (INTB) */
    } > ROM

    .text : ALIGN(4)
    {
        *(.text .text.*)
        *(P P_*)               /* RX-native code (libc/libgcc) */
        *(.rodata .rodata.*)
        *(C C_1 C_2 C$*)       /* RX-native const */
        *(W W_1 W_2)           /* RX-native const (16/8-bit addressed) */
        *(L)                   /* literal pool */
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > ROM

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in .kickos_app_init_array and run from root_entry with the kernel live.
       RX prepends one underscore, so the C `__kickos_app_init_array_*` in kmain
       resolves to the triple-underscore linker symbols below. */
    .init_array : ALIGN(4)
    {
        ___init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_arch_rxv3.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_chip_rx72m.a:*(SORT(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT(.init_array.*) .init_array))
        ___init_array_end = .;
    } > ROM

    .kickos_app_init_array : ALIGN(4)
    {
        ___kickos_app_init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        ___kickos_app_init_array_end = .;
    } > ROM

    /* Copy/zero tables for the generic C-runtime init (kickos_ranges_init): flash
       rodata, read before .data exists. Copy triples {src,dst,len}; zero pairs
       {dst,len}. App ranges first, kernel ranges second (order is immaterial).
       RX symbol prefix: C `__kickos_copy_table_start` resolves to the linker symbol
       with one extra leading underscore (`___kickos_copy_table_start`). */
    .kickos_init_tables : ALIGN(4)
    {
        ___kickos_copy_table_start = .;
#if KICKOS_HAVE_MPU
        LONG(LOADADDR(.appdata)); LONG(ADDR(.appdata)); LONG(SIZEOF(.appdata));
#endif
        LONG(LOADADDR(.data));    LONG(ADDR(.data));    LONG(SIZEOF(.data));
        ___kickos_copy_table_end = .;
        ___kickos_zero_table_start = .;
#if KICKOS_HAVE_MPU
        LONG(ADDR(.appbss)); LONG(SIZEOF(.appbss));
#endif
        LONG(ADDR(.bss));    LONG(SIZEOF(.bss));
        ___kickos_zero_table_end = .;
    } > ROM

    /* App-owned globals as a pow2 MPU region at the base of RAM (ORIGIN is 0, so
       maximally aligned). Grouped by path (objects under a user/ dir = the app
       object + user runtime); placed BEFORE the kernel .data/.bss so those input
       sections are consumed here and the kernel catch-all below gets only
       kernel/arch/lib. .appdata is loaded from ROM; .appbss + the pad are zeroed. */
#if KICKOS_HAVE_MPU
    .appdata ORIGIN(RAM) : ALIGN(4)
    {
        ___kickos_appdata_start = .;
        *user*(.data .data.*)
        *user*(D D_1 D_2)      /* RX-native initialized data (if any) */
        . = ALIGN(4);
    } > RAM AT > ROM

    .appbss (NOLOAD) : ALIGN(4)
    {
        *user*(.bss .bss.*)
        *user*(B B_1 B_2)      /* RX-native bss (if any) */
        *user*(COMMON)
        . = ALIGN(4);
        _appdata_used_end = .;
        . = ___kickos_appdata_start + _appdata_size; /* pad the block up to pow2 */
        ___kickos_appdata_end = .;
    } > RAM
#endif

    __sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        __sdata = .;
        *(.data .data.*)
        *(D D_1 D_2)           /* RX-native initialized data */
        . = ALIGN(4);
        __edata = .;
    } > RAM AT > ROM

    .bss (NOLOAD) : ALIGN(4)
    {
        __sbss = .;
        *(.bss .bss.*)
        *(B B_1 B_2)           /* RX-native bss */
        *(COMMON)
        . = ALIGN(4);
        __ebss = .;
    } > RAM

    . = ALIGN(32);
    ___kickos_ram_start = .;
    ___kickos_ram_end = _istack - _kernel_stack_size;

    /* App code region for unprivileged threads (RX + read): the code flash. The RX
       MPU takes arbitrary 16-byte-page bounds, so this need not be pow2. Lets an
       unprivileged thread fetch its code + read flash rodata. END = ORIGIN+LENGTH
       (0xFFFFFF80, the start of the fixed vectors) and NOT 0xFFC00000+4M -- the
       latter is 0x1_0000_0000, which wraps to 0 in a 32-bit uintptr_t and would
       make domain.cc's `end > start` false, silently dropping the code region and
       faulting the thread on its first instruction fetch. The fixed vectors are
       never read by user code, so excluding the top 0x80 is harmless. */
    ___kickos_code_start = ORIGIN(ROM);
    ___kickos_code_end = ORIGIN(ROM) + LENGTH(ROM);

    /* Fixed exception vector table incl the reset vector at 0xFFFFFFFC. */
    .fvectors : { KEEP(*(.fvectors)) } > FVECT

    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}

ASSERT(___kickos_ram_start <= ___kickos_ram_end,
       "KickOS: static data overflows RAM (no room for the user-RAM pool + ISP stack)")

/* App globals must fit the pow2 region. If this trips, raise _appdata_size to the
   next power of two (it is the app's MPU-granted .data/.bss window). A zero-sized
   used-end means the user-path selector matched nothing -- check the build layout. */
#if KICKOS_HAVE_MPU
ASSERT(_appdata_used_end <= ___kickos_appdata_start + _appdata_size,
       "KickOS: app .data/.bss overflow _appdata_size (raise it to the next pow2)")
#endif
