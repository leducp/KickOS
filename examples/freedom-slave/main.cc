// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Out-of-tree KickOS consumer app: the KickCAT LAN9252 EtherCAT slave on the
// Freedom-K64F over DSPI0. The slave logic itself is NOT here -- it is the shared,
// CTT-proven core (freedom::run) compiled verbatim from the KickCAT example tree.
// This file is only the KickOS-specific shell around it: DSPI0 bring-up, an
// UNPRIVILEGED slave thread (MPU enforcement; the slave never touches MMIO), the
// synthetic process-data source (no accel/LED hardware on this bench), the kos::print
// console sink, and the transport heartbeat/SPI-counter diagnostics.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include "spi_transport.h"
#include "freedom_slave.h"

#include "kickcat/kickos/SPI.h"

#include <exception>
#include <memory>
#ifdef FREEDOM_SLAVE_COE_TRACE
#include <cstdio>
#endif

using namespace kickcat;

namespace
{
    // 16 KB slave-thread stack: pow2 + naturally aligned (a valid MPU region base
    // under enforcement; harmless when not enforced).
    KOS_STACK_DEFINE(g_slave_stack, 16384);

    char const* state_name(State state)
    {
        if (state == State::INIT)
        {
            return "INIT";
        }
        if (state == State::PRE_OP)
        {
            return "PRE_OP";
        }
        if (state == State::BOOT)
        {
            return "BOOT";
        }
        if (state == State::SAFE_OP)
        {
            return "SAFE_OP";
        }
        if (state == State::OPERATIONAL)
        {
            return "OPERATIONAL";
        }
        return "INVALID";
    }

    // KickOS-specific state carried across the shared core's per-cycle hooks.
    struct KickosCtx
    {
        uint16_t tick;
        State last;
        uint64_t last_hb;
    };

    void kickos_log(void*, char const* line)
    {
        kos::print(line);
    }

    // No accel/LED hardware: feed the accel TxPDO (0x6000..0x6005, slave->master) a
    // synthetic counter so the process data changes and the slave holds OP; the LED
    // RxPDO (0x7000..0x7002, master->slave) is accepted and ignored.
    void kickos_on_operational(void* vctx, freedom::Pdo const& io)
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

    // Transport diagnostics kept out of the shared core: a state-change trace and a
    // 1 Hz heartbeat showing whether the transport is issuing WRITES (mailbox
    // responses) when the master reads an SDO -- the read-vs-write signal for the
    // CS-release-write suspect.
    void kickos_on_cycle(void* vctx, State state)
    {
        KickosCtx* ctx = static_cast<KickosCtx*>(vctx);

        if (state != ctx->last)
        {
            char line[64];
            ksnprintf(line, sizeof(line), "[slave] state -> %s\n", state_name(state));
            kos::print(line);
            ctx->last = state;
        }

        uint64_t t = kos_clock_now();
        if (t - ctx->last_hb >= 1000000000ull)
        {
            ctx->last_hb = t;
            uint32_t rd = 0;
            uint32_t wr = 0;
            uint32_t toolong = 0;
            uint32_t maxlen = 0;
            spi_transfer_stats(&rd, &wr);
            spi_transfer_diag(&toolong, &maxlen);
            char line[112];
            ksnprintf(line, sizeof(line),
                      "[slave] hb state=%s rd=%lu wr=%lu toolong=%lu maxlen=%lu\n",
                      state_name(state), static_cast<unsigned long>(rd),
                      static_cast<unsigned long>(wr),
                      static_cast<unsigned long>(toolong),
                      static_cast<unsigned long>(maxlen));
            kos::print(line);
        }
    }

    // KickCAT throws (THROW_SYSTEM_ERROR_CODE); an exception escaping the thread
    // entry would terminate the image, so the whole body is wrapped in try/catch.
    void slave_thread(void*)
    {
        try
        {
            auto spi = std::make_shared<kickcat::SPI>();
            spi->open("dspi0", 0, 0, 1900000); // CPOL=0 CPHA=0 (mode 0), ~1.9 MHz

            KickosCtx ctx;
            ctx.tick = 0;
            ctx.last = State::INVALID;
            ctx.last_hb = kos_clock_now();

            freedom::Hooks hooks;
            hooks.ctx = &ctx;
            hooks.log = kickos_log;
            hooks.on_operational = kickos_on_operational;
            hooks.on_cycle = kickos_on_cycle;

            freedom::run(spi, hooks);
        }
        catch (std::exception const& e)
        {
            kos::print("[slave] exception: ");
            kos::print(e.what());
            kos::print("\n");
        }
    }
}

int main(int, char**)
{
    kos::print("[freedom-slave] KickCAT LAN9252 slave on K64F/DSPI0\n");

#ifdef FREEDOM_SLAVE_COE_TRACE
    // KickCAT _info traces go to stdout (fully buffered on newlib); force unbuffered so
    // the mailbox/CoE flow reaches the console live during bring-up.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
#endif

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
