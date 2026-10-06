// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/aspace.h>

#if KICKOS_HAVE_ASPACE

#include <kickos/ampshare.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/klink.h>
#include <kickos/kruntime.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

extern "C"
{
    // The chip linker script's app image split, stated all-zero by a chip that carves none.
    extern unsigned char __kickos_app_rom_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_app_rom_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_app_sram_start[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_app_sram_end[] KICKOS_LINK_BOUND;

    // Added to an app virtual address to name the frame the loader put those bytes in.
    extern unsigned char __kickos_app_load_delta[] KICKOS_LINK_BOUND;

    // The kernel address that corresponds to physical address 0 for the image's own DRAM.
    extern unsigned char __kickos_frame_pool_delta[];
}

namespace kickos
{
    namespace
    {
        // The last space written to this core's translation root.
        struct arch_aspace* g_current[KICKOS_NUM_CORES] = {};

        // The space holding the image's own static-data pages. Written by the first seed and
        // by that space's release, and cleared only after the snapshot beside it is frozen.
        struct arch_aspace* g_data_home = nullptr;

        // The static-data snapshot an explicit task's space copies from, and every space once
        // root is gone. Its frames leave the pool while root is seeded; the bytes are filled at
        // the first explicit task's seed, or at root's release if that comes first.
        arch_phys_addr_t g_data_template = 0;
        bool g_data_template_filled = false;

#if defined(KICKOS_ENABLE_SELFTEST)
        // Outstanding acquires, and releases that paired with none.
        size_t g_acq_live = 0;
        size_t g_acq_unpaired = 0;

        size_t g_unseated_switch_ins = 0;

        // Peer cores aspace_release found still holding the space it is destroying. A death
        // that vacates its space before dropping its reference leaves this at 0 forever.
        size_t g_release_peer_hits = 0;

        size_t g_release_runs = 0;
#endif

        // Every acquire in this file goes through these two, or the pairing count escapes.
        void const* acquire_page(struct arch_aspace* space, uintptr_t va)
        {
            void const* const p = arch_aspace_acquire(space, va, nullptr);
#if defined(KICKOS_ENABLE_SELFTEST)
            if (p != nullptr)
            {
                g_acq_live++;
            }
#endif
            return p;
        }

        void release_page(struct arch_aspace* space, uintptr_t va)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            if (g_acq_live == 0)
            {
                g_acq_unpaired++;
            }
            else
            {
                g_acq_live--;
            }
#endif
            arch_aspace_release(space, va);
        }

        // Ahead of a leaf of another memory type than the frames last had, and of a non-cacheable
        // one: the kernel's cacheable view holds lines of them, dirty from the pool's clear or a
        // cacheable mapping, or clean but stale after a non-cacheable one, and each is cleaned to
        // memory then dropped. A frame outside the pool has no such view.
        void sync_frames(arch_phys_addr_t pa, size_t pages)
        {
#if KICKOS_ALIAS_DCACHE
            size_t const g = arch_aspace_granule();
            for (size_t i = 0; i < pages; i++)
            {
                void const* const p = frame_pool_ptr(pa + static_cast<arch_phys_addr_t>(i * g));
                if (p != nullptr)
                {
                    alias_sync(p, g);
                }
            }
#else
            (void)pa;
            (void)pages;
#endif
        }

        // volatile keeps each a relocated word; a plain constant folds back into every reader.
        unsigned char* const volatile g_app_rom_lo = __kickos_app_rom_start;
        unsigned char* const volatile g_app_rom_hi = __kickos_app_rom_end;
        unsigned char* const volatile g_app_sram_lo = __kickos_app_sram_start;
        unsigned char* const volatile g_app_sram_hi = __kickos_app_sram_end;
        unsigned char* const volatile g_app_load_delta = __kickos_app_load_delta;
        unsigned char* const volatile g_pool_delta = __kickos_frame_pool_delta;

        // The frame the loader placed an app virtual address in. The linker states the
        // window where the loader puts it; a task reaches it at that address plus the user
        // offset.
        arch_phys_addr_t app_pa(uintptr_t va)
        {
            return aspace_frame_of(va)
                   + static_cast<arch_phys_addr_t>(
                       reinterpret_cast<uintptr_t>(g_app_load_delta));
        }

        uintptr_t app_va(unsigned char const* linked)
        {
            return reinterpret_cast<uintptr_t>(linked) + arch_aspace_user_offset();
        }

        uintptr_t page_down(uintptr_t a, size_t g)
        {
            return a & ~static_cast<uintptr_t>(g - 1u);
        }

        uintptr_t page_up(uintptr_t a, size_t g)
        {
            return (a + (g - 1u)) & ~static_cast<uintptr_t>(g - 1u);
        }

        struct Extent
        {
            uintptr_t base;
            size_t pages;
        };

        Extent extent_of(unsigned char const* lo, unsigned char const* hi, size_t g)
        {
            uintptr_t const a = app_va(lo);
            uintptr_t const b = app_va(hi);
            if (b <= a)
            {
                return Extent{0, 0};
            }
            uintptr_t const base = page_down(a, g);
            return Extent{base, static_cast<size_t>((page_up(b, g) - base) / g)};
        }

        Extent image_text(size_t g)
        {
            return extent_of(g_app_rom_lo, g_app_rom_hi, g);
        }

        Extent image_data(size_t g)
        {
            return extent_of(g_app_sram_lo, g_app_sram_hi, g);
        }

        // Check app ROM/RAM windows before applying their address offsets.
        // Equal bounds indicate an absent window.
        bool in_app_image(uintptr_t p)
        {
            uintptr_t const rom_lo = app_va(g_app_rom_lo);
            uintptr_t const rom_hi = app_va(g_app_rom_hi);
            if (p >= rom_lo and p < rom_hi)
            {
                return true;
            }
            uintptr_t const sram_lo = app_va(g_app_sram_lo);
            uintptr_t const sram_hi = app_va(g_app_sram_hi);
            return p >= sram_lo and p < sram_hi;
        }

        // The entry is claimed before anything is mapped, and every seeding path below keeps
        // that order: a mapping the list does not record is one teardown cannot see.
        bool claim(VirtualRanges* ranges, Extent const& e, uint8_t flags)
        {
            return ranges->reserve(e.base, e.pages, flags);
        }

        void commit(VirtualRanges* ranges, Extent const& e, uint32_t rights)
        {
            (void)ranges->grant(e.base, e.pages, rights, ARCH_MAP_NORMAL);
        }

        // Runs with the home's mappings still standing: at the first explicit task's seed and
        // on the home's way out.
        bool data_template_fill(Extent const& data, size_t g)
        {
            if (g_data_template_filled)
            {
                return true;
            }
            if (g_data_template == 0 or g_data_home == nullptr)
            {
                return false;
            }
            for (size_t i = 0; i < data.pages; i++)
            {
                uintptr_t const va = data.base + static_cast<uintptr_t>(i * g);
                void const* const src = acquire_page(g_data_home, va);
                if (src == nullptr)
                {
                    return false;
                }
                void* const dst =
                    frame_pool_ptr(g_data_template + static_cast<arch_phys_addr_t>(i * g));
                if (dst == nullptr)
                {
                    release_page(g_data_home, va);
                    return false;
                }
                kmemcpy(dst, src, g);
                release_page(g_data_home, va);
            }
            g_data_template_filled = true;
            return true;
        }

        // Copy data from the live `home`, or from the snapshot where `from_snapshot` or once
        // `home` is gone, under IrqLock.
        bool data_copy(struct arch_aspace* space, VirtualRanges* ranges, Extent const& data,
                       size_t g, bool from_snapshot, struct arch_aspace* home)
        {
            if (from_snapshot and not data_template_fill(data, g))
            {
                return false;
            }
            bool const live = home != nullptr and not from_snapshot;
            if (not live and not g_data_template_filled)
            {
                return false;
            }
            if (not claim(ranges, data, VR_IMAGE))
            {
                return false;
            }
            // Uncleared: the loop below writes every byte of every page.
            arch_phys_addr_t const run = frame_pool_alloc_run(data.pages);
            if (run == 0)
            {
                (void)ranges->release(data.base);
                return false;
            }
            for (size_t i = 0; i < data.pages; i++)
            {
                uintptr_t const va = data.base + static_cast<uintptr_t>(i * g);
                void* const dst = frame_pool_ptr(run + static_cast<arch_phys_addr_t>(i * g));
                void const* src = nullptr;
                bool held = false;
                if (live)
                {
                    src = acquire_page(home, va);
                    held = src != nullptr;
                }
                else
                {
                    src = frame_pool_ptr(g_data_template
                                         + static_cast<arch_phys_addr_t>(i * g));
                }
                if (dst == nullptr or src == nullptr)
                {
                    if (held)
                    {
                        release_page(home, va);
                    }
                    frame_pool_free_run(run, data.pages, g);
                    (void)ranges->release(data.base);
                    return false;
                }
                kmemcpy(dst, src, g);
                if (held)
                {
                    release_page(home, va);
                }
            }
            if (arch_aspace_map(space, data.base, run, data.pages, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                frame_pool_free_run(run, data.pages, g);
                (void)ranges->release(data.base);
                return false;
            }
            // The run is this space's alone; destroy frees it.
            commit(ranges, data, ARCH_MAP_R | ARCH_MAP_W);
            return true;
        }
    }

    bool aspace_image_seed(struct arch_aspace* space, VirtualRanges* ranges, bool from_snapshot,
                           struct arch_aspace* spawner)
    {
        size_t const g = arch_aspace_granule();
        if (not ranges->init(g))
        {
            return false;
        }
        Extent const text = image_text(g);
        if (text.pages != 0)
        {
            // The frames the loader put the app's text in, so one physical page carries that
            // text in every space. VR_BORROWED: aspace_release unmaps them and frees none.
            if (not claim(ranges, text, VR_IMAGE | VR_BORROWED))
            {
                return false;
            }
            if (arch_aspace_map(space, text.base, app_pa(text.base),
                                text.pages, ARCH_MAP_R | ARCH_MAP_X,
                                ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                (void)ranges->release(text.base);
                return false;
            }
            commit(ranges, text, ARCH_MAP_R | ARCH_MAP_X);
        }
        Extent const data = image_data(g);
        if (data.pages == 0)
        {
            return true;
        }
        if (g_data_home != nullptr or g_data_template_filled)
        {
            struct arch_aspace* home = spawner;
            if (home == nullptr)
            {
                home = g_data_home;
            }
            return data_copy(space, ranges, data, g, from_snapshot, home);
        }
        // The first space uses the image data initialized by root constructors.
        // A nonzero template with no live root means snapshot creation failed.
        if (g_data_template != 0)
        {
            return false;
        }
        if (not claim(ranges, data, VR_IMAGE | VR_BORROWED))
        {
            return false;
        }
        // Uncleared: data_template_fill writes every byte, and no seed reads it until it has.
        arch_phys_addr_t const tmpl = frame_pool_alloc_run(data.pages);
        if (tmpl == 0)
        {
            (void)ranges->release(data.base);
            return false;
        }
        if (arch_aspace_map(space, data.base, app_pa(data.base), data.pages,
                            ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
        {
            frame_pool_free_run(tmpl, data.pages, g);
            (void)ranges->release(data.base);
            return false;
        }
        commit(ranges, data, ARCH_MAP_R | ARCH_MAP_W);
        g_data_template = tmpl;
        g_data_home = space;
        return true;
    }

    void* aspace_image_alias(void const* app_ptr)
    {
        if (g_app_rom_hi <= g_app_rom_lo)
        {
            return nullptr; // this chip carves no app window
        }
        uintptr_t const va = reinterpret_cast<uintptr_t>(app_ptr);
        // Only one byte is tested; the signature carries no length.
        if (not in_app_image(va))
        {
            return nullptr;
        }
        uintptr_t const pa = static_cast<uintptr_t>(app_pa(va));
        return reinterpret_cast<void*>(pa + reinterpret_cast<uintptr_t>(g_pool_delta));
    }

    uintptr_t aspace_frame_token(struct arch_aspace* space, uintptr_t va)
    {
        size_t const g = arch_aspace_granule();
        Extent const text = image_text(g);
        if (space == nullptr or text.pages == 0)
        {
            return 0;
        }
        // Compare the frames themselves: a windowed backend answers the same acquire address
        // for every frame, so unequal frames would compare equal.
        arch_phys_addr_t const ref = arch_aspace_frame_at(space, text.base);
        if (ref == 0)
        {
            return 0;
        }
        arch_phys_addr_t const at = arch_aspace_frame_at(space, va);
        if (at == 0)
        {
            return 0;
        }
        // Frames apart, biased so the reference answers 1 and 0 stays "not mapped". Unsigned
        // wrap below the reference is deliberate: the value is compared, never ordered.
        return static_cast<uintptr_t>((at - ref) / g) + 1u;
    }

    uintptr_t aspace_reserve(VirtualRanges* ranges, size_t bytes)
    {
        if (ranges == nullptr or bytes == 0)
        {
            return 0;
        }
        size_t const g = arch_aspace_granule();
        if (bytes > SIZE_MAX - g)
        {
            return 0; // the round-up below would wrap
        }
        size_t const pages = (bytes + g - 1u) / g;
        // Cleared frames: neither the later self-grant nor the handoff writes the bytes first.
        arch_phys_addr_t const run = frame_pool_alloc_user_run(pages);
        if (run == 0)
        {
            return 0;
        }
        uintptr_t const va = aspace_user_va(run);
        if (not ranges->reserve(va, pages, 0))
        {
            frame_pool_free_run(run, pages, g);
            return 0;
        }
        return va;
    }

    bool aspace_frames_type_ok(arch_phys_addr_t pa, size_t pages, uint8_t memtype,
                               VirtualRange const* self)
    {
        size_t const g = arch_aspace_granule();
        arch_phys_addr_t const end = pa + static_cast<arch_phys_addr_t>(pages) * g;
#if KICKOS_AMP_SHARE
        uint8_t share_type = ARCH_MAP_NORMAL;
        if (AMP_SHARE_UNCACHED)
        {
            share_type = ARCH_MAP_NOCACHE;
        }
        if (amp_share_meets(pa, end) and memtype != share_type)
        {
            return false;
        }
#endif
        for (int d = 0; d < KICKOS_MAX_DOMAINS; d++)
        {
            Domain const* const dom = &kernel().domains[d];
            struct arch_aspace* const space = domain_space(dom);
            VirtualRanges const* const ranges = domain_ranges(dom);
            for (size_t i = 0; space != nullptr and ranges != nullptr
                               and i < VirtualRanges::capacity();
                 i++)
            {
                VirtualRange const* const e = ranges->at(i);
                if (e == nullptr or e == self or e->state != VirtualState::Granted
                    or (e->flags & VR_IMAGE) != 0 or e->memtype == memtype)
                {
                    continue;
                }
                // A window or a capability run sits where the kernel chose; everything else a
                // space maps sits at its frames' own address.
                arch_phys_addr_t lo = aspace_frame_of(e->base);
                if ((e->flags & (VR_WINDOW | VR_FRAMECAP)) != 0)
                {
                    lo = arch_aspace_frame_at(space, e->base);
                }
                arch_phys_addr_t const hi = lo + static_cast<arch_phys_addr_t>(e->pages) * g;
                if (lo < end and pa < hi)
                {
                    return false;
                }
            }
        }
        return true;
    }

    int aspace_self_grant(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t base,
                          size_t size, uint32_t rights, enum arch_map_memtype type)
    {
        if (space == nullptr or ranges == nullptr)
        {
            return -KOS_EPERM;
        }
        VirtualRange const* const e = ranges->find(base, size);
        if (e == nullptr)
        {
            // An address this space never reserved, a cross-task self-grant included.
            return -KOS_EPERM;
        }
        uint8_t const memtype = static_cast<uint8_t>(type);
        if (e->state == VirtualState::Granted and (e->rights & rights) == rights
            and e->memtype == memtype)
        {
            return 0;
        }
        if (not vr_caller_nameable(e))
        {
            // A stack run's base is its GUARD, so an admitted grant maps that page too.
            return -KOS_EPERM;
        }
        uintptr_t const b = e->base;
        size_t const pages = e->pages;
        // A window, a handoff or the donor mapping these frames with another type keeps them.
        if (not aspace_frames_type_ok(aspace_frame_of(b), pages, memtype, e))
        {
            return -KOS_EBUSY;
        }
        if (aspace_grant_syncs(e, memtype))
        {
            sync_frames(aspace_frame_of(b), pages);
        }
        if (arch_aspace_map(space, b, aspace_frame_of(b), pages, rights, type)
            != ARCH_ASPACE_OK)
        {
            return -KOS_ENOMEM;
        }
        if (not ranges->grant(b, pages, rights, memtype))
        {
            (void)arch_aspace_unmap(space, b, pages);
            return -KOS_ENOMEM;
        }
        return 0;
    }

    int aspace_cap_map(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                       int run_obj, arch_phys_addr_t base, uint32_t pages, uint32_t rights,
                       enum arch_map_memtype type)
    {
        if (space == nullptr or ranges == nullptr or pages == 0)
        {
            return -KOS_EINVAL;
        }
        size_t const g = arch_aspace_granule();
        if ((va % g) != 0 or (static_cast<uint64_t>(base) % g) != 0)
        {
            return -KOS_EINVAL;
        }
        if (va + static_cast<uintptr_t>(pages) * g < va)
        {
            return -KOS_EINVAL; // the range wraps
        }
        // A leaf is a holder: the frames come back at the last one, not the last capability.
        if (not frame_run_ref(run_obj))
        {
            return -KOS_ENOMEM;
        }
        // Require VR_FRAMECAP as well as VR_BORROWED. Store a biased run index
        // so real slot zero is distinct from no run.
        int const run_slot = frame_run_slot_of(run_obj);
        if (run_slot < 0)
        {
            frame_run_release(run_obj);
            return -KOS_EINVAL;
        }
        if (not ranges->reserve(va, pages, VR_BORROWED | VR_FRAMECAP,
                                static_cast<uint32_t>(run_slot) + 1u))
        {
            frame_run_release(run_obj);
            return -KOS_ENOMEM; // overlaps something this space already names, or the list is full
        }
        if (type == ARCH_MAP_NOCACHE)
        {
            sync_frames(base, pages);
        }
        if (arch_aspace_map(space, va, base, pages, rights, type) != ARCH_ASPACE_OK)
        {
            ranges->release(va);
            frame_run_release(run_obj);
            return -KOS_ENOMEM;
        }
        // The type the PTE carries: the list is what a second mapping's agreement is tested
        // against, and 0 would claim Normal over a non-cacheable leaf.
        if (not ranges->grant(va, pages, rights, static_cast<uint8_t>(type)))
        {
            (void)arch_aspace_unmap(space, va, pages);
            ranges->release(va);
            frame_run_release(run_obj);
            return -KOS_ENOMEM;
        }
        return 0;
    }

    int aspace_cap_unmap(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                         int run_obj)
    {
        if (space == nullptr or ranges == nullptr)
        {
            return -KOS_EINVAL;
        }
        VirtualRange const* const e = ranges->at_base(va);
        // The range must be one aspace_cap_map placed AND must name this run. Matching a page
        // count instead accepts the image and every handoff, which carry VR_BORROWED too.
        if (e == nullptr or e->base != va or (e->flags & VR_FRAMECAP) == 0)
        {
            return -KOS_EPERM;
        }
        int const run_slot = frame_run_slot_of(run_obj);
        if (run_slot < 0 or e->run != static_cast<uint32_t>(run_slot) + 1u)
        {
            return -KOS_EPERM;
        }
        uint32_t const pages = e->pages;
        arch_phys_addr_t uncached_pa = 0;
        if (e->memtype == static_cast<uint8_t>(ARCH_MAP_NOCACHE))
        {
            uncached_pa = arch_aspace_frame_at(space, va);
        }
        if (arch_aspace_unmap(space, va, pages) != ARCH_ASPACE_OK)
        {
            return -KOS_ENOMEM;
        }
        // The run outlives the leaf, and its next mapping may be cacheable.
        if (uncached_pa != 0)
        {
            sync_frames(uncached_pa, pages);
        }
        ranges->release(va);
        frame_run_release(run_obj);
        return 0;
    }

    int aspace_window_map(struct arch_aspace* space, VirtualRanges* ranges, arch_phys_addr_t pa,
                          size_t bytes, uint32_t rights, enum arch_map_memtype type,
                          uint16_t holder, uint32_t place, Domain* donor)
    {
        size_t const g = arch_aspace_granule();
        if (space == nullptr or ranges == nullptr or bytes == 0 or (pa % g) != 0)
        {
            return -KOS_EINVAL;
        }
        size_t const pages = (bytes + g - 1u) / g;
        uintptr_t area = 0;
        size_t area_bytes = 0;
        arch_aspace_window_area(&area, &area_bytes);
        // Asked again here and not only at admission: the spawn's own task data is handed off
        // after its windows were admitted and before they are mapped.
        if (donor != nullptr and not aspace_frames_type_ok(pa, pages, static_cast<uint8_t>(type),
                                                           nullptr))
        {
            return -KOS_EBUSY;
        }
        uintptr_t const va = ranges->place(area, area_bytes / g, pages);
        uint16_t donor_tag = 0;
        if (donor != nullptr)
        {
            donor_tag = static_cast<uint16_t>((domain_handle(donor) & 0xFFFF) + 1);
        }
        if (va == 0
            or not ranges->reserve(va, pages, VR_BORROWED | VR_WINDOW, place, holder,
                                   donor_tag))
        {
            return -KOS_ENOMEM;
        }
        if (type == ARCH_MAP_NOCACHE)
        {
            sync_frames(pa, pages);
        }
        if (arch_aspace_map(space, va, pa, pages, rights, type) != ARCH_ASPACE_OK)
        {
            ranges->release(va);
            return -KOS_ENOMEM;
        }
        if (not ranges->grant(va, pages, rights, static_cast<uint8_t>(type)))
        {
            (void)arch_aspace_unmap(space, va, pages);
            ranges->release(va);
            return -KOS_ENOMEM;
        }
        if (donor != nullptr)
        {
            domain_ref(donor);
        }
        return 0;
    }

    void aspace_window_unmap_holder(struct arch_aspace* space, VirtualRanges* ranges,
                                    uint16_t holder)
    {
        for (size_t i = 0; space != nullptr and ranges != nullptr and i < VirtualRanges::capacity();
             i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e == nullptr or (e->flags & VR_WINDOW) == 0 or e->holder != holder)
            {
                continue;
            }
            uint16_t const donor = e->donor;
            (void)arch_aspace_unmap(space, e->base, e->pages);
            ranges->release(e->base);
            if (donor != 0)
            {
                domain_release(&kernel().domains[donor - 1u]);
            }
        }
    }

    int aspace_handoff(VirtualRanges const* donor, struct arch_aspace* space,
                       VirtualRanges* ranges, uintptr_t base, size_t size,
                       enum arch_map_memtype type)
    {
        if (donor == nullptr or space == nullptr or ranges == nullptr)
        {
            return -KOS_EPERM;
        }
        VirtualRange const* const e = donor->find(base, size);
        if (not vr_caller_nameable(e))
        {
            // A donor's live stack would reach the target and outlive the donor's own run.
            return -KOS_EPERM;
        }
        size_t const g = arch_aspace_granule();
        if (g == 0 or size > SIZE_MAX - (g - 1u))
        {
            return -KOS_EPERM;
        }
        // find() answers for a contained subrange, and what is mapped below is the whole entry,
        // so the reservation's own base and its own page count are required here.
        if (base != e->base or (size + g - 1u) / g != static_cast<size_t>(e->pages))
        {
            return -KOS_EPERM;
        }
        uintptr_t const b = e->base;
        size_t const pages = e->pages;
        uint32_t const rights = ARCH_MAP_R | ARCH_MAP_W;
        // The donor, a window or another handoff mapping these frames with another type keeps
        // them.
        if (not aspace_frames_type_ok(aspace_frame_of(b), pages, static_cast<uint8_t>(type),
                                      nullptr))
        {
            return -KOS_EBUSY;
        }
        // The borrower takes the donor's address; reserve refuses an overlap.
        if (not ranges->reserve(b, pages, VR_BORROWED))
        {
            return -KOS_ENOMEM;
        }
        if (type == ARCH_MAP_NOCACHE)
        {
            sync_frames(aspace_frame_of(b), pages);
        }
        if (arch_aspace_map(space, b, aspace_frame_of(b), pages, rights, type)
            != ARCH_ASPACE_OK)
        {
            (void)ranges->release(b);
            return -KOS_ENOMEM;
        }
        if (not ranges->grant(b, pages, rights, static_cast<uint8_t>(type)))
        {
            (void)arch_aspace_unmap(space, b, pages);
            (void)ranges->release(b);
            return -KOS_ENOMEM;
        }
        return 0;
    }

    void aspace_release(struct arch_aspace* space, VirtualRanges* ranges)
    {
        if (space == nullptr)
        {
            return;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        g_release_runs++;
#endif
        // Switch away before freeing active tables. Clear every core's cached root
        // so a later activation cannot skip installing a replacement.
        uint32_t const cpu = arch_cpu_id();
        for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
        {
            if (g_current[c] != space)
            {
                continue;
            }
#if defined(KICKOS_ENABLE_SELFTEST)
            if (c != cpu)
            {
                g_release_peer_hits++;
            }
#endif
            g_current[c] = nullptr;
            if (c == cpu)
            {
                arch_aspace_activate(arch_aspace_boot());
            }
        }
        size_t const g = arch_aspace_granule();
        if (g_data_home == space)
        {
            // The last moment root's static data exists. A failure leaves it unfilled.
            (void)data_template_fill(image_data(g), g);
            g_data_home = nullptr;
        }
        for (size_t i = 0; ranges != nullptr and i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e == nullptr)
            {
                continue;
            }
            if ((e->flags & VR_USTACK) != 0)
            {
                // ustack_free already released unmapped stack runs. Still-mapped stack pages
                // are freed by destroy; only their guard remains to release here.
                continue;
            }
#if KICKOS_AMP_SHARE
            if ((e->flags & VR_SHARE) != 0)
            {
                if (e->state == VirtualState::Granted)
                {
                    (void)arch_aspace_unmap(space, e->base, e->pages);
                }
                continue;
            }
#endif
            if ((e->flags & VR_BORROWED) != 0)
            {
                // Another space's frames: unmapped here, freed by their owner.
                // A window's donor reference is not dropped here: its holder's exit unmapped it
                // already, and releasing from the teardown would recurse into this function.
                (void)arch_aspace_unmap(space, e->base, e->pages);
                if ((e->flags & VR_FRAMECAP) != 0)
                {
                    // Stored plus one; the VR_FRAMECAP flag is what says this entry named a
                    // run at all, so the subtraction cannot reach VR_RUN_NONE.
                    frame_run_release_by_slot(static_cast<int>(e->run) - 1);
                }
                continue;
            }
            if (e->state == VirtualState::Reserved and (e->flags & VR_IMAGE) == 0)
            {
                // No leaf points at these, so the destroy walk cannot see them; the image is
                // excluded, its pages not being the pool's to take back.
                frame_pool_free_run(aspace_frame_of(e->base), e->pages, g);
            }
        }
        arch_aspace_destroy(space);
    }

    struct arch_aspace* aspace_activate_for(Thread const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        struct arch_aspace* const space = domain_space(task_domain(t->task));
        if (space == nullptr)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            g_unseated_switch_ins++;
#endif
#if KICKOS_KERNEL_CORES > 1
            // On SMP, spaceless threads must install the boot root so they cannot
            // retain a table another core frees. On one core, aspace_release switches
            // away before freeing, so no extra switch is needed here.
            aspace_install_boot();
#endif
            return nullptr;
        }
        uint32_t const cpu = arch_cpu_id();
        if (space == g_current[cpu])
        {
            return space;
        }
        g_current[cpu] = space;
        arch_aspace_activate(space);
        return space;
    }

    bool aspace_seated_for(Thread const* t)
    {
        if (t == nullptr)
        {
            return false;
        }
        struct arch_aspace* const space = domain_space(task_domain(t->task));
        if (space == nullptr)
        {
            return false;
        }
        uint32_t const cpu = arch_cpu_id();
        return space == g_current[cpu];
    }

    void aspace_install_boot(void)
    {
        struct arch_aspace* const boot = arch_aspace_boot();
        uint32_t const cpu = arch_cpu_id();
        if (g_current[cpu] == boot)
        {
            return;
        }
        g_current[cpu] = boot;
        arch_aspace_activate(boot);
    }

    void aspace_forget_current(void)
    {
        uint32_t const cpu = arch_cpu_id();
        g_current[cpu] = nullptr;
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    uint64_t aspace_acquire_balance(void)
    {
        IrqLock lock;
        return (static_cast<uint64_t>(g_acq_live) << 32) | static_cast<uint64_t>(g_acq_unpaired);
    }

    uint64_t aspace_unseated_switch_ins(void)
    {
        IrqLock lock;
        return g_unseated_switch_ins;
    }

    uint64_t aspace_release_peer_hits(void)
    {
        IrqLock lock;
        return g_release_peer_hits;
    }

    uint64_t aspace_release_runs(void)
    {
        IrqLock lock;
        return g_release_runs;
    }

    void aspace_data_home_forget(void)
    {
        IrqLock lock;
        size_t const g = arch_aspace_granule();
        // Mirrors aspace_release exactly, snapshot included.
        (void)data_template_fill(image_data(g), g);
        g_data_home = nullptr;
    }
#endif
}

#endif
