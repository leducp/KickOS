// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One root register serves both privilege levels here, so every space carries the kernel's
// mappings as a copy of the boot root's TOP-LEVEL entries.

#ifndef KICKOS_ARCH_ASPACE_H
#define KICKOS_ARCH_ASPACE_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::x86_64
{
    // Adopt the live regime as the boot space and install this port's kernel window in it.
    // Call after ring3_init and BEFORE any space exists.
    // `ram_base` and `ram_size` are the conventional-memory run the UEFI memory map named.
    void aspace_init(uintptr_t ram_base, size_t ram_size);

    // The paging level count this processor is RUNNING at, 4 or 5.
    unsigned aspace_levels(void);

    // IA32_PAT as this processor is running it, or the power-up layout on a part reporting no
    // attribute table at all. Read on every composition and cached nowhere.
    uint64_t aspace_attribute_table(void);

    // The PWT, PCD and PAT bits a granule leaf of `type` carries under the attribute table `pat`.
    // The three bits select one of eight fields and the field names the type, so they are SEARCHED
    // out of `pat`. False where no field of `pat` encodes `type`.
    // `pat` is a parameter so a witness can drive the decode without WRITING IA32_PAT, which would
    // retype whatever the adopted regime already maps through the fields it moves.
    bool aspace_memtype_bits(uint64_t pat, enum arch_map_memtype type, uint64_t* out);

    // The virtual base of the port's own kernel range, one granule per page mapped through
    // aspace_kernel_map below, and 0 before aspace_init has run or where no slot was free. It
    // takes a top-level slot of the boot root, which is sound only BEFORE the first create: a
    // top-level entry added later reaches no space that has already copied the root.
    uintptr_t aspace_kernel_window(void);
    size_t aspace_kernel_window_pages(void);

    // Present entries in the table under the boot root's first present slot. 512 means full.
    unsigned aspace_first_child_entries(void);

    // Map or unmap one granule of the kernel window, privileged and never executable. True on
    // success. The edit reaches every space at once; nothing above the seam may call these.
    bool aspace_kernel_map(size_t page, arch_phys_addr_t pa);
    bool aspace_kernel_unmap(size_t page);

    // The half a space may map: the top-level slots the boot root left ABSENT. `lo` is
    // inclusive and `hi` exclusive; both are 0 where no slot is free.
    uintptr_t aspace_user_lo(void);
    uintptr_t aspace_user_hi(void);

    // Top-level slots the boot root has present, which is the kernel half every create copies.
    unsigned aspace_kernel_slots(void);
    arch_phys_addr_t aspace_root_installed(void);

    // The frame one page of `space` names, taken WITHOUT the range test arch_aspace_frame_at
    // makes, so it answers about the kernel half and the identity range too. Below the seam only.
    arch_phys_addr_t aspace_frame_at_unchecked(struct arch_aspace* space, uintptr_t va);

    // The whole granule leaf standing at `va` in `space`, or 0 where none does. Below the seam
    // only.
    uint64_t aspace_leaf_desc(struct arch_aspace* space, uintptr_t va);

    // The identifier the machine offers, in bits, and whether it offers the instruction that
    // invalidates by one. A machine can expose the second without the first.
    unsigned aspace_tag_bits(void);
    bool aspace_tag_invalidate_present(void);
}

#endif
