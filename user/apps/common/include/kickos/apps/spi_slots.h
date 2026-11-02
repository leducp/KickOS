// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two clients of one SPI bus service, each handed the bus endpoint badged with its own device
// slot by its composition: the holder opens slot 0 and transfers on it, the intruder opens its own
// slot 1 and is refused slot 0 on both ops, then prints the verdict. Judged by
// tests/integration/check_spi_slots.sh.

#ifndef KICKOS_APPS_SPI_SLOTS_H
#define KICKOS_APPS_SPI_SLOTS_H

#include <stdint.h>

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>

#include <kickos/driver/spi.h>

namespace kickos::apps::spi_slots
{
    // The slots the composition's `uses` hands each client.
    constexpr uint8_t HOLDER_SLOT = 0u;
    constexpr uint8_t INTRUDER_SLOT = 1u;

    // How the board's engine opens a device.
    struct Profile
    {
        char const* tag;
        uint32_t hz;
        uint8_t cs_policy;
    };

    class Run
    {
    public:
        explicit Run(Profile const& profile)
            : profile_(profile)
        {
        }

        void report(char const* what, bool ok, int32_t rc)
        {
            char s[112];
            char const* verdict = "PASS";
            if (not ok)
            {
                verdict = "FAIL";
                failures_++;
            }
            ksnprintf(s, sizeof(s), "[%s] %s: %s (rc=%d)\n", profile_.tag, what, verdict,
                      static_cast<int>(rc));
            kos::print(s);
        }

        // Opens the bus over the task's /svc/spi0 endpoint, or answers false.
        bool bus_open()
        {
            struct kos_spi_bus_config bcfg = {};
            bcfg.ep = kos_grant_endpoint("/svc/spi0");
            bcfg.irq = KOS_CAP_NONE;
            bcfg.notify = KOS_CAP_NONE;
            int32_t rc = -KOS_EBADF;
            if (bcfg.ep != KOS_CAP_NONE)
            {
                rc = kos_spi_bus_open(&bus_, &bcfg);
            }
            report("bus open", rc == 0, rc);
            return rc == 0;
        }

        // The achieved clock, or a negative kos_errno.
        int32_t device_open(struct kos_spi_device* dev, uint8_t slot)
        {
            struct kos_spi_device_config dcfg = {};
            dcfg.hz = profile_.hz;
            dcfg.slot = slot;
            dcfg.mode = 0u;
            dcfg.word_bits = 8u;
            dcfg.cs_policy = profile_.cs_policy;
            dcfg.cs_index = 0u;
            return kos_spi_device_open(dev, &bus_, &dcfg);
        }

        int32_t transfer(struct kos_spi_device* dev)
        {
            unsigned char buf[1] = {0xA5u};
            struct kos_bus_seg seg = {1u, 0u, 0u};
            return kos_spi_transfer(dev, &seg, 1u, buf, 1u);
        }

        bool failed() const
        {
            return failures_ != 0;
        }

        int verdict()
        {
            char s[48];
            if (failures_ != 0)
            {
                ksnprintf(s, sizeof(s), "[%s] FAIL (%d failed)\n", profile_.tag, failures_);
                kos::print(s);
                return 1;
            }
            ksnprintf(s, sizeof(s), "[%s] PASS\n", profile_.tag);
            kos::print(s);
            return 0;
        }

    private:
        Profile const profile_;
        struct kos_spi_bus bus_ = {};
        int failures_ = 0;
    };

    inline int holder(Profile const& profile)
    {
        Run r(profile);
        if (r.bus_open())
        {
            struct kos_spi_device dev = {};
            int32_t const hz = r.device_open(&dev, HOLDER_SLOT);
            r.report("holder opens its slot 0", hz > 0, hz);
            if (hz > 0)
            {
                int32_t const n = r.transfer(&dev);
                r.report("holder transfers on its slot 0", n == 1, n);
            }
        }
        if (r.failed())
        {
            return 1;
        }
        return 0;
    }

    inline int intruder(Profile const& profile)
    {
        Run r(profile);
        if (r.bus_open())
        {
            struct kos_spi_device own = {};
            int32_t const hz = r.device_open(&own, INTRUDER_SLOT);
            r.report("intruder opens its slot 1", hz > 0, hz);
            struct kos_spi_device stolen = {};
            int32_t const taken = r.device_open(&stolen, HOLDER_SLOT);
            r.report("intruder is refused the holder's slot 0", taken == -KOS_EACCES, taken);
            // A handle naming slot 0, as a client that skips the open would send it.
            stolen = own;
            stolen.slot = HOLDER_SLOT;
            int32_t const n = r.transfer(&stolen);
            r.report("intruder is refused a transfer on slot 0", n == -KOS_EACCES, n);
        }
        return r.verdict();
    }
}

#endif
