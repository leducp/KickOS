// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The partition's node-to-core map: the ONE place a node index becomes a hardware core mask.
//
// A node index and a core index coincide only under the shared image, where a node's identity
// IS the core register. Under one image per node they are unrelated, so the map is stated
// (KICKOS_AMP_NODE_CORES) and CMakeLists.txt refuses a build that leaves it unstated.

#include <kickos/ampwindow.h>

#if KICKOS_AMP_NODE

#include <kickos/arch/arch.h>

namespace kickos
{
    namespace amp
    {
#if KICKOS_AMP_OWN_IMAGE
        namespace
        {
            // One entry per node, in node order.
            constexpr uint32_t NODE_CORE[] = KICKOS_AMP_NODE_CORE_LIST;
            static_assert(sizeof(NODE_CORE) / sizeof(NODE_CORE[0]) == NODE_MAX,
                          "the map owes a core for every node the partition holds");
        }
#endif

        uint32_t core_of(uint32_t node)
        {
            if (node >= NODE_MAX)
            {
                return 0u;
            }
#if KICKOS_AMP_OWN_IMAGE
            return NODE_CORE[node];
#else
            return node;
#endif
        }

        void ring(uint32_t to)
        {
            if (to >= NODE_MAX)
            {
                return;
            }
#if KICKOS_AMP_OWN_IMAGE
            arch_ipi_send(1u << NODE_CORE[to]);
#else
            arch_ipi_send(1u << to);
#endif
        }
    }
}

extern "C" uint32_t kickos_amp_node_core(uint32_t node)
{
    return ::kickos::amp::core_of(node);
}

#endif
