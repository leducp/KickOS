// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The shared USB CDC-ACM class layer, templated over a concrete per-controller device
// class. No MMIO and no controller knowledge live here: only the shared ring block's
// layout, chapter-9 and CDC request handling, the enumeration state machine, and the two
// loops the driver's two threads run. It reuses the UART's ring, its wire ABI
// (<kickos/sys/uart.h>) and its counters.
//
// The UsbDev class supplies the implicit interface (design-m4.6.2-usb-cdc.md sec 3.1):
//     void     bring_up();
//     void     attach();                      // present the device to the bus, LAST
//     uint32_t take_events();                 // KOS_USB_EV_* ; clears what it reports
//     uint32_t take_buff_status();            // bit 2n = EPn IN, 2n+1 = EPn OUT
//     void     setup_read(struct kos_usb_setup* out);
//     void     bus_reset_recover();
//     void     set_address(uint8_t addr);
//     void     ep_open_all();
//     void     ep0_in(unsigned char const* p, uint32_t n, uint8_t pid);
//     void     ep0_out_arm(uint32_t n, uint8_t pid);
//     uint32_t ep0_out_read(unsigned char* out, uint32_t max);
//     void     ep0_stall();
//     void     ep_in(uint8_t ep, unsigned char const* p, uint32_t n, uint8_t pid);
//     void     ep_out_arm(uint8_t ep, uint8_t pid);
//     uint32_t ep_out_read(uint8_t ep, unsigned char* out, uint32_t max);
//     void     ep_stall(uint8_t ep, bool on);
// EVERY method touches the granted register window, so all of them may be called ONLY
// from the IRQ thread.
//
// The i.MX RT1062 backend teensy41 needs is absent rather than stubbed: its controller
// walks software-built descriptor lists as a bus master and needs a cache posture this
// tree does not have (design-m4.6.2-usb-cdc.md sections 2 and 4.3).
//
// The link is host-controlled and may never come up, so `configured` is a SEPARATE,
// NON-LATCHING flag beside the UART's `ready` latch (design sec 4.5) and a bus reset
// clears it. Nothing here ever blocks on the link: an un-enumerated device holds every
// byte, and a full TX ring is a short accept.
//
// TX pacing is self-sustaining only while a buffer is in flight, because a bulk IN
// completion is the wake that pumps the next packet. A host that stops issuing IN tokens
// without a bus reset (a closed tty, suspend, a bare unplug) leaves tx_in_flight_ set
// with no completion coming, so the ring fills and stays full and no doorbell shortens
// it.

#ifndef KICKOS_SYS_USB_CDC_SERVICE_H
#define KICKOS_SYS_USB_CDC_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/byte_ring.h>
#include <kickos/sys/bytes.h> // mem_copy, mem_zero
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>
#include <kickos/sys/usb_cdc.h>

#include <stdint.h>
#include <stddef.h>

namespace kickos
{
namespace usb
{

// Child cap indices the two threads read. A driver NAMES them; the bring-up chooses them.
enum
{
    KOS_USB_CAP_EP = KOS_SPAWN_DELEGATED_CAP0,
    KOS_USB_CAP_DOORBELL = KOS_SPAWN_DELEGATED_CAP0 + 1,
    KOS_USB_CAP_LINE = KOS_SPAWN_DELEGATED_CAP0
};

// Controller events the class layer reacts to. A backend reports only what it saw and
// clears the source before returning.
enum kos_usb_event
{
    KOS_USB_EV_BUS_RESET = 1u << 0,
    KOS_USB_EV_SETUP = 1u << 1,
    KOS_USB_EV_BUFFER = 1u << 2
};

// One power-of-two naturally-aligned allocation: the RAM arm of grant_region_admissible
// requires it of every caller, privileged included. TX is larger than the UART's because
// the dark window before a host attaches is unbounded.
enum
{
    KOS_USB_TX_SIZE = 1024,
    KOS_USB_RX_SIZE = 512,
    KOS_USB_BLOCK_SIZE = 2048
};

struct Shared
{
    struct kos_byte_ring tx;
    struct kos_byte_ring rx;
    struct kos_uart_stats stats;
    // One-way latch: the IRQ thread reached its loop. Written once by one thread.
    volatile uint32_t ready;
    // NOT a latch: a bus reset clears it. It does NOT catch a bare unplug, because no
    // backend arms a disconnect or suspend source and the RP backend forces
    // VBUS-detected, so a host that goes away without a later reset leaves this at 1.
    volatile uint32_t configured;
    unsigned char tx_buf[KOS_USB_TX_SIZE];
    unsigned char rx_buf[KOS_USB_RX_SIZE];
};

static_assert(sizeof(struct Shared) <= KOS_USB_BLOCK_SIZE,
              "the USB CDC shared block must fit one 2 KiB power-of-two grant");

// Called by the BRING-UP, before either thread exists, so it races nothing.
inline void shared_init(Shared* s)
{
    mem_zero(s, sizeof(*s));
    kos_byte_ring_init(&s->tx, s->tx_buf, KOS_USB_TX_SIZE);
    kos_byte_ring_init(&s->rx, s->rx_buf, KOS_USB_RX_SIZE);
}

// ---------------------------------------------------------------------------------
// The class layer. Owns the whole USB protocol; the service thread never sees it.
template <typename UsbDev>
class Cdc
{
public:
    Cdc(UsbDev& dev, Shared* sh) : dev_(dev), sh_(sh)
    {
        line_coding_.dwDTERate = 115200u;
        line_coding_.bCharFormat = 0;
        line_coding_.bParityType = 0;
        line_coding_.bDataBits = 8;
    }

    void bring_up()
    {
        dev_.bring_up();
        reset_state();
        dev_.ep_open_all();
        // The bus pull-up goes on LAST. A host that sees the device before EP0 can
        // answer starts enumerating into a controller that is not armed.
        dev_.attach();
    }

    // One pass. A wake is NOT proof of a hardware event (the doorbell is a pure post),
    // so finding nothing asserted must be harmless and must still pump TX.
    void service_irq()
    {
        uint32_t const ev = dev_.take_events();
        if ((ev & KOS_USB_EV_BUS_RESET) != 0u)
        {
            on_bus_reset();
        }
        if ((ev & KOS_USB_EV_SETUP) != 0u)
        {
            on_setup();
        }
        if ((ev & KOS_USB_EV_BUFFER) != 0u)
        {
            on_buffers(dev_.take_buff_status());
        }
        pump_tx();
    }

private:
    enum ep0_state
    {
        EP0_IDLE = 0,
        EP0_IN_DATA = 1,   // a device-to-host data stage is running
        EP0_IN_STATUS = 2, // the zero-length IN that acknowledges a no-data request
        EP0_OUT_DATA = 3   // a host-to-device data stage is running
    };

    // The one piece of controller encoding the class layer relies on: BUFF_STATUS bit 2n
    // is EPn IN, bit 2n+1 is EPn OUT. A backend whose hardware differs must translate.
    static uint32_t bit_in(uint8_t ep) { return 1u << (2u * ep); }
    static uint32_t bit_out(uint8_t ep) { return 1u << (2u * ep + 1u); }

    void reset_state()
    {
        ep0_ = EP0_IDLE;
        ep0_src_ = nullptr;
        ep0_left_ = 0;
        ep0_zlp_ = false;
        ep0_pid_ = 1;
        ep0_out_req_ = 0;
        pending_addr_ = 0;
        addr_pending_ = false;
        config_ = 0;
        bulk_in_pid_ = 0;
        bulk_out_pid_ = 0;
        tx_in_flight_ = false;
        tx_in_flight_len_ = 0;
        sh_->configured = 0;
    }

    // Abandon a bulk IN buffer the controller still holds. Those bytes already left the
    // ring, so the loss must be counted here or it is invisible.
    void drop_in_flight()
    {
        if (tx_in_flight_)
        {
            sh_->stats.tx_dropped += tx_in_flight_len_;
        }
        tx_in_flight_ = false;
        tx_in_flight_len_ = 0;
    }

    void on_bus_reset()
    {
        drop_in_flight(); // the re-open below discards whatever the controller holds
        dev_.bus_reset_recover();
        reset_state();
        dev_.ep_open_all();
    }

    void on_setup()
    {
        struct kos_usb_setup s;
        dev_.setup_read(&s);
        // Chapter 9: the DATA stage that follows a SETUP is always DATA1, whatever the
        // previous transfer left behind.
        ep0_pid_ = 1;
        ep0_ = EP0_IDLE;
        ep0_left_ = 0;
        ep0_zlp_ = false;
        uint8_t const type = static_cast<uint8_t>(s.bmRequestType & KOS_USB_REQ_TYPE_MASK);
        if (type == KOS_USB_REQ_TYPE_STANDARD)
        {
            std_request(s);
            return;
        }
        if (type == KOS_USB_REQ_TYPE_CLASS)
        {
            class_request(s);
            return;
        }
        dev_.ep0_stall();
    }

    void std_request(struct kos_usb_setup const& s)
    {
        switch (s.bRequest)
        {
        case KOS_USB_SET_ADDRESS:
        {
            // The address must not be live before the status stage completes, so it is
            // staged here and written when the zero-length IN comes back. The flag is
            // separate from the value because SET_ADDRESS(0) is legal (it returns the
            // device to the default state) and a zero sentinel would swallow it.
            pending_addr_ = static_cast<uint8_t>(s.wValue & 0x7Fu);
            addr_pending_ = true;
            ep0_ack();
            return;
        }
        case KOS_USB_SET_CONFIGURATION:
        {
            config_ = static_cast<uint8_t>(s.wValue & 0xFFu);
            bulk_in_pid_ = 0;
            bulk_out_pid_ = 0;
            drop_in_flight();
            dev_.ep_open_all();
            if (config_ != 0u)
            {
                // RULE T1 in its USB form: an unarmed endpoint simply NAKs, so waiting for
                // an interrupt before arming the first OUT buffer waits forever while the
                // host politely retries.
                dev_.ep_out_arm(KOS_USB_CDC_EP_DATA, bulk_out_pid_);
                sh_->configured = 1;
            }
            else
            {
                sh_->configured = 0;
            }
            ep0_ack();
            return;
        }
        case KOS_USB_GET_DESCRIPTOR:
        {
            uint8_t const* p = nullptr;
            uint32_t const n = kos_usb_cdc_descriptor(s.wValue, &p);
            if (n == 0u)
            {
                dev_.ep0_stall(); // a short answer reads as a malformed device
                return;
            }
            ep0_send(p, n, s.wLength);
            return;
        }
        case KOS_USB_GET_CONFIGURATION:
        {
            ep0_send(&config_, 1u, s.wLength);
            return;
        }
        case KOS_USB_GET_STATUS:
        {
            uint8_t const recip = static_cast<uint8_t>(s.bmRequestType & KOS_USB_REQ_RECIP_MASK);
            scratch_[0] = 0;
            scratch_[1] = 0;
            if (recip == KOS_USB_REQ_RECIP_ENDPOINT)
            {
                if (halted_ep(static_cast<uint8_t>(s.wIndex & 0xFFu)))
                {
                    scratch_[0] = 1;
                }
            }
            ep0_send(scratch_, 2u, s.wLength);
            return;
        }
        case KOS_USB_CLEAR_FEATURE:
        {
            feature(s, false);
            return;
        }
        case KOS_USB_SET_FEATURE:
        {
            feature(s, true);
            return;
        }
        case KOS_USB_SET_INTERFACE:
        {
            ep0_ack();
            return;
        }
        case KOS_USB_GET_INTERFACE:
        {
            scratch_[0] = 0;
            ep0_send(scratch_, 1u, s.wLength);
            return;
        }
        default:
        {
            dev_.ep0_stall();
            return;
        }
        }
    }

    void feature(struct kos_usb_setup const& s, bool on)
    {
        uint8_t const recip = static_cast<uint8_t>(s.bmRequestType & KOS_USB_REQ_RECIP_MASK);
        if (recip != KOS_USB_REQ_RECIP_ENDPOINT
            or s.wValue != KOS_USB_FEATURE_ENDPOINT_HALT)
        {
            // DEVICE_REMOTE_WAKEUP is accepted and ignored: there is no suspend entry for
            // the flag to gate.
            ep0_ack();
            return;
        }
        uint8_t const addr = static_cast<uint8_t>(s.wIndex & 0xFFu);
        uint8_t const num = static_cast<uint8_t>(addr & 0x0Fu);
        dev_.ep_stall(addr, on);
        if (num == KOS_USB_CDC_EP_DATA and not on)
        {
            // Clearing a halt resets the endpoint's data toggle to DATA0 (chapter 9), and
            // the RP block's PID is software-owned, so nothing else does it.
            if ((addr & 0x80u) != 0u)
            {
                bulk_in_pid_ = 0;
                drop_in_flight();
            }
            else
            {
                bulk_out_pid_ = 0;
                dev_.ep_out_arm(KOS_USB_CDC_EP_DATA, bulk_out_pid_);
            }
        }
        ep0_ack();
    }

    // Always false: no halt state is tracked, and nothing here stalls an endpoint except
    // on the host's own SET_FEATURE.
    bool halted_ep(uint8_t) const
    {
        return false;
    }

    void class_request(struct kos_usb_setup const& s)
    {
        switch (s.bRequest)
        {
        case KOS_CDC_SET_LINE_CODING:
        {
            if (s.wLength != KOS_CDC_LINE_CODING_LEN)
            {
                dev_.ep0_stall();
                return;
            }
            ep0_out_req_ = KOS_CDC_SET_LINE_CODING;
            ep0_ = EP0_OUT_DATA;
            dev_.ep0_out_arm(s.wLength, ep0_pid_);
            return;
        }
        case KOS_CDC_GET_LINE_CODING:
        {
            kos_cdc_line_coding_pack(scratch_, &line_coding_);
            ep0_send(scratch_, KOS_CDC_LINE_CODING_LEN, s.wLength);
            return;
        }
        case KOS_CDC_SET_CONTROL_LINE_STATE:
        {
            // DTR/RTS are accepted and dropped: a console has no modem lines, and gating
            // output on DTR would silence a board whose host opened the port without
            // raising it.
            ep0_ack();
            return;
        }
        case KOS_CDC_SEND_BREAK:
        {
            ep0_ack();
            return;
        }
        default:
        {
            dev_.ep0_stall();
            return;
        }
        }
    }

    // Device-to-host data stage. `want` is the host's wLength: a device may answer with
    // less, but then it must terminate with a short or zero-length packet.
    void ep0_send(uint8_t const* p, uint32_t n, uint16_t want)
    {
        if (want == 0u)
        {
            // Chapter 9: a request with wLength 0 has NO data stage whatever its
            // direction bit says, and its status stage is an IN. Falling through would
            // send a zero-length IN and then arm a status OUT the host never sends.
            ep0_ack();
            return;
        }
        if (n > want)
        {
            n = want;
        }
        ep0_src_ = p;
        ep0_left_ = n;
        ep0_zlp_ = false;
        if (n < want and n != 0u and (n % KOS_USB_CDC_EP0_MAX_PACKET) == 0u)
        {
            ep0_zlp_ = true;
        }
        ep0_ = EP0_IN_DATA;
        ep0_pump_in();
    }

    // Zero-length IN: the status stage of a request with no data stage.
    void ep0_ack()
    {
        ep0_ = EP0_IN_STATUS;
        ep0_src_ = nullptr;
        ep0_left_ = 0;
        ep0_zlp_ = false;
        dev_.ep0_in(nullptr, 0u, ep0_pid_);
        ep0_pid_ = 1;
    }

    void ep0_pump_in()
    {
        uint32_t n = ep0_left_;
        if (n > KOS_USB_CDC_EP0_MAX_PACKET)
        {
            n = KOS_USB_CDC_EP0_MAX_PACKET;
        }
        dev_.ep0_in(ep0_src_, n, ep0_pid_);
        ep0_pid_ = static_cast<uint8_t>(ep0_pid_ ^ 1u);
        ep0_src_ += n;
        ep0_left_ -= n;
        if (ep0_left_ == 0u and n != KOS_USB_CDC_EP0_MAX_PACKET)
        {
            ep0_zlp_ = false; // a short packet already terminated the transfer
        }
    }

    void on_buffers(uint32_t bits)
    {
        if ((bits & bit_in(0)) != 0u)
        {
            on_ep0_in_done();
        }
        if ((bits & bit_out(0)) != 0u)
        {
            on_ep0_out_done();
        }
        if ((bits & bit_in(KOS_USB_CDC_EP_DATA)) != 0u)
        {
            tx_in_flight_ = false;
        }
        if ((bits & bit_out(KOS_USB_CDC_EP_DATA)) != 0u)
        {
            drain_out();
        }
        // The notification endpoint is declared and buffered but never queued, so a
        // completion on it cannot happen and needs no arm.
    }

    void on_ep0_in_done()
    {
        if (ep0_ == EP0_IN_DATA)
        {
            if (ep0_left_ != 0u or ep0_zlp_)
            {
                if (ep0_left_ == 0u)
                {
                    ep0_zlp_ = false;
                }
                ep0_pump_in();
                return;
            }
            // The data stage is done: arm the host's zero-length status OUT, DATA1.
            ep0_ = EP0_IDLE;
            dev_.ep0_out_arm(0u, 1u);
            return;
        }
        if (ep0_ == EP0_IN_STATUS)
        {
            ep0_ = EP0_IDLE;
            if (addr_pending_)
            {
                dev_.set_address(pending_addr_);
                addr_pending_ = false;
            }
            return;
        }
    }

    void on_ep0_out_done()
    {
        if (ep0_ != EP0_OUT_DATA)
        {
            return; // the status stage of a device-to-host transfer: nothing to do
        }
        uint32_t const n = dev_.ep0_out_read(scratch_, sizeof(scratch_));
        ep0_ = EP0_IDLE;
        if (ep0_out_req_ == KOS_CDC_SET_LINE_CODING and n >= KOS_CDC_LINE_CODING_LEN)
        {
            kos_cdc_line_coding_unpack(&line_coding_, scratch_);
        }
        ep0_out_req_ = 0;
        ep0_pid_ = 1;
        ep0_ack();
    }

    void drain_out()
    {
        unsigned char buf[KOS_USB_CDC_BULK_MAX_PACKET];
        uint32_t const n = dev_.ep_out_read(KOS_USB_CDC_EP_DATA, buf, sizeof(buf));
        for (uint32_t i = 0; i < n; i++)
        {
            // A full RX ring drops the NEWEST byte and counts it, as on the UART.
            if (kos_byte_ring_push(&sh_->rx, &buf[i], 1u) == 0u)
            {
                sh_->stats.rx_dropped++;
            }
            else
            {
                sh_->stats.rx_bytes++;
            }
        }
        bulk_out_pid_ = static_cast<uint8_t>(bulk_out_pid_ ^ 1u);
        dev_.ep_out_arm(KOS_USB_CDC_EP_DATA, bulk_out_pid_);
    }

    void pump_tx()
    {
        if (sh_->configured == 0u)
        {
            return; // no host has selected a configuration: the bytes wait in the ring
        }
        if (tx_in_flight_)
        {
            return; // one buffer is with the controller; its completion re-enters here
        }
        unsigned char buf[KOS_USB_CDC_BULK_MAX_PACKET];
        uint32_t n = kos_byte_ring_pop(&sh_->tx, buf, sizeof(buf));
        if (n == 0u)
        {
            return;
        }
        dev_.ep_in(KOS_USB_CDC_EP_DATA, buf, n, bulk_in_pid_);
        bulk_in_pid_ = static_cast<uint8_t>(bulk_in_pid_ ^ 1u);
        tx_in_flight_ = true;
        tx_in_flight_len_ = n;
    }

    UsbDev& dev_;
    Shared* sh_;
    struct kos_cdc_line_coding line_coding_;
    // Sized by the largest EP0 payload either direction: one 64-byte control packet.
    uint8_t scratch_[KOS_USB_CDC_EP0_MAX_PACKET];
    uint8_t const* ep0_src_ = nullptr;
    uint32_t ep0_left_ = 0;
    uint32_t tx_in_flight_len_ = 0;
    uint8_t ep0_ = EP0_IDLE;
    uint8_t ep0_pid_ = 1;
    uint8_t ep0_out_req_ = 0;
    uint8_t pending_addr_ = 0;
    uint8_t config_ = 0;
    uint8_t bulk_in_pid_ = 0;
    uint8_t bulk_out_pid_ = 0;
    bool ep0_zlp_ = false;
    bool addr_pending_ = false;
    bool tx_in_flight_ = false;
};

// ---------------------------------------------------------------------------------
// The IRQ thread. Owns every register and the whole control endpoint; parks in
// irq_wait and services one pass per wake.
template <typename UsbDev>
void irq_loop(Cdc<UsbDev>& cdc, Shared* sh)
{
    sh->ready = 1;
    while (true)
    {
        // The FIRST wait is also what arms the line: a claim leaves it masked so no
        // window exists in which it is armed and unowned (INVARIANT H1).
        if (kos_irq_wait(KOS_USB_CAP_LINE) != 0)
        {
            break; // the cap went away: the line is gone, so this thread has no work
        }
        sh->stats.irq_wakes++;
        uint32_t const rx_before = sh->stats.rx_bytes;
        bool const tx_had_work = (kos_byte_ring_used(&sh->tx) != 0u);
        cdc.service_irq();
        if (sh->stats.rx_bytes == rx_before and not tx_had_work)
        {
            sh->stats.irq_spurious++;
        }
    }
    kos_exit(0);
}

// ---------------------------------------------------------------------------------
// Queue bytes for transmit and ring the doorbell. Returns the bytes ACCEPTED, which is
// less than n on a full ring; the retry policy for the remainder belongs to the caller.
// EVERY producer path goes through here rather than open-coding push + notify.
//
// Rung on EVERY call, including one that accepted nothing: with the ring full the parked IRQ
// thread has no other wake source, so gating on an accepted push strands the channel, and
// gating on an idle -> busy edge loses the wake the other way (the IRQ thread can drain and
// park between the test and the push). The producer owns `head`, the IRQ thread `tail`;
// neither may act on the other's index.
//
// The woken pass re-reads LIVE controller status, which recovers a bulk IN completion the
// class layer missed: the one state in which a full ring has no completion coming.
//
// PRECONDITION: KOS_USB_CAP_DOORBELL is the line's SIGNAL cap, which the two-thread
// spawn provides. Any other caller has the notify refused on the cap TYPE check.
inline uint32_t tx_write(Shared* sh, unsigned char const* p, uint32_t n)
{
    uint32_t const took = kos_byte_ring_push(&sh->tx, p, n);
    sh->stats.tx_bytes += took;
    (void)kos_irq_notify(KOS_USB_CAP_DOORBELL);
    return took;
}

// ---------------------------------------------------------------------------------
// The service thread. Parks in recv, replies out of ring state, never touches the device.
// A kos_call is a <kickos/sys/uart.h> frame; a plain send is a raw console write.
inline void reply_status(int reply_cap, int32_t status, uint16_t len)
{
    struct kos_uart_rsp rsp;
    rsp.status = status;
    rsp.len = len;
    rsp.rsv = 0;
    (void)kos_reply(reply_cap, &rsp, sizeof(rsp));
}

inline void serve_one(Shared* sh, unsigned char const* msg, size_t n, int reply_cap)
{
    if (reply_cap < 0)
    {
        return; // a plain send, not a call: nothing to reply to
    }
    if (n < sizeof(struct kos_uart_req))
    {
        reply_status(reply_cap, -KOS_EINVAL, 0);
        return;
    }
    struct kos_uart_req req;
    mem_copy(&req, msg, sizeof(req));
    unsigned char const* payload = msg + sizeof(req);
    size_t const payload_len = n - sizeof(req);

    switch (req.op)
    {
    case KOS_UART_WRITE:
    {
        if (req.len > payload_len)
        {
            reply_status(reply_cap, -KOS_EINVAL, 0);
            return;
        }
        // A short accept, zero included, is not an error: the client sees len < req.len
        // and retries, and every retry must re-run the consumer. See tx_write.
        uint32_t const took = tx_write(sh, payload, req.len);
        reply_status(reply_cap, 0, static_cast<uint16_t>(took));
        return;
    }
    case KOS_UART_READ:
    {
        if ((req.flags & KOS_UART_F_BLOCK) != 0)
        {
            reply_status(reply_cap, -KOS_ENOSYS, 0);
            return;
        }
        unsigned char out[KOS_EP_MSG_MAX];
        uint32_t want = req.len;
        if (want > KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp))
        {
            want = KOS_EP_MSG_MAX - sizeof(struct kos_uart_rsp);
        }
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.rsv = 0;
        uint32_t const got = kos_byte_ring_pop(&sh->rx, out + sizeof(rsp), want);
        rsp.len = static_cast<uint16_t>(got);
        mem_copy(out, &rsp, sizeof(rsp));
        (void)kos_reply(reply_cap, out, sizeof(rsp) + got);
        return;
    }
    case KOS_UART_STATS:
    {
        unsigned char out[sizeof(struct kos_uart_rsp) + sizeof(struct kos_uart_stats)];
        struct kos_uart_rsp rsp;
        rsp.status = 0;
        rsp.len = static_cast<uint16_t>(sizeof(struct kos_uart_stats));
        rsp.rsv = 0;
        mem_copy(out, &rsp, sizeof(rsp));
        mem_copy(out + sizeof(rsp), &sh->stats, sizeof(sh->stats));
        (void)kos_reply(reply_cap, out, sizeof(out));
        return;
    }
    case KOS_UART_CONFIGURE:
    {
        // A CDC line coding is set by the HOST, over the control endpoint, in the IRQ
        // thread. There is nothing here for a client to program.
        reply_status(reply_cap, -KOS_ENOSYS, 0);
        return;
    }
    default:
    {
        reply_status(reply_cap, -KOS_EINVAL, 0);
        return;
    }
    }
}

// Recv/dispatch loop for a CONSOLE endpoint: a kos_call is a request frame, a plain
// send is raw console bytes. Returns only when the endpoint dies.
inline void console_serve_loop(Shared* sh)
{
    unsigned char msg[KOS_EP_MSG_MAX];
    while (true)
    {
        struct kos_recv_info info;
        long const n = kos_recv(KOS_USB_CAP_EP, msg, sizeof(msg), &info);
        if (n < 0)
        {
            break; // endpoint dead: let the bring-up respawn us
        }
        if (info.reply_cap >= 0)
        {
            serve_one(sh, msg, static_cast<size_t>(n), info.reply_cap);
            continue;
        }
        // Raw console write. A plain send cannot report a short accept, so the overflow
        // is counted. On USB that counter is load-bearing: with no host attached every
        // byte past the ring's depth lands here.
        uint32_t const took = tx_write(sh, msg, static_cast<uint32_t>(n));
        // Refused at the ring, so these bytes never entered tx_bytes; disjoint from the
        // in-flight loss drop_in_flight() counts.
        sh->stats.tx_dropped += static_cast<uint32_t>(n) - took;
    }
    kos_exit(0);
}

} // namespace usb
} // namespace kickos

#endif
