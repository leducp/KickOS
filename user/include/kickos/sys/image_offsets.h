// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Offsets into struct kos_image_header (<kickos/sys/image.h>) that assembly reads, on the 32-bit
// arches with no atomic. kernel/task/image.cc asserts each against offsetof. Assembly-safe.

#ifndef KICKOS_SYS_IMAGE_OFFSETS_H
#define KICKOS_SYS_IMAGE_OFFSETS_H

// The switch rewinds a thread it finds inside any image's user CAS window, reading each header
// of the link's table at these offsets.
#define KICKOS_IMAGE_HEADER_OFF_CAS_START 144
#define KICKOS_IMAGE_HEADER_OFF_CAS_STORE 148

#endif
