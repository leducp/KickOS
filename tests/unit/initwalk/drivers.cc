// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "drivers.h"

#include <kickos/sys/driver_service.h>

namespace drv = kickos::driver;

namespace drivers
{
    Behaviour behaviour;

    namespace
    {
        constexpr uint16_t READY_OFFSET = 8u;
        constexpr uint32_t CONSOLE_BLOCK = 1024u;

        unsigned char* g_block = nullptr;

        void uartirq(void*)
        {
        }

        void service(void*)
        {
        }

        void bus(void*)
        {
        }

        int console_block_init(void* blk, struct kos_service_cfg const*)
        {
            g_block = static_cast<unsigned char*>(blk);
            return 0;
        }

        // xmcuartirq's shape: the IRQ thread on the line and the notification, the receiver
        // ringing its doorbell, the barrier between them, the console handed over.
        constexpr drv::Descriptor k_console = {
            .tag = "[xmcuartirq] ",
            .expected_base = 0x40030000u,
            .block_size = CONSOLE_BLOCK,
            .block_flags = 0,
            .ready_offset = READY_OFFSET,
            .ep_posture = drv::KOS_DRV_EP_HANDOVER,
            .svc_kind = KOS_SVC_CONSOLE,
            .line_count = 1,
            .thread_count = 2,
            .barrier_after = 1,
            .lines = {{84, KOS_IRQ_EDGE}},
            .threads = {{.entry = uartirq,
                         .name = "uartirq",
                         .prio_delta = 1,
                         .arg = drv::KOS_DRV_ARG_BLOCK,
                         .window_grant = true,
                         .cap_count = 2,
                         .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                                  {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}},
                        {.entry = service,
                         .name = "service",
                         .prio_delta = 0,
                         .arg = drv::KOS_DRV_ARG_BLOCK,
                         .window_grant = false,
                         .cap_count = 2,
                         .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},
                                  {drv::KOS_DRV_RES_NOTIFY, KOS_CAP_SIGNAL, drv::doorbell_badge(1)}}}},
            .block_init = console_block_init
        };

        // xmcssc's shape: one thread holding the endpoint, the notification and the line, the
        // endpoint retained.
        constexpr drv::Descriptor k_spi = {
            .tag = "[xmcssc] ",
            .expected_base = 0x40030200u,
            .block_size = 0,
            .block_flags = 0,
            .ready_offset = drv::KOS_DRV_READY_NONE,
            .ep_posture = drv::KOS_DRV_EP_RETAIN,
            .svc_kind = KOS_SVC_SPI,
            .line_count = 1,
            .thread_count = 1,
            .barrier_after = 1,
            .lines = {{85, KOS_IRQ_EDGE}},
            .threads = {{.entry = bus,
                         .name = "bus",
                         .prio_delta = 0,
                         .arg = drv::KOS_DRV_ARG_WINDOW,
                         .window_grant = true,
                         .cap_count = 3,
                         .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},
                                  {drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                                  {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}}},
            .block_init = nullptr
        };

        void entry(void*)
        {
        }

        void worker(void*)
        {
        }

        // The test driver: an entry thread holding nothing, and a worker receiving on the endpoint.
        constexpr drv::Descriptor k_test = {
            .tag = "[testdrv] ",
            .expected_base = 0,
            .block_size = 0,
            .block_flags = 0,
            .ready_offset = drv::KOS_DRV_READY_NONE,
            .ep_posture = drv::KOS_DRV_EP_RETAIN,
            .svc_kind = KOS_SVC_SPI,
            .line_count = 0,
            .thread_count = 2,
            .barrier_after = 2,
            .lines = {},
            .threads = {{.entry = entry,
                         .name = "entry",
                         .prio_delta = 0,
                         .arg = drv::KOS_DRV_ARG_NONE,
                         .window_grant = false,
                         .cap_count = 0,
                         .caps = {}},
                        {.entry = worker,
                         .name = "worker",
                         .prio_delta = 0,
                         .arg = drv::KOS_DRV_ARG_NONE,
                         .window_grant = false,
                         .cap_count = 1,
                         .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
            .block_init = nullptr
        };

        static_assert(drv::valid(k_console) and drv::valid(k_spi) and drv::valid(k_test),
                      "a test driver is not a driver shape");
    }

    void spawned(fake::Thread const& thread)
    {
        if (thread.params.entry == uartirq and behaviour.latch and g_block != nullptr)
        {
            g_block[READY_OFFSET] = 1u;
        }
    }
}

extern "C"
{
    int xmcuartirq_console_start(struct kos_service_cfg const* cfg)
    {
        if (drivers::behaviour.uncached_console)
        {
            drv::Descriptor uncached = drivers::k_console;
            uncached.block_flags = KOS_MEM_NOCACHE;
            return drv::bring_up(uncached, cfg, nullptr);
        }
        return drv::bring_up(drivers::k_console, cfg, nullptr);
    }

    int testdrv_start(struct kos_service_cfg const* cfg)
    {
        return drv::bring_up(drivers::k_test, cfg, nullptr);
    }

    int xmc_spi0_start(struct kos_service_cfg const* cfg)
    {
        kos_cap_t ep = KOS_CAP_NONE;
        return drv::bring_up(drivers::k_spi, cfg, &ep);
    }
}
