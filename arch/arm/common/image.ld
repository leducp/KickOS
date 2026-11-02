/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a linker script: the image template of every Cortex-M chip, run through cpp at configure
 * beside the chip script (arch/CMakeLists.txt). tools/image-link.py instantiates each part below
 * per image, for KIMGN its number, KIMGBASE the first window's base (the base part) and nothing
 * for the others, KIMGALIGN its window's base alignment and KIMGHEAP its heap, and KIMGRULE states
 * the protection unit's region rule it places windows by.
 */
#include <cortexm.ld.h>

KIMGRULE(KICKOS_HAVE_MPU, KICKOS_CHIP_REGION_POW2, KICKOS_IMAGE_WINDOW_GRANULE)
KIMGPART(base)
KICKOS_CORTEXM_WINDOWS_BASE
KIMGPART(code)
KICKOS_CORTEXM_IMAGE_CODE(KIMGN)
KIMGPART(image)
KICKOS_CORTEXM_IMAGE(KIMGN, KIMGBASE, KIMGALIGN, KIMGHEAP)
KIMGPART(tls)
KICKOS_IMAGE_N_TLS(KIMGN)
KIMGPART(tdata)
KICKOS_IMAGE_N_TDATA(KIMGN)
KIMGPART(table)
KICKOS_IMAGE_N_ENTRY(KIMGN, LONG)
KIMGPART(asserts)
KICKOS_IMAGE_WINDOW_ASSERT(KIMGN)
KICKOS_TLS_IMAGE_FIT_ASSERT(KIMGN)
