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

#include <kickos/driver/declared/k64dspi.h>
#include <kickos/driver/spi.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/spi_service.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace spi = kickos::spi;
namespace declared = kickos::driver::declared::k64dspi;

namespace
{
    // UNPRIVILEGED driver thread. The window base arrives as the arg VALUE, never
    // dereferenced as memory.
    void bus_thread(void* arg)
    {
        struct kos_spi_bus_config cfg;
        cfg.base = reinterpret_cast<uintptr_t>(arg);
        cfg.ep = KOS_CAP_NONE;  // a local engine reaches no endpoint
        cfg.irq = KOS_CAP_NONE; // the DSPI pump polls its FIFOs
        cfg.notify = KOS_CAP_NONE; // and so blocks on nothing
        cfg.notify_bit = 0;
        cfg.irq_index = 0u;

        struct kos_spi_bus bus;
        int32_t const opened = kos_spi_bus_open(&bus, &cfg);
        if (opened < 0)
        {
            kos::print("[k64dspi] ERROR: bus bring-up refused, DSPI0 unreachable\n");
            drv::trap();
        }

        kos::print("[k64dspi] SPI service up (DSPI0, polled FIFO, GPIO CS)\n");

        (void)spi::serve_loop(&bus);

        (void)kos_spi_bus_close(&bus);
        drv::trap();
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[k64dspi] ",
        // NO base guard: spi_dspi.cc is base-parameterised across DSPI0/1/2.
        .expected_base = 0,
        .block_size = declared::k_declared.block_size, // no Shared, ring, doorbell or latch
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = declared::k_declared.ep_posture,
        .line_count = declared::k_declared.line_count, // the DSPI pump polls its FIFOs
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        .lines = {},
        // The window grant is inert on coarse-AIPS for the peripheral itself, but it is what
        // AUTHORISES the driver's kos_periph_enable.
        .threads = {{.entry = bus_thread,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 1,
                     // WAIT only: the driver receives, it does not send or re-delegate.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the k64dspi descriptor is not a well-formed driver shape");
    static_assert(spi::desc_ok(k_desc), "the k64dspi cap positions do not match KOS_SPI_CAP_*");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the k64dspi descriptor departs from its kickos_add_driver declaration");
}

extern "C"
{
    int k64dspi_spi_start(struct kos_driver_instance* instance)
    {
        // Here, on the init's thread: the driver's threads hold no pinmux authority.
        int32_t const muxed = k64dspi_bus_mux(instance->mmio_base);
        if (muxed != 0)
        {
            kos::print("[k64dspi] ERROR: the board bus over the DSPI window was not muxed\n");
            return muxed;
        }
        return drv::bring_up(k_desc, instance);
    }
}
