/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AMP partition's addresses, DERIVED so that two images cannot state them apart.
 * Plain integer constants only: read by a chip linker script and by C++.
 *
 * KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE, KICKOS_AMP_SHARED_SIZE,
 * KICKOS_AMP_NODES and KICKOS_AMP_NODE_ID arrive as -D from the resolved configuration;
 * CMakeLists.txt refuses a partition that leaves any of the first three at zero.
 */
#ifndef KICKOS_ARCH_COMMON_AMP_PARTITION_LD_H
#define KICKOS_ARCH_COMMON_AMP_PARTITION_LD_H

/* Where THIS image links: the only per-node value. */
#define KICKOS_AMP_IMAGE_BASE \
    (KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_NODE_ID * KICKOS_AMP_NODE_SHARE)

/* Above every node's share, so it is outside every image's own allocations. */
#define KICKOS_AMP_SHARED_BASE \
    (KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_NODES * KICKOS_AMP_NODE_SHARE)

#define KICKOS_AMP_PARTITION_END (KICKOS_AMP_SHARED_BASE + KICKOS_AMP_SHARED_SIZE)

#endif
