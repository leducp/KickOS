// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/aspace.h>

#include "frame_pool_unwritten.h"

#if KICKOS_HAVE_ASPACE

#include <kickos/ampshare.h>
#include <kickos/debug.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/instance.h>
#include <kickos/irq_route.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/klink.h>
#include <kickos/kruntime.h>
#include <kickos/sched.h>
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
}

namespace kickos
{
    // Every caller writes every byte of the run before anything reads it or a task maps it.
    class AspaceUnwritten
    {
    public:
        static arch_phys_addr_t alloc_run(size_t pages)
        {
            return UnwrittenFrames::alloc_run(pages);
        }
    };

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

        // Granules copied under the kernel lock, for a new space's static data or the snapshot.
        uint32_t g_locked_pages = 0;
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

#if KICKOS_PRESYNC
#if defined(KICKOS_ENABLE_SELFTEST)
        // Written under IrqLock.
        Atomic<uint32_t, Order::RELAXED> g_presync_refusals = 0;
        Atomic<uint32_t, Order::RELAXED> g_presync_granules = 0;
        Atomic<uint32_t, Order::RELAXED> g_staged_granules = 0;
        Atomic<uint32_t, Order::RELAXED> g_presync_masked_max = 0;
        uint64_t g_presync_masked_ns_max = 0;

        void presync_refused()
        {
            g_presync_refusals.store(g_presync_refusals.load() + 1u);
        }
#else
        void presync_refused() {}
#endif

        PresyncRecord* presync_of(Thread const* t)
        {
            int const i = kernel().threads.index_of(t);
            if (i < 0)
            {
                return nullptr;
            }
            return &kernel().presync[i];
        }

        void presync_drop(PresyncRecord& r)
        {
            if (r.live)
            {
                r.live = false;
                kernel().presync_live--;
            }
        }

#if KICKOS_ARCH_ALIAS_DCACHE
        size_t pool_pages(arch_phys_addr_t pa, size_t pages)
        {
            arch_phys_addr_t lo = 0;
            arch_phys_addr_t hi = 0;
            frame_pool_phys_bounds(&lo, &hi);
            return presync_pool_pages(pa, pages, arch_aspace_granule(), lo, hi);
        }

        // Drops every other live record meeting [pa, pa + pages granules). Caller holds IrqLock.
        void presync_void(arch_phys_addr_t pa, size_t pages)
        {
            Kernel& k = kernel();
            if (k.presync_live == 0)
            {
                return;
            }
            PresyncRecord const* const mine = presync_record();
            size_t const g = arch_aspace_granule();
            for (PresyncRecord& r : k.presync)
            {
                if (&r != mine and presync_dropped_by(r, pa, pages, g))
                {
                    presync_drop(r);
                }
            }
        }
#endif

        void presync_unstage(PresyncRecord& r)
        {
            if (r.staged != 0)
            {
                frame_pool_free_run(r.staged, r.staged_pages, arch_aspace_granule());
            }
            r.staged = 0;
            r.staged_pages = 0;
            r.stage = PresyncStage::NONE;
            r.staged_full = false;
            r.staged_home = nullptr;
        }

        // An unprivileged caller's work outside the lock, and the interrupt window after each
        // granule of it.
        class GranuleWindows
        {
          public:
            explicit GranuleWindows(Thread const* c, PresyncRecord& r)
                : open_(not c->privileged), r_(r)
            {
#if defined(KICKOS_ENABLE_SELFTEST)
                line_ = r.inject_line;
                r.inject_line = 0;
                idle_ = r.idle_window;
                r.idle_window = false;
                from_ = arch_clock_now();
#endif
            }

            // Selftest only: the armed line's window, for a run with no granule to open one.
            void idle()
            {
#if defined(KICKOS_ENABLE_SELFTEST)
                if (open_ and idle_ and line_ != 0)
                {
                    inject(line_);
                    line_ = 0;
                    r_.windows_opened++;
                    arch_irq_window();
                }
#endif
            }

            // One granule synced, cleared, copied or freed, with interrupts masked since the
            // last window.
            void granule(bool staged)
            {
#if defined(KICKOS_ENABLE_SELFTEST)
                if (staged)
                {
                    staged_++;
                }
                else
                {
                    granules_++;
                }
                run_++;
                uint64_t const ns = arch_clock_now() - from_;
                if (open_ and run_ > run_max_)
                {
                    run_max_ = run_;
                }
                if (open_ and ns > ns_max_)
                {
                    ns_max_ = ns;
                }
#else
                (void)staged;
#endif
                if (not open_)
                {
                    return;
                }
#if defined(KICKOS_ENABLE_SELFTEST)
                if (line_ != 0)
                {
                    inject(line_);
                    line_ = 0;
                }
                r_.windows_opened++;
#endif
                arch_irq_window();
#if defined(KICKOS_ENABLE_SELFTEST)
                run_ = 0;
                from_ = arch_clock_now();
#endif
            }

            // Takes IrqLock.
            void publish()
            {
#if defined(KICKOS_ENABLE_SELFTEST)
                IrqLock lock;
                g_presync_granules.store(g_presync_granules.load() + granules_);
                g_staged_granules.store(g_staged_granules.load() + staged_);
                if (run_max_ > g_presync_masked_max.load())
                {
                    g_presync_masked_max.store(run_max_);
                }
                if (ns_max_ > g_presync_masked_ns_max)
                {
                    g_presync_masked_ns_max = ns_max_;
                }
#endif
            }

          private:
            // A frame of its own: inlined, the bracket widens the SYSWIN chain's frame.
            __attribute__((noinline)) static void inject(uint16_t line)
            {
                IrqLock lock;
                irq_inject(static_cast<int>(line), lock);
            }

            bool open_;
            PresyncRecord& r_;
#if defined(KICKOS_ENABLE_SELFTEST)
            uint16_t line_ = 0;
            bool idle_ = false;
            uint32_t granules_ = 0;
            uint32_t staged_ = 0;
            uint32_t run_ = 0;
            uint32_t run_max_ = 0;
            uint64_t from_ = 0;
            uint64_t ns_max_ = 0;
#endif
        };
#endif

        // The locked pass's half of the sync a mapping of another memory type than its frames
        // last had, or a non-cacheable one, owes them: true where this call synced every pool
        // frame of the span ahead of the lock and no other call has completed a mapping over
        // them since. False, marking the call refused, otherwise. Maintains nothing.
        [[nodiscard]] bool sync_frames(arch_phys_addr_t pa, size_t pages)
        {
#if KICKOS_ARCH_ALIAS_DCACHE
            if (pool_pages(pa, pages) == 0)
            {
                return true;
            }
            PresyncRecord* const r = presync_record();
            if (r != nullptr and presync_excuses(*r, pa, pages, arch_aspace_granule()))
            {
                return true;
            }
            presync_refused();
            return false;
#else
            (void)pa;
            (void)pages;
            return true;
#endif
        }

        // The locked pass mapped [pa, pa + pages granules), which drops every other record
        // meeting them when the call completes (presync_commit) and never before.
        void edited(arch_phys_addr_t pa, size_t pages)
        {
#if KICKOS_ARCH_ALIAS_DCACHE
            if (pool_pages(pa, pages) == 0)
            {
                return;
            }
            // Each editor is reached only from a presync_wanted syscall, whose spans fit.
            PresyncRecord* const r = presync_record();
            bool kept = r != nullptr and r->active and presync_add(r->edits, pa, pages);
#if defined(KICKOS_ENABLE_SELFTEST)
            kept = kept or (r != nullptr and r->probe);
#endif
            KICKOS_ASSERT(kept);
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

        // Copies granule `i` of the image's static data based at `base` into the run at `to`,
        // from the live `home` or from the snapshot where it is null; false when either end is
        // gone. Both ends are cacheable (an image range refuses a memory-type change), so no
        // alias sync is owed. Out of line: inlined, it widens the seed's frame on the spawn chain.
        __attribute__((noinline)) bool copy_data_granule(arch_phys_addr_t to,
                                                         struct arch_aspace* home, uintptr_t base,
                                                         size_t i, size_t g)
        {
            arch_phys_addr_t const step = static_cast<arch_phys_addr_t>(i * g);
            void* const dst = frame_pool_ptr(to + step);
            if (dst == nullptr)
            {
                return false;
            }
            if (home == nullptr)
            {
                void const* const src = frame_pool_ptr(g_data_template + step);
                if (src == nullptr)
                {
                    return false;
                }
                kmemcpy(dst, src, g);
                return true;
            }
            uintptr_t const va = base + static_cast<uintptr_t>(i * g);
            void const* const src = acquire_page(home, va);
            if (src == nullptr)
            {
                return false;
            }
            kmemcpy(dst, src, g);
            release_page(home, va);
            return true;
        }

        // Runs with the home's mappings still standing: at the first explicit task's seed and
        // on the home's way out. Inlined: a frame of its own deepens a dying thread's chain
        // through its space's release.
        __attribute__((always_inline)) inline bool data_template_fill(Extent const& data, size_t g)
        {
            if (g_data_template_filled)
            {
                return true;
            }
            if (g_data_template == 0 or g_data_home == nullptr)
            {
                return false;
            }
#if defined(KICKOS_ENABLE_SELFTEST)
            g_locked_pages += static_cast<uint32_t>(data.pages);
#endif
            for (size_t i = 0; i < data.pages; i++)
            {
                if (not copy_data_granule(g_data_template, g_data_home, data.base, i, g))
                {
                    return false;
                }
            }
            g_data_template_filled = true;
            return true;
        }

        // The source a new space's static data is copied from: the live `*home`, or the
        // snapshot where it is null; false when there is none. The stage a system call makes
        // ahead of its lock and the locked seed both ask here: a seed that disagrees with its
        // stage refuses it, and the call goes round again forever.
        bool image_data_source(bool from_snapshot, struct arch_aspace* spawner,
                               Extent const& data, size_t g, struct arch_aspace** home)
        {
            if (g_data_home == nullptr and not g_data_template_filled)
            {
                return false;
            }
            *home = nullptr;
            if (from_snapshot)
            {
                return data_template_fill(data, g);
            }
            *home = g_data_home;
            if (spawner != nullptr)
            {
                *home = spawner;
            }
            return *home != nullptr or g_data_template_filled;
        }

        // Maps the written `run` as `data`, whose range is claimed; frees both on a refusal.
        // Inlined: a frame of its own deepens the spawn's chain on the kernel block.
        __attribute__((always_inline)) inline bool data_map(struct arch_aspace* space,
                                                            VirtualRanges* ranges,
                                                            Extent const& data,
                                                            arch_phys_addr_t run, size_t g)
        {
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

        // Copies data from the source image_data_source chose, under IrqLock.
        bool data_copy(struct arch_aspace* space, VirtualRanges* ranges, Extent const& data,
                       size_t g, struct arch_aspace* home)
        {
            if (not claim(ranges, data, VR_IMAGE))
            {
                return false;
            }
#if KICKOS_PRESYNC
            // A system call copies ahead of its lock (presync_stage_image), and goes round
            // again when what it copied is not what this space takes.
            PresyncRecord* const r = presync_record();
            if (r != nullptr and r->active and r->staged_short)
            {
                (void)ranges->release(data.base);
                return false;
            }
            if (r != nullptr and r->active)
            {
                if (r->stage != PresyncStage::IMAGE or not r->staged_full
                    or r->staged_pages != data.pages or r->staged_home != home)
                {
                    r->refused = true;
                    presync_refused();
                    (void)ranges->release(data.base);
                    return false;
                }
                arch_phys_addr_t const staged = r->staged;
                r->staged = 0;
                presync_unstage(*r);
                return data_map(space, ranges, data, staged, g);
            }
#endif
            // Uncleared: the loop below writes every byte of every page.
            arch_phys_addr_t const run = AspaceUnwritten::alloc_run(data.pages);
            if (run == 0)
            {
                (void)ranges->release(data.base);
                return false;
            }
#if defined(KICKOS_ENABLE_SELFTEST)
            g_locked_pages += static_cast<uint32_t>(data.pages);
#endif
            for (size_t i = 0; i < data.pages; i++)
            {
                if (not copy_data_granule(run, home, data.base, i, g))
                {
                    frame_pool_free_run(run, data.pages, g);
                    (void)ranges->release(data.base);
                    return false;
                }
            }
            return data_map(space, ranges, data, run, g);
        }
    }

    namespace
    {
#if KICKOS_PRESYNC
        // Writes granule `j` of `r`'s staged run, cleared or copied from its source; false when
        // the source has gone. Takes IrqLock for the one granule.
        bool stage_granule(Thread const* c, PresyncRecord const& r, uint32_t j, size_t g)
        {
            IrqLock lock;
            if (r.stage == PresyncStage::CLEAR)
            {
                void* const dst = frame_pool_ptr(r.staged + static_cast<arch_phys_addr_t>(j) * g);
                if (dst == nullptr)
                {
                    return false;
                }
                kmemset(dst, 0, g);
                return true;
            }
            // Root's space may be released between two granules.
            if (r.staged_home != nullptr and r.staged_home != g_data_home
                and r.staged_home != domain_space(thread_domain(c)))
            {
                return false;
            }
            return copy_data_granule(r.staged, r.staged_home, image_data(g).base, j, g);
        }
#endif
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
        struct arch_aspace* home = nullptr;
        if (image_data_source(from_snapshot, spawner, data, g, &home))
        {
            return data_copy(space, ranges, data, g, home);
        }
        // Only the first seed finds no template; every later one with no source is refused.
        if (g_data_template != 0)
        {
            return false;
        }
        // The first space uses the image data initialized by root constructors.
        if (not claim(ranges, data, VR_IMAGE | VR_BORROWED))
        {
            return false;
        }
        // Uncleared: data_template_fill writes every byte, and no seed reads it until it has.
        arch_phys_addr_t const tmpl = AspaceUnwritten::alloc_run(data.pages);
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
        return frame_pool_alias(app_pa(va));
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

#if KICKOS_PRESYNC
    PresyncRecord* presync_record()
    {
        return presync_of(sched::current());
    }

    bool presync_begin()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr)
        {
            return false;
        }
        bool const entering = not r->active;
        presync_drop(*r);
        presync_unstage(*r);
        r->noted.count = 0;
        r->edits.count = 0;
        r->refused = false;
        r->staged_short = false;
        r->active = true;
        if (entering)
        {
            r->has_params = false;
            r->has_windows = false;
        }
        return entering;
    }

#if KICKOS_ARCH_ALIAS_DCACHE
    void presync_note(arch_phys_addr_t pa, size_t pages)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or pool_pages(pa, pages) == 0)
        {
            return;
        }
        // A call notes at most a span per window and one for its task data.
        bool const noted = presync_add(r->noted, pa, pages);
        KICKOS_ASSERT(noted);
        if (not r->live)
        {
            r->live = true;
            kernel().presync_live++;
        }
    }
#endif

    void presync_stage_image(bool from_snapshot, struct arch_aspace* spawner)
    {
        PresyncRecord* const r = presync_record();
        size_t const g = arch_aspace_granule();
        Extent const data = image_data(g);
        struct arch_aspace* home = nullptr;
        if (r == nullptr or data.pages == 0
            or not image_data_source(from_snapshot, spawner, data, g, &home))
        {
            return;
        }
        // Unwritten: presync_run writes every byte of every page before anything maps it.
        arch_phys_addr_t const run = AspaceUnwritten::alloc_run(data.pages);
        if (run == 0)
        {
            r->staged_short = true;
            return;
        }
        presync_unstage(*r);
        r->staged = run;
        r->staged_pages = static_cast<uint32_t>(data.pages);
        r->stage = PresyncStage::IMAGE;
        r->staged_home = home;
    }

    arch_phys_addr_t aspace_reserve_stage(VirtualRanges const* ranges, size_t bytes)
    {
        PresyncRecord* const r = presync_record();
        size_t const g = arch_aspace_granule();
        if (r == nullptr or ranges == nullptr or bytes == 0 or bytes > SIZE_MAX - g
            or bytes / g > UINT32_MAX)
        {
            return 0;
        }
        size_t const pages = (bytes + g - 1u) / g;
        // Unwritten: presync_run clears every byte before the range names the frames.
        arch_phys_addr_t const run = AspaceUnwritten::alloc_run(pages);
        if (run == 0)
        {
            return 0;
        }
        presync_unstage(*r);
        r->staged = run;
        r->staged_pages = static_cast<uint32_t>(pages);
        r->stage = PresyncStage::CLEAR;
        return run;
    }

    uintptr_t aspace_reserve_commit(VirtualRanges* ranges)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or r->stage != PresyncStage::CLEAR or not r->staged_full
            or ranges == nullptr)
        {
            return 0;
        }
        uintptr_t const va = aspace_user_va(r->staged);
        if (not ranges->reserve(va, r->staged_pages, 0))
        {
            return 0;
        }
        r->staged = 0;
        presync_unstage(*r);
        return va;
    }

    void presync_run()
    {
        Thread const* const c = sched::current();
        PresyncRecord* const r = presync_of(c);
        if (r == nullptr)
        {
            return;
        }
        size_t const g = arch_aspace_granule();
        GranuleWindows windows(c, *r);
#if KICKOS_ARCH_ALIAS_DCACHE
        for (uint8_t i = 0; i < r->noted.count; i++)
        {
            for (uint32_t j = 0; j < r->noted.pages[i]; j++)
            {
                void const* const p =
                    frame_pool_ptr(r->noted.pa[i] + static_cast<arch_phys_addr_t>(j) * g);
                if (p == nullptr)
                {
                    continue;
                }
                arch_irq_state_t const s = arch_irq_save();
                alias_sync(p, g);
                arch_irq_restore(s);
                windows.granule(false);
            }
        }
#endif
        bool full = r->stage != PresyncStage::NONE;
        for (uint32_t j = 0; full and j < r->staged_pages; j++)
        {
            full = stage_granule(c, *r, j, g);
            windows.granule(true);
        }
        r->staged_full = full;
        windows.idle();
        windows.publish();
#if defined(KICKOS_ENABLE_SELFTEST)
        IrqLock lock;
        if (r->drops_left != 0)
        {
            if (r->drops_left != UINT8_MAX)
            {
                r->drops_left--;
            }
            presync_drop(*r);
        }
#endif
    }

    void presync_commit()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr)
        {
            return;
        }
#if KICKOS_ARCH_ALIAS_DCACHE
        for (uint8_t i = 0; i < r->edits.count; i++)
        {
            presync_void(r->edits.pa[i], r->edits.pages[i]);
        }
#endif
        r->edits.count = 0;
    }

    bool presync_end()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr)
        {
            return false;
        }
        bool const refused = r->refused;
        presync_drop(*r);
        r->noted.count = 0;
        r->edits.count = 0;
        r->refused = false;
        r->staged_short = false;
        if (not refused)
        {
            r->active = false;
            r->has_params = false;
            r->has_windows = false;
        }
        return refused;
    }

    void presync_release(bool succeeded)
    {
        Thread const* const c = sched::current();
        PresyncRecord* const r = presync_of(c);
        if (r == nullptr or r->staged == 0)
        {
            return;
        }
        // A slay at a window below would drop what a succeeded call delivered.
        KICKOS_ASSERT(not succeeded);
        size_t const g = arch_aspace_granule();
        GranuleWindows windows(c, *r);
        while (true)
        {
            {
                IrqLock lock;
                if (r->staged_pages == 0)
                {
                    break;
                }
                // The rest of the run stays the record's, so a thread slain at the window
                // below frees it from its exit.
                kickos_frame_free(r->staged);
                r->staged += static_cast<arch_phys_addr_t>(g);
                r->staged_pages--;
            }
            windows.granule(true);
        }
        windows.publish();
        IrqLock lock;
        r->staged = 0;
        presync_unstage(*r);
    }

    void presync_fresh(Thread const* t)
    {
        PresyncRecord* const r = presync_of(t);
        if (r == nullptr)
        {
            return;
        }
        presync_drop(*r);
        presync_unstage(*r);
        kmemset(r, 0, sizeof(*r));
    }

    void presync_exit()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr)
        {
            return;
        }
        presync_drop(*r);
        presync_unstage(*r);
        r->noted.count = 0;
        r->edits.count = 0;
        r->refused = false;
        r->staged_short = false;
        r->active = false;
        r->has_params = false;
        r->has_windows = false;
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    uint32_t presync_refusals()
    {
        return g_presync_refusals.load();
    }

    uint64_t presync_masked_max()
    {
        IrqLock lock;
        return (g_presync_masked_ns_max << 32) | g_presync_masked_max.load();
    }

    uint64_t presync_granules()
    {
        IrqLock lock;
        return (static_cast<uint64_t>(g_staged_granules.load()) << 32)
               | g_presync_granules.load();
    }

    uint32_t presync_live_count()
    {
        IrqLock lock;
        return kernel().presync_live;
    }

    bool presync_live_of(Thread const* t)
    {
        IrqLock lock;
        PresyncRecord const* const r = presync_of(t);
        return r != nullptr and r->live;
    }

    uint32_t presync_windows_mine()
    {
        IrqLock lock;
        PresyncRecord const* const r = presync_record();
        if (r == nullptr)
        {
            return 0;
        }
        return r->windows_opened;
    }

    void presync_probe(bool on)
    {
        PresyncRecord* const r = presync_record();
        if (r != nullptr)
        {
            r->probe = on;
        }
    }

    void presync_arm(uint16_t line, uint8_t drops, bool fault_out, bool idle_window)
    {
        PresyncRecord* const r = presync_record();
        if (r != nullptr)
        {
            r->inject_line = line;
            r->drops_left = drops;
            r->fault_out = fault_out;
            r->idle_window = idle_window;
        }
    }

    bool presync_take_fault_out()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or not r->fault_out)
        {
            return false;
        }
        r->fault_out = false;
        return true;
    }
#endif
#endif

    namespace
    {
        // 0 with the range to map in *out, 1 where it is mapped so already, or the refusal.
        int self_grant_admit(VirtualRanges const* ranges, uintptr_t base, size_t size,
                             uint32_t rights, uint8_t memtype, VirtualRange const** out)
        {
            VirtualRange const* const e = ranges->find(base, size);
            if (e == nullptr)
            {
                // An address this space never reserved, a cross-task self-grant included.
                return -KOS_EPERM;
            }
            if (e->state == VirtualState::Granted and (e->rights & rights) == rights
                and e->memtype == memtype)
            {
                return 1;
            }
            if (not vr_caller_nameable(e))
            {
                // A stack run's base is its guard, so an admitted grant would map that page too.
                return -KOS_EPERM;
            }
            // A window, a handoff or the donor mapping these frames with another type keeps
            // them.
            if (not aspace_frames_type_ok(aspace_frame_of(e->base), e->pages, memtype, e))
            {
                return -KOS_EBUSY;
            }
            *out = e;
            return 0;
        }

#if KICKOS_ARCH_ALIAS_DCACHE
        // A whole reservation of `own`'s at `base`, `bytes` long, or null.
        VirtualRange const* whole_reservation(VirtualRanges const* own, uintptr_t base,
                                              size_t bytes)
        {
            size_t const g = arch_aspace_granule();
            if (own == nullptr or g == 0 or bytes > SIZE_MAX - (g - 1u))
            {
                return nullptr;
            }
            VirtualRange const* const e = own->at_base(base);
            if (not vr_caller_nameable(e) or static_cast<size_t>(e->pages) != (bytes + g - 1u) / g)
            {
                return nullptr;
            }
            return e;
        }
#endif

        bool cacheable(enum arch_map_memtype type)
        {
            return type == ARCH_MAP_NORMAL;
        }

#if KICKOS_ARCH_ALIAS_DCACHE
        // The reservation whose frames start at `pa`, in the space that made it, or null: the
        // one record of what its frames owe, whichever space maps them.
        VirtualRange const* reservation_at(arch_phys_addr_t pa, VirtualRanges** in)
        {
            uintptr_t const va = aspace_user_va(pa);
            for (int d = 0; d < KICKOS_MAX_DOMAINS; d++)
            {
                Domain* const dom = &kernel().domains[d];
                VirtualRanges* const ranges = domain_ranges_mut(dom);
                if (domain_space(dom) == nullptr or ranges == nullptr)
                {
                    continue;
                }
                VirtualRange const* const e = ranges->at_base(va);
                if (vr_caller_nameable(e) and (e->flags & VR_BORROWED) == 0)
                {
                    *in = ranges;
                    return e;
                }
            }
            return nullptr;
        }

        // The reservation a mapping's frames belong to, read once per locked pass: the list
        // that holds it, its base, and whether its frames owe a sync.
        struct Owner
        {
            VirtualRanges* in = nullptr;
            uintptr_t base = 0;
            bool owed = false;
        };

        Owner owner_from(VirtualRanges* in, VirtualRange const* e)
        {
            Owner o;
            if (e != nullptr)
            {
                o.in = in;
                o.base = e->base;
                o.owed = (e->flags & VR_SYNC_OWED) != 0;
            }
            return o;
        }

        Owner owner_of(arch_phys_addr_t pa)
        {
            VirtualRanges* in = nullptr;
            VirtualRange const* const e = reservation_at(pa, &in);
            return owner_from(in, e);
        }

        // `e` of `ranges`, which is the reservation itself unless the space borrowed it.
        Owner owner_for(VirtualRanges* ranges, VirtualRange const* e)
        {
            if ((e->flags & VR_BORROWED) != 0)
            {
                return owner_of(aspace_frame_of(e->base));
            }
            return owner_from(ranges, e);
        }
#else
        struct Owner
        {
            bool owed = false;
        };

        Owner owner_of(arch_phys_addr_t)
        {
            return Owner{};
        }

        Owner owner_for(VirtualRanges*, VirtualRange const*)
        {
            return Owner{};
        }
#endif

        // After a mapping of `type` over the owner's frames is installed: a cacheable one was
        // synced or owed nothing, and any other leaves the next cacheable one a sync.
        void owner_mapped(Owner const& o, enum arch_map_memtype type)
        {
#if KICKOS_ARCH_ALIAS_DCACHE
            if (o.in != nullptr)
            {
                (void)o.in->set_sync_owed(o.base, not cacheable(type));
            }
#else
            (void)o;
            (void)type;
#endif
        }
    }

#if KICKOS_ARCH_ALIAS_DCACHE
    void aspace_self_grant_note(VirtualRanges const* ranges, uintptr_t base, size_t size,
                                uint32_t rights, enum arch_map_memtype type)
    {
        VirtualRange const* e = nullptr;
        uint8_t const memtype = static_cast<uint8_t>(type);
        if (ranges != nullptr
            and self_grant_admit(ranges, base, size, rights, memtype, &e) == 0
            and (aspace_grant_syncs(e, memtype) or owner_of(aspace_frame_of(e->base)).owed))
        {
            presync_note(aspace_frame_of(e->base), e->pages);
        }
    }

    void aspace_cap_map_note(int run_obj, arch_phys_addr_t base, uint32_t pages,
                             enum arch_map_memtype type)
    {
        if ((not cacheable(type) or frame_run_sync_owed(run_obj))
            and aspace_frames_type_ok(base, pages, static_cast<uint8_t>(type), nullptr))
        {
            presync_note(base, pages);
        }
    }

    void aspace_reservation_note(VirtualRanges const* own, uintptr_t base, size_t bytes,
                                 enum arch_map_memtype type)
    {
        VirtualRange const* const e = whole_reservation(own, base, bytes);
        if (e != nullptr
            and (aspace_grant_syncs(e, static_cast<uint8_t>(type))
                 or owner_of(aspace_frame_of(base)).owed)
            and aspace_frames_type_ok(aspace_frame_of(base), e->pages,
                                      static_cast<uint8_t>(type), nullptr))
        {
            presync_note(aspace_frame_of(base), e->pages);
        }
    }
#endif

    int aspace_self_grant(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t base,
                          size_t size, uint32_t rights, enum arch_map_memtype type)
    {
        if (space == nullptr or ranges == nullptr)
        {
            return -KOS_EPERM;
        }
        uint8_t const memtype = static_cast<uint8_t>(type);
        VirtualRange const* e = nullptr;
        int const admit = self_grant_admit(ranges, base, size, rights, memtype, &e);
        if (admit != 0)
        {
            if (admit > 0)
            {
                return 0;
            }
            return admit;
        }
        uintptr_t const b = e->base;
        size_t const pages = e->pages;
        Owner const owner = owner_for(ranges, e);
        bool const syncs = aspace_grant_syncs(e, memtype) or owner.owed;
        if (syncs and not sync_frames(aspace_frame_of(b), pages))
        {
            return -KOS_ENOMEM;
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
        edited(aspace_frame_of(b), pages);
        owner_mapped(owner, type);
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
        // Another space mapping the run with another type keeps it.
        if (not aspace_frames_type_ok(base, pages, static_cast<uint8_t>(type), nullptr))
        {
            return -KOS_EBUSY;
        }
        // A leaf is a holder: the frames come back at the last one, not the last capability.
        if (not frame_run_ref(run_obj))
        {
            return -KOS_ENOMEM;
        }
        int const run_slot = frame_run_slot_of(run_obj);
        if (not ranges->reserve(va, pages, VR_BORROWED | VR_FRAMECAP,
                                static_cast<uint32_t>(run_slot) + 1u))
        {
            frame_run_release(run_obj);
            return -KOS_ENOMEM; // overlaps something this space already names, or the list is full
        }
        bool const syncs = not cacheable(type) or frame_run_sync_owed(run_obj);
        if ((syncs and not sync_frames(base, pages))
            or arch_aspace_map(space, va, base, pages, rights, type) != ARCH_ASPACE_OK)
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
        edited(base, pages);
        frame_run_set_sync_owed(run_obj, not cacheable(type));
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
        // The range must be one aspace_cap_map placed and must name this run: a page count
        // alone accepts the image and every handoff, which carry VR_BORROWED too.
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
        Owner owner;
        if (donor != nullptr)
        {
            owner = owner_of(pa);
        }
        bool const syncs = type == ARCH_MAP_NOCACHE or owner.owed;
        if ((syncs and not sync_frames(pa, pages))
            or arch_aspace_map(space, va, pa, pages, rights, type) != ARCH_ASPACE_OK)
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
        edited(pa, pages);
        if (donor != nullptr)
        {
            owner_mapped(owner, type);
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

    int aspace_handoff_admit(VirtualRanges const* donor, uintptr_t base, size_t size)
    {
        if (donor == nullptr)
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
        // find() answers for a contained subrange, and what is mapped is the whole entry, so
        // the reservation's own base and its own page count are required.
        if (base != e->base or (size + g - 1u) / g != static_cast<size_t>(e->pages))
        {
            return -KOS_EPERM;
        }
        return 0;
    }

    int aspace_handoff(VirtualRanges const* donor, struct arch_aspace* space,
                       VirtualRanges* ranges, uintptr_t base, size_t size,
                       enum arch_map_memtype type)
    {
        int const arc = aspace_handoff_admit(donor, base, size);
        if (arc != 0 or space == nullptr or ranges == nullptr)
        {
            return -KOS_EPERM;
        }
        size_t const g = arch_aspace_granule();
        uintptr_t const b = base;
        size_t const pages = (size + g - 1u) / g;
        uint32_t const rights = ARCH_MAP_R | ARCH_MAP_W;
        // The donor, a window or another handoff mapping these frames with another type keeps
        // them.
        if (not aspace_frames_type_ok(aspace_frame_of(b), pages, static_cast<uint8_t>(type),
                                      nullptr))
        {
            return -KOS_EBUSY;
        }
        Owner const owner = owner_of(aspace_frame_of(b));
        bool const syncs = type == ARCH_MAP_NOCACHE or owner.owed;
        // The borrower takes the donor's address; reserve refuses an overlap.
        if (not ranges->reserve(b, pages, VR_BORROWED))
        {
            return -KOS_ENOMEM;
        }
        if ((syncs and not sync_frames(aspace_frame_of(b), pages))
            or arch_aspace_map(space, b, aspace_frame_of(b), pages, rights, type)
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
        edited(aspace_frame_of(b), pages);
        owner_mapped(owner, type);
        return 0;
    }

    // Residency a backend records here is read under the kernel lock, so it is written under
    // it too.
    static void activate_locked(struct arch_aspace* space)
    {
#if KICKOS_KERNEL_CORES > 1
        KICKOS_DEBUG_ASSERT(arch_kernel_lock_held() != 0);
#endif
        arch_aspace_activate(space);
    }

    static void seat(struct arch_aspace* space)
    {
        uint32_t const cpu = arch_cpu_id();
        if (g_current[cpu] == space)
        {
            return;
        }
        g_current[cpu] = space;
        activate_locked(space);
    }

    // The snapshot is filled while root's static data still exists; a failure leaves it unfilled.
    static void data_home_leave()
    {
        size_t const g = arch_aspace_granule();
        (void)data_template_fill(image_data(g), g);
        g_data_home = nullptr;
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
                activate_locked(arch_aspace_boot());
            }
        }
        if (g_data_home == space)
        {
            data_home_leave();
        }
        size_t const g = arch_aspace_granule();
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
        struct arch_aspace* const space = domain_space(thread_domain(t));
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
        seat(space);
        return space;
    }

    bool aspace_seated_for(Thread const* t)
    {
        struct arch_aspace* const space = domain_space(thread_domain(t));
        if (space == nullptr)
        {
            return false;
        }
        uint32_t const cpu = arch_cpu_id();
        return space == g_current[cpu];
    }

    void aspace_install_boot(void)
    {
        seat(arch_aspace_boot());
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

    uint64_t aspace_locked_pages(void)
    {
        IrqLock lock;
        return g_locked_pages;
    }

    void aspace_data_home_forget(void)
    {
        IrqLock lock;
        data_home_leave();
    }
#endif
}

#endif
