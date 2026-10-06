// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The USB CDC class layer's bulk IN pump and the service's flush over a recording fake
// controller: a transfer that ends on a full packet is closed by a zero-length one, and the flush
// completes only once the host has taken it.

#include <kickos/sys/usb_cdc_service.h>

#include <stdint.h>
#include <string.h>

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
    namespace usb = kickos::usb;

    struct FakeDev
    {
        std::vector<uint32_t> in_lens;
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
        void setup_read(struct kos_usb_setup* out) { memset(out, 0, sizeof(*out)); }
        void bus_reset_recover() {}
        void set_address(uint8_t) {}
        void ep_open_all() {}
        void ep0_in(uint8_t const*, uint32_t, uint8_t) {}
        void ep0_out_arm(uint32_t, uint8_t) {}
        uint32_t ep0_out_read(uint8_t*, uint32_t) { return 0u; }
        void ep0_stall() {}
        void ep_in(uint8_t ep, uint8_t const*, uint32_t n, uint8_t)
        {
            if (ep == KOS_USB_CDC_EP_DATA)
            {
                in_lens.push_back(n);
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
