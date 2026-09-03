// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What a process is made of above the arch map editor.
// A space frees what it maps; a borrower unmaps before it dies.

#ifndef KICKOS_ASPACE_H
#define KICKOS_ASPACE_H

#include <kickos/arch/arch.h>
#include <kickos/vrange.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    struct Thread;

    // The kernel's only route to memory a process owns: each splits at granule boundaries and
    // reaches every page through the acquire seam of the space that owns it.
    //
    // False is not "nothing happened": the move stops at the first granule the owning space
    // refuses, so the destination holds a head of the source over a tail of what it held.
    // Callers on a syscall path assert on false. Two report instead and must stay that way:
    // cap_console_deliver's payload copy and write_recv_info, that route being the fault
    // reporter's way to a published console, where an assert re-enters the record it is writing.
    [[nodiscard]] bool kaccess_from_user(void* kdst, struct arch_aspace* sspace, uintptr_t usrc,
                                         size_t n);
    [[nodiscard]] bool kaccess_to_user(struct arch_aspace* dspace, uintptr_t udst,
                                       void const* ksrc, size_t n);

    // The peer with BOTH ends in user memory, one of them possibly a PARKED thread's in a space
    // the running translation does not name. One space requires the two ranges be disjoint.
    [[nodiscard]] bool ep_copy(struct arch_aspace* dspace, uintptr_t dst,
                               struct arch_aspace* sspace, uintptr_t src, size_t n);

#if KICKOS_HAVE_ASPACE

    // Map the process image into a freshly created space: app text read-execute on the same
    // physical pages every other space uses, app static data read-write as a per-process copy.
    // False leaves the space with whatever the failure reached; the caller destroys it through
    // aspace_release. `ranges` is seeded with the same extents and rights, which is what the
    // syscall entry validates an app pointer against and what teardown reads to tell a borrowed
    // page from an owned one. The first space seeded keeps the image's own data pages and every
    // later one copies them, from root while root lives and from a snapshot of root once it does
    // not; a seed reaching a lost home with no snapshot behind it fails.
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

    // The kernel's own alias of a byte in the app's window: where the loader put it. Answers
    // before any space exists. Null for a pointer outside the app image and where the chip
    // carves no app window; every caller falls back to the pointer it passed. One byte is
    // tested, this signature carrying no length.
    void* aspace_image_alias(void const* app_ptr);

    // A small stable name for the frame backing `va` in `space`, or 0 where that page is not
    // mapped. Selftest scaffolding, biased off the image's first text frame so the number is an
    // offset between frames: comparable across spaces, and nothing else may be read out of it.
    uintptr_t aspace_frame_token(struct arch_aspace* space, uintptr_t va);

    // The capability map: put a frame RUN a capability names into `space` at the address
    // the holder chose. The range is recorded BORROWED, because the frames belong to the
    // capability and come back when its last holder drops it, so this space must free
    // nothing at teardown. -KOS_ENOMEM when the space cannot take the range there,
    // -KOS_EINVAL on a bad shape.
    int aspace_cap_map(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                       int run_obj, arch_phys_addr_t base, uint32_t pages, uint32_t rights,
                       enum arch_map_memtype type);

    // The capability unmap: take that range back out, and surrender the mapping's
    // reference. -KOS_EPERM unless the range at `va` was placed by aspace_cap_map AND names
    // `run_obj`. Matching a page count instead accepts the image and every handoff, which
    // carry VR_BORROWED too.
    int aspace_cap_unmap(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                         int run_obj);

    // The handoff: map the donor's reservation into `space` at the same virtual address and
    // record it borrowed, so the target unmaps and frees nothing. -KOS_EPERM when the donor
    // holds no such range, -KOS_ENOMEM when the target cannot take it there. `base` must be a
    // reservation's own base and `size` must round up to its page count.
    int aspace_handoff(VirtualRanges const* donor, struct arch_aspace* space,
                       VirtualRanges* ranges, uintptr_t base, size_t size,
                       enum arch_map_memtype type);

    // Install the incoming thread's task space, or leave the running one where the thread holds
    // none. Skips the root write when the space is already current.
    void aspace_activate_for(Thread const* t);

    // Whether the translation root this core holds is `t`'s own space, which is the condition
    // for the app's half being addressable on its behalf at all. False for a thread whose task
    // holds no space, where a kernel write to an app-half address lands in another process's
    // memory, so the switch path asks this before it seats libc's reentrant state.
    bool aspace_seated_for(Thread const* t);

    void aspace_forget_current(void);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Outstanding acquires in the high half of the word, releases that paired with no acquire in
    // the low half. Both must be 0 outside a map-editing call.
    uint64_t aspace_acquire_balance(void);

    uint64_t aspace_unseated_switch_ins(void);

    // Drop the space holding the image's own data pages, as its release would.
    void aspace_data_home_forget(void);
#endif

#else

    inline void aspace_activate_for(Thread const*) {}
    inline bool aspace_seated_for(Thread const*) { return true; }

#endif
}

#endif
