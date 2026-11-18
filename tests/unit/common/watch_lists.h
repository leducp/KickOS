// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The INSTANCE watches as their declarations' lists hold them (kernel/task/start_watch.cc): a
// link is a thread slot plus one in its high byte and a capability index in its low one, 0 for
// none; an entry's obj_gen holds its previous link in the high half and its next in the low
// one; a raised entry keeps its place with no badge.

#ifndef KICKOS_TESTS_UNIT_COMMON_WATCH_LISTS_H
#define KICKOS_TESTS_UNIT_COMMON_WATCH_LISTS_H

#include <gtest/gtest.h>

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/thread.h>

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace kickos::testfix
{
    inline CapEntry* watch_entry(uint16_t link)
    {
        Thread const& th = kernel().threads.slots()[(link >> 8) - 1];
        return cap_slot(th.caps, link & 0xFFu);
    }

    inline uint16_t watch_next(CapEntry const& e)
    {
        return static_cast<uint16_t>(e.obj_gen & 0xFFFFu);
    }

    inline uint16_t watch_prev(CapEntry const& e)
    {
        return static_cast<uint16_t>(e.obj_gen >> 16);
    }

    inline bool watch_armed(CapEntry const& e)
    {
        return cap_badge(e) != KCAP_BADGE_NONE;
    }

    // The CAP_WATCH on `decl` that `link` names, or null where it names none.
    inline CapEntry* watch_entry_on(uint16_t link, size_t decl)
    {
        int const slot = (link >> 8) - 1;
        uint32_t const index = link & 0xFFu;
        if (slot < 0 or slot >= kernel().threads.count())
        {
            return nullptr;
        }
        Thread const& th = kernel().threads.slots()[slot];
        if (th.caps == nullptr or index < KICKOS_CAP_FIRST_DYNAMIC
            or index >= thread_cap_capacity(&th))
        {
            return nullptr;
        }
        CapEntry* const e = cap_slot(th.caps, index);
        if (e->type != static_cast<uint8_t>(CapType::CAP_WATCH) or cap_watch_decl(*e) != decl)
        {
            return nullptr;
        }
        return e;
    }

    // `decl`'s list from its head, each entry's link. Empty, `broken` then set, where a link
    // names no watch on `decl`, a back link disagrees with the walk, a thread is listed twice
    // (a cycle included), or a raised entry precedes an armed one.
    inline std::vector<uint16_t> watch_list(size_t decl, bool* broken = nullptr)
    {
        std::vector<uint16_t> out;
        std::vector<bool> seen(static_cast<size_t>(kernel().threads.count()), false);
        uint16_t prev = 0;
        bool raised = false;
        bool bad = false;
        for (uint16_t x = kernel().start_watches()[static_cast<int>(decl)]; x != 0;)
        {
            CapEntry const* const e = watch_entry_on(x, decl);
            if (e == nullptr or watch_prev(*e) != prev or seen[static_cast<size_t>((x >> 8) - 1)])
            {
                bad = true;
                break;
            }
            seen[static_cast<size_t>((x >> 8) - 1)] = true;
            if (not watch_armed(*e))
            {
                raised = true;
            }
            else if (raised)
            {
                bad = true;
                break;
            }
            out.push_back(x);
            prev = x;
            x = watch_next(*e);
        }
        if (bad)
        {
            if (broken != nullptr)
            {
                *broken = true;
            }
            return {};
        }
        return out;
    }

    // Whether any declaration's list holds an armed watch; a broken list fails the test and
    // answers true.
    inline bool any_watch_armed()
    {
        bool armed = false;
        for (int d = 0; d < kernel().start_watches().size(); d++)
        {
            bool broken = false;
            for (uint16_t x : watch_list(static_cast<size_t>(d), &broken))
            {
                if (watch_armed(*watch_entry(x)))
                {
                    armed = true;
                }
            }
            if (broken)
            {
                ADD_FAILURE() << "declaration " << d << "'s watch list is broken";
                armed = true;
            }
        }
        return armed;
    }

    // Whether every declaration's list is empty.
    inline bool no_watch_listed()
    {
        for (int d = 0; d < kernel().start_watches().size(); d++)
        {
            if (kernel().start_watches()[d] != 0)
            {
                return false;
            }
        }
        return true;
    }
}

#endif
