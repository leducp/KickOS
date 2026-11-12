/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Linker script for the ESP32 (WROOM-32 module), Xtensa LX6. Regions from the ESP32 TRM v5.8,
 * Table 3.3-2 and Table 4.3-9:
 *
 *   IRAM       SRAM0's upper 128 KiB from 0x4008_0000: the kernel's vectors and text. The low
 *              64 KiB is the two caches' pools.
 *   FLASHTEXT  VAddr1 from 0x400D_0000, flash through the cache: every image's text, which the
 *              build places in flash at __kickos_esp32_flash_text_load and Reset_Handler maps.
 *   DRAM       SRAM2 from 0x3FFB_0000: what the ROM loads, every image's rodata included, the
 *              kernel's .data and .bss, then the arena to its end.
 *   DRAM1      SRAM1's data alias above the ROM's words: every image's window, and the boot
 *              stacks at its top.
 *
 * tools/esp-image.py hides FLASHTEXT from esptool, so the ROM loads IRAM and DRAM alone. The
 * instruction bus is word-access only, which is why rodata lives in DRAM.
 *
 * Windowed ABI: the mandatory WindowOverflow/Underflow vectors are part of the .vectors table
 * placed at VECBASE (startup.S); they spill/fill to each thread's OWN stack, so no dedicated
 * exception stack is reserved here.
 */

#include <chip_layout.h>
#include <kickos/arch/core_stack.ld.h>
#include "esp32_image.ld.h"

#define KICKOS_ESP32_FLASH_PAGE 0x10000
#define KICKOS_ESP32_FLASH_IMAGE_OFFSET 0x1000
#define KICKOS_ESP32_FLASH_TEXT_LOAD 0x70000
#define KICKOS_ESP32_VADDR1_END 0x40400000

ENTRY(_start)

#ifndef KICKOS_LAYOUT_BOARD_FLASH_SIZE
#error "the board file states no `flash` size, which bounds the text the cache maps"
#endif

MEMORY
{
    KICKOS_LINK_IRAM
    KICKOS_LINK_FLASHTEXT
    KICKOS_LINK_DRAM
    KICKOS_LINK_DRAM1
}

_estack = ORIGIN(DRAM1) + LENGTH(DRAM1); /* core 0's boot stack top: top of DRAM1 */
_kernel_stack_size = KICKOS_LX6_CORE_STACK; /* boot/interrupt stack reserve, PER CORE */
/* ONE BOOT STACK PER CORE, stacked downward from the top of DRAM1. */
_kickos_boot_stacks = _estack - KICKOS_NUM_CORES * _kernel_stack_size;
#if KICKOS_NUM_CORES > 1
_estack_core1 = _estack - _kernel_stack_size;
#endif

SECTIONS
{
    /* Xtensa exception/interrupt vectors at VECBASE. Must be 1 KiB aligned; the
       startup sets wsr.vecbase to _kickos_vectors. Placed at IRAM origin. */
    .vectors ORIGIN(IRAM) :
    {
        _kickos_vectors = .;
        KICKOS_KERNEL_KEEP(.vectors)
        . = ALIGN(4);
    } > IRAM

    .text : ALIGN(4)
    {
        KICKOS_KERNEL_SECTIONS(.literal .literal.*)
        KICKOS_KERNEL_SECTIONS(.text .text.*)
        KICKOS_CODE_DEBRIS_SECTIONS
        . = ALIGN(4);
    } > IRAM

    /* Every image's text, its code span, which tools/image-link.py writes per link
       (KICKOS_ESP32_IMAGE_TEXT). */
    INCLUDE kickos_images_code.ld

    /* Zeros decode as `ill`: a prefetch or a fall-through past the last image's text stays in a
       mapped page and traps. */
    .kickos_flash_text_tail : ALIGN(4)
    {
        LONG(0)
        . += 60;
        . = ALIGN(KICKOS_ESP32_FLASH_PAGE);
    } > FLASHTEXT =0
    __kickos_esp32_flash_text_start = ABSOLUTE(ORIGIN(FLASHTEXT));
    __kickos_esp32_flash_text_end =
        ABSOLUTE(ADDR(.kickos_flash_text_tail) + SIZEOF(.kickos_flash_text_tail));
    __kickos_esp32_flash_text_load = ABSOLUTE(KICKOS_ESP32_FLASH_TEXT_LOAD);
    __kickos_esp32_flash_image_offset = ABSOLUTE(KICKOS_ESP32_FLASH_IMAGE_OFFSET);

    .rodata : ALIGN(4)
    {
        KICKOS_KERNEL_SECTIONS(.rodata .rodata.*)
        KICKOS_KERNEL_SECTIONS(.rodata_wlog .rodata_wlog.*)
        . = ALIGN(4);
    } > DRAM

    /* Global ctors SPLIT: .init_array keeps ONLY the closed KickOS-owned set
       (kernel/arch/chip/lib), run early by Reset_Handler; other (app/lib) ctors
       land in each image's constructor window and run from its startup with the kernel live.
       The xtensa GCC, configured without --enable-initfini-array (conan/toolchain), emits
       .ctors: a table section no selector names is dropped by --gc-sections, constructor and
       all. SORT_BY_INIT_PRIORITY reads either spelling's priority for a forward walk. */
    .init_array : ALIGN(4)
    {
        __init_array_start = .;
        KEEP(*libkickos_kernel.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_kernel.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        KEEP(*libkickos_arch_lx6.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_arch_lx6.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        KEEP(*libkickos_chip_esp32.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_chip_esp32.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        KEEP(*libkickos_lib.a:*(SORT_BY_INIT_PRIORITY(.init_array.*) .init_array))
        KEEP(*libkickos_lib.a:*(SORT_BY_INIT_PRIORITY(.ctors.*) .ctors))
        __init_array_end = .;
    } > DRAM

    /* .fini_array, app side, ASSERTed empty below; nothing walks it. See
       arch/arm/chip/stm32f411/stm32f411.ld. */
    .kickos_app_fini_array : ALIGN(4)
    {
        __kickos_app_fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array .dtors))
        __kickos_app_fini_array_end = .;
    } > DRAM

    KICKOS_IMAGE_TABLE(DRAM, LONG)

    /* The kernel's own DWARF EH, kept whole and ended in a zero-length CIE, the thread-local
       template of every image, then what the ROM loads of every image: header, rodata,
       constructor window and .data template (KICKOS_ESP32_IMAGE), which tools/image-link.py
       writes per link. Every byte the ROM loads in DRAM is one span up to the kernel's .data, so
       esptool makes it one segment however many images there are. */
    .kickos_kernel_eh_frame : ALIGN(4)
    {
        KEEP(KICKOS_KERNEL_SECTIONS(.eh_frame))
        LONG(0)
        . = ALIGN(8);
    } > DRAM

    KICKOS_TLS_TEMPLATE(DRAM)

    INCLUDE kickos_images.ld

    _sidata = LOADADDR(.data);

    .data : ALIGN(4)
    {
        _sdata = .;
        KICKOS_KERNEL_SECTIONS(.data .data.*)
        . = ALIGN(4);
        _edata = .;
    } > DRAM

    .bss (NOLOAD) : ALIGN(4)
    {
        _sbss = .;
        KICKOS_KERNEL_SECTIONS(.bss .bss.*)
        KICKOS_KERNEL_SECTIONS(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > DRAM

    /* Every image's window (KICKOS_ESP32_WINDOW), which tools/image-link.py writes per link,
       image 1's named as the app's below. */
    INCLUDE kickos_images_windows.ld
    .kickos_windows_end (NOLOAD) :
    {
        __kickos_esp32_windows_end = .;
    } > DRAM1
    __eh_frame_start = KICKOS_LD_IMAGE_SYM(1, eh_frame);
    __kickos_app_init_array_start = KICKOS_LD_IMAGE_SYM(1, ctor_start);
    __kickos_app_init_array_end = KICKOS_LD_IMAGE_SYM(1, ctor_end);
    _appdata_used_end = KICKOS_LD_IMAGE_SYM(1, bss_end);
    _kickos_heap_start = KICKOS_LD_IMAGE_SYM(1, heap_start);
    _kickos_heap_limit = KICKOS_LD_IMAGE_SYM(1, heap_end);
    KICKOS_IMAGE0_TLS()
    KICKOS_IMAGE0_NAMES()

    /* User-RAM pool (arch_ram_alloc): the rest of DRAM past the kernel's .bss. */
    __kickos_ram_start = ALIGN(_ebss, 16);
    __kickos_ram_end = ORIGIN(DRAM) + LENGTH(DRAM);

    /* Chip memory extents read by the non-enforcing user-pointer admission
       (arch_ram_common.cc). No code extent: all code sits behind the instruction bus. */
    __kickos_rom_start = 0;
    __kickos_rom_end = 0;
    __kickos_sram_start = ORIGIN(DRAM);
    KICKOS_IMAGE_BOUNDS(ORIGIN(KICKOS_ESP32_IMAGE_TEXT_REGION),
                        ORIGIN(KICKOS_ESP32_IMAGE_TEXT_REGION) + LENGTH(KICKOS_ESP32_IMAGE_TEXT_REGION),
                        ORIGIN(IRAM), ADDR(.text) + SIZEOF(.text),
                        ORIGIN(DRAM1), ORIGIN(DRAM1) + LENGTH(DRAM1),
                        _sdata, _ebss, _kickos_boot_stacks, _estack, 0)

    /* The images' windows, where the user-pointer admission finds a task's own data. */
    __kickos_app_rom_start = 0;
    __kickos_app_rom_end = 0;
    __kickos_app_sram_start = ORIGIN(DRAM1);
    __kickos_app_sram_end = __kickos_esp32_windows_end;
    __kickos_app_load_delta = 0;

    /DISCARD/ : { *(.xtensa.info) *(.comment) *(.xt.*) }

    KICKOS_NONALLOC_SECTIONS
}

KICKOS_STATIC_RELOC_ASSERT()
KICKOS_TLS_ALIGN_ASSERT()
KICKOS_TLS_FIT_ASSERT()
INCLUDE kickos_images_asserts.ld

ASSERT(__kickos_ram_start <= __kickos_ram_end,
       "KickOS: static data overflows DRAM (no room for the user-RAM pool)")

ASSERT(__kickos_esp32_windows_end <= _kickos_boot_stacks,
       "KickOS: the task images' windows overflow DRAM1 into the boot stacks")

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

KICKOS_FINI_EMPTY_ASSERT()

KICKOS_HEAP_MIN_ASSERT()

/* --- The coherency condition, as a refusal ---------------------------------------------
 * Both CPUs reach embedded memory with no cache in the path: the two 32 KiB caches are
 * carved out of SRAM0 and serve the external flash and PSRAM path alone, and the TRM
 * documents no invalidate anywhere and no flush but a whole-cache one that DISCARDS dirty
 * data. Shared kernel state is coherent across the two CPUs only while it is IN internal
 * SRAM. Unconditional, not gated on the core count.
 *
 * The bounds are the SoC's, from TRM v5.8 Table 3.3-2, and NOT this script's own regions:
 * an assert reading ORIGIN(DRAM) against LENGTH(DRAM) passes wherever the region is
 * repointed.
 *
 * Data bus: SRAM2 opens at 0x3FFA_E000 and SRAM1 closes at 0x3FFF_FFFF, contiguous.
 * Instruction bus: the low 64 KiB of SRAM0 IS the cache pool, so the floor is 0x4008_0000
 * rather than SRAM0's own 0x4007_0000, and SRAM0 closes at 0x4009_FFFF.
 */
#define KICKOS_ESP32_ISRAM_DATA_FLOOR  0x3FFAE000
#define KICKOS_ESP32_ISRAM_DATA_CEIL   0x40000000
#define KICKOS_ESP32_ISRAM_TEXT_FLOOR  0x40080000
#define KICKOS_ESP32_ISRAM_TEXT_CEIL   0x400A0000

ASSERT(ADDR(.rodata) >= KICKOS_ESP32_ISRAM_DATA_FLOOR && _estack <= KICKOS_ESP32_ISRAM_DATA_CEIL,
       "KickOS: kernel data leaves internal SRAM. Run queues, control blocks, the capability table and the kernel lock counters are coherent across the two CPUs only by there being no cache over them, and the caches sit on the external path; a shared kernel over this placement loses writes with no fault")

ASSERT(_kickos_boot_stacks >= KICKOS_ESP32_ISRAM_DATA_FLOOR && _estack <= KICKOS_ESP32_ISRAM_DATA_CEIL,
       "KickOS: a boot stack leaves internal SRAM. Every core's boot and interrupt stack is carved below _estack, and the level-1 entry spills the interruptee's register windows onto whichever stack it is standing on; behind a cache neither core can invalidate, that spill is lost rather than faulted")

ASSERT(_kickos_vectors >= KICKOS_ESP32_ISRAM_TEXT_FLOOR && ADDR(.text) + SIZEOF(.text) <= KICKOS_ESP32_ISRAM_TEXT_CEIL,
       "KickOS: kernel text leaves internal SRAM, or reaches into the SRAM0 cache pool. Both CPUs fetch it directly and neither can invalidate the other's copy; the only flush this part has discards dirty data")

/* SRAM1 is one set of words on two buses (TRM v5.8 3.3.2.4): DRAM1 is its data alias, so no
 * code may sit at its instruction alias. The ROM keeps its data and both CPUs' stacks at data
 * 0x3FFE_0000 to 0x3FFE_7E30 (`__stack_app`, the highest SRAM1 symbol in the ROM ELF): the loader
 * runs on them while it loads, and APP_CPU's ROM entry runs on its part until core 1 arrives.
 */
#define KICKOS_ESP32_SRAM1_DATA_FLOOR  0x3FFE0000
#define KICKOS_ESP32_ROM_SRAM1_DATA_END 0x3FFE7E30

ASSERT(ORIGIN(DRAM) + LENGTH(DRAM) <= KICKOS_ESP32_SRAM1_DATA_FLOOR
       && ORIGIN(DRAM1) >= KICKOS_ESP32_ROM_SRAM1_DATA_END,
       "KickOS: a data region reaches the SRAM1 words the ROM keeps its data and boot stacks in; the boot loads over the loader, or APP_CPU's release runs its ROM entry on our data")

/* The data a reading system call may take from a task, against the addresses only the
 * instruction bus reaches (TRM v5.8 3.3.1: 0x4000_0000 to 0x4FFF_FFFF), where a byte or
 * unaligned load raises an exception in the kernel. */
#define KICKOS_ESP32_IBUS_FLOOR 0x40000000
#define KICKOS_ESP32_IBUS_CEIL  0x50000000

ASSERT(__kickos_rom_start == __kickos_rom_end
       || __kickos_rom_end <= KICKOS_ESP32_IBUS_FLOOR || __kickos_rom_start >= KICKOS_ESP32_IBUS_CEIL,
       "KickOS: the code extent the user-pointer admission reads from reaches the instruction bus; a task passing a pointer into text makes the kernel fault on its read")

ASSERT(__kickos_sram_start >= KICKOS_ESP32_ISRAM_DATA_FLOOR && __kickos_ram_start <= KICKOS_ESP32_ISRAM_DATA_CEIL,
       "KickOS: the static-data extent the user-pointer admission reads from leaves the data bus")

/* --- Task text in flash ---------------------------------------------------------------------
 * The ROM boots the image at KICKOS_ESP32_FLASH_IMAGE_OFFSET (tools/flash-esptool.sh), and the
 * build places the FLASHTEXT span at KICKOS_ESP32_FLASH_TEXT_LOAD in the same file. Every byte
 * esptool writes is at most what IRAM, DRAM and DRAM1 can hold plus its headers (8 bytes a
 * segment, 16 segments, the checksum and digest), so a load past that bound is past any boot
 * image the regions admit.
 */
#define KICKOS_ESP32_ESPTOOL_HEADERS 0x1000

ASSERT(__kickos_esp32_flash_text_end <= KICKOS_ESP32_VADDR1_END,
       "KickOS: the flash text reaches past VAddr1, whose 64 pages the cache MMU maps")

ASSERT(KICKOS_ESP32_FLASH_TEXT_LOAD % KICKOS_ESP32_FLASH_PAGE == 0
       && __kickos_esp32_flash_text_start % KICKOS_ESP32_FLASH_PAGE == 0,
       "KickOS: the flash text's load or its window is not on a 64 KiB cache MMU page")

ASSERT(KICKOS_ESP32_FLASH_TEXT_LOAD >= KICKOS_ESP32_FLASH_IMAGE_OFFSET + LENGTH(IRAM) + LENGTH(DRAM)
                                       + LENGTH(DRAM1) + KICKOS_ESP32_ESPTOOL_HEADERS,
       "KickOS: the flash text's load is within the reach of the boot image the regions admit; a large enough image would be overwritten by its own text")

ASSERT(KICKOS_ESP32_FLASH_TEXT_LOAD + (__kickos_esp32_flash_text_end - __kickos_esp32_flash_text_start)
       <= KICKOS_LAYOUT_BOARD_FLASH_SIZE,
       "KickOS: the task text runs past the end of the board's flash")

/* The app windows this script does not carve, stated EMPTY rather than left absent:
   the kernel's references are strong, so an omitted bound is a link error. */
KICKOS_APP_CODE_WINDOW_NONE()
KICKOS_APP_DATA_WINDOW_NONE()
