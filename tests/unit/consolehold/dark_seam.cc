// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch console and the device window under the REAL kernel/init/console.cc and
// kernel/init/console_tx.cc, for the K-seam built with KSEAM_REAL_CONSOLE: a polled device whose
// bytes are recorded, a reclaim that is counted, and a register window held until a test frees it.

#include <string>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>

#include "dark_seam.h"

namespace darkseam
{
    std::string g_wire;
    uint32_t g_reclaims = 0;
    bool g_window_free = true;

    void reset()
    {
        g_wire.clear();
        g_reclaims = 0;
        g_window_free = true;
    }

    bool publish(kickos::Thread* publisher, kickos::Endpoint* ep, kickos::Task* served)
    {
        using namespace kickos;
        {
            IrqLock lock;
            int const handle = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
            if (not cap_console_publish(publisher, handle, lock))
            {
                return false;
            }
            cap_console_serve(served, lock);
            console_handover_begin();
        }
        console_owner_set_user();
        return true;
    }
}

extern "C"
{
    bool arch_console_write_sync(char const* buf, size_t n)
    {
        darkseam::g_wire.append(buf, n);
        return true;
    }

    void arch_console_flush_sync(void)
    {
    }

    void arch_console_reclaim(void)
    {
        darkseam::g_reclaims = darkseam::g_reclaims + 1;
    }

    struct console_tx_backend const* arch_console_tx_backend(int*)
    {
        return nullptr;
    }

#if KICKOS_CONSOLE_RTT
    void kickos_rtt_write(char const*, size_t) {}
#endif
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t)
    {
        return darkseam::g_window_free;
    }
}
