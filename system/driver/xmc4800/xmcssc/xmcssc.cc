// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800/USIC0-CH1 SSC (SPI) bus SERVICE (see <kickos/driver/xmcssc.h>). This file owns NO
// register: the silicon is spi_usic.cc and the wire choreography is <kickos/sys/spi_service.h>.
//
// THE SERVICE'S CLASS INSTANCE IS PRIVATE TO THIS LIBRARY: its four class symbols are renamed
// by the target's own compile definitions (CMakeLists.txt beside this file), because an image
// hosting this service may also hold a consumer that reached the public names through the
// proxy. A FIFTH call that forgets its rename is reported as a duplicate symbol ONLY where
// another definer of that public name (the proxy, the selftest's mock) is in the same link:
// spi_usic.cc.o is extracted here by its renamed four regardless, so the missed name enters
// the link as a definition. Forgetting the rename on ALL FOUR is not reported at all; see the
// shadowing warning in <kickos/driver/spi.h>.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcssc.h>

#include <kickos/driver/spi.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/emit.h> // publish-aware write; kos_print is dropped once published
#include <kickos/sys/service.h>
#include <kickos/sys/spi_service.h>

#include <kickos/chip_mmap.h>

#include <stdint.h>
#include <stdlib.h>

namespace drv = kickos::driver;
namespace spi = kickos::spi;
namespace mmap = kickos::xmc::mmap;

namespace
{
    constexpr int USIC0_SR1_IRQ = 85; // RM Table 4-3

    // KOS_CAP_NONE = not up, or already taken.
    kos_cap_t g_spi0_ep = KOS_CAP_NONE;

    // UNPRIVILEGED driver thread. The window base arrives as the arg VALUE, never dereferenced
    // as memory. Diagnostics go through emit, not kos_print: the console is already USER_OWNED
    // here, so the kernel chip path drops every byte.
    //
    // NO FAILURE PATH MAY exit: root KEEPS a WAIT-bearing cap on E under KOS_DRV_EP_RETAIN, so
    // recv_holders never reaches 0 when this thread dies, the last-receiver-gone -KOS_EPIPE
    // wake never fires, and a client parked in kos_call would block forever. A bring-up refusal
    // panics instead. serve_loop returns only after an EPIPE, which means no client is parked.
    void bus_thread(void* arg)
    {
        // The line is already owned before the bus open arms RIEN/AIEN, which is the ordering
        // the first receive event needs.
        struct kos_spi_bus_config cfg;
        cfg.base = reinterpret_cast<uintptr_t>(arg);
        cfg.ep = KOS_CAP_NONE; // a local engine reaches no endpoint
        cfg.irq = spi::KOS_SPI_CAP_LINE;

        struct kos_spi_bus bus;
        if (kos_spi_bus_open(&bus, &cfg) < 0)
        {
            kickos::emit("[xmcssc] ERROR: channel bring-up refused (a PV register store was "
                         "discarded)\n");
            kos_panic("[xmcssc] channel bring-up refused (see the ERROR line above)");
        }

        kickos::emit("[xmcssc] SPI service up (USIC0-CH1 SSC, IRQ-paced, HW CS on SELO0)\n");

        spi::serve_loop(&bus);

        (void)kos_spi_bus_close(&bus);
        exit(0);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[xmcssc] ",
        // SR1 below is claimed BY NUMBER, so a cfg naming the sibling channel would grant one
        // window and interrupt on the other. The console owns U0C0; SPI is U0C1.
        .expected_base = mmap::USIC0_CH1_BASE,
        .block_size = 0, // no Shared, no ring, no doorbell, no readiness latch
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .svc_kind = KOS_SVC_SPI,
        .line_count = 1,
        .thread_count = 1,
        .barrier_after = 1,
        // EDGE: the receive flags are W1C'd by the engine before it acks.
        .lines = {{USIC0_SR1_IRQ, KOS_IRQ_EDGE}},
        // No register access by root: it holds no DEV region at all (ARCH_MPU_DEV is attached
        // only by thread_spawn), so this thread is the only one that can address the channel.
        // USIC0's module clock is already ungated by the console (U0C0) bring-up.
        .threads = {{.entry = bus_thread,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .mem_grant = false,
                     .window_grant = true,
                     .cap_count = 2,
                     // Both WAIT only: the driver receives and services, it does not send,
                     // ring its own doorbell, or re-delegate.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the xmcssc descriptor is not a well-formed driver shape");
    static_assert(spi::desc_ok(k_desc), "the xmcssc cap positions do not match KOS_SPI_CAP_*");
}

extern "C"
{
    kos_cap_t xmc_spi0_take_endpoint(void)
    {
        kos_cap_t const ep = g_spi0_ep;
        g_spi0_ep = KOS_CAP_NONE; // one-shot: device slots are caller-named, so ONE client only
        return ep;
    }

    // Root KEEPS the full-rights cap so the app, on the same thread and table, can delegate a
    // SIGNAL-narrowed copy to each client.
    int xmc_spi0_start(struct kos_service_cfg const* cfg)
    {
        return drv::bring_up(k_desc, cfg, &g_spi0_ep);
    }
}
