// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The USB CDC class layer's bulk IN pump and the service's flush over a recording fake
// controller: a transfer that ends on a full packet is closed by a zero-length one, the flush
// completes only once the host has taken it, and each configuration opens the stream with the
// identity rows.

#include <kickos/sys/usb_cdc_service.h>

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include <gtest/gtest.h>

extern "C"
{
    int kos_notify(kos_cap_t)
    {
        return 0;
    }

    void kos_sleep_ns(uint64_t)
    {
    }
}

namespace
{
    // Longer than one bulk packet, so the rows take two.
    char const IDENTITY[] = "\n   KickOS 0.0.0  -  microkernel RTOS\n   board   planted\n"
                            "   commit  0123abcd\n";
}

size_t kickos::banner_identity(char* out, size_t cap)
{
    size_t const n = sizeof(IDENTITY) - 1u;
    EXPECT_LT(n, cap);
    memcpy(out, IDENTITY, n + 1u);
    return n;
}

namespace
{
    namespace usb = kickos::usb;

    struct FakeDev
    {
        std::vector<uint32_t> in_lens;
        std::string in_bytes;
        struct kos_usb_setup setup = {};
        uint32_t events = 0u;
        uint32_t buff = 0u;

        int bring_up() { return 0; }
        void attach() {}
        uint32_t take_events()
        {
            uint32_t const e = events;
            events = 0u;
            return e;
        }
        uint32_t take_buff_status()
        {
            uint32_t const b = buff;
            buff = 0u;
            return b;
        }
        void setup_read(struct kos_usb_setup* out) { *out = setup; }
        void bus_reset_recover() {}
        void set_address(uint8_t) {}
        void ep_open_all() {}
        void ep0_in(uint8_t const*, uint32_t, uint8_t) {}
        void ep0_out_arm(uint32_t, uint8_t) {}
        uint32_t ep0_out_read(uint8_t*, uint32_t) { return 0u; }
        void ep0_stall() {}
        void ep_in(uint8_t ep, uint8_t const* p, uint32_t n, uint8_t)
        {
            if (ep == KOS_USB_CDC_EP_DATA)
            {
                in_lens.push_back(n);
                in_bytes.append(reinterpret_cast<char const*>(p), n);
            }
        }
        void ep_out_arm(uint8_t, uint8_t) {}
        uint32_t ep_out_read(uint8_t, uint8_t*, uint32_t) { return 0u; }
        void ep_stall(uint8_t, bool) {}
    };

    struct Rig
    {
        usb::Shared sh;
        FakeDev dev;
        usb::Cdc<FakeDev> cdc{dev, &sh};

        Rig()
        {
            kickos::console::shared_init<usb::Transport>(&sh);
            sh.configured = 1;
        }

        void queue(uint32_t n)
        {
            uint8_t bytes[256];
            memset(bytes, 'x', sizeof(bytes));
            ASSERT_EQ(kos_byte_ring_push(&sh.tx, bytes, n), n);
        }

        void pass() { cdc.service_irq(); }

        // The host selects configuration 1.
        void configure()
        {
            dev.setup = {};
            dev.setup.bmRequestType = KOS_USB_REQ_TYPE_STANDARD;
            dev.setup.bRequest = KOS_USB_SET_CONFIGURATION;
            dev.setup.wValue = 1u;
            dev.events = usb::KOS_USB_EV_SETUP;
            cdc.service_irq();
        }

        // The host takes every buffer until the controller holds none.
        void drain()
        {
            for (size_t before = dev.in_lens.size() + 1u; before != dev.in_lens.size();)
            {
                before = dev.in_lens.size();
                complete();
            }
        }

        // The host takes the bulk IN buffer the controller holds.
        void complete()
        {
            dev.events = usb::KOS_USB_EV_BUFFER;
            dev.buff = 1u << (2u * KOS_USB_CDC_EP_DATA);
            cdc.service_irq();
        }
    };
}

TEST(UsbZlp, a_transfer_ending_on_a_full_packet_is_closed_by_a_zero_length_one)
{
    Rig r;
    r.queue(KOS_USB_CDC_BULK_MAX_PACKET);
    r.pass();
    r.complete();
    std::vector<uint32_t> const want = {KOS_USB_CDC_BULK_MAX_PACKET, 0u};
    EXPECT_EQ(r.dev.in_lens, want);
    r.complete();
    r.pass();
    EXPECT_EQ(r.dev.in_lens, want) << "one zero-length packet closes the transfer";
}

TEST(UsbZlp, a_short_last_packet_needs_no_zero_length_one)
{
    Rig r;
    r.queue(KOS_USB_CDC_BULK_MAX_PACKET + 1u);
    r.pass();
    r.complete();
    r.complete();
    r.pass();
    std::vector<uint32_t> const want = {KOS_USB_CDC_BULK_MAX_PACKET, 1u};
    EXPECT_EQ(r.dev.in_lens, want);
}

TEST(UsbZlp, bytes_queued_behind_a_full_packet_continue_the_transfer)
{
    Rig r;
    r.queue(KOS_USB_CDC_BULK_MAX_PACKET);
    r.pass();
    r.queue(10u);
    r.complete();
    r.complete();
    r.pass();
    std::vector<uint32_t> const want = {KOS_USB_CDC_BULK_MAX_PACKET, 10u};
    EXPECT_EQ(r.dev.in_lens, want);
}

TEST(UsbZlp, the_flush_completes_only_once_the_closing_packet_was_taken)
{
    Rig r;
    r.queue(KOS_USB_CDC_BULK_MAX_PACKET);
    r.pass();
    r.complete();
    EXPECT_EQ(usb::Transport::flush(&r.sh), -KOS_EBUSY) << "the host's read is still open";
    r.complete();
    EXPECT_EQ(usb::Transport::flush(&r.sh), 0);
}

TEST(UsbZlp, a_bus_reset_owes_no_zero_length_packet)
{
    Rig r;
    r.queue(KOS_USB_CDC_BULK_MAX_PACKET);
    r.pass();
    r.dev.events = usb::KOS_USB_EV_BUS_RESET;
    r.pass();
    EXPECT_EQ(usb::Transport::flush(&r.sh), 0);
    r.sh.configured = 1;
    r.pass();
    std::vector<uint32_t> const want = {KOS_USB_CDC_BULK_MAX_PACKET};
    EXPECT_EQ(r.dev.in_lens, want);
}

TEST(UsbIdentity, each_configuration_opens_the_stream_with_the_rows_ahead_of_the_ring)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.queue(10u);
    r.pass();
    EXPECT_TRUE(r.dev.in_lens.empty()) << "nothing goes before a host configures the device";
    r.configure();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(IDENTITY) + std::string(10u, 'x'));
    EXPECT_EQ(r.dev.in_lens.back(), 10u) << "the ring's bytes follow in a packet of their own";
    r.configure();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(IDENTITY) + std::string(10u, 'x') + IDENTITY)
        << "a later configuration sends the rows again";
}

TEST(UsbIdentity, a_bus_reset_restarts_the_rows_and_counts_them_as_no_lost_byte)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    ASSERT_EQ(r.dev.in_lens.size(), 1u);
    r.dev.events = usb::KOS_USB_EV_BUS_RESET;
    r.pass();
    EXPECT_EQ(static_cast<uint32_t>(r.sh.tx_lost_link), 0u);
    EXPECT_EQ(usb::Transport::flush(&r.sh), 0);
    r.dev.in_bytes.clear();
    r.configure();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, IDENTITY);
}
