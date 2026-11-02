/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The init image's header, compiled beside image_header.c into each executable where the init
 * is an image of its own (KICKOS_INIT_IMAGE).
 */

#define KICKOS_IMAGE_INIT 1
#include "image_header.c"
