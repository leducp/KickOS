/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: where the ESP32-C6's script (esp32c6.ld) and its image template
 * (esp32c6.image.ld) link every image's code span, and the flash geometry the script maps it by.
 * An own-image AMP node keeps its code in HP SRAM: the LP CPU cannot reach external memory
 * (TRM 5.3.1), and node 0's partition is one ROM-loaded image.
 */
#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_ESP32C6_FLASH_LD_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_ESP32C6_FLASH_LD_H

#if !KICKOS_AMP_OWN_IMAGE
#define KICKOS_RV32_IMAGE_CODE_APART 1
#define KICKOS_RV32_IMAGE_CODE_REGION FLASHTEXT
#endif

/* External memory through the cache (TRM 5.3.3.1), which the MMU's 256 entries cover. */
#define KICKOS_ESP32C6_WINDOW_BASE 0x42000000
#define KICKOS_ESP32C6_WINDOW_END 0x43000000
#define KICKOS_ESP32C6_FLASH_PAGE 0x10000
#define KICKOS_ESP32C6_MMU_ENTRIES 256
#define KICKOS_ESP32C6_FLASH_IMAGE_OFFSET 0x0
#define KICKOS_ESP32C6_FLASH_TEXT_LOAD 0x90000

#endif
