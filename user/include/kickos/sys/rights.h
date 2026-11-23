// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A service endpoint badge: the device-slot mask in the low KOS_BADGE_SLOT_BITS bits, one right per
// bit above it.
//
// THE ONE LIST: tools/manifest/genmanifest.py and cmake/driver_metadata.cmake parse it as text, so
// keep each X(NAME, name, bit) on its own line and KOS_BADGE_SLOT_BITS a plain `<n>u`.

#ifndef KICKOS_SYS_RIGHTS_H
#define KICKOS_SYS_RIGHTS_H

#include <stdint.h>

#define KOS_BADGE_SLOT_BITS 24u

#define KOS_RIGHTS(X)          \
    X(WRITE, write, 0)         \
    X(READ, read, 1)           \
    X(CONTROL, control, 2)     \
    X(CONFIG, config, 3)       \
    X(TRANSFER, transfer, 4)

#define KOS_RIGHT_ENUMERATOR(NAME, name, bit) KOS_RIGHT_##NAME = 1u << (KOS_BADGE_SLOT_BITS + (bit)),

enum kos_right
{
    KOS_RIGHTS(KOS_RIGHT_ENUMERATOR) KOS_RIGHT_NONE = 0u
};

#undef KOS_RIGHT_ENUMERATOR

// Nonzero when `badge` holds every right of `rights`, each a KOS_RIGHT_* or an OR of them.
static inline int kos_badge_grants(uint32_t badge, uint32_t rights)
{
    return (badge & rights) == rights;
}

#endif
