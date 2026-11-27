/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The AMP partition's addresses, DERIVED so that no two images can state them apart.
 * Plain integer constants, read by a chip linker script and by C++, and the ASSERT macros a
 * chip script holds its own image to.
 *
 * KICKOS_AMP_PARTITION_BASE, KICKOS_AMP_SHARED_SIZE, KICKOS_AMP_USER_SHARE_SIZE,
 * KICKOS_AMP_NODES and KICKOS_AMP_NODE_ID arrive as -D from the resolved configuration, and so
 * do KICKOS_AMP_IMAGE_OFFSET, KICKOS_AMP_IMAGE_SIZE and KICKOS_AMP_SLICES_SIZE, which
 * genconfig.py derives from the one per-node slice list every node is built against.
 *
 * A part that executes from a separate aperture slices its text span uniformly, with its own
 * base and share as parameters.
 */
#ifndef KICKOS_ARCH_COMMON_AMP_PARTITION_LD_H
#define KICKOS_ARCH_COMMON_AMP_PARTITION_LD_H

/* Where THIS image's slice of a uniformly sliced text span begins. */
#define KICKOS_AMP_SLICE_BASE(base, share) ((base) + KICKOS_AMP_NODE_ID * (share))

#define KICKOS_AMP_IMAGE_BASE (KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_IMAGE_OFFSET)
#define KICKOS_AMP_IMAGE_END (KICKOS_AMP_IMAGE_BASE + KICKOS_AMP_IMAGE_SIZE)

/* Above every node's slice, so it is outside every image's own allocations. */
#define KICKOS_AMP_SHARED_BASE (KICKOS_AMP_PARTITION_BASE + KICKOS_AMP_SLICES_SIZE)

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
           "KickOS: the AMP shared region is not at the address the partition derives. Every node's script computes KICKOS_AMP_PARTITION_BASE plus the sum of every node's slice, so a region elsewhere means this image was built against a different geometry than its peers") \
    ASSERT(__kickos_amp_shared_end - __kickos_amp_shared_start                \
               <= KICKOS_AMP_KERNEL_SHARED_SIZE,                              \
           "KickOS: the AMP shared objects overflow KICKOS_AMP_SHARED_SIZE less the tasks' KICKOS_AMP_USER_SHARE_SIZE at its top. Raise the first or lower the second in the partition's defconfig, in every node's, or narrow KICKOS_AMP_NODES / KICKOS_AMP_PARTITION_CORES")

/* The region every node writes, in one order on every node, so an object placed here cannot
 * land at two addresses. NOLOAD: a region carrying bytes is written at load time, and by then a
 * peer may already have published into it. Outside .bss, so no C runtime clears it;
 * arch_amp_shared_zero does, from the primary's Reset_Handler, before it releases any peer.
 */
#define KICKOS_AMP_SHARED_SECTION(region)                                     \
    .amp_shared (NOLOAD) : ALIGN(64)                                          \
    {                                                                         \
        __kickos_amp_shared_start = .;                                        \
        __kickos_amp_cells_start = .;                                         \
        KICKOS_KERNEL_KEEP(.amp_shared.cells.request)                         \
        KICKOS_KERNEL_KEEP(.amp_shared.cells.answer)                          \
        KICKOS_KERNEL_KEEP(.amp_shared.affinity)                              \
        KICKOS_KERNEL_KEEP(.amp_shared.affinity_seated)                       \
        __kickos_amp_cells_end = .;                                           \
        . = ALIGN(64);                                                        \
        __kickos_amp_window_start = .;                                        \
        KICKOS_KERNEL_KEEP(.amp_shared.window)                                \
        __kickos_amp_window_end = .;                                          \
        . = ALIGN(64);                                                        \
        __kickos_amp_counts_start = .;                                        \
        KICKOS_KERNEL_KEEP(.amp_shared.counts)                                \
        __kickos_amp_counts_end = .;                                          \
        __kickos_amp_chip_start = .;                                          \
        KICKOS_KERNEL_KEEP(.amp_shared.chip)                                  \
        __kickos_amp_chip_end = .;                                            \
        . = ALIGN(64);                                                        \
        KICKOS_KERNEL_KEEP(.amp_shared.clock)                                 \
        __kickos_amp_shared_end = .;                                          \
    } > region

/* A KICKOS_AMP_SHARED that expanded to nothing leaves its section EMPTY and every node on a
 * private copy, the failure no run reports. */
#define KICKOS_AMP_SHARED_FILLED_ASSERT()                                     \
    ASSERT(__kickos_amp_cells_end > __kickos_amp_cells_start,                 \
           "KickOS: .amp_shared.cells is empty: the doorbell backend's rendezvous cells are not placed in the shared region, so each node would rendezvous with itself") \
    ASSERT(__kickos_amp_window_end > __kickos_amp_window_start,               \
           "KickOS: .amp_shared.window is empty: kernel/amp/ampwindow.cc's window is not placed in the shared region, so each node would publish into a ring no peer reads") \
    ASSERT(__kickos_amp_counts_end > __kickos_amp_counts_start,               \
           "KickOS: .amp_shared.counts is empty: a node sweeping a peer's row would read its own")

/* [lo, hi) of this image inside its own slice. */
#define KICKOS_AMP_IMAGE_ASSERT(lo, hi)                                       \
    ASSERT((lo) >= KICKOS_AMP_IMAGE_BASE && (hi) <= KICKOS_AMP_IMAGE_END,     \
           "KickOS: this node's image is not inside its own slice of the partition, which starts at the partition base plus the slices of the nodes before it. A peer's slice is beside it, so the image loaded later would silently replace part of the other")

/* [lo, hi) of this image in the text span at base, inside this node's share of it. */
#define KICKOS_AMP_SLICE_ASSERT(lo, hi, base, share)                          \
    ASSERT((lo) >= KICKOS_AMP_SLICE_BASE(base, share)                         \
               && (hi) <= KICKOS_AMP_SLICE_BASE(base, share) + (share),       \
           "KickOS: this node's text is not inside its own slice of the text span, which starts at the span's base plus the node index times its share. A peer's slice is beside it, so the image loaded later would silently replace part of the other")

#endif
