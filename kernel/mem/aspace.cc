// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/aspace.h>

#if KICKOS_HAVE_ASPACE

#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/kruntime.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

extern "C"
{
    // The app's own low window (the chip linker script's image split). WEAK for the same
    // reason arch_ram_common.cc declares them weak: a chip that carves no such window
    // leaves all four at zero and seeds nothing.
    extern unsigned char __kickos_app_rom_start[] __attribute__((weak));
    extern unsigned char __kickos_app_rom_end[] __attribute__((weak));
    extern unsigned char __kickos_app_sram_start[] __attribute__((weak));
    extern unsigned char __kickos_app_sram_end[] __attribute__((weak));
}

namespace kickos
{
    namespace
    {
        // The last space written to the translation root. A switch to a thread of the same
        // task must not repeat the write: with no translation tag the backend drops the
        // whole low half on every root change.
        struct arch_aspace* g_current = nullptr;

        // The space holding the image's OWN static-data pages, which every later space
        // copies from. Root's, being the first seeded, and the app's ctors run in root.
        struct arch_aspace* g_data_home = nullptr;

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

        // Reserve then grant, which is the only way in: `grant` names a reservation this
        // list made, so a range nobody reserved cannot be granted (vrange.h, F10).
        //
        // THE ENTRY IS CLAIMED BEFORE ANYTHING IS MAPPED, and every seeding path below keeps
        // that order. A mapping the list does not record is a mapping teardown cannot see,
        // and for the image that means handing the frame pool the app's own pages.
        bool claim(VirtualRanges* ranges, Extent const& e, uint8_t flags)
        {
            return ranges->reserve(e.base, e.pages, flags);
        }

        void commit(VirtualRanges* ranges, Extent const& e, uint32_t rights)
        {
            // Cannot fail: the entry was reserved here, with these pages, and rights is
            // never 0 on any of these paths.
            (void)ranges->grant(e.base, e.pages, rights, ARCH_MAP_NORMAL);
        }

        void give_back(arch_phys_addr_t run, size_t pages, size_t g)
        {
            for (size_t i = 0; i < pages; i++)
            {
                kickos_frame_free(run + static_cast<arch_phys_addr_t>(i * g));
            }
        }

        // The image's static data into frames of this space's own. The source is the space
        // that holds the image pages themselves, reached through the map editor rather than
        // through the running translation: the creator is not always the process the image
        // belongs to, and a copy taken from whatever happened to be installed would make a
        // grandchild's globals depend on what its parent had scribbled.
        bool data_copy(struct arch_aspace* space, VirtualRanges* ranges, Extent const& data,
                       size_t g)
        {
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
                void* const dst = frame_pool_ptr(run + static_cast<arch_phys_addr_t>(i * g));
                void const* const src =
                    arch_aspace_acquire(g_data_home, data.base + static_cast<uintptr_t>(i * g));
                if (dst == nullptr or src == nullptr)
                {
                    give_back(run, data.pages, g);
                    (void)ranges->release(data.base);
                    return false;
                }
                kmemcpy(dst, src, g);
                arch_aspace_release(g_data_home, data.base + static_cast<uintptr_t>(i * g));
            }
            if (arch_aspace_map(space, data.base, run, data.pages, ARCH_MAP_R | ARCH_MAP_W,
                                ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                give_back(run, data.pages, g);
                (void)ranges->release(data.base);
                return false;
            }
            // OWNED, so destroy is what frees these: the run is this space's alone.
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
            // OUTPUT ADDRESS == LINK ADDRESS: the app's window is linked where it loads, so
            // one physical page carries that text in every space and the mapping is the
            // cheapest possible use of "two spaces, one frame". BORROWED: these pages came
            // from the image and not from the frame pool, so aspace_release takes them out
            // before destroy walks the tree.
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
        if (g_data_home != nullptr)
        {
            return data_copy(space, ranges, data, g);
        }
        // THE IMAGE'S OWN PAGES, and only for the first space seeded. Every process after
        // this one copies these bytes, so what is written here has to be what the app built:
        // root's ctors run in a thread, in this space, and a root holding a copy would leave
        // this page holding link-time bytes forever.
        if (not claim(ranges, data, VR_IMAGE | VR_BORROWED))
        {
            return false;
        }
        if (arch_aspace_map(space, data.base, static_cast<arch_phys_addr_t>(data.base),
                            data.pages, ARCH_MAP_R | ARCH_MAP_W,
                            ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
        {
            (void)ranges->release(data.base);
            return false;
        }
        commit(ranges, data, ARCH_MAP_R | ARCH_MAP_W);
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
        void const* const ref = arch_aspace_acquire(space, text.base);
        void const* const at = arch_aspace_acquire(space, va & ~static_cast<uintptr_t>(g - 1u));
        arch_aspace_release(space, text.base);
        arch_aspace_release(space, va & ~static_cast<uintptr_t>(g - 1u));
        if (ref == nullptr or at == nullptr)
        {
            return 0;
        }
        // Frames apart, biased so the reference itself answers 1 and 0 stays "not mapped".
        // Unsigned wrap on a page BELOW the reference is deliberate: the value is compared,
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
            give_back(run, pages, g);
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
            // An address this space never reserved, which is what a cross-task self-grant
            // is once a reservation names frames of its own (F10).
            return -KOS_EPERM;
        }
        uint8_t const memtype = static_cast<uint8_t>(type);
        if (e->state == VirtualState::Granted and (e->rights & rights) == rights
            and e->memtype == memtype)
        {
            return 0; // already MAPPED here with these attributes, so no page table changes
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
            return -KOS_EPERM; // the donor reserved no such range
        }
        uintptr_t const b = e->base;
        size_t const pages = e->pages;
        uint32_t const rights = ARCH_MAP_R | ARCH_MAP_W;
        // SAME ADDRESS OR NOTHING: reserve refuses an overlap, and that refusal IS the
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
        // THE SPACE BEING DESTROYED CAN BE THE RUNNING ONE: a task's last thread is the one
        // executing when its domain is released, so the tables about to go back to the pool
        // are still what the walker reads. The boot space is the one root that outlives
        // every process.
        if (g_current == space)
        {
            g_current = nullptr;
            arch_aspace_activate(arch_aspace_boot());
        }
        if (g_data_home == space)
        {
            g_data_home = nullptr;
        }
        size_t const g = arch_aspace_granule();
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
                give_back(static_cast<arch_phys_addr_t>(e->base), e->pages, g);
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
        if (space == nullptr or space == g_current)
        {
            return;
        }
        g_current = space;
        arch_aspace_activate(space);
    }

    void aspace_forget_current(void)
    {
        g_current = nullptr;
    }
}

#endif
