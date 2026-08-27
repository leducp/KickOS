// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What a process is made of, above the arch map editor: the image mapped into a task's
// address space, the space activated on the switch path, and the teardown that returns
// every frame the space OWNS while unmapping the ones it only borrows
// (docs/design-m6-mmu.md section 3.4).
//
// A space frees what it MAPS, and the borrower unmaps before it dies (F10). Shared app
// text is the commonest borrow: the same physical pages carry every process's text, they
// were never handed out by the frame pool, and a destroy walking over them would hand the
// pool an address it does not own.
//
// This is also where F10's allocator lives: a reservation names frames and maps nothing, the
// self-grant maps them into the reserving task's space, and the handoff maps one reservation
// into a second space at the SAME virtual address.
//
// Compiled to nothing without a translating backend.

#ifndef KICKOS_ASPACE_H
#define KICKOS_ASPACE_H

#include <kickos/arch/arch.h>
#include <kickos/vrange.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    struct Thread;

#if KICKOS_HAVE_ASPACE

    // Map the process image into a freshly created space: app text read-execute at its link
    // address on the SAME physical pages every other space uses, and app static data
    // read-write at its link address. False leaves the space with whatever the failure
    // reached; the caller destroys it through aspace_release.
    //
    // `ranges` IS SEEDED WITH THE SAME EXTENTS AND THE SAME RIGHTS, and that is not
    // bookkeeping: it is what the syscall entry validates an app pointer against on this
    // backend, no region array describing the image here (section 3.3), and it is the record
    // teardown reads to tell a borrowed page from an owned one. Passed in rather than
    // reached through the domain so the two halves cannot disagree about which space was
    // seeded.
    //
    // STATIC DATA IS A PER-PROCESS COPY (section 3.4): two processes writing one physical
    // page of globals are one process with a memory bug. Text is not, one physical page
    // carrying it in every space.
    //
    // THE FIRST SPACE SEEDED KEEPS THE IMAGE'S OWN DATA PAGES and every later one copies
    // them. That first space is root's, and it has to be: the app's ctors run in root, in a
    // thread, so a root holding a copy would construct its copy and leave the image pages
    // holding link-time bytes for every process after it (kmain.cc, root_entry).
    bool aspace_image_seed(struct arch_aspace* space, VirtualRanges* ranges);

    // Unmap what the space borrows, return the frames of a reservation it never mapped, then
    // destroy it. The one sanctioned way to end a space: arch_aspace_destroy alone would
    // hand the frame pool the image's own pages, and would strand a reservation that has no
    // leaf pointing at it.
    void aspace_release(struct arch_aspace* space, VirtualRanges* ranges);

    // F10's allocation: `bytes` rounded up to whole granules, RESERVED in this space and
    // mapped nowhere. 0 when the frame pool has no run that long or the list is full.
    //
    // THE FRAMES ARE TAKEN HERE AND THE VIRTUAL ADDRESS IS THEIR OWN, exactly as a stack's
    // is (section 3.4). That is what makes a reservation a globally unique name: the handoff
    // maps it into a second space at the same address, and no other space's allocator can
    // have named the same range.
    uintptr_t aspace_reserve(VirtualRanges* ranges, size_t bytes);

    // F10's self-grant: map a range the CALLER reserved, at the address it reserved.
    // -KOS_EPERM for an address this space never reserved, which is what a cross-task
    // self-grant now is; 0 when the range is already mapped with these attributes.
    int aspace_self_grant(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t base,
                          size_t size, uint32_t rights, enum arch_map_memtype type);

    // A small stable NAME for the frame backing `va` in `space`, or 0 where that page is not
    // mapped. Selftest scaffolding: two tasks comparing this for one address is what witnesses
    // that per-process static data is a COPY and that text is not (section 3.4).
    //
    // BIASED OFF THE IMAGE'S FIRST TEXT FRAME, which every space maps, so the number is an
    // offset between two frames rather than an address: the kernel's own map is what an
    // unbiased answer would disclose, and domain_space_id refuses to disclose one for the same
    // reason. Comparable across spaces, and nothing else may be read out of it.
    uintptr_t aspace_frame_token(struct arch_aspace* space, uintptr_t va);

    // F10's handoff: map the donor's reservation into `space` at the SAME virtual address
    // and record it BORROWED, so the target unmaps and frees nothing. -KOS_EPERM when the
    // donor holds no such range, -KOS_ENOMEM when the target cannot take it there. A target
    // that cannot take the range at that address REFUSES rather than relocating: nothing
    // guarantees the block's contents are position-independent.
    int aspace_handoff(VirtualRanges const* donor, struct arch_aspace* space,
                       VirtualRanges* ranges, uintptr_t base, size_t size,
                       enum arch_map_memtype type);

    // Install the incoming thread's task space, or leave the running one where the thread
    // holds none (a privileged thread executes out of the kernel's half alone). Skips the
    // root write when the space is already current, a root switch costing a whole-half TLB
    // sweep on a backend with no translation tag.
    void aspace_activate_for(Thread const* t);

    // Forget the cached current space. The selftest scaffolding activates spaces of its
    // own behind the switch path's back, so the cache has to be droppable.
    void aspace_forget_current(void);

#else

    inline void aspace_activate_for(Thread const*) {}

#endif
}

#endif
