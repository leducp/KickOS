// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The USB CDC class layer's bulk IN pump and the service's flush over a recording fake
// controller: a transfer that ends on a full packet is closed by a zero-length one, the flush
// completes only once the host has taken it, each configuration opens the stream with the
// identity rows, output is held until the host's terminal is ready for it, and nothing leaves
// the ring while the bus is suspended, an unplug included.

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

    uint64_t g_now = 1000u;

    uint64_t kos_clock_now(void)
    {
        return g_now;
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
        uint32_t ep0_out_read(uint8_t* out, uint32_t max)
        {
            uint32_t const n = KOS_CDC_LINE_CODING_LEN;
            EXPECT_LE(n, max);
            memset(out, 0, n);
            return n;
        }
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
            configure();
            open();
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

        void line_state(uint16_t bits)
        {
            dev.setup = {};
            dev.setup.bmRequestType = KOS_USB_REQ_TYPE_CLASS;
            dev.setup.bRequest = KOS_CDC_SET_CONTROL_LINE_STATE;
            dev.setup.wValue = bits;
            dev.events = usb::KOS_USB_EV_SETUP;
            cdc.service_irq();
        }

        // SET_LINE_CODING's setup, then its data stage.
        void line_coding()
        {
            dev.setup = {};
            dev.setup.bmRequestType = KOS_USB_REQ_TYPE_CLASS;
            dev.setup.bRequest = KOS_CDC_SET_LINE_CODING;
            dev.setup.wLength = KOS_CDC_LINE_CODING_LEN;
            dev.events = usb::KOS_USB_EV_SETUP;
            cdc.service_irq();
            dev.events = usb::KOS_USB_EV_BUFFER;
            dev.buff = 1u << 1u; // EP0 OUT
            cdc.service_irq();
        }

        // What Linux's cdc_acm sends for an opener that sets its termios: DTR, then the line
        // coding from inside tcsetattr.
        void open()
        {
            line_state(KOS_CDC_CTRL_DTR | KOS_CDC_CTRL_RTS);
            line_coding();
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

        void bus(uint32_t event)
        {
            dev.events = event;
            cdc.service_irq();
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
    r.configure();
    r.open();
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
    r.open();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(IDENTITY) + std::string(10u, 'x'));
    EXPECT_EQ(r.dev.in_lens.back(), 10u) << "the ring's bytes follow in a packet of their own";
    r.configure();
    r.line_state(0u);
    r.open();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(IDENTITY) + std::string(10u, 'x') + IDENTITY)
        << "a later configuration sends the rows again";
}

TEST(UsbIdentity, a_bus_reset_restarts_the_rows_and_counts_them_as_no_lost_byte)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    r.open();
    ASSERT_EQ(r.dev.in_lens.size(), 1u);
    r.dev.events = usb::KOS_USB_EV_BUS_RESET;
    r.pass();
    EXPECT_EQ(static_cast<uint32_t>(r.sh.tx_lost_link), 0u);
    EXPECT_EQ(usb::Transport::flush(&r.sh), 0);
    r.dev.in_bytes.clear();
    r.configure();
    r.line_state(0u);
    r.open();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, IDENTITY);
}

TEST(UsbHold, output_waits_for_the_line_coding_sent_after_the_host_raises_dtr)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.queue(10u);
    r.configure();
    r.line_coding();
    EXPECT_TRUE(r.dev.in_lens.empty()) << "a line coding before any open releases nothing";
    r.line_state(KOS_CDC_CTRL_DTR | KOS_CDC_CTRL_RTS);
    r.pass();
    EXPECT_TRUE(r.dev.in_lens.empty()) << "the opener's termios is not set yet";
    EXPECT_NE(r.cdc.max_irq_wait_us(), KOS_TIMEOUT_NONE) << "a hold must bound the IRQ thread's wait";
    r.line_coding();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(IDENTITY) + std::string(10u, 'x'));
    EXPECT_EQ(r.cdc.max_irq_wait_us(), KOS_TIMEOUT_NONE);
}

TEST(UsbHold, an_opener_that_sets_no_line_coding_is_answered_once_the_hold_expires)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    r.line_state(KOS_CDC_CTRL_DTR);
    g_now += usb::KOS_USB_HOLD_OPENED_NS - 1u;
    r.pass();
    EXPECT_TRUE(r.dev.in_lens.empty());
    g_now += 1u;
    EXPECT_EQ(r.cdc.max_irq_wait_us(), 0u);
    r.pass();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, IDENTITY);
}

TEST(UsbHold, a_host_that_never_raises_dtr_is_answered_once_the_hold_expires)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    g_now += usb::KOS_USB_HOLD_UNOPENED_NS - 1u;
    r.pass();
    EXPECT_TRUE(r.dev.in_lens.empty());
    g_now += 1u;
    r.pass();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, IDENTITY);
}

TEST(UsbHold, every_dtr_rise_holds_again)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    r.open();
    r.drain();
    r.dev.in_bytes.clear();
    r.line_state(0u);
    r.line_state(KOS_CDC_CTRL_DTR);
    r.queue(10u);
    r.pass();
    EXPECT_TRUE(r.dev.in_bytes.empty()) << "a second opener gets the same hold";
    r.line_coding();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(10u, 'x'));
}

TEST(UsbHold, a_hold_that_ends_while_the_bus_sleeps_lets_the_irq_thread_park)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    r.bus(usb::KOS_USB_EV_SUSPEND);
    g_now += usb::KOS_USB_HOLD_UNOPENED_NS;
    r.pass();
    EXPECT_EQ(r.cdc.max_irq_wait_us(), KOS_TIMEOUT_NONE) << "the IRQ thread polls a sleeping bus";
}

TEST(UsbHold, a_hold_that_ends_with_no_configuration_lets_the_irq_thread_park)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    r.dev.setup = {};
    r.dev.setup.bmRequestType = KOS_USB_REQ_TYPE_STANDARD;
    r.dev.setup.bRequest = KOS_USB_SET_CONFIGURATION;
    r.dev.setup.wValue = 0u;
    r.bus(usb::KOS_USB_EV_SETUP);
    g_now += usb::KOS_USB_HOLD_UNOPENED_NS;
    r.pass();
    EXPECT_EQ(r.cdc.max_irq_wait_us(), KOS_TIMEOUT_NONE) << "the IRQ thread polls an unconfigured bus";
}

TEST(UsbUnplug, an_unplug_holds_the_ring_and_the_replug_opens_with_the_rows)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    r.open();
    r.drain();
    ASSERT_EQ(r.dev.in_bytes, IDENTITY);
    size_t const sent = r.dev.in_lens.size();
    r.bus(usb::KOS_USB_EV_SUSPEND);
    r.queue(10u);
    r.pass();
    EXPECT_EQ(r.dev.in_lens.size(), sent) << "a packet went to a bus with no host";
    r.bus(usb::KOS_USB_EV_BUS_RESET);
    EXPECT_EQ(static_cast<uint32_t>(r.sh.tx_lost_link), 0u) << "the replug lost the held bytes";
    r.pass();
    EXPECT_EQ(r.dev.in_lens.size(), sent) << "nothing goes before the new host configures";
    r.configure();
    r.open();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(IDENTITY) + IDENTITY + std::string(10u, 'x'))
        << "the replug's stream opens with the rows, then the bytes held across the unplug";
}

TEST(UsbUnplug, a_resume_continues_the_stream_without_the_rows)
{
    Rig r;
    ASSERT_EQ(r.cdc.bring_up(), 0);
    r.configure();
    r.open();
    r.drain();
    r.bus(usb::KOS_USB_EV_SUSPEND);
    r.queue(10u);
    r.pass();
    size_t const sent = r.dev.in_lens.size();
    r.bus(usb::KOS_USB_EV_RESUME);
    ASSERT_EQ(r.dev.in_lens.size(), sent + 1u) << "the resume itself restarts the pump";
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(IDENTITY) + std::string(10u, 'x'));
}

TEST(UsbUnplug, a_setup_proves_a_bus_awake_that_was_last_read_suspended)
{
    Rig r;
    r.bus(usb::KOS_USB_EV_SUSPEND);
    r.queue(10u);
    r.configure();
    r.open();
    r.drain();
    EXPECT_EQ(r.dev.in_bytes, std::string(10u, 'x'));
}

TEST(UsbUnplug, a_suspend_read_after_a_setup_in_the_same_pass_holds)
{
    Rig r;
    r.queue(10u);
    // DTR already high: this setup starts no output hold.
    r.dev.setup = {};
    r.dev.setup.bmRequestType = KOS_USB_REQ_TYPE_CLASS;
    r.dev.setup.bRequest = KOS_CDC_SET_CONTROL_LINE_STATE;
    r.dev.setup.wValue = KOS_CDC_CTRL_DTR | KOS_CDC_CTRL_RTS;
    r.bus(usb::KOS_USB_EV_SETUP | usb::KOS_USB_EV_SUSPEND);
    EXPECT_TRUE(r.dev.in_lens.empty()) << "the state read last is the bus's now";
}
