// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Out-of-tree KickOS consumer app: the KickCAT LAN9252 EtherCAT slave on the
// Freedom-K64F over DSPI0. Neither the slave logic (freedom::run) nor the app shell
// (freedom::app_run -- console, state trace, run loop) live here: both are shared,
// CTT-proven sources compiled verbatim from the KickCAT example tree. This file is
// only the two KickOS-specific seams: the DSPI0 SPI backend (brought up on an
// UNPRIVILEGED slave thread under MPU enforcement) and the OPERATIONAL data source
// (a synthetic counter -- no accel/LED hardware on this bench; real support post-M3).

#include <kickos/kos.h>
#include <kickos/sys.h>

#include "spi_transport.h"
#include "freedom_app.h"

#include "kickcat/kickos/SPI.h"

#include <memory>

using namespace kickcat;

namespace
{
    // 16 KB slave-thread stack: pow2 + naturally aligned (a valid MPU region base
    // under enforcement; harmless when not enforced).
    KOS_STACK_DEFINE(g_slave_stack, 16384);

    struct KickosCtx
    {
        uint16_t tick;
    };

    // No accel/LED hardware: feed the accel TxPDO (0x6000..0x6005, slave->master) a
    // synthetic counter so the process data changes and the slave holds OP; the LED
    // RxPDO (0x7000..0x7002, master->slave) is accepted and ignored.
    void kickos_data_source(void* vctx, freedom::Pdo const& io)
    {
        KickosCtx* ctx = static_cast<KickosCtx*>(vctx);
        ctx->tick++;
        int16_t const v = static_cast<int16_t>(ctx->tick);
        if (io.ax != nullptr)
        {
            *io.ax = v;
        }
        if (io.ay != nullptr)
        {
            *io.ay = static_cast<int16_t>(-v);
        }
        if (io.az != nullptr)
        {
            *io.az = 1000;
        }
        if (io.mx != nullptr)
        {
            *io.mx = 0;
        }
        if (io.my != nullptr)
        {
            *io.my = 0;
        }
        if (io.mz != nullptr)
        {
            *io.mz = 0;
        }
    }

    void slave_thread(void*)
    {
        auto spi = std::make_shared<kickcat::SPI>();
        spi->open("dspi0", 0, 0, 1900000); // CPOL=0 CPHA=0 (mode 0); transport sets the real baud

        KickosCtx ctx;
        ctx.tick = 0;

        freedom::app_run(spi, kickos_data_source, &ctx);
    }
}

int main(int, char**)
{
    kos::print("[freedom-slave] KickCAT LAN9252 slave on K64F/DSPI0\n");

    int r = spi_driver_start(0); // 0 = real-ESC baud/mux (not loopback)
    if (r < 0)
    {
        kos::print("[freedom-slave] ERROR: spi_driver_start failed\n");
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }

    // Unprivileged, no MMIO grant: the slave reaches the DSPI window only through
    // the transport's driver thread. Only the driver thread owns the DSPI window.
    int th = kos::thread::spawn(slave_thread, nullptr, "slave", 10, KOS_POLICY_FIFO, 0,
                                /*privileged=*/false,
                                /*mem=*/nullptr, /*mem_size=*/0,
                                /*stack=*/g_slave_stack, /*stack_size=*/sizeof(g_slave_stack),
                                /*mmio=*/nullptr, /*mmio_size=*/0);
    if (th < 0)
    {
        kos::print("[freedom-slave] ERROR: could not spawn slave thread\n");
        while (true)
        {
            kos_sleep_ns(1000000000ull);
        }
    }

    // Returning would hit arch_shutdown; the slave runs forever, so park here.
    while (true)
    {
        kos_sleep_ns(1000000000ull);
    }
}
