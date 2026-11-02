/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a linker script: the ESP32's image template, run through cpp at configure beside its
 * script (arch/CMakeLists.txt). tools/image-link.py instantiates each part below per image, for
 * KIMGN its number, KIMGBASE the first window's base (the base part, empty here) and nothing for
 * the others, KIMGALIGN its window's base alignment and KIMGHEAP its heap, and KIMGRULE states the
 * rule it places windows by: none is enforced here.
 */
#include "esp32_image.ld.h"

KIMGRULE(KICKOS_HAVE_MPU, KICKOS_CHIP_REGION_POW2, KICKOS_IMAGE_WINDOW_GRANULE)
KIMGPART(base)
KICKOS_ESP32_WINDOWS_BASE
KIMGPART(code)
KICKOS_ESP32_IMAGE_TEXT(KIMGN)
KIMGPART(image)
KICKOS_ESP32_IMAGE(KIMGN)
KIMGPART(window)
KICKOS_ESP32_WINDOW(KIMGN, KIMGBASE, KIMGALIGN, KIMGHEAP)
KIMGPART(tls)
KICKOS_IMAGE_N_TLS(KIMGN)
KIMGPART(tdata)
KICKOS_IMAGE_N_TDATA(KIMGN)
KIMGPART(table)
KICKOS_IMAGE_N_ENTRY(KIMGN, LONG)
KIMGPART(asserts)
KICKOS_IMAGE_WINDOW_ASSERT(KIMGN)
KICKOS_TLS_IMAGE_FIT_ASSERT(KIMGN)
