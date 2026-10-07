/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AMP partition's addresses, DERIVED so that no two images can state them apart.
 * Plain integer constants, read by a chip linker script and by C++, and the two ASSERT macros
 * a chip script holds its own image to.
 *
 * KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_NODE_SHARE, KICKOS_AMP_SHARED_SIZE,
 * KICKOS_AMP_USER_SHARE_SIZE, KICKOS_AMP_NODES and KICKOS_AMP_NODE_ID arrive as -D from the
 * resolved configuration; CMakeLists.txt refuses a partition that leaves any of the first three
 * at zero.
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

/* The tasks' share is the region's top; the kernel's own objects must end at or below it. */
#define KICKOS_AMP_USER_SHARE_BASE (KICKOS_AMP_PARTITION_END - KICKOS_AMP_USER_SHARE_SIZE)
#define KICKOS_AMP_KERNEL_SHARED_SIZE (KICKOS_AMP_SHARED_SIZE - KICKOS_AMP_USER_SHARE_SIZE)

/* One image against the partition. Every peer is built from node 0's resolved configuration
 * and refused if its Kconfig fingerprint differs (tools/amp/build-partition.sh), so each image
 * holding these is the partition agreeing: one shared region at one address, and every node
 * inside its own slice of each span, apart from every other.
 *
 * The bounds are the constants above and never a script's regions alone: a region compared
 * with itself passes wherever it is repointed.
 */
#define KICKOS_AMP_SHARED_ASSERT(shared_base)                                 \
    ASSERT((shared_base) == KICKOS_AMP_SHARED_BASE,                           \
           "KickOS: the AMP shared region is not at the address the partition derives. Every node's script computes KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_NODES * KICKOS_AMP_NODE_SHARE, so a region elsewhere means this image was built against a different geometry than its peers") \
    ASSERT(__kickos_amp_shared_end - __kickos_amp_shared_start                \
               <= KICKOS_AMP_KERNEL_SHARED_SIZE,                              \
           "KickOS: the AMP shared objects overflow KICKOS_AMP_SHARED_SIZE less the tasks' KICKOS_AMP_USER_SHARE_SIZE at its top. Raise the first or lower the second in the partition's defconfig, in every node's, or narrow KICKOS_AMP_NODES / KICKOS_AMP_PARTITION_CORES")

/* [lo, hi) of this image in the span at base, inside this node's share of it. */
#define KICKOS_AMP_SLICE_ASSERT(lo, hi, base, share)                          \
    ASSERT((lo) >= KICKOS_AMP_SLICE_BASE(base, share)                         \
               && (hi) <= KICKOS_AMP_SLICE_BASE(base, share) + (share),       \
           "KickOS: this node's image is not inside its own slice of the partition, which starts at the span's base plus the node index times its share. A peer's slice is beside it, so the image loaded later would silently replace part of the other")

#endif
