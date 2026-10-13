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
    // Adopt the live regime as the boot space. Call after ring3_init and BEFORE any space exists.
    // `ram_base` and `ram_size` are the conventional-memory run the UEFI memory map named.
    void aspace_init(uintptr_t ram_base, size_t ram_size);

    // IA32_PAT as this processor is running it, or the power-up layout on a part reporting no
    // attribute table at all. Read on every composition and cached nowhere.
    uint64_t aspace_attribute_table(void);

    // The PWT, PCD and PAT bits a granule leaf of `type` carries under the attribute table `pat`.
    // The three bits select one of eight fields and the field names the type, so they are SEARCHED
    // out of `pat`. False where no field of `pat` encodes `type`.
    // `pat` is a parameter so a witness can drive the decode without WRITING IA32_PAT, which would
    // retype whatever the adopted regime already maps through the fields it moves.
    bool aspace_memtype_bits(uint64_t pat, enum arch_map_memtype type, uint64_t* out);

    // The whole granule leaf standing at `va` in `space`, or 0 where none does. Below the seam
    // only.
    uint64_t aspace_leaf_desc(struct arch_aspace* space, uintptr_t va);

    // Relocate the app window a second time, for the user offset: every absolute word stored in
    // it and aimed into it gains arch_aspace_user_offset. Once, from the kernel's landing,
    // after aspace_init and before the first space is seeded; refuses the boot on a record it
    // cannot place (apprel_x86_64.cc).
    void app_relocate(void);
    // Where a task reaches memory, minus the kernel's view of it: the user offset once
    // app_relocate has run in the kernel image, zero in a bring-up image, whose ring-3 probes
    // run where the loader put them and whose stacks are the kernel's view itself.
    uintptr_t user_alias_offset(void);
}

#endif
