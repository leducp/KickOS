/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AMP partition's addresses, DERIVED so that no two images can state them apart.
 * Plain integer constants only: read by a chip linker script and by C++.
 *
 * KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE, KICKOS_AMP_SHARED_SIZE,
 * KICKOS_AMP_NODES and KICKOS_AMP_NODE_ID arrive as -D from the resolved configuration;
 * CMakeLists.txt refuses a partition that leaves any of the first three at zero.
 *
 * The slice arithmetic takes its base and share as parameters: a part that executes from a
 * separate aperture divides two spans, its text in one and its data in the other, with
 * different bases and different shares.
 */
#ifndef KICKOS_ARCH_COMMON_AMP_PARTITION_LD_H
#define KICKOS_ARCH_COMMON_AMP_PARTITION_LD_H

/* Where THIS image's slice of one span begins: the only per-node value. */
#define KICKOS_AMP_SLICE_BASE(base, share) ((base) + KICKOS_AMP_NODE_ID * (share))

/* Above every node's slice of that span, so it is outside every image's own allocations. */
#define KICKOS_AMP_SLICE_ABOVE(base, share) ((base) + KICKOS_AMP_NODES * (share))

/* The single-span geometry, which is every part in the tree that partitions one aperture. */
#define KICKOS_AMP_IMAGE_BASE \
    KICKOS_AMP_SLICE_BASE(KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE)

#define KICKOS_AMP_SHARED_BASE \
    KICKOS_AMP_SLICE_ABOVE(KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE)

#define KICKOS_AMP_PARTITION_END (KICKOS_AMP_SHARED_BASE + KICKOS_AMP_SHARED_SIZE)

#endif
