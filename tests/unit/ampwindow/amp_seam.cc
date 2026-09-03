// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The WHOLE seam between kernel/amp/ampwindow.cc and the rest of the image, re-derived with
//
//   nm --undefined-only <the object> | comm -23 - <its defined symbols>
//
// which is arch_cpu_id and arch_ipi_send at four nodes; kmemcpy folds to memcpy where no
// address space is enforced. One thread of execution runs the arms, so the doorbell only
// records: servicing it here would echo a payload an arm has not yet inspected.

#include "amp_seam.h"

#include <kickos/ampwindow.h>
#include <kickos/arch/arch.h>

static_assert(KICKOS_AMP_NODE,
              "this seam answers the AMP arm of the window; below it the whole translation "
              "unit compiles to nothing and no arm here is expressible");

namespace kickos
{
    namespace ampfix
    {
        uint32_t g_node = 0;
        uint32_t g_sends = 0;
        uint32_t g_sent_mask = 0;

        void reset()
        {
            // THE WINDOW AND THE MINT LIVE IN THE REAL ampwindow.cc, so this reseats those:
            // an arm that left a slot outstanding would hand it to the next, whose verdict
            // would then depend on the order GoogleTest ran them in.
            for (uint32_t to = 0; to < amp::NODE_MAX; to++)
            {
                for (uint32_t from = 0; from < amp::NODE_MAX; from++)
                {
                    amp::Ring& r = amp::ring_for(to, from);
                    r.head.v.store(0u);
                    r.tail.v.store(0u);
                    for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
                    {
                        r.slot[i].len = 0u;
                        r.slot[i].port = amp::PORT_MAX;
                        for (uint32_t b = 0; b < amp::SLOT_BYTES; b++)
                        {
                            r.slot[i].payload[b] = 0u;
                        }
                    }
                }
            }
            amp::window_init();
            g_node = 0;
            g_sends = 0;
            g_sent_mask = 0;
        }
    }
}

extern "C"
{

// THE GUARD SPELLING IS LOAD-BEARING AND MUST STAY EXACTLY THIS. check_cpu_id_fold.sh scans
// every tracked C or C++ file for a definition of arch_cpu_id and skips only a block opened by
// this literal line, so any other spelling makes this fixture a finding on every single-core
// preset in the fleet.
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void)
{
    return kickos::ampfix::g_node;
}

void arch_ipi_send(uint32_t cores)
{
    kickos::ampfix::g_sends++;
    kickos::ampfix::g_sent_mask |= cores;
}
#endif

}
