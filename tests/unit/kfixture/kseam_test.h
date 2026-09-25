// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The GoogleTest layer over the K-seam fixture, separate from kfixture.h for the link
// reason stated there.

#ifndef KICKOS_TESTS_UNIT_KFIXTURE_KSEAM_TEST_H
#define KICKOS_TESTS_UNIT_KFIXTURE_KSEAM_TEST_H

#include <string>

#include <gtest/gtest.h>

#include "kfixture.h"

#include <kickos/instance.h>
#include <kickos/thread.h>

namespace kickos
{
    namespace testfix
    {
        // A gate derives its own named subclass, because TEST_F takes the fixture CLASS name as
        // the ctest suite name.
        class KSeam : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                reset();
            }
        };
    }
}

// `msg` is a diag:: catalogue entry, never a sentence: KICKOS_DIAG_TERSE picks the column and a
// hardcoded sentence would gate the verbose build only.
#define KICKOS_EXPECT_PANIC(stmt, msg)                                                            \
    EXPECT_DEATH(                                                                                 \
        {                                                                                         \
            ::kickos::testfix::fold_stdout_into_stderr();                                          \
            stmt;                                                                                 \
        },                                                                                        \
        std::string("KERNEL PANIC: ") + (msg))

// The other way an arm ends: a fixture self-diagnostic, which exits rather than returning a
// state no assertion could interpret. `msg` is a substring of the refusal, so one naming a
// thread id can still be matched.
#define KICKOS_EXPECT_FIXTURE_REFUSAL(stmt, msg)                                                  \
    EXPECT_DEATH(                                                                                 \
        {                                                                                         \
            ::kickos::testfix::fold_stdout_into_stderr();                                          \
            stmt;                                                                                 \
        },                                                                                        \
        std::string("FIXTURE FAIL: ") + (msg))

#if KICKOS_KERNEL_CORES > 1
namespace kickos
{
    namespace testfix
    {
        // Seat a thread as a core's running one, the way the scheduler would. A per-core ready
        // structure makes `current[c]` and `queue_core` one fact, and no real path can separate
        // them: a core only ever picks out of its own queue. A hand seat that writes `current`
        // alone leaves the thread linked into the core that published it, and that core's own
        // scan then meets a thread a peer is running.
        //
        // The membership test is not a link test: a sole member has both links null, exactly as
        // a detached node does, so only the list's own head separates them; unlink refuses a
        // node it does not hold and push_back refuses one that is still linked.
        inline void seat_running_on(Thread* t, uint32_t core)
        {
            Kernel& k = kernel();
            (void)unstage(t);
            // Searched and not indexed: a list is named by the owner and the priority, and a
            // fixture that set either after queueing would have this look in the wrong one,
            // find nothing, and append a node that is still linked somewhere else. The scan
            // costs a fixture nothing and cannot be wrong.
            for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
            {
                for (uint32_t p = 0; p < KICKOS_NUM_PRIO; p++)
                {
                    List& from = k.ready[c][p];
                    bool found = false;
                    for (ListNode const* n = from.head; n != nullptr; n = n->next)
                    {
                        if (n == &t->link)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (not found)
                    {
                        continue;
                    }
                    from.unlink(&t->link);
                    if (from.empty())
                    {
                        k.ready_bitmap[c] &= ~(1u << p);
                    }
                }
            }
            t->queue_core = static_cast<uint8_t>(core);
            t->rq_prio = t->prio;
            k.ready[core][t->prio].push_back(&t->link);
            k.ready_bitmap[core] |= (1u << t->prio);
            // A thread this seat displaces is still on the core's queue, which is what a switch
            // leaves behind it.
            Thread* const was = k.current[core];
            if (was != nullptr and was != t and was->state == ThreadState::RUNNING)
            {
                was->state = ThreadState::READY;
            }
            t->state = ThreadState::RUNNING;
            k.current[core] = t;
            // What the core's last pass seated, or a later pass seating lower sees no drop.
            k.seated_prio[core] = t->prio;
            // Started, as its own first seat would publish it.
            k.sched_out[core].level.store(
                static_cast<uint8_t>(32 - __builtin_clz(k.ready_bitmap[core])));
        }

        // Whether `asker` has a drop ask standing against `holder` that the holder has not
        // answered.
        inline bool drop_asked(uint32_t asker, uint32_t holder)
        {
            Kernel& k = kernel();
            return k.sched_out[asker].drop_asked[holder].load()
                   != k.sched_in[holder].drop_took[asker].load();
        }

        // One doorbell dispatch on `core`, in the vector's own order: consume the ask, then enter
        // the scheduler, whose entry drains what was handed to that core before its pass.
        inline void dispatch_as(uint32_t core)
        {
            uint32_t const was = g_core;
            g_core = core;
            if (kickos_kernel_core_resched_take() != 0)
            {
                kickos_kernel_core_resched();
            }
            g_core = was;
        }
    }
}
#endif

#endif
