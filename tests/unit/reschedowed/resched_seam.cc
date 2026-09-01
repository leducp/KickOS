// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The WHOLE seam between kernel/sync/klock.cc and the rest of the image, re-derived with
//
//   nm --undefined-only <the object> | comm -23 - <its defined symbols>
//
// which is 5 symbols at two kernel cores. kpanic and the instance below answer the ARMS rather
// than that object, so they are not in that set. One thread of execution runs the arms, so
// arch_kernel_lock and arch_kernel_unlock only count.

#include "resched_seam.h"

#include <stdio.h>
#include <stdlib.h>

#include <kickos/arch/arch.h>
#include <kickos/instance.h>

static_assert(KICKOS_NUM_CORES > 1 and KICKOS_KERNEL_CORES > 1,
              "this seam answers the multi-core arm of every declaration below; at one core "
              "the lock and the core identity are macros and these are redefinitions");

namespace kickos
{
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;
    }

    namespace reschedfix
    {
        uint32_t g_core = 0;
        uint32_t g_raised[CORES] = {};
        uint32_t g_acquired = 0;
        uint32_t g_released = 0;
        uint32_t g_held_at_raise = 0;
        void (*g_raise_action)() = nullptr;
        uint32_t g_sends = 0;
        uint32_t g_sent_mask = 0;
        uint32_t g_owed_at_send[CORES] = {};

        uint32_t raise_total()
        {
            uint32_t total = 0;
            for (uint32_t core = 0; core < CORES; core++)
            {
                total += g_raised[core];
            }
            return total;
        }

        void reset()
        {
            // THE CELLS LIVE IN THE REAL klock.cc AND NO SEAM VARIABLE MIRRORS THEM: an arm
            // that leaves one standing would otherwise hand it to the next arm, whose verdict
            // then depends on the order GoogleTest ran them in.
            for (uint32_t core = 0; core < CORES; core++)
            {
                g_core = core;
                (void)kickos_kernel_core_resched_take();
            }
            g_core = 0;
            for (uint32_t core = 0; core < CORES; core++)
            {
                g_raised[core] = 0;
            }
            g_acquired = 0;
            g_released = 0;
            g_held_at_raise = 0;
            g_raise_action = nullptr;
            g_sends = 0;
            g_sent_mask = 0;
            for (uint32_t core = 0; core < CORES; core++)
            {
                g_owed_at_send[core] = 0;
            }
        }
    }

    void kpanic(char const* msg)
    {
        printf("KERNEL PANIC: %s\n", msg);
        fflush(stdout);
        abort();
    }
}

extern "C"
{

#if KICKOS_NUM_CORES > 1
// At one core arch_cpu_id is a macro folding to a literal and no source in the tree may
// define it.
uint32_t arch_cpu_id(void)
{
    return kickos::reschedfix::g_core;
}
#endif

arch_irq_state_t arch_irq_save(void)
{
    return 0;
}

void arch_irq_restore(arch_irq_state_t)
{
}

void arch_kernel_lock(void)
{
    kickos::reschedfix::g_acquired++;
}

void arch_kernel_unlock(void)
{
    kickos::reschedfix::g_released++;
}

// The cross-core raise the ask goes over. THE CELL IS SAMPLED FROM THE TARGET'S SEAT, which is
// what a real target's absorbing consumer would read: the raise is an edge and the poll in an
// acquire loop acknowledges it without entering any scheduler, so the ask has to already stand
// here or the edge can be absorbed before anything says one was owed.
void arch_ipi_send(uint32_t cores)
{
    kickos::reschedfix::g_sends++;
    kickos::reschedfix::g_sent_mask |= cores;
    uint32_t const was = kickos::reschedfix::g_core;
    for (uint32_t core = 0; core < kickos::reschedfix::CORES; core++)
    {
        if ((cores & (1u << core)) == 0)
        {
            continue;
        }
        kickos::reschedfix::g_core = core;
        if (kickos_kernel_core_resched_owed() != 0)
        {
            kickos::reschedfix::g_owed_at_send[core] = 1u;
        }
    }
    kickos::reschedfix::g_core = was;
}

// Where the doorbell would be raised on this core. An armed action fires here and is disarmed
// first, so an action that asks again cannot arm itself.
void arch_ipi_resched_self(void)
{
    kickos::reschedfix::g_raised[kickos::reschedfix::g_core]++;
    uint32_t const held = kickos::reschedfix::g_acquired - kickos::reschedfix::g_released;
    if (held > kickos::reschedfix::g_held_at_raise)
    {
        kickos::reschedfix::g_held_at_raise = held;
    }
    void (*const action)() = kickos::reschedfix::g_raise_action;
    if (action != nullptr)
    {
        kickos::reschedfix::g_raise_action = nullptr;
        action();
    }
}

}
