// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/aspace.h>

#if KICKOS_HAVE_ASPACE

#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/irqlock.h>
#include <kickos/kruntime.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

extern "C"
{
    // The app's own low window (the chip linker script's image split). Weak: a chip that
    // carves no such window leaves all four at zero and seeds nothing.
    extern unsigned char __kickos_app_rom_start[] __attribute__((weak));
    extern unsigned char __kickos_app_rom_end[] __attribute__((weak));
    extern unsigned char __kickos_app_sram_start[] __attribute__((weak));
    extern unsigned char __kickos_app_sram_end[] __attribute__((weak));
}

namespace kickos
{
    namespace
    {
        // The last space written to this core's translation root. A switch to a thread of
        // the same task must not repeat the write: with no translation tag the backend drops
        // the whole low half on every root change.
        //
        // One cell per core: a shared cell would let a core skip a root switch it has never
        // made.
        struct arch_aspace* g_current[KICKOS_NUM_CORES] = {};
        static_assert(sizeof(g_current) / sizeof(g_current[0]) == KICKOS_NUM_CORES,
                      "the installed-root cache must have one cell per core, never one shared");

        // The space holding the image's own static-data pages: root's, since it is seeded
        // first and the app's ctors run in it. Written only by the first seed and by that
        // space's release, both of which run with interrupts masked, and cleared only after
        // the snapshot beside it is frozen.
        struct arch_aspace* g_data_home = nullptr;

        // The pristine static-data snapshot a space seeded once the home is gone copies from:
        // frames of the pool's own, taken while root is seeded, filled once out of
        // g_data_home on that home's way out, and never written again.
        //
        // The split between the two halves is load-bearing. The bytes have to be root's
        // last, so they cannot be taken while root still runs; the frames have to leave the
        // pool before any caller measures it, or a process created inside a balance window
        // reads as a leak of them.
        arch_phys_addr_t g_data_template = 0;
        bool g_data_template_filled = false;

#if defined(KICKOS_ENABLE_SELFTEST)
        // Outstanding acquires, and releases that paired with none. arch_aspace_release is a
        // no-op on the backend that translates today, so nothing else here can tell a
        // mispaired release from a correct one.
        size_t g_acq_live = 0;
        size_t g_acq_unpaired = 0;

        // Switch-ins of a thread whose task holds no space.
        size_t g_unseated_switch_ins = 0;
#endif

        // Every acquire in this file goes through these two; a direct arch_aspace_acquire
        // would escape the pairing count.
        void const* acquire_page(struct arch_aspace* space, uintptr_t va)
        {
            void const* const p = arch_aspace_acquire(space, va);
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
            uintptr_t const a = reinterpret_cast<uintptr_t>(lo);
            uintptr_t const b = reinterpret_cast<uintptr_t>(hi);
            if (b <= a)
            {
                return Extent{0, 0};
            }
            uintptr_t const base = page_down(a, g);
            return Extent{base, static_cast<size_t>((page_up(b, g) - base) / g)};
        }

        Extent image_text(size_t g)
        {
            return extent_of(__kickos_app_rom_start, __kickos_app_rom_end, g);
        }

        Extent image_data(size_t g)
        {
            return extent_of(__kickos_app_sram_start, __kickos_app_sram_end, g);
        }

        // The entry is claimed before anything is mapped, and every seeding path below keeps
        // that order: a mapping the list does not record is one teardown cannot see, and for
        // the image that means handing the frame pool the app's own pages.
        bool claim(VirtualRanges* ranges, Extent const& e, uint8_t flags)
        {
            return ranges->reserve(e.base, e.pages, flags);
        }

        void commit(VirtualRanges* ranges, Extent const& e, uint32_t rights)
        {
            // Cannot fail: the entry was reserved here, with these pages, and rights is
            // never 0.
            (void)ranges->grant(e.base, e.pages, rights, ARCH_MAP_NORMAL);
        }

        // Runs on the home's way out and nowhere else, with the home's mappings still
        // standing: what it freezes is what root held when it died. After this the home is
        // never read again, which is what stops a space seeded later from mapping the image's
        // own pages and becoming a second home, handing every process after it a live
        // process's mutable globals.
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

        // The image's static data into frames of this space's own.
        //
        // The source is the live home while it lives and the snapshot it left once it is
        // gone. Root maps the image's own pages and the app's ctors run in root, so a
        // process created while root runs takes what root holds now; once root is gone the
        // source is the frozen snapshot, never a later process's pages, which would make
        // some live process the template for every process after it.
        //
        // The home is reached through the map editor and not through the running
        // translation: the creator is not always the process the image belongs to, and a
        // copy taken from whatever happened to be installed would make a grandchild's
        // globals depend on what its parent had scribbled.
        //
        // The caller's IrqLock is what makes the whole copy one snapshot, and it is the
        // SOURCE that needs it: root's live static data, which with interrupts on a user IRQ
        // handler or a sibling thread of root's task can write between two pages, leaving
        // the child on an image no writer ever saw. Nothing this function writes needs the
        // mask.
        bool data_copy(struct arch_aspace* space, VirtualRanges* ranges, Extent const& data,
                       size_t g)
        {
            if (g_data_home == nullptr and not g_data_template_filled)
            {
                return false;
            }
            if (not claim(ranges, data, VR_IMAGE))
            {
                return false;
            }
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
                if (g_data_home != nullptr)
                {
                    src = acquire_page(g_data_home, va);
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
                        release_page(g_data_home, va);
                    }
                    frame_pool_free_run(run, data.pages, g);
                    (void)ranges->release(data.base);
                    return false;
                }
                kmemcpy(dst, src, g);
                if (held)
                {
                    release_page(g_data_home, va);
                }
            }
            if (arch_aspace_map(space, data.base, run, data.pages, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                frame_pool_free_run(run, data.pages, g);
                (void)ranges->release(data.base);
                return false;
            }
            // Owned, not borrowed: the run is this space's alone and destroy frees it.
            commit(ranges, data, ARCH_MAP_R | ARCH_MAP_W);
            return true;
        }
    }

    bool aspace_image_seed(struct arch_aspace* space, VirtualRanges* ranges)
    {
        size_t const g = arch_aspace_granule();
        if (not ranges->init(g))
        {
            return false;
        }
        Extent const text = image_text(g);
        if (text.pages != 0)
        {
            // Output address == link address: the app's window is linked where it loads, so
            // one physical page carries that text in every space. Borrowed, because these
            // pages came from the image and not from the frame pool: aspace_release takes
            // them out of the tree and frees none of them.
            if (not claim(ranges, text, VR_IMAGE | VR_BORROWED))
            {
                return false;
            }
            if (arch_aspace_map(space, text.base, static_cast<arch_phys_addr_t>(text.base),
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
            return data_copy(space, ranges, data, g);
        }
        // The image's own pages, and only for the first space seeded: every process after
        // this one copies these bytes, so root's ctors have to run on the image itself and
        // not on a copy, which would leave this page holding link-time bytes forever.
        //
        // One home for the life of the kernel. The snapshot's run is allocated here and
        // nowhere else, so a non-zero one means a home has already existed and its release
        // could not take the snapshot; seeding a second space onto the image's own pages
        // would make that space the template carrying the dead root's leftovers.
        if (g_data_template != 0)
        {
            return false;
        }
        if (not claim(ranges, data, VR_IMAGE | VR_BORROWED))
        {
            return false;
        }
        // The snapshot's frames, off the pool while root is seeded and filled at root's
        // release.
        arch_phys_addr_t const tmpl = frame_pool_alloc_run(data.pages);
        if (tmpl == 0)
        {
            (void)ranges->release(data.base);
            return false;
        }
        if (arch_aspace_map(space, data.base, static_cast<arch_phys_addr_t>(data.base),
                            data.pages, ARCH_MAP_R | ARCH_MAP_W,
                            ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
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

    uintptr_t aspace_frame_token(struct arch_aspace* space, uintptr_t va)
    {
        size_t const g = arch_aspace_granule();
        Extent const text = image_text(g);
        if (space == nullptr or text.pages == 0)
        {
            return 0;
        }
        uintptr_t const page = va & ~static_cast<uintptr_t>(g - 1u);
        // Released only where one was taken: an acquire that answered null took no hold, and
        // a release paired with it releases somebody else's. A backend whose release is a
        // no-op hides that from every arm.
        void const* const ref = acquire_page(space, text.base);
        if (ref == nullptr)
        {
            return 0;
        }
        void const* const at = acquire_page(space, page);
        if (at == nullptr)
        {
            release_page(space, text.base);
            return 0;
        }
        release_page(space, text.base);
        release_page(space, page);
        // Frames apart, biased so the reference itself answers 1 and 0 stays "not mapped".
        // Unsigned wrap on a page below the reference is deliberate: the value is compared,
        // never ordered.
        return ((reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(ref)) / g) + 1u;
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
        arch_phys_addr_t const run = frame_pool_alloc_run(pages);
        if (run == 0)
        {
            return 0;
        }
        uintptr_t const va = static_cast<uintptr_t>(run);
        if (not ranges->reserve(va, pages, 0))
        {
            frame_pool_free_run(run, pages, g);
            return 0;
        }
        return va;
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
        if ((e->flags & VR_IMAGE) != 0)
        {
            return -KOS_EPERM; // the image is nobody's reservation
        }
        uintptr_t const b = e->base;
        size_t const pages = e->pages;
        if (arch_aspace_map(space, b, static_cast<arch_phys_addr_t>(b), pages, rights, type)
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

    int aspace_handoff(VirtualRanges const* donor, struct arch_aspace* space,
                       VirtualRanges* ranges, uintptr_t base, size_t size,
                       enum arch_map_memtype type)
    {
        if (donor == nullptr or space == nullptr or ranges == nullptr)
        {
            return -KOS_EPERM;
        }
        VirtualRange const* const e = donor->find(base, size);
        if (e == nullptr or (e->flags & VR_IMAGE) != 0)
        {
            return -KOS_EPERM;
        }
        uintptr_t const b = e->base;
        size_t const pages = e->pages;
        uint32_t const rights = ARCH_MAP_R | ARCH_MAP_W;
        // Same address or nothing: reserve refuses an overlap, and that refusal is the
        // contract, an address the donor computed being able to sit inside the block.
        if (not ranges->reserve(b, pages, VR_BORROWED))
        {
            return -KOS_ENOMEM;
        }
        if (arch_aspace_map(space, b, static_cast<arch_phys_addr_t>(b), pages, rights, type)
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
        // The space being destroyed can be the running one: a task's last thread is the one
        // executing when its domain is released, so the tables about to go back to the pool
        // are still what the walker reads. The boot space is the one root that outlives
        // every process.
        //
        // Every core's cell, not this core's alone: a root about to be freed left cached on
        // another core is a switch that core would skip, into tables the pool has handed out.
        // Only the running core's own root is rewritten here, a second core being switched
        // off a dying space by its own scheduler.
        uint32_t const cpu = arch_cpu_id();
        for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
        {
            if (g_current[c] != space)
            {
                continue;
            }
            g_current[c] = nullptr;
            if (c == cpu)
            {
                arch_aspace_activate(arch_aspace_boot());
            }
        }
        size_t const g = arch_aspace_granule();
        if (g_data_home == space)
        {
            // The snapshot is taken here, with this space's mappings still standing: the
            // last moment root's static data exists at all. A failure leaves it unfilled,
            // and the next seed then refuses.
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
            if ((e->flags & VR_BORROWED) != 0)
            {
                // Another space's frames: out of the tree, and not one of them freed.
                (void)arch_aspace_unmap(space, e->base, e->pages);
                continue;
            }
            if (e->state == VirtualState::Reserved and (e->flags & VR_IMAGE) == 0)
            {
                // Frames with no leaf pointing at them, so the destroy walk cannot see them.
                // The image is excluded because its pages are not the pool's to take back.
                frame_pool_free_run(static_cast<arch_phys_addr_t>(e->base), e->pages, g);
            }
        }
        arch_aspace_destroy(space);
    }

    void aspace_activate_for(Thread const* t)
    {
        if (t == nullptr)
        {
            return;
        }
        struct arch_aspace* const space = domain_space(task_domain(t->task));
        if (space == nullptr)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            g_unseated_switch_ins++;
#endif
            return;
        }
        uint32_t const cpu = arch_cpu_id();
        if (space == g_current[cpu])
        {
            return;
        }
        g_current[cpu] = space;
        arch_aspace_activate(space);
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

    void aspace_data_home_forget(void)
    {
        IrqLock lock;
        size_t const g = arch_aspace_granule();
        // Exactly what the release does, snapshot included: staging the loss of the home
        // only in part would witness a posture no release produces.
        (void)data_template_fill(image_data(g), g);
        g_data_home = nullptr;
    }
#endif
}

#endif
