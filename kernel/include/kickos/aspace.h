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

// The kernel can reach user memory a task holds non-cacheable through a cacheable view of its own.
#define KICKOS_ALIAS_DCACHE (KICKOS_ARCH_ALIAS_DCACHE or KICKOS_ARCH_ARENA_DCACHE)

namespace kickos
{
    struct Thread;
    struct Domain;
    class MpuSet;

    // What answers for a user end: its space where the backend translates, else the thread
    // whose region set holds it. Null names kernel storage, and every user end of a region
    // backend with no data cache over the arena.
#if KICKOS_HAVE_ASPACE
    using UserOwner = struct arch_aspace*;
#else
    using UserOwner = Thread const*;
#endif

    // Copy through each owning space's acquire interface, one granule at a time.
    // Failure can leave a copied prefix. Return an error rather than panic;
    // the fault reporter also uses these functions.
    [[nodiscard]] bool kaccess_from_user(void* kdst, UserOwner sspace, uintptr_t usrc, size_t n);
    [[nodiscard]] bool kaccess_to_user(UserOwner dspace, uintptr_t udst, void const* ksrc,
                                       size_t n);

    // Copy one naturally aligned pointer-sized word through one acquire.
    // Return false for misalignment or failed acquisition.
    [[nodiscard]] bool kaccess_word_to_user(UserOwner dspace, uintptr_t udst, void const* kword);

    // Copy between user ranges, each with its own space.
    // Reject overlapping ranges in the same space.
    [[nodiscard]] bool ep_copy(UserOwner dspace, uintptr_t dst, UserOwner sspace, uintptr_t src,
                               size_t n);

#if KICKOS_ALIAS_DCACHE
    // Maintain the data cache over a kernel pointer that reaches non-cacheable user memory
    // cacheably: before the kernel reads or writes through it, and again after it writes.
    void alias_sync(void const* p, size_t n);
#endif
#if defined(KICKOS_ENABLE_SELFTEST)
    // alias_sync calls since boot, 0 where none is compiled.
    uint32_t alias_sync_count();
#endif

#if KICKOS_ARCH_ARENA_DCACHE and not KICKOS_HAVE_ASPACE
    // Ahead of a region of `attr` over [base, base + size): one that is non-cacheable, or that
    // retypes a region `held` names at exactly that extent, has the block's lines cleaned to
    // memory then dropped, so none an earlier use left is evicted over it or read through it.
    void grant_sync(MpuSet const* held, uintptr_t base, size_t size, uint32_t attr);
#else
    inline void grant_sync(MpuSet const* held, uintptr_t base, size_t size, uint32_t attr)
    {
        (void)held;
        (void)base;
        (void)size;
        (void)attr;
    }
#endif

#if KICKOS_HAVE_ASPACE

    // A task reaches a frame the kernel hands it at the frame's physical address plus the
    // arch's user offset (arch_aspace_user_offset): one rule on every translating backend.
    inline uintptr_t aspace_user_va(arch_phys_addr_t frame)
    {
        return static_cast<uintptr_t>(frame) + arch_aspace_user_offset();
    }
    inline arch_phys_addr_t aspace_frame_of(uintptr_t va)
    {
        return static_cast<arch_phys_addr_t>(va - arch_aspace_user_offset());
    }

    // Seed shared RX text, private RW data and matching validation ranges.
    // The first space uses the image data. A later one copies the snapshot of
    // root's data where `from_snapshot`, an explicit task's, the first such seed
    // taking it out of the live root; any other copies `spawner`'s live data where
    // it names a space, else the live root while it lives and the snapshot after,
    // which root's release takes if nothing did. Fail if no source exists. On
    // failure, release the partially built space with aspace_release.
    bool aspace_image_seed(struct arch_aspace* space, VirtualRanges* ranges, bool from_snapshot,
                           struct arch_aspace* spawner);

    // Unmap what the space borrows, return the frames of a reservation it never mapped, then
    // destroy it. The one sanctioned way to end a space; arch_aspace_destroy alone strands both.
    void aspace_release(struct arch_aspace* space, VirtualRanges* ranges);

    // Allocation: `bytes` rounded up to whole granules, reserved in this space and mapped
    // nowhere. 0 when the frame pool has no run that long or the list is full. The virtual
    // address is the frames' own, which is what makes a reservation a globally unique name.
    uintptr_t aspace_reserve(VirtualRanges* ranges, size_t bytes);

    // Whether every live mapping of [pa, pa + pages granules) in any space, `self` aside, carries
    // `memtype`: one block mapped cacheable in one place and not in another is incoherent.
    bool aspace_frames_type_ok(arch_phys_addr_t pa, size_t pages, uint8_t memtype,
                               VirtualRange const* self);

    // Whether mapping `e`'s frames with `memtype` owes the kernel's cacheable view of them a
    // sync: a non-cacheable mapping, or any change of type over a granted range.
    inline bool aspace_grant_syncs(VirtualRange const* e, uint8_t memtype)
    {
        if (memtype == static_cast<uint8_t>(ARCH_MAP_NOCACHE))
        {
            return true;
        }
        return e != nullptr and e->state == VirtualState::Granted and e->memtype != memtype;
    }

    // The self-grant: map a range the caller reserved, at the address it reserved.
    // -KOS_EPERM for an address this space never reserved, a cross-task self-grant included;
    // -KOS_EBUSY when another mapping of its frames carries another memory type; 0 when the
    // range is already mapped with these attributes.
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

    // Map a spawn window's `bytes` at `pa` where the backend's window area has room, recorded as
    // a VR_WINDOW range held by `holder` (a thread slot index plus one) at `place` in its spawn
    // list. A memory window names the `donor` domain whose reservation it maps and holds a
    // reference on it until unmapped; a device window names none. -KOS_ENOMEM when the area,
    // the list or the tables are full.
    int aspace_window_map(struct arch_aspace* space, VirtualRanges* ranges, arch_phys_addr_t pa,
                          size_t bytes, uint32_t rights, enum arch_map_memtype type,
                          uint16_t holder, uint32_t place, Domain* donor);

    // Unmap every window `holder` holds, completing the backend's shootdown, and release their
    // donors. A holder with none is a no-op.
    void aspace_window_unmap_holder(struct arch_aspace* space, VirtualRanges* ranges,
                                    uint16_t holder);

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
