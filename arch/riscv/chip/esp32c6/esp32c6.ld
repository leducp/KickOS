/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * ESP32-C6 linker script. The kernel and every window run from the 512 KiB HP SRAM at
 * 0x4080_0000; every image's code span, rodata and .data template included, links in FLASHTEXT,
 * flash through the cache, which Reset_Handler maps. Fetch and load share one address in both
 * (TRM Table 5.3-1), so ONE TOR code window covers unprivileged fetch AND the shared-RX read.
 */

#include <chip_layout.h>
#include <amp_partition.ld.h>
#include "esp32c6_flash.ld.h"

/* The kernel's .data loads where it runs; every image's loads at its template
   (esp32c6.image.ld). */
#define KICKOS_RV32_LOADS_AT_VMA 1
#define KICKOS_RV32_IMAGE_DATA_LOADED 1

#if !KICKOS_AMP_OWN_IMAGE && !defined(KICKOS_LAYOUT_BOARD_FLASH_SIZE)
#error "the board file states no `flash` size, which bounds the text the cache maps"
#endif

MEMORY
{
#if KICKOS_AMP_OWN_IMAGE
    RAM (rwx) : ORIGIN = KICKOS_AMP_IMAGE_BASE, LENGTH = KICKOS_AMP_NODE_SHARE
    SHARED (rw) : ORIGIN = KICKOS_AMP_SHARED_BASE, LENGTH = KICKOS_AMP_SHARED_SIZE
#else
    KICKOS_LINK_RAM
    KICKOS_LINK_FLASHTEXT
#endif
}

#include <rv32imac.ld.h>

#include <boot_arena.ld.h>
KICKOS_BOOT_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)
KICKOS_POOL_ARENA_ASSERT(__kickos_ram_start, __kickos_ram_end)

#if KICKOS_AMP_OWN_IMAGE
ASSERT(KICKOS_AMP_PARTITION_BASE == 0x40800000 && KICKOS_AMP_PARTITION_END <= 0x40880000,
       "KickOS: ESP32-C6 AMP partition must fit its 512 KiB HP SRAM")
#else
SECTIONS
{
    /* All-zero halfwords are illegal instructions: a fall-through past the last image's code
       stays in a mapped page and traps. */
    .kickos_flash_text_tail : ALIGN(4)
    {
        LONG(0)
        . += 60;
        . = ALIGN(KICKOS_ESP32C6_FLASH_PAGE);
    } > FLASHTEXT =0
}
__kickos_esp32_flash_text_start = ABSOLUTE(ORIGIN(FLASHTEXT));
__kickos_esp32_flash_text_end =
    ABSOLUTE(ADDR(.kickos_flash_text_tail) + SIZEOF(.kickos_flash_text_tail));
__kickos_esp32_flash_text_load = ABSOLUTE(KICKOS_ESP32C6_FLASH_TEXT_LOAD);
__kickos_esp32_flash_image_offset = ABSOLUTE(KICKOS_ESP32C6_FLASH_IMAGE_OFFSET);
__kickos_esp32_flash_window_start = ABSOLUTE(ORIGIN(FLASHTEXT));
__kickos_esp32_flash_window_end = ABSOLUTE(ORIGIN(FLASHTEXT) + LENGTH(FLASHTEXT));

/* --- Task text in flash ---------------------------------------------------------------------
 * The ROM boots the image at KICKOS_ESP32C6_FLASH_IMAGE_OFFSET and loads its SRAM segments
 * alone; tools/esp-image.py places the FLASHTEXT span at KICKOS_ESP32C6_FLASH_TEXT_LOAD in the
 * same file. Every byte esptool writes is at most HP SRAM plus its headers, so a load past that
 * bound is past any boot image the region admits.
 */
#define KICKOS_ESP32C6_ESPTOOL_HEADERS 0x1000

ASSERT(__kickos_images_code_start == ORIGIN(FLASHTEXT),
       "KickOS: the images' code span does not open the flash window, which the MMU maps from its first page")

ASSERT(__kickos_esp32_flash_text_start >= KICKOS_ESP32C6_WINDOW_BASE
       && __kickos_esp32_flash_text_end <= KICKOS_ESP32C6_WINDOW_END,
       "KickOS: the flash text has a page outside the external memory window the cache MMU maps")

ASSERT(__kickos_esp32_flash_text_end - __kickos_esp32_flash_text_start
       <= KICKOS_ESP32C6_MMU_ENTRIES * KICKOS_ESP32C6_FLASH_PAGE,
       "KickOS: the flash text needs more pages than the cache MMU has entries")

ASSERT(KICKOS_ESP32C6_FLASH_TEXT_LOAD % KICKOS_ESP32C6_FLASH_PAGE == 0
       && __kickos_esp32_flash_text_start % KICKOS_ESP32C6_FLASH_PAGE == 0,
       "KickOS: the flash text's load or its window is not on a 64 KiB cache MMU page")

ASSERT(KICKOS_ESP32C6_FLASH_TEXT_LOAD >= KICKOS_ESP32C6_FLASH_IMAGE_OFFSET + LENGTH(RAM)
                                         + KICKOS_ESP32C6_ESPTOOL_HEADERS,
       "KickOS: the flash text's load is within the reach of the boot image the region admits; a large enough image would be overwritten by its own text")

ASSERT(KICKOS_ESP32C6_FLASH_TEXT_LOAD + (__kickos_esp32_flash_text_end - __kickos_esp32_flash_text_start)
       <= KICKOS_LAYOUT_BOARD_FLASH_SIZE,
       "KickOS: the task text runs past the end of the board's flash")
#endif
