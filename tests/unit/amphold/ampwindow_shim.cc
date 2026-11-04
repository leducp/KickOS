// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/amp/ampwindow.cc, and the cold start of its state: one process runs every test, and a
// record, a strike or a generation a test leaves would reach the next one.

#include "../../../kernel/amp/ampwindow.cc"

#include <new>

#include "amp_hold_seam.h"

namespace kickos
{
    namespace amphold
    {
        void window_cold()
        {
            new (&amp::g_window) amp::Window{};
            for (uint32_t me = 0; me < amp::NODE_MAX; me++)
            {
                amp::g_minted[me] = 0u;
                new (&amp::g_counts[me]) amp::Counts{};
                for (uint32_t from = 0; from < amp::NODE_MAX; from++)
                {
                    amp::g_tail_strikes[me][from] = 0u;
                    amp::g_reply_gen[me][from] = 0u;
                    for (unsigned cls = 0; cls < static_cast<unsigned>(amp::Class::CLASS_MAX); cls++)
                    {
                        amp::g_depth_strikes[cls][me][from] = 0u;
                        amp::g_inbox[cls][me][from] = amp::Inbox{};
                    }
                    for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
                    {
                        amp::g_inbound[me][from][i] = amp::Inbound{};
                    }
                }
                for (uint32_t port = 0; port < amp::PORT_MAX; port++)
                {
                    amp::g_port_ep1[me][port] = 0u;
                }
            }
        }
    }
}
