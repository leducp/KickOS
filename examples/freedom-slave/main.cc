// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Out-of-tree KickOS consumer app: the KickCAT LAN9252 EtherCAT slave on the
// Freedom-K64F over DSPI0, ported off KickCAT's NuttX freedom-k64f example. The
// NuttX device/ioctl/sensor/LED plumbing is dropped; the ESC talks to the LAN9252
// through the KickOS unprivileged DSPI0 transport (spi_transport.{h,cc}). The
// privileged app main brings DSPI0 up (spi_driver_start) and spawns the slave in
// an unprivileged thread; the slave never touches MMIO.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include "spi_transport.h"

#include "kickcat/kickos/SPI.h"
#include "kickcat/ESC/Lan9252.h"
#include "kickcat/slave/Slave.h"
#include "kickcat/PDO.h"
#include "kickcat/Mailbox.h"
#include "kickcat/CoE/OD.h"
#include "kickcat/protocol.h"

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

    // KickCAT throws (THROW_SYSTEM_ERROR_CODE); an exception escaping the thread
    // entry would terminate the image, so the whole body is wrapped in try/catch.
    void slave_thread(void*)
    {
        try
        {
            auto spi = std::make_shared<kickcat::SPI>();
            spi->open("dspi0", 0, 0, 1900000); // CPOL=0 CPHA=0 (mode 0), ~1.9 MHz

            kickcat::Lan9252 esc(spi);
            int32_t rc = esc.init();
            if (rc != 0)
            {
                char line[64];
                ksnprintf(line, sizeof(line), "[slave] Lan9252 init failed rc=%ld\n",
                          static_cast<long>(rc));
                kos::print(line);
                return;
            }
            kos::print("[slave] Lan9252 init ok\n");

            kickcat::PDO pdo(&esc);
            kickcat::slave::Slave slave(&esc, &pdo);

            // Persist for the thread's lifetime (the thread never returns). The
            // master may map fewer bytes; these are the space it plays with.
            uint8_t buffer_in[16] = {};
            uint8_t buffer_out[16] = {};
            pdo.setInput(buffer_in, 16);
            pdo.setOutput(buffer_out, 16);

            kickcat::mailbox::response::Mailbox mbx(&esc, 1024);
            auto dict = CoE::createOD();
            mbx.enableCoE(dict);          // the firmware owns the OD; the mailbox references it
            slave.setMailbox(&mbx);
            slave.setDictionary(&dict);   // and the slave uses it for bind / PDO mapping

            slave.start();

            // Process-data pointers, bound once at SAFE_OP. No accel/LED hardware on
            // this bench: the accel TxPDO (0x6000..0x6005, slave->master) is fed a
            // synthetic counter so the process data changes and the slave holds OP; the
            // LED RxPDO (0x7000..0x7002, master->slave) is accepted and ignored.
            int16_t* ax = nullptr;
            int16_t* ay = nullptr;
            int16_t* az = nullptr;
            int16_t* mx = nullptr;
            int16_t* my = nullptr;
            int16_t* mz = nullptr;
            uint8_t* led_r = nullptr;
            uint8_t* led_g = nullptr;
            uint8_t* led_b = nullptr;
            bool bound = false;
            uint16_t tick = 0;

            State last = State::INVALID;
            uint64_t last_hb = kos_clock_now();
            while (true)
            {
                slave.routine();

                State now = slave.state();
                if (now != last)
                {
                    char line[64];
                    ksnprintf(line, sizeof(line), "[slave] state -> %s\n", state_name(now));
                    kos::print(line);
                    last = now;
                }

                // A fresh master session drops the slave back to INIT/PRE_OP and
                // reconfigures SM2/SM3; clear the latch so the PDO is re-bound at the
                // next SAFE_OP (a persistent daemon outlives each master run).
                if (now == State::INIT or now == State::PRE_OP)
                {
                    bound = false;
                }

                // Map the PDO entries once the master has reached SAFE_OP, then ack the
                // output data -- this wires SM2/SM3 so the master can drive OPERATIONAL.
                if (now == State::SAFE_OP and not bound)
                {
                    slave.bind(0x6000, ax);
                    slave.bind(0x6001, ay);
                    slave.bind(0x6002, az);
                    slave.bind(0x6003, mx);
                    slave.bind(0x6004, my);
                    slave.bind(0x6005, mz);
                    slave.bind(0x7000, led_r);
                    slave.bind(0x7001, led_g);
                    slave.bind(0x7002, led_b);
                    slave.validateOutputData();
                    bound = true;
                }

                // In OP, refresh the TxPDO each cycle with synthetic accel data.
                if (now == State::OPERATIONAL and bound)
                {
                    tick++;
                    int16_t const v = static_cast<int16_t>(tick);
                    if (ax != nullptr)
                    {
                        *ax = v;
                    }
                    if (ay != nullptr)
                    {
                        *ay = static_cast<int16_t>(-v);
                    }
                    if (az != nullptr)
                    {
                        *az = 1000;
                    }
                    if (mx != nullptr)
                    {
                        *mx = 0;
                    }
                    if (my != nullptr)
                    {
                        *my = 0;
                    }
                    if (mz != nullptr)
                    {
                        *mz = 0;
                    }
                }

                // 1 Hz heartbeat: proves routine() keeps looping and shows whether the
                // transport is issuing WRITES (mailbox responses) when the master reads
                // an SDO -- the read-vs-write signal for the CS-release-write suspect.
                uint64_t t = kos_clock_now();
                if (t - last_hb >= 1000000000ull)
                {
                    last_hb = t;
                    uint32_t rd = 0;
                    uint32_t wr = 0;
                    uint32_t toolong = 0;
                    uint32_t maxlen = 0;
                    spi_transfer_stats(&rd, &wr);
                    spi_transfer_diag(&toolong, &maxlen);
                    char line[112];
                    ksnprintf(line, sizeof(line),
                              "[slave] hb state=%s rd=%lu wr=%lu toolong=%lu maxlen=%lu\n",
                              state_name(now), static_cast<unsigned long>(rd),
                              static_cast<unsigned long>(wr),
                              static_cast<unsigned long>(toolong),
                              static_cast<unsigned long>(maxlen));
                    kos::print(line);
                }
            }
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
