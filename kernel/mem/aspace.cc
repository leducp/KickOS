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
    // The chip linker script's app image split; a chip that carves no window leaves all four zero.
    extern unsigned char __kickos_app_rom_start[] __attribute__((weak));
    extern unsigned char __kickos_app_rom_end[] __attribute__((weak));
    extern unsigned char __kickos_app_sram_start[] __attribute__((weak));
    extern unsigned char __kickos_app_sram_end[] __attribute__((weak));

    // Added to an app virtual address to name the frame the loader put those bytes in.
    extern unsigned char __kickos_app_load_delta[] __attribute__((weak));

    // The kernel address that corresponds to physical address 0 for the image's own DRAM.
    extern unsigned char __kickos_frame_pool_delta[];
}

namespace kickos
{
    namespace
    {
        // The last space written to this core's translation root.
        struct arch_aspace* g_current[KICKOS_NUM_CORES] = {};
        static_assert(sizeof(g_current) / sizeof(g_current[0]) == KICKOS_NUM_CORES,
                      "the installed-root cache must have one cell per core, never one shared");

        // The space holding the image's own static-data pages. Written by the first seed and
        // by that space's release, and cleared only after the snapshot beside it is frozen.
        struct arch_aspace* g_data_home = nullptr;

        // The pristine static-data snapshot a space seeded once the home is gone copies from.
        // Its frames leave the pool while root is seeded; the bytes are filled at root's release.
        arch_phys_addr_t g_data_template = 0;
        bool g_data_template_filled = false;

#if defined(KICKOS_ENABLE_SELFTEST)
        // Outstanding acquires, and releases that paired with none.
        size_t g_acq_live = 0;
        size_t g_acq_unpaired = 0;

        size_t g_unseated_switch_ins = 0;
#endif

        // Every acquire in this file goes through these two, or the pairing count escapes.
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

        // volatile keeps each a relocated word; a plain constant folds back into every reader.
        unsigned char* const volatile g_app_rom_lo = __kickos_app_rom_start;
        unsigned char* const volatile g_app_rom_hi = __kickos_app_rom_end;
        unsigned char* const volatile g_app_sram_lo = __kickos_app_sram_start;
        unsigned char* const volatile g_app_sram_hi = __kickos_app_sram_end;
        unsigned char* const volatile g_app_load_delta = __kickos_app_load_delta;
        unsigned char* const volatile g_pool_delta = __kickos_frame_pool_delta;

        // The frame the loader placed an app virtual address in.
        arch_phys_addr_t app_pa(uintptr_t va)
        {
            return static_cast<arch_phys_addr_t>(va)
                   + static_cast<arch_phys_addr_t>(
                       reinterpret_cast<uintptr_t>(g_app_load_delta));
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
            return extent_of(g_app_rom_lo, g_app_rom_hi, g);
        }

        Extent image_data(size_t g)
        {
            return extent_of(g_app_sram_lo, g_app_sram_hi, g);
        }

        // Whether `p` is a byte of the app image, which is the domain the two deltas below are
        // valid for. The chip carves the app's read-only window (__kickos_app_rom_*) and its
        // writable one (__kickos_app_sram_*, covering .appdata, .appbss and the app heap); a
        // window a chip does not carve leaves its pair equal, which admits nothing.
        bool in_app_image(uintptr_t p)
        {
            uintptr_t const rom_lo = reinterpret_cast<uintptr_t>(g_app_rom_lo);
            uintptr_t const rom_hi = reinterpret_cast<uintptr_t>(g_app_rom_hi);
            if (p >= rom_lo and p < rom_hi)
            {
                return true;
            }
            uintptr_t const sram_lo = reinterpret_cast<uintptr_t>(g_app_sram_lo);
            uintptr_t const sram_hi = reinterpret_cast<uintptr_t>(g_app_sram_hi);
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

        // Runs on the home's way out and nowhere else, with the home's mappings still standing.
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

        // The image's static data into frames of this space's own: from the live home while it
        // lives, from the frozen snapshot once it is gone. The caller's IrqLock is what makes
        // the copy one point-in-time snapshot of the source.
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
            // The run is this space's alone; destroy frees it.
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
            return data_copy(space, ranges, data, g);
        }
        // Only the first space seeded maps the image's own pages: root's ctors run on the image
        // itself, and every process after this one copies these bytes. A non-zero template means
        // a home has already existed and its release could not take the snapshot.
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
        if (not vr_caller_nameable(e))
        {
            // A stack run's base is its GUARD, so an admitted grant maps that page too.
            return -KOS_EPERM;
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
        // VR_FRAMECAP and not VR_BORROWED alone: the image and every handoff carry that
        // bit too, and a revoke keyed on it accepts ranges this call never placed.
        // PLUS ONE, so the stored word is never the no-run encoding for a real slot 0.
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
        if (arch_aspace_unmap(space, va, pages) != ARCH_ASPACE_OK)
        {
            return -KOS_ENOMEM;
        }
        ranges->release(va);
        frame_run_release(run_obj);
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
        // The borrower takes the donor's address; reserve refuses an overlap.
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
        // The space being destroyed can be the running one, so the tables about to go back to
        // the pool are still what the walker reads. Every core's cell is cleared: a root left
        // cached on another core is a switch that core would skip, into freed tables.
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
                // NEITHER ARM BELOW, which is the whole reason the flag exists. ustack_free
                // released this run before the task's reference dropped; where it never ran,
                // the destroy walk below frees the still-mapped stack pages off their leaves
                // and strands only the guard. The reserved arm would free frames a leaf still
                // points at.
                continue;
            }
            if ((e->flags & VR_BORROWED) != 0)
            {
                // Another space's frames: unmapped here, freed by their owner.
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
#if KICKOS_KERNEL_CORES > 1
            // A core's translation base names the running thread's space or the boot root: an
            // outgoing space left installed here would have this core walking tables the
            // release path frees and the pool reissues.
            struct arch_aspace* const boot = arch_aspace_boot();
            uint32_t const here = arch_cpu_id();
            if (g_current[here] != boot)
            {
                g_current[here] = boot;
                arch_aspace_activate(boot);
            }
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
        // Mirrors aspace_release exactly, snapshot included.
        (void)data_template_fill(image_data(g), g);
        g_data_home = nullptr;
    }
#endif
}

#endif
