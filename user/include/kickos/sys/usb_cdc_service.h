// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The shared USB CDC-ACM class layer, templated over a concrete per-controller device class.
// No MMIO and no controller knowledge live here. It reuses the UART's ring, its wire ABI
// (<kickos/sys/uart.h>) and its counters.
//
// The UsbDev class supplies the implicit interface (design-m4.6.2-usb-cdc.md sec 3.1):
//     int      bring_up();                    // 0, or negative: nothing else may be called
//     void     attach();                      // present the device to the bus, LAST
//     uint32_t take_events();                 // KOS_USB_EV_* ; clears what it reports
//     uint32_t take_buff_status();            // bit 2n = EPn IN, 2n+1 = EPn OUT
//     void     setup_read(struct kos_usb_setup* out);
//     void     bus_reset_recover();
//     void     set_address(uint8_t addr);
//     void     ep_open_all();
//     void     ep0_in(uint8_t const* p, uint32_t n, uint8_t pid);
//     void     ep0_out_arm(uint32_t n, uint8_t pid);
//     uint32_t ep0_out_read(uint8_t* out, uint32_t max);
//     void     ep0_stall();
//     void     ep_in(uint8_t ep, uint8_t const* p, uint32_t n, uint8_t pid);
//     void     ep_out_arm(uint8_t ep, uint8_t pid);
//     uint32_t ep_out_read(uint8_t ep, uint8_t* out, uint32_t max);
//     void     ep_stall(uint8_t ep, bool on);
// EVERY method touches the granted register window, so all of them may be called ONLY
// from the IRQ thread.
//
// The RP block owns DPRAM and hands the DATA PID to software. The i.MX RT1062 owns the PID,
// so its `pid` arguments are ignored, and it reads software-built descriptor lists out of
// system RAM as a BUS MASTER, so its shared block must stay coherent with a device that never
// looks at the D-cache; teensy41 gets that from KICKOS_IMXRT_DCACHE=OFF, which the build
// derives from the service list (design-m4.6.2-usb-cdc.md sections 2 and 4.3).
//
// The link is host-controlled and may never come up, so `configured` is a SEPARATE,
// NON-LATCHING flag beside the UART's `ready` latch (design sec 4.5) and a bus reset clears
// it. This ring's consumer may never exist, so the endpoint starts NON-BLOCKING.
//
// TX pacing is self-sustaining only while a buffer is in flight, a bulk IN completion being
// the wake that pumps the next packet. A host that stops issuing IN tokens without a bus
// reset (a closed tty, suspend, a bare unplug) leaves `Shared::tx_inflight` set with no
// completion coming, so the ring fills and stays full and no doorbell shortens it.

#ifndef KICKOS_SYS_USB_CDC_SERVICE_H
#define KICKOS_SYS_USB_CDC_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/byte_ring.h>
#include <kickos/sys/bytes.h> // mem_copy, mem_zero
#include <kickos/sys/console_ring.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>
#include <kickos/sys/usb_cdc.h>

#include <kickos/sys/atomic.h>

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

namespace kickos::usb
{

// Child cap indices the two threads read. A driver NAMES them; the bring-up chooses them.
enum
{
    KOS_USB_CAP_EP = KOS_SPAWN_DELEGATED_CAP0,
    KOS_USB_CAP_DOORBELL = console::KOS_CONSOLE_CAP_DOORBELL,
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
// requires it of every caller, privileged included. TX is larger than the UART's: the dark
// window before a host attaches is unbounded.
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
    Atomic<uint32_t, Order::RELAXED> ready;
    // NOT a latch: a bus reset clears it. It does NOT catch a bare unplug, because no
    // backend arms a disconnect or suspend source and the RP backend forces
    // VBUS-detected, so a host that goes away without a later reset leaves this at 1.
    Atomic<uint32_t, Order::RELAXED> configured;
    // Bytes taken OUT of the tx ring and handed to the controller, not yet completed. The
    // IRQ thread is its only writer; it sits in the shared block because the flush protocol
    // runs on the SERVICE thread, which cannot see the Cdc class.
    Atomic<uint32_t, Order::RELAXED> tx_inflight;
    // Write policy for the unframed console arm, from kos_uart_flags. The service thread is
    // its only writer.
    Atomic<uint32_t, Order::RELAXED> mode;
    // Link-loss bytes, IRQ THREAD ONLY. Separate from stats.tx_dropped, which the service
    // thread owns: `+=` on one field from both threads is a lost update. STATS sums them.
    Atomic<uint32_t, Order::RELAXED> tx_lost_link;
    uint8_t tx_buf[KOS_USB_TX_SIZE];
    uint8_t rx_buf[KOS_USB_RX_SIZE];
};

static_assert(sizeof(struct Shared) <= KOS_USB_BLOCK_SIZE,
              "the USB CDC shared block must fit one 2 KiB power-of-two grant");

// kos_byte_ring_init REFUSES a non-power-of-two or sub-2 size and leaves the ring reporting
// empty-and-full forever, which a blocking (unbounded) console write would spin on.
static_assert(KOS_USB_TX_SIZE >= 2 and (KOS_USB_TX_SIZE & (KOS_USB_TX_SIZE - 1)) == 0,
              "the TX ring size must be a power of two >= 2 or it never accepts a byte");

// Called by the BRING-UP, before either thread exists, so it races nothing.
//
// Seats NON-BLOCKING, unlike the UART: no IN token is issued until a host both enumerates the
// device AND opens the tty, so a blocking write here is unbounded. That is a static property
// of the transport and must NOT be keyed on Shared::configured, which never clears on
// unplug.
void shared_init(Shared* s);

// The offset a generic bring-up polls the readiness latch through, it being unable to name
// Shared. Never write the offset as a literal in a descriptor: the latch must be the atomic
// this expression locates, and `configured` sits beside it.
constexpr uint16_t KOS_USB_READY_OFFSET = static_cast<uint16_t>(offsetof(Shared, ready));

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

    // 0, or the backend's negative code. A refusal must NOT fall through to ep_open_all:
    // the window may still be unreachable, and its first read is what faults.
    int bring_up()
    {
        int const rc = dev_.bring_up();
        if (rc != 0)
        {
            return rc;
        }
        reset_state();
        dev_.ep_open_all();
        // The bus pull-up goes on LAST. A host that sees the device before EP0 can
        // answer starts enumerating into a controller that is not armed.
        dev_.attach();
        return 0;
    }

    // One pass. A wake is NOT proof of a hardware event (the doorbell is a pure post), so
    // finding nothing asserted must be harmless and must still pump TX.
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
        sh_->tx_inflight = 0;
        sh_->configured = 0;
        // `mode` is deliberately NOT reset: it is the caller's choice, not link state, so a
        // bus reset must not silently restore back-pressure a caller opted out of.
    }

    // Abandon a bulk IN buffer the controller still holds. Those bytes already left the
    // ring, so the loss must be counted here or it is invisible.
    void drop_in_flight()
    {
        // Not stats.tx_dropped: that is the service thread's, and this would race it.
        uint32_t const lost = sh_->tx_lost_link + sh_->tx_inflight;
        sh_->tx_lost_link = lost;
        sh_->tx_inflight = 0;
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
            // staged here and written when the zero-length IN comes back. SET_ADDRESS(0) is
            // legal (it returns the device to the default state), so the flag cannot be a
            // zero sentinel on the value.
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
            sh_->tx_inflight = 0;
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
        uint8_t buf[KOS_USB_CDC_BULK_MAX_PACKET];
        uint32_t const n = dev_.ep_out_read(KOS_USB_CDC_EP_DATA, buf, sizeof(buf));
        for (uint32_t i = 0; i < n; i++)
        {
            // A full RX ring drops the NEWEST byte and counts it, as on the UART.
            if (kos_byte_ring_push(&sh_->rx, &buf[i], 1u) == 0u)
            {
                kos_counter_increment(&sh_->stats.rx_dropped, 1u);
            }
            else
            {
                kos_counter_increment(&sh_->stats.rx_bytes, 1u);
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
        if (sh_->tx_inflight != 0u)
        {
            return; // one buffer is with the controller; its completion re-enters here
        }
        uint8_t buf[KOS_USB_CDC_BULK_MAX_PACKET];
        uint32_t n = kos_byte_ring_pop(&sh_->tx, buf, sizeof(buf));
        if (n == 0u)
        {
            return;
        }
        dev_.ep_in(KOS_USB_CDC_EP_DATA, buf, n, bulk_in_pid_);
        bulk_in_pid_ = static_cast<uint8_t>(bulk_in_pid_ ^ 1u);
        // One count carries both "a buffer is with the controller" and "how many bytes it
        // holds", n being nonzero here.
        sh_->tx_inflight = n;
    }

    UsbDev& dev_;
    Shared* sh_;
    struct kos_cdc_line_coding line_coding_;
    // Sized by the largest EP0 payload either direction: one 64-byte control packet.
    uint8_t scratch_[KOS_USB_CDC_EP0_MAX_PACKET];
    uint8_t const* ep0_src_ = nullptr;
    uint32_t ep0_left_ = 0;
    uint8_t ep0_ = EP0_IDLE;
    uint8_t ep0_pid_ = 1;
    uint8_t ep0_out_req_ = 0;
    uint8_t pending_addr_ = 0;
    uint8_t config_ = 0;
    uint8_t bulk_in_pid_ = 0;
    uint8_t bulk_out_pid_ = 0;
    bool ep0_zlp_ = false;
    bool addr_pending_ = false;
};

// ---------------------------------------------------------------------------------
// The IRQ thread. Owns every register and the whole control endpoint; parks in irq_wait and
// services one pass per wake.
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
        kos_counter_increment(&sh->stats.irq_wakes, 1u);
        uint32_t const rx_before = kos_counter_load(&sh->stats.rx_bytes);
        bool const tx_had_work = (kos_byte_ring_used(&sh->tx) != 0u);
        cdc.service_irq();
        if (kos_counter_load(&sh->stats.rx_bytes) == rx_before and not tx_had_work)
        {
            kos_counter_increment(&sh->stats.irq_spurious, 1u);
        }
    }
    exit(0);
}

// ---------------------------------------------------------------------------------
// The ring side of the console is <kickos/sys/console_ring.h>; the rules, the budgets and
// the CRLF posture are stated there. These three bind it to this layer's Shared block.
//
// The woken pass re-reads LIVE controller status, which recovers a bulk IN completion the
// class layer missed: the one state in which a full ring has no completion coming.
//
// PRECONDITION: KOS_USB_CAP_DOORBELL is the line's SIGNAL cap, which the two-thread spawn
// provides. Any other caller has the notify refused on the cap TYPE check.
uint32_t tx_write(Shared* sh, uint8_t const* p, uint32_t n);

// An empty ring is NOT an empty channel here: up to one bulk packet still sits in the
// controller's DPRAM buffer, holding the tail of the stream.
uint32_t console_flush(Shared* sh);

uint32_t console_write(Shared* sh, uint8_t const* p, uint32_t n);

// ---------------------------------------------------------------------------------
// The service thread. Parks in recv, replies out of ring state, never touches the device.
// A kos_call is a <kickos/sys/uart.h> frame; a plain send is a raw console write.
// Returns kos_reply's result: a reply can fail on a dead cap, and a caller that has gone is
// the one thing this arm cannot see from its own state.
int reply_status(kos_cap_t reply_cap, int32_t status, uint16_t len);

// Parse + run one request frame; the reply is this function's, on every path.
//
// `mode` is null for a service with no unframed console arm, which is what makes
// KOS_UART_SET_MODE refuse there instead of storing a mode nothing reads.
int serve_one(Shared* sh, Atomic<uint32_t, Order::RELAXED>* mode, uint8_t const* msg, size_t n,
              kos_cap_t reply_cap);

// Recv/dispatch loop for a CONSOLE endpoint: a kos_call is a request frame, a plain
// send is raw console bytes. Returns only when the endpoint dies.
void console_serve_loop(Shared* sh);

// ---------------------------------------------------------------------------------
// The class-side half of the descriptor check. The generic validator cannot know that the
// loops above read KOS_USB_CAP_EP == 1, KOS_USB_CAP_DOORBELL == 2 and KOS_USB_CAP_LINE ==
// 1, so a descriptor that grants the right caps in the wrong ORDER passes valid() and
// stalls silently.
//
// valid() only RANGE-checks ready_offset, so a literal there can land on a field already
// non-zero when the poll first reads it, which turns the barrier into a silent no-op.
constexpr bool desc_ok(driver::Descriptor const& d)
{
    return driver::ring_doorbell_shape_ok(d, KOS_USB_READY_OFFSET,
                                          static_cast<uint32_t>(KOS_USB_BLOCK_SIZE));
}

}

#endif
