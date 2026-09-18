// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Kernel address-space management. Borrowed mappings must be unmapped
// before destroying a space, which frees its owned frames.

#ifndef KICKOS_ASPACE_H
#define KICKOS_ASPACE_H

#include <kickos/arch/arch.h>
#include <kickos/vrange.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    struct Thread;

    // Copy through each owning space's acquire interface, one granule at a time.
    // Failure can leave a copied prefix. Return an error rather than panic;
    // the fault reporter also uses these functions.
    [[nodiscard]] bool kaccess_from_user(void* kdst, struct arch_aspace* sspace, uintptr_t usrc,
                                         size_t n);
    [[nodiscard]] bool kaccess_to_user(struct arch_aspace* dspace, uintptr_t udst,
                                       void const* ksrc, size_t n);

    // Copy one naturally aligned pointer-sized word through one acquire.
    // Return false for misalignment or failed acquisition.
    [[nodiscard]] bool kaccess_word_to_user(struct arch_aspace* dspace, uintptr_t udst,
                                            void const* kword);

    // Copy between user ranges, each with its own space.
    // Reject overlapping ranges in the same space.
    [[nodiscard]] bool ep_copy(struct arch_aspace* dspace, uintptr_t dst,
                               struct arch_aspace* sspace, uintptr_t src, size_t n);

#if KICKOS_HAVE_ASPACE

    // Seed shared RX text, private RW data and matching validation ranges.
    // The first space uses the image data; later spaces copy the live root or
    // its saved snapshot. Fail if neither source exists. On failure, release
    // the partially built space with aspace_release.
    bool aspace_image_seed(struct arch_aspace* space, VirtualRanges* ranges);

    // Unmap what the space borrows, return the frames of a reservation it never mapped, then
    // destroy it. The one sanctioned way to end a space; arch_aspace_destroy alone strands both.
    void aspace_release(struct arch_aspace* space, VirtualRanges* ranges);

    // Allocation: `bytes` rounded up to whole granules, reserved in this space and mapped
    // nowhere. 0 when the frame pool has no run that long or the list is full. The virtual
    // address is the frames' own, which is what makes a reservation a globally unique name.
    uintptr_t aspace_reserve(VirtualRanges* ranges, size_t bytes);

    // The self-grant: map a range the caller reserved, at the address it reserved.
    // -KOS_EPERM for an address this space never reserved, a cross-task self-grant included;
    // 0 when the range is already mapped with these attributes.
    int aspace_self_grant(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t base,
                          size_t size, uint32_t rights, enum arch_map_memtype type);

    // Return the kernel alias of one app-image byte, even before spaces exist.
    // Return null outside the image or if no app window is configured.
    void* aspace_image_alias(void const* app_ptr);

    // Self-test frame identity relative to the first text frame; zero if unmapped.
    // Values can be compared across spaces but are not physical addresses.
    uintptr_t aspace_frame_token(struct arch_aspace* space, uintptr_t va);

    // Map a capability-owned frame run at the chosen VA and record it as borrowed.
    // The capability retains frame ownership. Return -KOS_ENOMEM for unavailable
    // space or -KOS_EINVAL for an invalid range.
    int aspace_cap_map(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                       int run_obj, arch_phys_addr_t base, uint32_t pages, uint32_t rights,
                       enum arch_map_memtype type);

    // Unmap a capability mapping and release its reference. Return -KOS_EPERM
    // unless both the mapping kind and run_obj match; VR_BORROWED alone is insufficient.
    int aspace_cap_unmap(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                         int run_obj);

    // Map the donor's complete reservation at the same VA without taking ownership.
    // Require its exact base and rounded page count. Return -KOS_EPERM for a
    // missing reservation or -KOS_ENOMEM if the destination cannot accept it.
    int aspace_handoff(VirtualRanges const* donor, struct arch_aspace* space,
                       VirtualRanges* ranges, uintptr_t base, size_t size,
                       enum arch_map_memtype type);

    // Install and return the thread's task space, skipping redundant root writes.
    // For spaceless threads, return null; SMP installs the boot root, while
    // single-core builds retain the current root.
    struct arch_aspace* aspace_activate_for(Thread const* t);

    // aspace_seated_for's verdict read off what aspace_activate_for just answered, for a caller
    // that already has it.
    inline bool aspace_seated_with(struct arch_aspace* space) { return space != nullptr; }

    // Return whether this core holds the thread's own space. Spaceless threads
    // must not write libc state through another process's app mapping.
    bool aspace_seated_for(Thread const* t);

    // Install the boot root on THIS core and record it. For a caller about to stop being a
    // member of the space it is running in: the last member's release frees these tables.
    void aspace_install_boot(void);

    void aspace_forget_current(void);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Outstanding acquires in the high half of the word, releases that paired with no acquire in
    // the low half. Both must be 0 outside a map-editing call.
    uint64_t aspace_acquire_balance(void);

    uint64_t aspace_unseated_switch_ins(void);

    // Peer cores a destroy found still holding the space. Must stay 0: a dying member vacates
    // its space before its reference drops.
    uint64_t aspace_release_peer_hits(void);

    // Destroys run since boot. The counter above reads 0 for a sweep that found nothing and
    // for a destroy that never ran, so a caller asserting the 0 needs this beside it.
    uint64_t aspace_release_runs(void);

    // Drop the space holding the image's own data pages, as its release would.
    void aspace_data_home_forget(void);
#endif

#else

    inline struct arch_aspace* aspace_activate_for(Thread const*) { return nullptr; }
    // Null here names no space rather than no seat: the app half is one flat view and every
    // thread is seated in it.
    inline bool aspace_seated_with(struct arch_aspace*) { return true; }
    inline bool aspace_seated_for(Thread const*) { return true; }
    inline void aspace_install_boot(void) {}
    inline void* aspace_image_alias(void const*) { return nullptr; }

#endif
}

#endif
