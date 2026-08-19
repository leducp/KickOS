// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 SPI bus SERVICE (see <kickos/driver/k64dspi.h>). This file owns NO register: the
// silicon is spi_dspi.cc and the wire choreography is <kickos/sys/spi_service.h>.
//
// THE SERVICE'S CLASS INSTANCE IS PRIVATE TO THIS LIBRARY: its four class symbols are renamed
// by the target's own compile definitions (CMakeLists.txt beside this file), because an image
// hosting this service may also hold a consumer that reached the public names through the
// proxy. A FIFTH call that forgets its rename is reported as a duplicate symbol ONLY where
// another definer of that public name (the proxy, the selftest's mock) is in the same link:
// spi_dspi.cc.o is extracted here by its renamed four regardless, so the missed name enters
// the link as a definition. Forgetting the rename on ALL FOUR is not reported at all; see the
// shadowing warning in <kickos/driver/spi.h>.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/k64dspi.h>

#include <kickos/driver/spi.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/emit.h> // publish-aware write; kos_print is dropped once published
#include <kickos/sys/service.h>
#include <kickos/sys/spi_service.h>

#include <stdint.h>
#include <stdlib.h>

namespace drv = kickos::driver;
namespace spi = kickos::spi;

namespace
{
    // KOS_CAP_NONE = not up, or already taken.
    kos_cap_t g_spi0_ep = KOS_CAP_NONE;

    // UNPRIVILEGED driver thread. The window base arrives as the arg VALUE, never
    // dereferenced as memory.
    void bus_thread(void* arg)
    {
        struct kos_spi_bus_config cfg;
        cfg.base = reinterpret_cast<uintptr_t>(arg);
        cfg.ep = KOS_CAP_NONE;  // a local engine reaches no endpoint
        cfg.irq = KOS_CAP_NONE; // the DSPI pump polls its FIFOs

        struct kos_spi_bus bus;
        if (kos_spi_bus_open(&bus, &cfg) < 0)
        {
            // emit, not kos_print: the console is already USER_OWNED here, so the kernel
            // chip path drops every byte.
            kickos::emit("[k64dspi] ERROR: bus bring-up refused, DSPI0 unreachable\n");
            // NO exit HERE: root keeps a WAIT-bearing cap on the endpoint under
            // KOS_DRV_EP_RETAIN, so recv_holders never reaches 0 when this thread dies, the
            // last-receiver-gone -KOS_EPIPE wake never fires, and a client parked in kos_call
            // would block forever.
            kos_panic("[k64dspi] bus bring-up refused (see the ERROR line above)");
        }

        kickos::emit("[k64dspi] SPI service up (DSPI0, polled FIFO, GPIO CS on PTC4)\n");

        spi::serve_loop(&bus);

        (void)kos_spi_bus_close(&bus);
        exit(0);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[k64dspi] ",
        // NO base guard: spi_dspi.cc is base-parameterised across DSPI0/1/2 and no vector is
        // claimed by number, so there is nothing to pin the cfg against. Adding a line makes
        // leg L9 demand one.
        .expected_base = 0,
        .block_size = 0, // no Shared, no ring, no doorbell, no readiness latch
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .svc_kind = KOS_SVC_SPI,
        .line_count = 0, // the DSPI pump polls its FIFOs
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        // The window grant is inert on coarse-AIPS for the peripheral itself, but it is what
        // AUTHORISES the driver's kos_periph_enable.
        .threads = {{.entry = bus_thread,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 1,
                     // WAIT only: the driver receives, it does not send or re-delegate.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the k64dspi descriptor is not a well-formed driver shape");
    static_assert(spi::desc_ok(k_desc), "the k64dspi cap positions do not match KOS_SPI_CAP_*");
}

extern "C"
{
    kos_cap_t k64dspi_take_endpoint(void)
    {
        kos_cap_t const ep = g_spi0_ep;
        g_spi0_ep = KOS_CAP_NONE; // one-shot: device slots are caller-named, so ONE client only
        return ep;
    }

    // Root KEEPS the full-rights cap so the app, on the same thread and table, can delegate a
    // SIGNAL-narrowed copy to each client.
    int k64dspi_spi_start(struct kos_service_cfg const* cfg)
    {
        return drv::bring_up(k_desc, cfg, &g_spi0_ep);
    }
}
