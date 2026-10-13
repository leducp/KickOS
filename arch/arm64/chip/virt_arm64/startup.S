/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * QEMU `virt` (AArch64, Cortex-A53) startup. `-kernel <elf>` loads the image into DRAM at
 * 0x4000_0000 and enters at the ELF entry point with the MMU and caches off. A64 loads no SP
 * from a vector table, so SP is set here; .data and .bss are Reset_Handler's, as on
 * virt_rv32.
 *
 * The entry exception level is a property of the QEMU INVOCATION: `-M virt` hands over at
 * EL1, `virtualization=on` at EL2, `secure=on` at EL3. This port implements EL1 and refuses
 * the others by name.
 *
 * THE MMU COMES UP BEFORE ANY C. While SCTLR_EL1.M is 0 every access is Device-nGnRnE, which
 * mandates natural alignment whatever SCTLR_EL1.A says, and AArch64 GCC stages array and
 * struct copies through unaligned `ldur`/`stur` of q registers, as does newlib inside memcpy.
 * So compiled code alignment-faults (EC 0x25, DFSC 0x21) on its first wide copy, and the
 * boot tables are what buy Normal memory and with it the caches. Link-time constants.
 *
 * EVERYTHING IN .text.init IS REACHED UNTRANSLATED, so an absolute literal here holds a high
 * virtual address that no walk can resolve yet and `adrp` to a high symbol is out of its
 * 4 GiB reach. x9 carries the offset back to the physical address, and the branch into the
 * high half is what stops needing it. The EL refusal reporter is linked into this section
 * for the same reason (chip_virt_arm64.cc).
 */

#include <vmsa.h>
#include <boot_el1.inc>

#if !KICKOS_AMP_OWN_IMAGE
#include <chip_layout.h>

#if ((KICKOS_LAYOUT_DRAM_SIZE) % L2_BLOCK_SIZE) != 0 || ((KICKOS_LAYOUT_DRAM_SIZE) / L2_BLOCK_SIZE) > L2_ENTRIES
#error "KICKOS_LAYOUT_DRAM_SIZE is not a whole number of L2_BLOCK_SIZE blocks within one level-2 table"
#endif
#define RAM_L2_BLOCKS   ((KICKOS_LAYOUT_DRAM_SIZE) / L2_BLOCK_SIZE)
#endif

/* The level-2 table describes the PARTITION and not this image.
 *
 * A slot's index is its own VA, so the output has to be MAP_BASE + index * block for the map
 * to be an identity: an output biased by this image's base would leave the image unreachable
 * from its own entry point.
 *
 * Each node maps ITS OWN share and the shared region, which sits above every node's share, so
 * a count describing one share leaves that region outside the table and every write to it
 * faults. A peer's memory is left invalid. Nothing on this machine ENFORCES the boundary.
 */
#if KICKOS_AMP_OWN_IMAGE
#include <amp_partition.ld.h>

/* Every derivation below is an integer division that TRUNCATES rather than failing, and an
 * index whose meaning is fixed by the level-1 entry this table hangs off, so a partition
 * breaking any of the three links here would link cleanly and fault at the first touch of
 * memory no descriptor ever covered.
 */
#if (KICKOS_AMP_PARTITION_BASE) != (DRAM_L1_ENTRY * L1_BLOCK_SIZE)
#error "KICKOS_AMP_PARTITION_BASE is not the base of the DRAM level-1 entry this table hangs \
off. A descriptor's slot index IS its virtual address here, so slot 0 describes DRAM_L1_ENTRY \
* L1_BLOCK_SIZE whatever output it names: a partition based anywhere else maps every node at \
an offset from where it linked, and this image is unreachable from its own entry point. Base \
the partition at that address, or add the level-1 entry that covers the one it states."
#endif

#if ((KICKOS_AMP_NODE_SHARE) % L2_BLOCK_SIZE) != 0
#error "KICKOS_AMP_NODE_SHARE is not a whole number of L2_BLOCK_SIZE blocks. This table maps 2 \
MiB blocks and nothing finer, so OWN_L2_LO truncates a node's base DOWN into its predecessor's \
share and OWN_L2_HI truncates its top DOWN inside its own: node 0 keeps a mapping it does not \
own and every node loses the tail of its RAM, with a clean link. Round the share up to a \
multiple of 2 MiB."
#endif

#if ((KICKOS_AMP_SHARED_SIZE) % L2_BLOCK_SIZE) != 0
#error "KICKOS_AMP_SHARED_SIZE is not a whole number of L2_BLOCK_SIZE blocks. SHARED_L2_HI \
truncates DOWN, so a region narrower than one 2 MiB block is described by NO descriptor at all \
and every write to the region every node writes takes a level-2 translation fault, while a \
wider unaligned one loses its tail. This is the ONE region both nodes write, so it fails as a \
partition that boots and never converses. Round it up to a multiple of 2 MiB."
#endif

#define MAP_BASE        KICKOS_AMP_PARTITION_BASE
#define MAP_L2_BLOCKS   ((KICKOS_AMP_PARTITION_END - KICKOS_AMP_PARTITION_BASE) / L2_BLOCK_SIZE)

#if (MAP_L2_BLOCKS) > (L2_ENTRIES)
#error "The AMP partition spans more than L2_ENTRIES blocks of L2_BLOCK_SIZE, one L1_BLOCK_SIZE \
(1 GiB) in total, and this table has one level-2 page for the whole partition. The .fill below \
would run with a negative count, which the assembler only WARNS about, so the table ends short \
of the partition and a slot past it aliases the next table in memory instead of faulting. Shrink \
KICKOS_AMP_NODE_SHARE, KICKOS_AMP_NODES or KICKOS_AMP_SHARED_SIZE below 1 GiB, or add the second \
level-2 table and level-1 entry a wider partition needs."
#endif

#define OWN_L2_LO       ((KICKOS_AMP_IMAGE_BASE - KICKOS_AMP_PARTITION_BASE) / L2_BLOCK_SIZE)
#define OWN_L2_HI       (OWN_L2_LO + KICKOS_AMP_NODE_SHARE / L2_BLOCK_SIZE)
#define SHARED_L2_LO    ((KICKOS_AMP_SHARED_BASE - KICKOS_AMP_PARTITION_BASE) / L2_BLOCK_SIZE)
#define SHARED_L2_HI    (SHARED_L2_LO + KICKOS_AMP_SHARED_SIZE / L2_BLOCK_SIZE)

/* An uncached user share is mapped non-cacheable here too, a block at a time, so no cacheable
   alias of it exists in either root. */
#if KICKOS_AMP_USER_SHARE_UNCACHED
#if ((KICKOS_AMP_USER_SHARE_SIZE) % L2_BLOCK_SIZE) != 0 || (KICKOS_AMP_USER_SHARE_SIZE) == 0
#error "an uncached KICKOS_AMP_USER_SHARE_SIZE is not a whole number of L2_BLOCK_SIZE blocks. \
This table maps 2 MiB blocks and nothing finer, so the kernel's own view of the share could only \
be non-cacheable together with the kernel objects below it in the same block. Make the share a \
multiple of 2 MiB, and KICKOS_AMP_SHARED_SIZE wide enough to hold it above those objects."
#endif
#define SHARE_NC_L2_LO  (SHARED_L2_HI - KICKOS_AMP_USER_SHARE_SIZE / L2_BLOCK_SIZE)
#else
#define SHARE_NC_L2_LO  SHARED_L2_HI
#endif
#endif

    .section .text.init, "ax"
    .global _start
_start:
    KICKOS_ARM64_BOOT_ENTRY
    cmp     x0, #1
    b.ne    .Lbad_el

    KICKOS_ARM64_BOOT_EL1

#include <boot_tables.inc>

    .end
