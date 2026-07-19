// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Capability-table manager (see cap.h): the per-task naming+rights layer over the
// global object pools, plus the object-side refcount (kernel().sem_refs) that owns
// destroy-on-last-close. slotpool.h stays generic -- refs[] lives here.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>

namespace kickos
{
    namespace
    {
        // Slot index of the semaphore a global handle names (via the live object, so
        // the SlotPool handle codec is never assumed here). -1 if it does not resolve.
        int sem_index_of(int obj_handle)
        {
            Semaphore* s = kernel().sems.resolve(obj_handle);
            if (s == nullptr)
            {
                return -1;
            }
            return static_cast<int>(s - kernel().sems.at(0));
        }

        // Drop one reference to semaphore `obj_handle`; free it at refs -> 0. `teardown`
        // = the noreturn exit path: it must never strand a parked waiter, so a would-be
        // free with waiters still linked LEAKS (floors refs at 1). That branch is
        // unreachable via close today (a parked waiter pins its own cap => refs >= 1),
        // so it asserts teardown. Same accounting shape every future pool's arm mirrors.
        void sem_ref_drop(int obj_handle, bool teardown)
        {
            int const idx = sem_index_of(obj_handle);
            if (idx < 0)
            {
                return; // already gone -- cannot happen under correct refcounting
            }
            uint8_t& r = kernel().sem_refs[idx];
            if (r > 0)
            {
                r--;
            }
            if (r == 0)
            {
                Semaphore* s = kernel().sems.resolve(obj_handle);
                if (s != nullptr and not s->waiters.empty())
                {
                    KICKOS_ASSERT(teardown); // refs->0 with a waiter parked is unreachable via close
                    r = 1;                   // leak, never strand
                    return;
                }
                kernel().sems.free(obj_handle);
            }
        }

        // Drop one reference to the object a (now-detached) cap entry named; dispatch to
        // the per-type accounting arm. A future type reaching the default without its own
        // arm traps in debug -- a silent skip would leak its reference with no diagnostic
        // (release builds still avoid treating a foreign handle as a sem index: safe leak).
        void obj_ref_drop(CapEntry const& e, bool teardown)
        {
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_SEM:
                sem_ref_drop(e.obj, teardown);
                return;
            default:
                KICKOS_ASSERT(false);
                return;
            }
        }

        // Per-type close/exit protocol, run BEFORE detach + drop at both call sites.
        // Returns 0, or -1 to refuse a voluntary (non-teardown) close. CAP_SEM has no
        // protocol -- this is why semaphores never needed the hook. The seam #3
        // (refuse-owned / force-unlock) and #4 (EPIPE-wake) fill their arms here.
        int obj_close_protocol(Thread* closer, CapEntry const& e, bool teardown)
        {
            (void)closer;
            (void)teardown;
            switch (static_cast<CapType>(e.type))
            {
            case CapType::CAP_SEM:
                return 0;
            default:
                return 0;
            }
        }
    }

    // Public (replaces sem_ref_inc; also the type-agnostic delegation entry point). Bump
    // one reference to the object a global handle names. Handle MUST resolve. Holds IrqLock.
    void obj_ref_inc(CapType type, int obj_handle)
    {
        switch (type)
        {
        case CapType::CAP_SEM:
        {
            int const idx = sem_index_of(obj_handle);
            if (idx < 0)
            {
                return;
            }
            kernel().sem_refs[idx]++;
            return;
        }
        default:
            KICKOS_ASSERT(false); // unknown type must trap in debug
            return;
        }
    }

    CapEntry* cap_lookup(Thread* c, int cap_handle)
    {
        if (c == nullptr)
        {
            return nullptr;
        }
        int const idx = cap_handle & ((1 << KCAP_INDEX_BITS) - 1);
        if (idx >= KICKOS_MAX_HANDLES)
        {
            return nullptr;
        }
        CapEntry& e = c->handles[idx];
        if (e.type == static_cast<uint8_t>(CapType::CAP_EMPTY))
        {
            return nullptr;
        }
        // Full high bits (not truncated): a handle carrying junk above the cap-gen field
        // must fail to resolve, not alias -- mirrors slotpool.h's resolve().
        uint32_t const cgen = static_cast<uint32_t>(cap_handle) >> KCAP_INDEX_BITS;
        if (static_cast<uint32_t>(e.gen) != cgen)
        {
            return nullptr;
        }
        return &e;
    }

    void* cap_resolve(Thread* c, int cap_handle, CapType want, uint8_t need)
    {
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return nullptr;
        }
        if (e->type != static_cast<uint8_t>(want))
        {
            return nullptr;
        }
        if ((e->rights & need) != need) // rights enforced HERE, nowhere else
        {
            return nullptr;
        }
        // WRAP: the stored global handle re-checks the object-gen in its own pool.
        if (want == CapType::CAP_SEM)
        {
            return kernel().sems.resolve(e->obj);
        }
        return nullptr; // CAP_MUTEX / CAP_ENDPOINT pools are additive, land later
    }

    void cap_install_at(Thread* c, int index, int obj_handle, CapType type, uint8_t rights)
    {
        CapEntry& e = c->handles[index];
        e.obj = obj_handle;
        e.type = static_cast<uint8_t>(type);
        e.rights = rights;
    }

    int cap_install(Thread* c, int obj_handle, CapType type, uint8_t rights)
    {
        // Find any free slot (the design's "find CAP_EMPTY slot"). Index 0 stays
        // "reserved by convention" only in that cap_install_defaults seats nothing there
        // and delegation places at i+1 (so a fresh delegated child keeps 0 empty); a
        // thread's OWN sem_create may use index 0 -- else the reservation would cost every
        // table a usable slot (root then cannot hold the stress soak's 8 concurrent caps).
        for (int i = 0; i < KICKOS_MAX_HANDLES; i++)
        {
            if (c->handles[i].type == static_cast<uint8_t>(CapType::CAP_EMPTY))
            {
                cap_install_at(c, i, obj_handle, type, rights);
                return static_cast<int>((static_cast<uint32_t>(c->handles[i].gen) << KCAP_INDEX_BITS)
                                        | static_cast<uint32_t>(i));
            }
        }
        return -1;
    }

    int handle_close(Thread* c, int cap_handle)
    {
        CapEntry* e = cap_lookup(c, cap_handle);
        if (e == nullptr)
        {
            return -1;
        }
        if (obj_close_protocol(c, *e, /*teardown=*/false) != 0)
        {
            return -1; // protocol refused the close (e.g. #3: owner closing a held mutex)
        }
        CapEntry const detached = *e;
        // Stale the handle + empty the slot BEFORE dropping the ref, so the slot is
        // cleanly reusable and no stale handle resolves during the drop.
        e->gen++;
        e->type = static_cast<uint8_t>(CapType::CAP_EMPTY);
        e->obj = 0;
        e->rights = 0;
        obj_ref_drop(detached, /*teardown=*/false);
        return 0;
    }

    void cap_teardown(Thread* c)
    {
        for (int i = 0; i < KICKOS_MAX_HANDLES; i++)
        {
            CapEntry& e = c->handles[i];
            if (e.type == static_cast<uint8_t>(CapType::CAP_EMPTY))
            {
                continue;
            }
            obj_close_protocol(c, e, /*teardown=*/true);
            CapEntry const detached = e;
            e.gen++;
            e.type = static_cast<uint8_t>(CapType::CAP_EMPTY);
            e.obj = 0;
            e.rights = 0;
            obj_ref_drop(detached, /*teardown=*/true);
        }
    }

    void cap_install_defaults(Thread* child)
    {
        (void)child; // installs nothing today; index 0 reserved by convention (section 6)
    }
}
