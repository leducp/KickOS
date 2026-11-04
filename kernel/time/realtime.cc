// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The wall clock: the monotonic clock plus one offset. A partition whose nodes read one counter
// keeps one wall clock in the region every node writes, one record per node, each written by its
// own node alone; the record with the highest (generation, node) holds the offset. Elsewhere a
// kernel instance keeps its own. A realtime deadline waits on the monotonic clock, keyed to the
// offset its node last saw, and every set moves each one by the change of that offset.

#include <kickos/time.h>
#include <kickos/board_config.h>
#include <kickos/cap.h>
#include <kickos/irqlock.h>
#include <kickos/thread.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/amp_shared.h>
#include <kickos/instance_local.h>

#include <kickos/sys/atomic.h>
#include <kickos/sys/errno.h>

#if KICKOS_AMP_CLOCK_SHARED
namespace kickos
{
    namespace amp
    {
        // kernel/amp/ampmap.cc: ring `to`'s doorbell.
        void ring(uint32_t to);
    }
}
#endif

namespace kickos
{
    namespace
    {
        using Word = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

        struct WallValue
        {
            Word gen;
            Word offset_lo;
            Word offset_hi;
        };

        // A set writes the value `seq` does not name, then moves `seq` to it: a node stopped
        // anywhere inside its set leaves the value its readers take whole.
        struct WallSet
        {
            Word seq;
            WallValue value[2];
        };

        struct WallRead
        {
            uint32_t gen;
            uint64_t offset_ns;
        };

#if KICKOS_AMP_CLOCK_SHARED
        constexpr uint32_t WALL_SETS = KICKOS_AMP_NODES;
        KICKOS_AMP_SHARED("clock") WallSet g_wall[WALL_SETS];

        WallSet* wall_sets()
        {
            return g_wall;
        }

        uint32_t wall_self()
        {
#if KICKOS_AMP_OWN_IMAGE
            return KICKOS_AMP_NODE_ID;
#else
            return kickos_instance_index();
#endif
        }
#else
        constexpr uint32_t WALL_SETS = 1u;
        constinit InstanceLocal<WallSet[WALL_SETS]> g_wall_all = {};

        WallSet* wall_sets()
        {
            return g_wall_all.get();
        }

        uint32_t wall_self()
        {
            return 0u;
        }
#endif

        // How many reads a peer's record may change under before this node passes it over.
        constexpr uint32_t PEER_TRIES = 4u;

        // False when the record moved under every one of `tries` reads; zero tries is no bound.
        bool wall_read(WallSet const& w, uint32_t tries, WallRead& out)
        {
            uint32_t n = 0u;
            while (tries == 0u or n < tries)
            {
                n++;
                uint32_t const seq = w.seq;
                WallValue const& v = w.value[seq & 1u];
                out.gen = 0u;
                if (WALL_SETS > 1u)
                {
                    out.gen = v.gen;
                }
                out.offset_ns = kos_u64_join(v.offset_lo, v.offset_hi);
                // The set after the next one rewrites this value, after publishing the next.
                if (w.seq == seq)
                {
                    return true;
                }
            }
            return false;
        }

        // The newest set, ties to the higher node. Its own record is read with no bound: only this
        // node writes it.
        WallRead wall_newest()
        {
            WallSet const* const sets = wall_sets();
            uint32_t const self = wall_self();
            WallRead best;
            wall_read(sets[self], 0u, best);
            uint32_t best_node = self;
            for (uint32_t i = 0u; i < WALL_SETS; ++i)
            {
                WallRead r;
                if (i == self or not wall_read(sets[i], PEER_TRIES, r))
                {
                    continue;
                }
                if (r.gen > best.gen or (r.gen == best.gen and i > best_node))
                {
                    best = r;
                    best_node = i;
                }
            }
            return best;
        }

#if KICKOS_AMP_CLOCK_SHARED
        // The offset each node's marked deadlines are keyed to, its own cell written by that
        // node alone.
        uint64_t g_keyed[WALL_SETS];

        // The offset this node's marked deadlines are keyed to, moved to the newest first.
        uint64_t wall_keyed(Held held)
        {
            uint64_t const newest = wall_newest().offset_ns;
            uint64_t& keyed = g_keyed[wall_self()];
            if (newest != keyed)
            {
                ktime_wall_shift(static_cast<int64_t>(keyed - newest), held);
                keyed = newest;
            }
            return keyed;
        }
#else
        uint64_t wall_keyed(Held)
        {
            return wall_newest().offset_ns;
        }
#endif
    }

#if KICKOS_AMP_CLOCK_SHARED
    void realtime_rekey(Held held)
    {
        (void)wall_keyed(held);
    }
#endif

    void realtime_deadline_arm(Thread* t, uint64_t unix_ns, Held held)
    {
        uint64_t const offset = wall_keyed(held);
        uint64_t const now = arch_clock_now();
        uint64_t const wall = now + offset;
        uint64_t left = 0;
        if (unix_ns > wall)
        {
            left = unix_ns - wall;
        }
        uint64_t deadline = now + left;
        if (deadline < now)
        {
            deadline = UINT64_MAX;
        }
        ktime_deadline_arm_at(t, ktime_floored(now, deadline), true, held);
    }

    uint64_t realtime_now()
    {
        return arch_clock_now() + wall_newest().offset_ns;
    }

    int32_t realtime_set(Thread* caller, uint64_t unix_ns)
    {
        if (not cap_check_authority(caller, AUTH_REALTIME))
        {
            return -KOS_EPERM;
        }
        IrqLock lock;
#if not KICKOS_AMP_CLOCK_SHARED
        uint64_t const was = wall_keyed(lock);
#endif
        uint32_t gen = 0u;
        if (WALL_SETS > 1u)
        {
            gen = wall_newest().gen + 1u;
        }
        WallSet& own = wall_sets()[wall_self()];
        uint32_t const seq = own.seq + 1u;
        WallValue& v = own.value[seq & 1u];
        v.gen = gen;
        uint64_t const offset = unix_ns - arch_clock_now();
        v.offset_lo = kos_u64_lo(offset);
        v.offset_hi = kos_u64_hi(offset);
        own.seq = seq;
#if KICKOS_AMP_CLOCK_SHARED
        (void)wall_keyed(lock);
        // After the publishing store: a peer the raise reaches reads this set.
        for (uint32_t node = 0u; node < WALL_SETS; ++node)
        {
            if (node != wall_self())
            {
                amp::ring(node);
            }
        }
#else
        ktime_wall_shift(static_cast<int64_t>(was - offset), lock);
#endif
        return 0;
    }
}
