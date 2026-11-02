/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a linker script: the ESP32-C6's image template, run through cpp at configure beside its
 * script (arch/CMakeLists.txt), as arch/riscv/common/image.ld is for the other RV32IMAC chips.
 * Every image's .data loads at its template in its code span, and every part of a code span ends
 * on 8 bytes, so with no input section aligned beyond that the bytes the ROM loads are one run
 * and esptool makes them one segment however many images there are.
 */
#define KICKOS_RV32_IMAGE_PAD 8
#include <rv32imac_image.ld.h>

KIMGRULE(KICKOS_HAVE_MPU, KICKOS_CHIP_REGION_POW2, KICKOS_IMAGE_WINDOW_GRANULE)
KIMGPART(base)
KICKOS_RV32_WINDOWS_BASE
KIMGPART(code)
KICKOS_RV32_IMAGE_CODE(KIMGN, 1)
KIMGPART(image)
KICKOS_RV32_IMAGE_WINDOW_LOADED(KIMGN, KIMGBASE, KIMGALIGN, KIMGHEAP)
KIMGPART(tls)
KICKOS_IMAGE_N_TLS(KIMGN)
KIMGPART(tdata)
KICKOS_IMAGE_N_TDATA(KIMGN)
KIMGPART(table)
KICKOS_IMAGE_N_ENTRY(KIMGN, LONG)
KIMGPART(asserts)
KICKOS_IMAGE_WINDOW_ASSERT(KIMGN)
KICKOS_TLS_IMAGE_FIT_ASSERT(KIMGN)
