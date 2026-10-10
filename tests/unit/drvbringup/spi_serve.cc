// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/spi_service.h>

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <string.h>

namespace spi = kickos::spi;

namespace
{
    struct kos_spi_bus g_bus;
    spi::SlotTable* g_slots = nullptr;
    uint32_t g_opens = 0u;
    uint32_t g_transfers = 0u;
}

extern "C"
{
    int32_t kos_spi_bus_open(struct kos_spi_bus* b, struct kos_spi_bus_config const* cfg)
    {
        b->base = 1u;
        b->ep = cfg->ep;
        b->irq = cfg->irq;
        return 0;
    }

    int32_t kos_spi_device_open(struct kos_spi_device* d, struct kos_spi_bus* b,
                                struct kos_spi_device_config const* cfg)
    {
        g_opens++;
        if (cfg->slot >= KOS_BUS_DEV_MAX)
        {
            return -KOS_EINVAL;
        }
        memset(d, 0, sizeof(*d));
        d->bus = b;
        d->hz = cfg->hz;
        d->slot = cfg->slot;
        d->word_bits = cfg->word_bits;
        return static_cast<int32_t>(cfg->hz);
    }

    // Runs on whatever handle it is given: refusing an unopened one is the service's job.
    int32_t kos_spi_transfer(struct kos_spi_device* d, struct kos_bus_seg const* seg,
                             uint8_t nseg, unsigned char* buf, uint32_t len)
    {
        g_transfers++;
        int32_t const bad = kos_spi_seg_check(seg, nseg, len);
        if (bad != 0)
        {
            return bad;
        }
        memset(buf, d->word_bits, len);
        return static_cast<int32_t>(len);
    }

    int32_t kos_spi_bus_close(struct kos_spi_bus* b)
    {
        b->base = 0u;
        return 0;
    }

    int32_t kos_call(kos_cap_t, void* buf, size_t send_len, size_t)
    {
        return static_cast<int32_t>(
            spi::serve_one(&g_bus, *g_slots, static_cast<unsigned char*>(buf), send_len));
    }
}

namespace
{
    size_t frame(unsigned char* buf, uint8_t op, uint8_t dev, uint8_t nseg)
    {
        struct kos_bus_req req = {};
        req.proto = KOS_BUS_SPI;
        req.op = op;
        req.device = dev;
        req.nseg = nseg;
        req.region_cap = -1;
        req.offset = 0;
        memcpy(buf, &req, sizeof(req));
        return sizeof(req);
    }

    int32_t xfer(uint8_t dev, size_t len, unsigned char* rx)
    {
        unsigned char buf[32];
        size_t framing = frame(buf, KOS_BUS_OP_XFER, dev, 1);
        struct kos_bus_seg seg = {};
        seg.len = static_cast<uint16_t>(len);
        memcpy(buf + framing, &seg, sizeof(seg));
        framing += sizeof(seg);
        memset(buf + framing, 0, len);

        int32_t const rc = kos_call(2, buf, framing + len, sizeof(buf));
        struct kos_bus_rsp rsp;
        memcpy(&rsp, buf, sizeof(rsp));
        if (rsp.status < 0)
        {
            return rsp.status;
        }
        EXPECT_EQ(static_cast<size_t>(rc), sizeof(rsp) + rsp.len);
        memcpy(rx, buf + sizeof(rsp), len);
        return rsp.len;
    }

    int32_t config(uint8_t dev, uint8_t word_bits)
    {
        unsigned char buf[32];
        size_t const framing = frame(buf, KOS_BUS_OP_CONFIG, dev, 0);
        struct kos_bus_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.hz = 1000000u;
        cfg.word_bits = word_bits;
        cfg.cs_policy = KOS_BUS_CS_NONE;
        memcpy(buf + framing, &cfg, sizeof(cfg));

        (void)kos_call(2, buf, framing + sizeof(cfg), sizeof(buf));
        struct kos_bus_rsp rsp;
        memcpy(&rsp, buf, sizeof(rsp));
        return rsp.status;
    }

    struct SpiServe : ::testing::Test
    {
        spi::SlotTable slots;

        void SetUp() override
        {
            memset(&g_bus, 0, sizeof(g_bus));
            struct kos_spi_bus_config bcfg = {0u, KOS_CAP_NONE, KOS_CAP_NONE, KOS_CAP_NONE, 0u, 0u};
            ASSERT_EQ(kos_spi_bus_open(&g_bus, &bcfg), 0);
            g_slots = &slots;
            g_opens = 0u;
            g_transfers = 0u;
        }

        void TearDown() override { g_slots = nullptr; }
    };

    TEST_F(SpiServe, each_slot_keeps_its_own_profile_after_another_is_configured)
    {
        EXPECT_EQ(config(0, 8), 0);
        EXPECT_EQ(config(1, 16), 0);
        unsigned char b0[2] = {0, 0};
        unsigned char b1[2] = {0, 0};
        EXPECT_EQ(xfer(0, sizeof(b0), b0), 2);
        EXPECT_EQ(b0[0], 8);
        EXPECT_EQ(b0[1], 8);
        EXPECT_EQ(xfer(1, sizeof(b1), b1), 2);
        EXPECT_EQ(b1[0], 16);
        EXPECT_EQ(b1[1], 16);
    }

    TEST_F(SpiServe, a_transfer_on_a_slot_never_configured_is_refused)
    {
        ASSERT_EQ(config(0, 8), 0);
        unsigned char sink[2];
        EXPECT_EQ(xfer(2, sizeof(sink), sink), -KOS_EINVAL);
        EXPECT_EQ(g_transfers, 0u) << "the backend ran a transfer on an unopened handle";
    }

    TEST_F(SpiServe, a_slot_past_the_table_is_refused_before_the_backend)
    {
        unsigned char sink[2];
        EXPECT_EQ(config(KOS_BUS_DEV_MAX, 8), -KOS_EINVAL);
        EXPECT_EQ(xfer(KOS_BUS_DEV_MAX, sizeof(sink), sink), -KOS_EINVAL);
        EXPECT_EQ(g_opens, 0u) << "a slot past the table reached the backend";
        EXPECT_EQ(g_transfers, 0u) << "a slot past the table reached the backend";
    }
}
