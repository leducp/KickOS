// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/aspace.h>

#include "frame_pool_unwritten.h"

#if KICKOS_HAVE_ASPACE

#include <kickos/ampshare.h>
#include <kickos/debug.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/grant.h>
#include <kickos/image.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/klink.h>
#include <kickos/kruntime.h>
#include <kickos/sched.h>
#include <kickos/start_watch.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>
#include <kickos/sys/init.h>

extern "C"
{
    // Per image, in image order, the spans every space of it maps and where the loader put
    // their bytes (tools/image-link.py): rows of eight words, rom_lo, rom_hi, rom_delta,
    // sram_lo, sram_hi, sram_delta, tls_lo, tls_hi. A delta plus the anchor's address, added to
    // a program address, names the frame holding that byte.
    extern unsigned char __kickos_image_frames[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_image_frames_end[] KICKOS_LINK_BOUND;
    extern unsigned char __kickos_image_frames_anchor[] KICKOS_LINK_BOUND;
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

#if KICKOS_PRESYNC
        PresyncRecord* presync_of(Thread const* t)
        {
            int const i = kernel().threads.index_of(t);
            if (i < 0)
            {
                return nullptr;
            }
            return &kernel().presync()[i];
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
            for (PresyncRecord& r : k.presync())
            {
                if (&r != mine and presync_dropped_by(r, pa, pages, g))
                {
                    presync_drop(r);
                }
            }
        }
#endif

        // The declaration record an IMAGE stage of `r`'s is for, or null.
        Task* image_stage_for(PresyncRecord const& r)
        {
            if (r.stage != PresyncStage::IMAGE or r.staged_decl == 0)
            {
                return nullptr;
            }
            return task_decl_record(r.staged_decl - 1u);
        }

        ThreadLink presync_slot_link(PresyncRecord const& r)
        {
            return static_cast<ThreadLink>(&r - &kernel().presync()[0] + 1);
        }

        // Whether this dropped a declaration's stager mark, which may have been the last thing in
        // the way of a create of it.
        bool presync_unstage(PresyncRecord& r)
        {
            if (r.staged != 0)
            {
                frame_pool_free_run(r.staged, r.staged_pages);
            }
            ThreadLink* const stager = task_image_stager(image_stage_for(r));
            bool const marked = stager != nullptr and *stager == presync_slot_link(r);
            if (marked)
            {
                *stager = 0;
            }
            r.staged = 0;
            r.staged_pages = 0;
            r.stage = PresyncStage::NONE;
            r.staged_decl = 0;
            r.staged_full = false;
            return marked;
        }

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
            bool const kept = r != nullptr and r->active and presync_add(r->edits, pa, pages);
            KICKOS_ASSERT(kept);
#else
            (void)pa;
            (void)pages;
#endif
        }

        // volatile keeps each a relocated word; a plain constant folds back into every reader.
        unsigned char* const volatile g_frames_lo = __kickos_image_frames;
        unsigned char* const volatile g_frames_hi = __kickos_image_frames_end;
        unsigned char* const volatile g_frames_anchor = __kickos_image_frames_anchor;

        struct FramesRow
        {
            uintptr_t rom_lo;
            uintptr_t rom_hi;
            uintptr_t rom_delta;
            uintptr_t sram_lo;
            uintptr_t sram_hi;
            uintptr_t sram_delta;
            uintptr_t tls_lo;
            uintptr_t tls_hi;
        };

        size_t frames_count()
        {
            return static_cast<size_t>(reinterpret_cast<uintptr_t>(g_frames_hi)
                                       - reinterpret_cast<uintptr_t>(g_frames_lo))
                   / sizeof(FramesRow);
        }

        constexpr FramesRow NO_FRAMES = {};

        // An image past the table spans nothing.
        FramesRow const& row_of(size_t image)
        {
            if (image >= frames_count())
            {
                return NO_FRAMES;
            }
            return reinterpret_cast<FramesRow const*>(g_frames_lo)[image];
        }

        // The frame the loader placed byte `va` of image `image` in: every space maps the image
        // at the addresses it was linked at.
        arch_phys_addr_t app_pa(FramesRow const& r, uintptr_t va)
        {
            uintptr_t delta = r.rom_delta;
            if (va >= r.sram_lo and va < r.sram_hi)
            {
                delta = r.sram_delta;
            }
            return static_cast<arch_phys_addr_t>(
                va + delta + reinterpret_cast<uintptr_t>(g_frames_anchor));
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

        Extent extent_of(uintptr_t a, uintptr_t b, size_t g)
        {
            if (b <= a)
            {
                return Extent{0, 0};
            }
            uintptr_t const base = page_down(a, g);
            return Extent{base, static_cast<size_t>((page_up(b, g) - base) / g)};
        }

        Extent image_text(FramesRow const& r, size_t g)
        {
            return extent_of(r.rom_lo, r.rom_hi, g);
        }

        Extent image_data(FramesRow const& r, size_t g)
        {
            return extent_of(r.sram_lo, r.sram_hi, g);
        }

        bool in_app_image(FramesRow const& r, uintptr_t p)
        {
            return (p >= r.rom_lo and p < r.rom_hi) or (p >= r.sram_lo and p < r.sram_hi);
        }

        // The frames the loader put image `image`'s data window in: the pristine copy every
        // space of it copies its own from, which no space maps and nothing writes after boot.
        arch_phys_addr_t data_template(size_t image, size_t g)
        {
            FramesRow const& r = row_of(image);
            return app_pa(r, image_data(r, g).base);
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

        bool admit_pages(Task const* payer, size_t pages)
        {
            return pages <= UINT32_MAX
                   and task_admit(payer, BudgetKind::PAGES, static_cast<uint32_t>(pages));
        }

        // Copies granule `i` of the pristine image at `tmpl` into the run at `to`; false when the
        // run is gone. Both ends are cacheable (an image range refuses a memory-type change), so
        // no alias sync is owed. Out of line: inlined, it widens the seed's frame on the spawn
        // chain.
        __attribute__((noinline)) bool copy_data_granule(arch_phys_addr_t to,
                                                         arch_phys_addr_t tmpl, size_t i,
                                                         size_t g)
        {
            arch_phys_addr_t const step = static_cast<arch_phys_addr_t>(i * g);
            void* const dst = frame_pool_ptr(to + step);
            if (dst == nullptr)
            {
                return false;
            }
            kmemcpy(dst, frame_pool_alias(tmpl + step), g);
            return true;
        }

        // Maps the written `run` as `data`, whose range is claimed; frees both on a refusal.
        // Inlined: a frame of its own deepens the spawn's chain on the kernel block.
        __attribute__((always_inline)) inline int data_map(Task const* payer,
                                                           struct arch_aspace* space,
                                                           VirtualRanges* ranges,
                                                           Extent const& data,
                                                           arch_phys_addr_t run)
        {
            int rc = 0;
            if (not aspace_tables_admit(payer, space, data.base, data.pages))
            {
                rc = -KOS_EAGAIN;
            }
            else if (arch_aspace_map(space, data.base, run, data.pages, ARCH_MAP_R | ARCH_MAP_W,
                                     ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                rc = -KOS_ENOMEM;
            }
            if (rc != 0)
            {
                frame_pool_free_run(run, data.pages);
                (void)ranges->release(data.base);
                return rc;
            }
            // The run is this space's alone; destroy frees it.
            commit(ranges, data, ARCH_MAP_R | ARCH_MAP_W);
            return 0;
        }

        // Copies image `image`'s pristine data into a new space, under IrqLock.
        int data_copy(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                      Extent const& data, size_t image, size_t g)
        {
#if KICKOS_PRESYNC
            // A system call copies ahead of its lock (presync_stage_image), its stage already
            // charged to `payer`, and goes round again when its stage is not a whole copy.
            PresyncRecord* const r = presync_record();
            if (r != nullptr and r->active and r->staged_short)
            {
                if (not admit_pages(payer, data.pages))
                {
                    return -KOS_EAGAIN;
                }
                // Short at the plan, admissible now: a pool holding the run now sends the call
                // round, and only one that still has none answers.
                arch_phys_addr_t const run = AspaceUnwritten::alloc_run(data.pages);
                if (run != 0)
                {
                    frame_pool_free_run(run, data.pages);
                    r->refused = true;
                }
                return -KOS_ENOMEM;
            }
            if (r != nullptr and r->active)
            {
                if (r->stage != PresyncStage::IMAGE or not r->staged_full
                    or r->staged_pages != data.pages or r->staged_image != image
                    or image_stage_for(*r) != payer)
                {
                    r->refused = true;
                    return -KOS_ENOMEM;
                }
                if (not claim(ranges, data, VR_IMAGE))
                {
                    return -KOS_ENOMEM;
                }
                arch_phys_addr_t const staged = r->staged;
                r->staged = 0;
                if (presync_unstage(*r))
                {
                    r->stage_taken = true;
                }
                return data_map(payer, space, ranges, data, staged);
            }
#endif
            if (not admit_pages(payer, data.pages))
            {
                return -KOS_EAGAIN;
            }
            if (not claim(ranges, data, VR_IMAGE))
            {
                return -KOS_ENOMEM;
            }
            // Uncleared: the loop below writes every byte of every page.
            arch_phys_addr_t const run = AspaceUnwritten::alloc_run(data.pages);
            if (run == 0)
            {
                (void)ranges->release(data.base);
                return -KOS_ENOMEM;
            }
            arch_phys_addr_t const tmpl = data_template(image, g);
            for (size_t i = 0; i < data.pages; i++)
            {
                if (not copy_data_granule(run, tmpl, i, g))
                {
                    frame_pool_free_run(run, data.pages);
                    (void)ranges->release(data.base);
                    return -KOS_ENOMEM;
                }
            }
            return data_map(payer, space, ranges, data, run);
        }
    }

    namespace
    {
#if KICKOS_PRESYNC
        // Writes granule `j` of `r`'s staged run, cleared or copied from the pristine image;
        // false when the run's frame has gone. Takes IrqLock for the one granule.
        bool stage_granule(PresyncRecord const& r, uint32_t j, size_t g)
        {
            IrqLock lock;
            if (r.stage == PresyncStage::CLEAR or r.stage == PresyncStage::RESERVE)
            {
                void* const dst = frame_pool_ptr(r.staged + static_cast<arch_phys_addr_t>(j) * g);
                if (dst == nullptr)
                {
                    return false;
                }
                kmemset(dst, 0, g);
                return true;
            }
            return copy_data_granule(r.staged, data_template(r.staged_image, g), j, g);
        }
#endif
    }

    int aspace_image_seed(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                          size_t image)
    {
        size_t const g = arch_aspace_granule();
        if (not ranges->init(g))
        {
            return -KOS_ENOMEM;
        }
        FramesRow const& r = row_of(image);
        Extent const text = image_text(r, g);
        if (text.pages != 0)
        {
            // The frames the loader put the app's text in, so one physical page carries that
            // text in every space. VR_BORROWED: aspace_release unmaps them and frees none.
            if (not claim(ranges, text, VR_IMAGE | VR_BORROWED))
            {
                return -KOS_ENOMEM;
            }
            int rc = 0;
            if (not aspace_tables_admit(payer, space, text.base, text.pages))
            {
                rc = -KOS_EAGAIN;
            }
            else if (arch_aspace_map(space, text.base, app_pa(r, text.base),
                                     text.pages, ARCH_MAP_R | ARCH_MAP_X,
                                     ARCH_MAP_NORMAL) != ARCH_ASPACE_OK)
            {
                rc = -KOS_ENOMEM;
            }
            if (rc != 0)
            {
                (void)ranges->release(text.base);
                return rc;
            }
            commit(ranges, text, ARCH_MAP_R | ARCH_MAP_X);
        }
        Extent const data = image_data(r, g);
        if (data.pages == 0)
        {
            return 0;
        }
        return data_copy(payer, space, ranges, data, image, g);
    }

    bool aspace_tables_admit(Task const* payer, struct arch_aspace* space, uintptr_t va,
                             size_t pages)
    {
        return admit_pages(payer, arch_aspace_map_tables(space, va, pages));
    }

    uint32_t aspace_task_usage(Domain const* d, BudgetKind kind)
    {
        VirtualRanges const* const ranges = domain_ranges(d);
        if (ranges == nullptr)
        {
            return 0;
        }
        size_t const g = arch_aspace_granule();
        // A plain reservation is kos_ram_alloc's; a range of frames another space or a frame
        // run owns is neither a reservation nor this space's page.
        uint8_t const foreign = static_cast<uint8_t>(VR_BORROWED | VR_FRAMECAP | VR_SHARE
                                                     | VR_WINDOW);
        uint32_t used = 0;
        for (size_t i = 0; i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e == nullptr)
            {
                continue;
            }
            bool const plain = (e->flags & static_cast<uint8_t>(~VR_SYNC_OWED)) == 0;
            if (kind == BudgetKind::RAM_BLOCKS and plain)
            {
                used++;
            }
            else if (kind == BudgetKind::RAM_BYTES and plain)
            {
                used = used + static_cast<uint32_t>(e->pages * g);
            }
            else if (kind == BudgetKind::PAGES and (e->flags & foreign) == 0)
            {
                used = used + e->pages;
            }
        }
        if (kind == BudgetKind::PAGES)
        {
            used = used + static_cast<uint32_t>(arch_aspace_table_pages(domain_space(d)));
        }
        return used;
    }

    void (*aspace_task_start(size_t image))(void*)
    {
        return reinterpret_cast<void (*)(void*)>(image_record(image).task_start);
    }

    size_t aspace_image_frames_count()
    {
        return frames_count();
    }

    void aspace_image_spans(size_t image, ImageSpan* code, ImageSpan* ram, ImageSpan* tls)
    {
        FramesRow const& r = row_of(image);
        *code = ImageSpan{r.rom_lo, r.rom_hi};
        *ram = ImageSpan{r.sram_lo, r.sram_hi};
        *tls = ImageSpan{r.tls_lo, r.tls_hi};
    }

    void* aspace_image_alias(size_t image, void const* app_ptr)
    {
        FramesRow const& r = row_of(image);
        uintptr_t const va = reinterpret_cast<uintptr_t>(app_ptr);
        // Only one byte is tested; the signature carries no length.
        if (not in_app_image(r, va))
        {
            return nullptr;
        }
        return frame_pool_alias(app_pa(r, va));
    }

    namespace
    {
        // A window or a capability run sits where the kernel chose; everything else a space
        // maps sits at its frames' own address.
        arch_phys_addr_t range_first_frame(struct arch_aspace* space, VirtualRange const* e)
        {
            if ((e->flags & (VR_WINDOW | VR_FRAMECAP)) != 0)
            {
                return arch_aspace_frame_at(space, e->base);
            }
            return aspace_frame_of(e->base);
        }
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
        for (int d = 0; d < kernel().domains().size(); d++)
        {
            Domain const* const dom = &kernel().domains()[d];
            struct arch_aspace* const space = domain_space(dom);
            VirtualRanges const* const ranges = domain_ranges(dom);
            for (size_t i = 0; space != nullptr and ranges != nullptr
                               and i < VirtualRanges::capacity();
                 i++)
            {
                VirtualRange const* const e = ranges->at(i);
                if (e == nullptr or e == self or (e->flags & VR_IMAGE) != 0)
                {
                    continue;
                }
                // A range under an edit holds the edit's type beside the one it was granted.
                bool clash = vr_pending(e) and e->pending != static_cast<uint8_t>(memtype + 1u);
                if (e->state == VirtualState::Granted and e->memtype != memtype)
                {
                    clash = true;
                }
                if (not clash)
                {
                    continue;
                }
                arch_phys_addr_t const lo = range_first_frame(space, e);
                arch_phys_addr_t const hi = lo + static_cast<arch_phys_addr_t>(e->pages) * g;
                if (lo < end and pa < hi)
                {
                    return false;
                }
            }
        }
        return true;
    }

    bool aspace_maps_writable(struct arch_aspace* space, VirtualRanges const* ranges,
                              arch_phys_addr_t pa)
    {
        size_t const g = arch_aspace_granule();
        for (size_t i = 0; space != nullptr and ranges != nullptr and i < VirtualRanges::capacity();
             i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e == nullptr or (e->flags & VR_IMAGE) != 0 or e->state != VirtualState::Granted
                or (e->rights & ARCH_MAP_W) == 0)
            {
                continue;
            }
            arch_phys_addr_t const lo = range_first_frame(space, e);
            arch_phys_addr_t const hi = lo + static_cast<arch_phys_addr_t>(e->pages) * g;
            if (lo != 0 and pa >= lo and pa < hi)
            {
                return true;
            }
        }
        return false;
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
        if (presync_unstage(*r))
        {
            start_watch_poke();
        }
        r->noted.count = 0;
        r->edits.count = 0;
        r->refused = false;
        r->staged_short = false;
        r->active = true;
        if (entering)
        {
            r->has_params = false;
            r->has_windows = false;
            r->has_task_params = false;
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

    void presync_stage_image(size_t decl)
    {
        PresyncRecord* const r = presync_record();
        Task* const t = task_decl_record(decl);
        size_t const image = image_of_decl(decl);
        size_t const g = arch_aspace_granule();
        Extent const data = image_data(row_of(image), g);
        ThreadLink* const stager = task_image_stager(t);
        if (r == nullptr or stager == nullptr or data.pages == 0)
        {
            return;
        }
        presync_unstage(*r);
        // The copy is the declaration's from before its frames are taken; past its budget, or
        // another create's, the locked pass refuses rather than copying under the lock.
        if (*stager != 0 or not admit_pages(t, data.pages))
        {
            r->staged_short = true;
            return;
        }
        // Unwritten: presync_run writes every byte of every page before anything maps it.
        arch_phys_addr_t const run = AspaceUnwritten::alloc_run(data.pages);
        if (run == 0)
        {
            r->staged_short = true;
            return;
        }
        r->staged = run;
        r->staged_pages = static_cast<uint32_t>(data.pages);
        r->staged_image = static_cast<uint8_t>(image);
        r->staged_decl = static_cast<uint8_t>(decl + 1u);
        r->stage = PresyncStage::IMAGE;
        *stager = presync_slot_link(*r);
    }

    bool presync_image_staged_elsewhere(Task const* t, Thread const* asker)
    {
        ThreadLink const* const stager = task_image_stager(t);
        PresyncRecord const* const own = presync_of(asker);
        return stager != nullptr and *stager != 0
               and (own == nullptr or *stager != presync_slot_link(*own));
    }

    arch_phys_addr_t aspace_reserve_stage(VirtualRanges const* ranges, size_t bytes,
                                          PresyncStage stage)
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
        r->stage = stage;
        return run;
    }

    uintptr_t aspace_reserve_commit(VirtualRanges* ranges)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or r->stage != PresyncStage::RESERVE or not r->staged_full
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

    bool aspace_reserve_run(int run_obj)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or r->stage != PresyncStage::CLEAR or not r->staged_full
            or not frame_run_fill(run_obj, r->staged, r->staged_pages))
        {
            return false;
        }
        r->staged = 0;
        presync_unstage(*r);
        return true;
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
                arch_irq_window();
            }
        }
#endif
        bool full = r->stage != PresyncStage::NONE;
        for (uint32_t j = 0; full and j < r->staged_pages; j++)
        {
            full = stage_granule(*r, j, g);
            arch_irq_window();
        }
        r->staged_full = full;
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
        if (r == nullptr)
        {
            return;
        }
        if (r->stage_taken)
        {
            IrqLock lock;
            r->stage_taken = false;
            if (not succeeded)
            {
                start_watch_poke();
            }
        }
        if (r->staged == 0)
        {
            return;
        }
        // A slay at a window below would drop what a succeeded call delivered.
        KICKOS_ASSERT(not succeeded);
        size_t const g = arch_aspace_granule();
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
                frame_pool_free_run(r->staged, 1);
                r->staged += static_cast<arch_phys_addr_t>(g);
                r->staged_pages--;
            }
            arch_irq_window();
        }
        IrqLock lock;
        r->staged = 0;
        if (presync_unstage(*r))
        {
            start_watch_poke();
        }
    }

    uint32_t presync_task_staged(Task const* t, BudgetKind kind)
    {
        Kernel& k = kernel();
        uint32_t staged = 0;
        for (ListNode* m = task_members(t); m != nullptr; m = m->next)
        {
            Thread const* const th = KICKOS_CONTAINER_OF(m, Thread, member);
            PresyncRecord const& r = k.presync()[static_cast<int>(th - k.threads.slots())];
            if (r.staged == 0)
            {
                continue;
            }
            bool const reserve = r.stage == PresyncStage::RESERVE;
            if (kind == BudgetKind::PAGES and (reserve or r.stage == PresyncStage::CLEAR))
            {
                staged = staged + r.staged_pages;
            }
            else if (kind == BudgetKind::RAM_BLOCKS and reserve)
            {
                staged++;
            }
            else if (kind == BudgetKind::RAM_BYTES and reserve)
            {
                staged = staged
                         + static_cast<uint32_t>(r.staged_pages * arch_aspace_granule());
            }
        }
        ThreadLink const* const stager = task_image_stager(t);
        if (kind == BudgetKind::PAGES and stager != nullptr and *stager != 0)
        {
            PresyncRecord const& r = k.presync()[*stager - 1u];
            if (image_stage_for(r) == t)
            {
                staged = staged + r.staged_pages;
            }
        }
        return staged;
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

    namespace
    {
        arch_phys_addr_t* chained(arch_phys_addr_t frame)
        {
            return static_cast<arch_phys_addr_t*>(frame_pool_alias(frame));
        }

        // The reserve's charge moves to `payer`: false, keeping it where it was, where `payer`
        // cannot take it.
        bool tables_recharge(PresyncRecord* r, Task const* payer)
        {
            uint16_t gen = 0;
            if (payer != nullptr)
            {
                gen = payer->gen;
            }
            if (r->table_payer == payer and r->table_payer_gen == gen)
            {
                return true;
            }
            task_tables_reserved(r->table_payer, r->table_payer_gen, r->table_count, false);
            if (not admit_pages(payer, r->table_count))
            {
                task_tables_reserved(r->table_payer, r->table_payer_gen, r->table_count, true);
                return false;
            }
            r->table_payer = payer;
            r->table_payer_gen = gen;
            task_tables_reserved(payer, gen, r->table_count, true);
            return true;
        }
    }

    int presync_tables_take_pass(Task const* payer, size_t n)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or r->table_count >= n)
        {
            return 0;
        }
        // Disarmed first: kickos_frame_alloc would otherwise hand the reserve back to itself.
        r->tables_armed = false;
        if (not tables_recharge(r, payer))
        {
            return -KOS_EAGAIN;
        }
        size_t k = n - r->table_count;
        if (k > ASPACE_TABLE_PASS_FRAMES)
        {
            k = ASPACE_TABLE_PASS_FRAMES;
        }
        if (not admit_pages(payer, k))
        {
            return -KOS_EAGAIN;
        }
        for (size_t i = 0; i < k; i++)
        {
            arch_phys_addr_t const f = kickos_frame_alloc();
            if (f == 0)
            {
                return -KOS_ENOMEM;
            }
            *chained(f) = r->tables;
            r->tables = f;
            r->table_count++;
            task_tables_reserved(payer, r->table_payer_gen, 1u, true);
        }
        if (r->table_count < n)
        {
            return 1;
        }
        return 0;
    }

    int presync_tables_arm(Task const* payer, size_t n)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or n == 0)
        {
            return 0;
        }
        if (r->table_count < n)
        {
            if (r->table_refusal != 0)
            {
                return r->table_refusal;
            }
            if (not admit_pages(payer, n - r->table_count))
            {
                return -KOS_EAGAIN;
            }
            return -KOS_ENOMEM;
        }
        if (not tables_recharge(r, payer))
        {
            return -KOS_EAGAIN;
        }
        r->tables_armed = true;
        return 0;
    }

    bool presync_tables_held()
    {
        PresyncRecord const* const r = presync_record();
        return r != nullptr and r->tables_armed and r->table_count != 0;
    }

    bool presync_tables_release_pass()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr)
        {
            return false;
        }
        r->tables_armed = false;
        r->table_refusal = 0;
        size_t k = r->table_count;
        if (k > ASPACE_TABLE_PASS_FRAMES)
        {
            k = ASPACE_TABLE_PASS_FRAMES;
        }
        task_tables_reserved(r->table_payer, r->table_payer_gen, static_cast<uint32_t>(k), false);
        for (size_t i = 0; i < k; i++)
        {
            arch_phys_addr_t const f = r->tables;
            r->tables = *chained(f);
            kickos_frame_free(f);
        }
        r->table_count = r->table_count - static_cast<uint32_t>(k);
        if (r->table_count != 0)
        {
            return true;
        }
        r->tables = 0;
        r->table_payer = nullptr;
        r->table_payer_gen = 0;
        return false;
    }

    bool presync_map_unwind_pass(Held held)
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr)
        {
            return false;
        }
        if (r->table_count != 0 and presync_tables_release_pass())
        {
            return true;
        }
        if (r->map_into == nullptr)
        {
            return false;
        }
        Domain* const d = r->map_into;
        uint16_t const holder =
            static_cast<uint16_t>(kernel().threads.index_of(sched::current()) + 1);
        MapPass pass;
        aspace_cap_map_unwind(domain_space(d), domain_ranges_mut(d), r->map_at, holder, &pass);
        if (pass.bytes != 0)
        {
            word_reach_lost(domain_space(d), pass.base, pass.bytes, held);
        }
        if (pass.done)
        {
            r->map_into = nullptr;
            r->map_at = 0;
        }
        return true;
    }

    bool presync_map_retract()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr or r->map_into == nullptr)
        {
            return false;
        }
        uint16_t const holder =
            static_cast<uint16_t>(kernel().threads.index_of(sched::current()) + 1);
        VirtualRanges* const ranges = domain_ranges_mut(r->map_into);
        VirtualRange const* e = nullptr;
        if (ranges != nullptr)
        {
            e = ranges->at_base(r->map_at);
        }
        if (e == nullptr or e->base != r->map_at or (e->flags & VR_FRAMECAP) == 0 or vr_pending(e)
            or e->state != VirtualState::Granted or e->holder != holder)
        {
            r->map_into = nullptr;
            r->map_at = 0;
            return false;
        }
        bool const noted = ranges->set_pending(r->map_at, VR_UNWIND_MEMTYPE, e->pages);
        KICKOS_ASSERT(noted);
        return true;
    }

    void presync_exit()
    {
        PresyncRecord* const r = presync_record();
        if (r == nullptr)
        {
            return;
        }
        presync_drop(*r);
        if (presync_unstage(*r) or r->stage_taken)
        {
            start_watch_poke();
        }
        r->stage_taken = false;
        r->noted.count = 0;
        r->edits.count = 0;
        r->refused = false;
        r->staged_short = false;
        r->active = false;
        r->has_params = false;
        r->has_windows = false;
    }
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
            // Ahead of the already-mapped answer, which would otherwise admit a mapping no
            // caller reserved. A stack run's base is its guard, so an admitted grant would map
            // that page too.
            if (not vr_caller_nameable(e))
            {
                return -KOS_EPERM;
            }
            // A grant under way is resumed by one asking for its type, whoever began it.
            if (vr_pending(e))
            {
                if (e->pending != static_cast<uint8_t>(memtype + 1u))
                {
                    return -KOS_EBUSY;
                }
            }
            else if (e->state == VirtualState::Granted and (e->rights & rights) == rights
                     and e->memtype == memtype)
            {
                return 1;
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
            for (int d = 0; d < kernel().domains().size(); d++)
            {
                Domain* const dom = &kernel().domains()[d];
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

    namespace
    {
        // The pages one pass takes of `left`.
        size_t pass_pages(size_t left)
        {
            if (left > ASPACE_MAP_PASS_PAGES)
            {
                return ASPACE_MAP_PASS_PAGES;
            }
            return left;
        }

        // One pass of the map under way over `e`, whose frames start at `pa`: its next pages
        // mapped, and on its last page the range granted. -KOS_EAGAIN or -KOS_ENOMEM with
        // nothing more mapped.
        int map_pass(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                     VirtualRange const* e, arch_phys_addr_t pa, uint32_t rights,
                     enum arch_map_memtype type, MapPass* pass)
        {
            // A right no range holds, refused before a leaf goes in.
            if (rights == 0 or rights != static_cast<uint32_t>(static_cast<uint8_t>(rights)))
            {
                return -KOS_ENOMEM;
            }
            size_t const g = arch_aspace_granule();
            uintptr_t const b = e->base;
            size_t const pages = e->pages;
            size_t const from = e->pending_pages;
            size_t const n = pass_pages(pages - from);
            uintptr_t const at = b + from * g;
            if (not presync_tables_held() and not aspace_tables_admit(payer, space, at, n))
            {
                return -KOS_EAGAIN;
            }
            if (arch_aspace_map(space, at, pa + static_cast<arch_phys_addr_t>(from * g), n,
                                rights, type)
                != ARCH_ASPACE_OK)
            {
                return -KOS_ENOMEM;
            }
            pass->base = at;
            pass->bytes = n * g;
            uint8_t const memtype = static_cast<uint8_t>(type);
            if (from + n < pages)
            {
                bool const noted = ranges->set_pending(b, memtype, static_cast<uint32_t>(from + n));
                KICKOS_ASSERT(noted);
                return 0;
            }
            bool const granted = ranges->grant(b, pages, rights, memtype);
            KICKOS_ASSERT(granted);
            pass->done = true;
            return 0;
        }

        // The pages a create's locked pass maps of a run before it leaves the rest to passes:
        // one at least, so the range names its frames, and at most what `budget` has left.
        size_t first_pages(size_t pages, size_t* budget)
        {
            size_t n = *budget;
            if (n == 0)
            {
                n = 1;
            }
            if (n > pages)
            {
                n = pages;
            }
            if (*budget > n)
            {
                *budget = *budget - n;
            }
            else
            {
                *budget = 0;
            }
            return n;
        }

        // The shed domain whose reservation at `base` a mapping in another space is the last to
        // borrow, the frames above its first still to free; null otherwise.
        Domain* last_lender_shed(uintptr_t base)
        {
            for (int d = 0; d < kernel().domains().size(); d++)
            {
                Domain* const dom = &kernel().domains()[d];
                if (domain_space(dom) != nullptr or dom->immortal)
                {
                    continue;
                }
                // A shed domain's list, which domain_ranges does not answer.
                VirtualRange const* const e = dom->ranges.at_base(base);
                if (aspace_range_lent(e) and e->lends == 1 and e->pages > 1)
                {
                    return dom;
                }
            }
            return nullptr;
        }

        // One pass freeing the frames of `lender`'s reservation at `base` above its first, which
        // the last borrower's release frees.
        void shed_reap_pass(Domain* lender, uintptr_t base)
        {
            VirtualRanges* const ranges = &lender->ranges;
            uint32_t const pages = ranges->at_base(base)->pages;
            uint32_t const n = static_cast<uint32_t>(pass_pages(pages - 1u));
            size_t const g = arch_aspace_granule();
            frame_pool_free_run(aspace_frame_of(base) + static_cast<arch_phys_addr_t>(pages - n) * g,
                                n);
            (void)ranges->shrink(base, pages - n);
        }

        // A create's pending range: a handoff, or a window `holder` holds.
        bool create_pending(VirtualRange const* e, uint16_t holder)
        {
            if (not vr_pending(e))
            {
                return false;
            }
            if ((e->flags & VR_WINDOW) != 0)
            {
                return e->holder == holder;
            }
            return aspace_range_handed_off(e);
        }
    }

    int aspace_self_grant(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                          uintptr_t base, size_t size, uint32_t rights,
                          enum arch_map_memtype type, MapPass* pass)
    {
        *pass = MapPass{};
        if (space == nullptr or ranges == nullptr)
        {
            return -KOS_EPERM;
        }
        if (not grant_memtype_at(type, aspace_frame_of(base), size))
        {
            return -KOS_ENOTSUP;
        }
        uint8_t const memtype = static_cast<uint8_t>(type);
        VirtualRange const* e = nullptr;
        int const admit = self_grant_admit(ranges, base, size, rights, memtype, &e);
        if (admit != 0)
        {
            if (admit > 0)
            {
                pass->done = true;
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
        int const mrc = map_pass(payer, space, ranges, e, aspace_frame_of(b), rights, type, pass);
        if (mrc == 0 and pass->done)
        {
            edited(aspace_frame_of(b), pages);
            owner_mapped(owner, type);
        }
        return mrc;
    }

    int aspace_cap_map(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                       uintptr_t* at, int run_obj, arch_phys_addr_t base, uint32_t pages,
                       uint32_t rights, enum arch_map_memtype type, MapPass* pass,
                       uint16_t holder, bool resuming)
    {
        *pass = MapPass{};
        if (space == nullptr or ranges == nullptr or pages == 0)
        {
            return -KOS_EINVAL;
        }
        // No range the kernel places sits at a run's own address, which names the run's frames.
        uintptr_t va = *at;
        if (va == 0)
        {
            va = aspace_user_va(base);
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
        if (not grant_memtype_at(type, base, static_cast<size_t>(pages) * g))
        {
            return -KOS_ENOTSUP;
        }
        int const run_slot = frame_run_slot_of(run_obj);
        uint8_t const memtype = static_cast<uint8_t>(type);
        bool const syncs = not cacheable(type) or frame_run_sync_owed(run_obj);
        VirtualRange const* e = ranges->at_base(va);
        bool const same = e != nullptr and (e->flags & VR_FRAMECAP) != 0 and run_slot >= 0
                          and e->run == static_cast<uint32_t>(run_slot) + 1u and e->pages == pages;
        if (resuming and same and not vr_pending(e) and e->memtype == memtype
            and e->state == VirtualState::Granted)
        {
            // Another map of the same run finished this one between two passes.
            pass->done = true;
            *at = va;
            return 0;
        }
        if (same and vr_pending(e))
        {
            if (e->pending != static_cast<uint8_t>(memtype + 1u))
            {
                return -KOS_EBUSY;
            }
            if (syncs and not sync_frames(base, pages))
            {
                return -KOS_ENOMEM;
            }
        }
        else
        {
            // Another space mapping the run with another type keeps it.
            if (not aspace_frames_type_ok(base, pages, memtype, nullptr))
            {
                return -KOS_EBUSY;
            }
            // A map of more than one pass takes its tables first, in passes of their own with
            // nothing reserved yet: no pass after them can be refused one.
            if (pages <= ASPACE_MAP_PASS_PAGES)
            {
                if (not aspace_tables_admit(payer, space, va, pages))
                {
                    return -KOS_EAGAIN;
                }
            }
            else
            {
                size_t const need = arch_aspace_range_tables(va, pages);
                int const trc = presync_tables_take_pass(payer, need);
                if (trc < 0)
                {
                    return trc;
                }
                if (trc > 0)
                {
                    *at = va;
                    return 0;
                }
                int const arc = presync_tables_arm(payer, need);
                if (arc != 0)
                {
                    return arc;
                }
            }
            // A leaf is a holder: the frames come back at the last one, not the last capability.
            if (not frame_run_ref(run_obj))
            {
                return -KOS_ENOMEM;
            }
            if (not ranges->reserve(va, pages, VR_BORROWED | VR_FRAMECAP,
                                    static_cast<uint32_t>(run_slot) + 1u, holder))
            {
                frame_run_release(run_obj);
                return -KOS_ENOMEM; // overlaps something this space already names, or the list is full
            }
            e = ranges->at_base(va);
            bool const noted = ranges->set_pending(va, memtype, 0);
            KICKOS_ASSERT(noted);
            if (syncs and not sync_frames(base, pages))
            {
                ranges->release(va);
                frame_run_release(run_obj);
                return -KOS_ENOMEM;
            }
        }
        bool const first = e->pending_pages == 0;
        int const mrc = map_pass(payer, space, ranges, e, base, rights, type, pass);
        if (mrc != 0)
        {
            if (first)
            {
                ranges->release(va);
                frame_run_release(run_obj);
            }
            return mrc;
        }
        *at = va;
        if (pass->done)
        {
            edited(base, pages);
            frame_run_set_sync_owed(run_obj, not cacheable(type));
        }
        return 0;
    }

    void aspace_cap_map_unwind(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                               uint16_t holder, MapPass* pass)
    {
        *pass = MapPass{};
        VirtualRange const* e = nullptr;
        if (ranges != nullptr)
        {
            e = ranges->at_base(va);
        }
        if (space == nullptr or e == nullptr or e->base != va or (e->flags & VR_FRAMECAP) == 0
            or not vr_pending(e) or e->holder != holder or holder == 0)
        {
            pass->done = true;
            return;
        }
        int const slot = static_cast<int>(e->run) - 1;
        // Only the first pass still finds the map's own type: later ones carry VR_UNWIND_MEMTYPE.
        if (e->pending != static_cast<uint8_t>(VR_UNWIND_MEMTYPE + 1u)
            and not cacheable(static_cast<enum arch_map_memtype>(e->pending - 1u)))
        {
            frame_run_set_sync_owed(kernel().frame_runs.handle_for(slot), true);
        }
        uint32_t const mapped = e->pending_pages;
        uint32_t const n = static_cast<uint32_t>(pass_pages(mapped));
        size_t const g = arch_aspace_granule();
        uintptr_t const top = va + static_cast<uintptr_t>(mapped - n) * g;
        if (n != 0)
        {
            (void)arch_aspace_unmap(space, top, n);
            pass->base = top;
            pass->bytes = static_cast<size_t>(n) * g;
        }
        if (n < mapped)
        {
            bool const noted = ranges->set_pending(va, VR_UNWIND_MEMTYPE, mapped - n);
            KICKOS_ASSERT(noted);
            return;
        }
        (void)ranges->release(va);
        frame_run_release_by_slot(slot);
        pass->done = true;
    }

    int aspace_cap_unmap(struct arch_aspace* space, VirtualRanges* ranges, uintptr_t va,
                         int run_obj, MapPass* pass)
    {
        *pass = MapPass{};
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
        if (vr_pending(e))
        {
            return -KOS_EBUSY;
        }
        size_t const g = arch_aspace_granule();
        uint32_t const pages = e->pages;
        uint32_t const n = static_cast<uint32_t>(pass_pages(pages));
        uintptr_t const top = va + static_cast<uintptr_t>(pages - n) * g;
        if (arch_aspace_unmap(space, top, n) != ARCH_ASPACE_OK)
        {
            return -KOS_ENOMEM;
        }
        pass->base = top;
        pass->bytes = static_cast<size_t>(n) * g;
        if (n < pages)
        {
            bool const cut = ranges->shrink(va, pages - n);
            KICKOS_ASSERT(cut);
            return 0;
        }
        ranges->release(va);
        frame_run_release(run_obj);
        pass->done = true;
        return 0;
    }

    int aspace_window_map(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                          arch_phys_addr_t pa, size_t bytes, uint32_t rights,
                          enum arch_map_memtype type, uint16_t holder, uint32_t place,
                          Domain* donor, size_t* budget)
    {
        size_t const g = arch_aspace_granule();
        if (space == nullptr or ranges == nullptr or bytes == 0 or (pa % g) != 0)
        {
            return -KOS_EINVAL;
        }
        size_t const pages = (bytes + g - 1u) / g;
        if (not grant_memtype_at(type, pa, pages * g))
        {
            return -KOS_ENOTSUP;
        }
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
        size_t const n = first_pages(pages, budget);
        if (va != 0 and not aspace_tables_admit(payer, space, va, n))
        {
            return -KOS_EAGAIN;
        }
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
        uint8_t const memtype = static_cast<uint8_t>(type);
        bool const syncs = type == ARCH_MAP_NOCACHE or owner.owed;
        // Not map_pass: its frame would deepen a spawn's descent the red zone prices.
        if ((syncs and not sync_frames(pa, pages))
            or arch_aspace_map(space, va, pa, n, rights, type) != ARCH_ASPACE_OK)
        {
            ranges->release(va);
            return -KOS_ENOMEM;
        }
        if (donor != nullptr)
        {
            domain_ref(donor);
            (void)donor->ranges.lend(aspace_user_va(pa), true);
        }
        if (n < pages)
        {
            bool const noted = ranges->set_pending(va, memtype, static_cast<uint32_t>(n))
                               and ranges->set_rights(va, rights);
            KICKOS_ASSERT(noted);
            return 0;
        }
        bool const granted = ranges->grant(va, pages, rights, memtype);
        KICKOS_ASSERT(granted);
        edited(pa, pages);
        owner_mapped(owner, type);
        return 0;
    }

    bool aspace_create_pending(VirtualRanges const* ranges, uint16_t holder)
    {
        for (size_t i = 0; ranges != nullptr and i < VirtualRanges::capacity(); i++)
        {
            if (create_pending(ranges->at(i), holder))
            {
                return true;
            }
        }
        return false;
    }

    size_t aspace_create_tables(VirtualRanges const* ranges, uint16_t holder)
    {
        size_t const g = arch_aspace_granule();
        size_t n = 0;
        for (size_t i = 0; ranges != nullptr and i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (create_pending(e, holder))
            {
                uint32_t const mapped = vr_mapped(e);
                n = n + arch_aspace_range_tables(e->base + static_cast<uintptr_t>(mapped) * g,
                                                 e->pages - mapped);
            }
        }
        return n;
    }

    int aspace_create_pass(Task const* payer, struct arch_aspace* space, VirtualRanges* ranges,
                           uint16_t holder, MapPass* pass)
    {
        *pass = MapPass{};
        for (size_t i = 0; space != nullptr and ranges != nullptr and i < VirtualRanges::capacity();
             i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (not create_pending(e, holder))
            {
                continue;
            }
            arch_phys_addr_t pa = aspace_frame_of(e->base);
            bool lent = true;
            if ((e->flags & VR_WINDOW) != 0)
            {
                pa = arch_aspace_frame_at(space, e->base);
                lent = e->donor != 0;
            }
            enum arch_map_memtype const type = static_cast<enum arch_map_memtype>(e->pending - 1u);
            Owner owner;
            if (lent)
            {
                owner = owner_of(pa);
            }
            size_t const pages = e->pages;
            if ((type == ARCH_MAP_NOCACHE or owner.owed) and not sync_frames(pa, pages))
            {
                return -KOS_ENOMEM;
            }
            int const rc = map_pass(payer, space, ranges, e, pa, e->rights, type, pass);
            if (rc != 0)
            {
                return rc;
            }
            if (pass->done)
            {
                edited(pa, pages);
                owner_mapped(owner, type);
            }
            return 1;
        }
        return 0;
    }

    bool aspace_window_unmap_pass(struct arch_aspace* space, VirtualRanges* ranges,
                                  uint16_t holder, MapPass* pass, Domain** lender,
                                  uintptr_t* lent)
    {
        *pass = MapPass{};
        *lender = nullptr;
        *lent = 0;
        for (size_t i = 0; space != nullptr and ranges != nullptr and i < VirtualRanges::capacity();
             i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e == nullptr or (e->flags & VR_WINDOW) == 0 or e->holder != holder)
            {
                continue;
            }
            size_t const g = arch_aspace_granule();
            uintptr_t const b = e->base;
            uint32_t const mapped = vr_mapped(e);
            uint32_t n = static_cast<uint32_t>(pass_pages(mapped));
            uint16_t const donor = e->donor;
            arch_phys_addr_t const pa = arch_aspace_frame_at(space, b);
            // The last borrower of a shed reservation frees it in passes, its first page last.
            Domain* shed = nullptr;
            if (donor != 0)
            {
                shed = last_lender_shed(aspace_user_va(pa));
            }
            if (shed != nullptr)
            {
                if (mapped == 1)
                {
                    shed_reap_pass(shed, aspace_user_va(pa));
                    return true;
                }
                n = static_cast<uint32_t>(pass_pages(mapped - 1u));
            }
            uintptr_t const top = b + static_cast<uintptr_t>(mapped - n) * g;
            (void)arch_aspace_unmap(space, top, n);
            pass->base = top;
            pass->bytes = static_cast<size_t>(n) * g;
            if (n < mapped)
            {
                if (vr_pending(e))
                {
                    (void)ranges->recede(b, mapped - n);
                }
                else
                {
                    (void)ranges->shrink(b, mapped - n);
                }
                return true;
            }
            ranges->release(b);
            if (donor != 0)
            {
                *lender = &kernel().domains()[donor - 1u];
                *lent = aspace_user_va(pa);
            }
            return true;
        }
        return false;
    }

    bool aspace_window_held(VirtualRanges const* ranges, uint16_t holder)
    {
        for (size_t i = 0; ranges != nullptr and i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e != nullptr and (e->flags & VR_WINDOW) != 0 and e->holder == holder)
            {
                return true;
            }
        }
        return false;
    }

    void aspace_window_unmap_holder(struct arch_aspace* space, VirtualRanges* ranges,
                                    uint16_t holder)
    {
        MapPass pass;
        Domain* lender = nullptr;
        uintptr_t lent = 0;
        while (aspace_window_unmap_pass(space, ranges, holder, &pass, &lender, &lent))
        {
            if (lender != nullptr)
            {
                domain_unlend(lender, lent);
                domain_release(lender);
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

    int aspace_handoff(Task const* payer, VirtualRanges const* donor, struct arch_aspace* space,
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
        if (not grant_memtype_at(type, aspace_frame_of(b), pages * g))
        {
            return -KOS_ENOTSUP;
        }
        uint32_t const rights = ARCH_MAP_R | ARCH_MAP_W;
        uint8_t const memtype = static_cast<uint8_t>(type);
        // The donor, a window or another handoff mapping these frames with another type keeps
        // them.
        if (not aspace_frames_type_ok(aspace_frame_of(b), pages, memtype, nullptr))
        {
            return -KOS_EBUSY;
        }
        Owner const owner = owner_of(aspace_frame_of(b));
        bool const syncs = type == ARCH_MAP_NOCACHE or owner.owed;
        size_t const n = pass_pages(pages);
        if (not aspace_tables_admit(payer, space, b, n))
        {
            return -KOS_EAGAIN;
        }
        // The borrower takes the donor's address; reserve refuses an overlap.
        if (not ranges->reserve(b, pages, VR_BORROWED))
        {
            return -KOS_ENOMEM;
        }
        if ((syncs and not sync_frames(aspace_frame_of(b), pages))
            or arch_aspace_map(space, b, aspace_frame_of(b), n, rights, type) != ARCH_ASPACE_OK)
        {
            (void)ranges->release(b);
            return -KOS_ENOMEM;
        }
        // Not map_pass: its frame would deepen a task create's descent the red zone prices.
        if (n < pages)
        {
            bool const noted = ranges->set_pending(b, memtype, static_cast<uint32_t>(n))
                               and ranges->set_rights(b, rights);
            KICKOS_ASSERT(noted);
            return 0;
        }
        bool const granted = ranges->grant(b, pages, rights, memtype);
        KICKOS_ASSERT(granted);
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

    void aspace_release_runs(struct arch_aspace* space, VirtualRanges* ranges)
    {
        for (size_t i = 0; space != nullptr and ranges != nullptr and i < VirtualRanges::capacity();
             i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e == nullptr or (e->flags & VR_FRAMECAP) == 0)
            {
                continue;
            }
            // Stored plus one; the VR_FRAMECAP flag is what says this entry named a run at all,
            // so the subtraction cannot reach VR_RUN_NONE.
            int const slot = static_cast<int>(e->run) - 1;
            uintptr_t const base = e->base;
            (void)arch_aspace_unmap(space, base, vr_mapped(e));
            (void)ranges->release(base);
            frame_run_release_by_slot(slot);
        }
    }

    bool aspace_range_handed_off(VirtualRange const* e)
    {
        return e != nullptr and (e->flags & static_cast<uint8_t>(~VR_SYNC_OWED)) == VR_BORROWED;
    }

    void aspace_release_handoff(struct arch_aspace* space, VirtualRanges* ranges)
    {
        for (size_t i = 0; space != nullptr and ranges != nullptr and i < VirtualRanges::capacity();
             i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (not aspace_range_handed_off(e))
            {
                continue;
            }
            if (vr_mapped(e) != 0)
            {
                (void)arch_aspace_unmap(space, e->base, vr_mapped(e));
            }
            (void)ranges->release_at(i);
        }
    }

    bool aspace_range_lent(VirtualRange const* e)
    {
        return e != nullptr and (e->flags & static_cast<uint8_t>(~VR_SYNC_OWED)) == 0
               and e->lends != 0;
    }

    void aspace_shed(struct arch_aspace* space, VirtualRanges* ranges)
    {
        if (space == nullptr or ranges == nullptr)
        {
            return;
        }
        // Unmapped first, so the destroy walk frees none of a lent reservation's frames.
        for (size_t i = 0; i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (not aspace_range_lent(e))
            {
                continue;
            }
            if (vr_mapped(e) != 0)
            {
                (void)arch_aspace_unmap(space, e->base, vr_mapped(e));
            }
            (void)ranges->clear_pending(e->base);
        }
        aspace_release(space, ranges, true);
        for (size_t i = 0; i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e != nullptr and not aspace_range_lent(e))
            {
                (void)ranges->release_at(i);
            }
        }
    }

    bool aspace_trim_pass(struct arch_aspace* space, VirtualRanges* ranges)
    {
        size_t const g = arch_aspace_granule();
        for (size_t i = 0; space != nullptr and ranges != nullptr and i < VirtualRanges::capacity();
             i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e == nullptr or (e->flags & (VR_IMAGE | VR_USTACK | VR_WINDOW | VR_SHARE)) != 0)
            {
                continue;
            }
            uintptr_t const b = e->base;
            uint32_t const pages = e->pages;
            uint32_t const mapped = vr_mapped(e);
            bool const owned = (e->flags & static_cast<uint8_t>(~VR_SYNC_OWED)) == 0
                               and e->lends == 0;
            if (not owned and (e->flags & VR_FRAMECAP) == 0 and mapped == 0)
            {
                if (not aspace_range_handed_off(e))
                {
                    continue;
                }
                Domain* const shed = last_lender_shed(b);
                if (shed != nullptr)
                {
                    shed_reap_pass(shed, b);
                    return true;
                }
                continue;
            }
            if (owned and pages > mapped)
            {
                // The frames no leaf names, the top first.
                uint32_t const n = static_cast<uint32_t>(pass_pages(pages - mapped));
                arch_phys_addr_t const tail = aspace_frame_of(b)
                                              + static_cast<arch_phys_addr_t>(pages - n) * g;
                frame_pool_free_run(tail, n);
                if (n == pages)
                {
                    (void)ranges->release_at(i);
                }
                else
                {
                    (void)ranges->shrink(b, pages - n);
                }
                return true;
            }
            uint32_t const n = static_cast<uint32_t>(pass_pages(mapped));
            uintptr_t const top = b + static_cast<uintptr_t>(mapped - n) * g;
            if (n != 0)
            {
                (void)arch_aspace_unmap(space, top, n);
            }
            if (owned)
            {
                frame_pool_free_run(aspace_frame_of(top), n);
                if (n == pages)
                {
                    (void)ranges->release_at(i);
                }
                else
                {
                    (void)ranges->shrink(b, pages - n);
                }
            }
            else if ((e->flags & VR_FRAMECAP) != 0 and n == mapped)
            {
                int const slot = static_cast<int>(e->run) - 1;
                (void)ranges->release_at(i);
                frame_run_release_by_slot(slot);
            }
            else
            {
                // Another record frees these frames: the range keeps its extent.
                (void)ranges->recede(b, mapped - n);
            }
            return true;
        }
        return false;
    }

    uint32_t aspace_shed_pages(VirtualRanges const* ranges)
    {
        uint32_t pages = 0;
        for (size_t i = 0; ranges != nullptr and i < VirtualRanges::capacity(); i++)
        {
            VirtualRange const* const e = ranges->at(i);
            if (e != nullptr)
            {
                pages = pages + e->pages;
            }
        }
        return pages;
    }

    void aspace_shed_free(VirtualRanges* ranges, size_t i)
    {
        VirtualRange const* const e = ranges->at(i);
        if (e == nullptr)
        {
            return;
        }
        frame_pool_free_run(aspace_frame_of(e->base), e->pages);
        (void)ranges->release_at(i);
    }

    void aspace_release(struct arch_aspace* space, VirtualRanges* ranges, bool keep_lent)
    {
        if (space == nullptr)
        {
            return;
        }
        // Switch away before freeing active tables. Clear every core's cached root
        // so a later activation cannot skip installing a replacement.
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
                activate_locked(arch_aspace_boot());
            }
        }
        aspace_release_runs(space, ranges);
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
                if (vr_mapped(e) != 0)
                {
                    (void)arch_aspace_unmap(space, e->base, vr_mapped(e));
                }
                continue;
            }
            if (keep_lent and aspace_range_lent(e))
            {
                continue;
            }
            if (e->state == VirtualState::Reserved and (e->flags & VR_IMAGE) == 0
                and e->pending_pages < e->pages)
            {
                // No leaf points at these past a pending self-grant's mapped pages, so the
                // destroy walk cannot see them; the image is excluded, its pages not being the
                // pool's to take back.
                arch_phys_addr_t const tail = aspace_frame_of(e->base)
                                              + static_cast<arch_phys_addr_t>(e->pending_pages) * g;
                frame_pool_free_run(tail, e->pages - e->pending_pages);
            }
        }
        arch_aspace_destroy(space);
    }

    // The space `t` may run on: null for a spaceless thread and, above one kernel core, for one
    // whose exit has left its task's space.
    static struct arch_aspace* space_for(Thread const* t)
    {
#if KICKOS_KERNEL_CORES > 1
        if (t != nullptr and t->left_space)
        {
            return nullptr;
        }
#endif
        return domain_space(thread_domain(t));
    }

    struct arch_aspace* aspace_activate_for(Thread const* t)
    {
        struct arch_aspace* const space = space_for(t);
        if (space == nullptr)
        {
#if KICKOS_KERNEL_CORES > 1
            // Another core may free the root this core would otherwise keep. At one core,
            // aspace_release leaves the root before it frees it.
            aspace_install_boot();
#endif
            return nullptr;
        }
        seat(space);
        return space;
    }

    void aspace_install_boot(void)
    {
        seat(arch_aspace_boot());
    }
}

#if KICKOS_PRESYNC
extern "C" arch_phys_addr_t kickos_presync_table_take(void)
{
    kickos::Kernel& k = kickos::kernel();
    int const i = k.threads.index_of(kickos::sched::current());
    if (i < 0 or k.presync()[i].table_count == 0)
    {
        return 0;
    }
    kickos::PresyncRecord* const r = &k.presync()[i];
    if (not r->tables_armed)
    {
        return 0;
    }
    arch_phys_addr_t const f = r->tables;
    r->tables = *static_cast<arch_phys_addr_t*>(kickos::frame_pool_alias(f));
    r->table_count--;
    kickos::task_tables_reserved(r->table_payer, r->table_payer_gen, 1u, false);
    return f;
}
#endif

#endif
