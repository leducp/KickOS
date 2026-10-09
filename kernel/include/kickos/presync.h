// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The record one system call keeps of the frames it synced ahead of the kernel lock, of the
// mappings its locked pass installed, and of a run it holds outside every range, and the
// questions its locked pass and every completed mapping edit ask of it. Pure: no lock, no arch
// call.

#ifndef KICKOS_PRESYNC_H
#define KICKOS_PRESYNC_H

#include <kickos/arch/arch.h> // arch_phys_addr_t
#include <kickos/config.h>
#include <kickos/sys/abi.h>

#include <stddef.h>
#include <stdint.h>

struct arch_aspace;

// A translating backend whose system calls can let an interrupt in: it clears, copies and syncs
// a call's frames outside the kernel lock.
#define KICKOS_PRESYNC (KICKOS_HAVE_ASPACE and KICKOS_ARCH_IRQ_WINDOW)

#if KICKOS_ARCH_ALIAS_DCACHE and not KICKOS_ARCH_IRQ_WINDOW
#error "a kernel view that owes a mapping's frames a sync syncs them outside the lock"
#endif

namespace kickos
{
#if not KICKOS_PRESYNC
    inline void presync_commit() {}
#endif

    constexpr size_t PRESYNC_SPANS = KICKOS_MAX_THREAD_WINDOWS + 1u;
    static_assert(PRESYNC_SPANS <= UINT8_MAX, "PresyncSpans::count must hold every span");

    struct PresyncSpans
    {
        arch_phys_addr_t pa[PRESYNC_SPANS] = {};
        uint32_t pages[PRESYNC_SPANS] = {};
        uint8_t count = 0;
    };

    // What a run held outside every range is for.
    enum class PresyncStage : uint8_t
    {
        NONE,
        // A reservation's frames, cleared outside the lock and then reserved.
        CLEAR,
        // A new space's copy of the image's static data, copied outside the lock from
        // `staged_home`, or from the snapshot where that is null.
        IMAGE
    };

    struct PresyncRecord
    {
        PresyncSpans noted;
        // The pool spans the locked pass mapped, dropped from every other record when the call
        // completes and never before.
        PresyncSpans edits;
        arch_phys_addr_t staged = 0;
        uint32_t staged_pages = 0;
        PresyncStage stage = PresyncStage::NONE;
        // Every granule of an IMAGE stage holds its source's bytes.
        bool staged_full = false;
        // The pool had no run for the stage the call needed: the locked pass fails rather than
        // sending the call round.
        bool staged_short = false;
        struct arch_aspace* staged_home = nullptr;
        bool active = false;
        // A spawn's parameters and window list, copied in once when the call enters: its plan
        // and its locked pass read these and never the caller's memory again.
        bool has_params = false;
        bool has_windows = false;
        kos_thread_params params = {};
        kos_window windows[KICKOS_MAX_THREAD_WINDOWS] = {};
        // Set when the first span is noted, cleared by a completed call of another thread
        // that mapped a noted frame: only a live record excuses the locked pass.
        bool live = false;
        // The locked pass met a span it owed a sync and this record did not cover, or an image
        // to copy with no full stage for it.
        bool refused = false;
    };

    // False, noting nothing, when the spans are full or the span is empty or too wide.
    inline bool presync_add(PresyncSpans& r, arch_phys_addr_t pa, size_t pages)
    {
        if (pages == 0 or pages > UINT32_MAX or r.count >= PRESYNC_SPANS)
        {
            return false;
        }
        r.pa[r.count] = pa;
        r.pages[r.count] = static_cast<uint32_t>(pages);
        r.count++;
        return true;
    }

    // Whether one span holds every granule of [pa, pa + pages granules).
    inline bool presync_covers(PresyncSpans const& r, arch_phys_addr_t pa, size_t pages,
                               size_t granule)
    {
        arch_phys_addr_t const end = pa + static_cast<arch_phys_addr_t>(pages) * granule;
        for (uint8_t i = 0; i < r.count; i++)
        {
            arch_phys_addr_t const s_end =
                r.pa[i] + static_cast<arch_phys_addr_t>(r.pages[i]) * granule;
            if (r.pa[i] <= pa and end <= s_end)
            {
                return true;
            }
        }
        return false;
    }

    // Whether [pa, pa + pages granules) shares a granule with any span.
    inline bool presync_meets(PresyncSpans const& r, arch_phys_addr_t pa, size_t pages,
                              size_t granule)
    {
        arch_phys_addr_t const end = pa + static_cast<arch_phys_addr_t>(pages) * granule;
        for (uint8_t i = 0; i < r.count; i++)
        {
            arch_phys_addr_t const s_end =
                r.pa[i] + static_cast<arch_phys_addr_t>(r.pages[i]) * granule;
            if (r.pa[i] < end and pa < s_end)
            {
                return true;
            }
        }
        return false;
    }

    // Whether the locked pass may map [pa, pa + pages granules) without a sync of its own: the
    // record is live and one noted span holds every granule. A refusal marks the record.
    inline bool presync_excuses(PresyncRecord& r, arch_phys_addr_t pa, size_t pages,
                                size_t granule)
    {
        if (r.live and presync_covers(r.noted, pa, pages, granule))
        {
            return true;
        }
        r.refused = true;
        return false;
    }

    // Whether another call's completed edit of [pa, pa + pages granules) drops `r`.
    inline bool presync_dropped_by(PresyncRecord const& r, arch_phys_addr_t pa, size_t pages,
                                   size_t granule)
    {
        return r.live and presync_meets(r.noted, pa, pages, granule);
    }

    // The granules of [pa, pa + pages granules) inside the pool's frames [lo, hi): the only
    // frames the kernel reaches through a cacheable view of its own, and so the only ones a
    // sync is owed. Constant time: the locked pass asks it.
    inline size_t presync_pool_pages(arch_phys_addr_t pa, size_t pages, size_t granule,
                                     arch_phys_addr_t lo, arch_phys_addr_t hi)
    {
        arch_phys_addr_t const end = pa + static_cast<arch_phys_addr_t>(pages) * granule;
        arch_phys_addr_t a = pa;
        arch_phys_addr_t b = end;
        if (a < lo)
        {
            a = lo;
        }
        if (b > hi)
        {
            b = hi;
        }
        if (granule == 0 or a >= b)
        {
            return 0;
        }
        return static_cast<size_t>((b - a + granule - 1u) / granule);
    }
}

#endif
