// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ampdiag.h>

#if defined(KICKOS_AMP_DIAG_REPORT) && KICKOS_AMP_DIAG_REPORT

#include <kickos/ampwindow.h>
#include <kickos/kernel.h>
#include <kickos/arch/amp_shared.h>
#include <kickos/sys/atomic.h>

#include <stdint.h>

namespace kickos
{
    namespace amp
    {
        namespace
        {
            // LAST in .amp_shared on every chip script: turning this on must move no object
            // above it, or the two images disagree about where the window and the cells are.
            //
            // [0] magic  [1] this node's index  [2] its exception vector base
            // [3] a build-stated tag  [4] the core clock this node derives its timing from.
            KICKOS_AMP_SHARED("diag")
            Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_cells[5] = {0u, 0u, 0u, 0u,
                                                                           0u};

            constexpr uint32_t MAGIC = 0x4B534431u;

            // Where this node's exception vectors are, read on the reporting node: the
            // register is core-local and no peer can see another core's. Zero where the arch
            // has no such register.
            uint32_t vector_base()
            {
#if defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) \
    || defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__)
                return *reinterpret_cast<volatile uint32_t*>(0xE000ED08u); // SCB->VTOR
#elif defined(__aarch64__)
                uint64_t v = 0;
                __asm volatile("mrs %0, vbar_el1" : "=r"(v));
                return static_cast<uint32_t>(v);
#else
                return 0u;
#endif
            }

            // Bounds the wait, so a partition whose peer never started reports instead of
            // hanging the primary at boot.
            constexpr uint32_t REPORT_SPINS = 20000000u;

#ifndef KICKOS_AMP_DIAG_TAG
#define KICKOS_AMP_DIAG_TAG 0u
#endif
        }

        void diag_peer_publish()
        {
            if (KICKOS_AMP_NODE_ID == 0)
            {
                return;
            }
            g_cells[1] = static_cast<uint32_t>(KICKOS_AMP_NODE_ID);
            g_cells[2] = vector_base();
            g_cells[3] = static_cast<uint32_t>(KICKOS_AMP_DIAG_TAG);
            g_cells[4] = arch_cpu_clock_hz();
            g_cells[0] = MAGIC; // last: the reader gates on this
        }

        void diag_primary_report()
        {
            if (KICKOS_AMP_NODE_ID != 0)
            {
                return;
            }
            // arch_amp_shared_zero clears this region on node 0 before any peer is
            // released, so a magic left by an earlier boot cannot read as this one's.
            uint32_t spins = 0;
            while (g_cells[0] != MAGIC)
            {
                spins++;
                if (spins > REPORT_SPINS)
                {
                    kprintf("# ampdiag: peer never reported (cell0=0x%x)\n",
                            static_cast<unsigned>(g_cells[0]));
                    return;
                }
            }
            kprintf("# ampdiag: peer node=%u vectors=0x%x tag=%u clk=%u self clk=%u\n",
                    static_cast<unsigned>(g_cells[1]), static_cast<unsigned>(g_cells[2]),
                    static_cast<unsigned>(g_cells[3]), static_cast<unsigned>(g_cells[4]),
                    static_cast<unsigned>(arch_cpu_clock_hz()));
        }

#if defined(KICKOS_ENABLE_SELFTEST)
        void diag_primary_doorbell_report()
        {
            if (KICKOS_AMP_NODE_ID != 0)
            {
                return;
            }
            for (uint32_t node = 0; node < NODE_MAX; node++)
            {
                // The cells are indexed by CORE and a node is not a core.
                uint32_t const core = core_of(node);
                uint32_t const bells = static_cast<uint32_t>(arch_ipi_counts(core));
                kprintf("# ampdiag: node=%u core=%u bells=%u drains=%u\n",
                        static_cast<unsigned>(node), static_cast<unsigned>(core),
                        static_cast<unsigned>(bells),
                        static_cast<unsigned>(counts(node).serviced.load()));
            }
        }
#else
        void diag_primary_doorbell_report()
        {
        }
#endif
    }
}

#endif
