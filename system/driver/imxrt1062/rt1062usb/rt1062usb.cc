// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// i.MX RT1062 (Teensy 4.1) USB CDC-ACM console driver on the
// <kickos/sys/usb_cdc_service.h> substrate. Registers from IMXRT1060RM Rev. 3 ch.42, via
// rt_usb_regs.h. Four chip facts bind:
//
//   - The controller is a BUS MASTER and reads descriptors AND data buffers out of RAM, so
//     every payload is copied into the granted block first: a stack buffer would be read
//     after the frame died, a flash pointer would go through FlexSPI.
//   - The data toggle is HARDWARE-owned (RM 42.7.40): the class layer's `pid` argument is
//     ignored and the sequence is reset with ENDPTCTRLn TXR/RXR.
//   - A SETUP packet is extracted under the USBCMD.SUTW tripwire (RM 42.5.6.4.2.1), and a
//     setup can arrive while a previous control transfer is still primed.
//   - Descriptor writes are Normal memory and the doorbell is Device, so the M7 may
//     reorder them; every prime is preceded by a DSB.
//
// One transfer per endpoint direction at a time, so RM 42.5.6.6.3 Case 1 applies at every
// add and the ATDTW loop of Case 2 is unused.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/chip_mmap.h>
#include <kickos/io/mmio.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/bytes.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h> // KOS_EPERM
#include <kickos/sys/service.h>
#include <kickos/sys/usb_cdc_service.h>

#include "irq.h"
#include "rt_usb_regs.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h> // exit


namespace drv = kickos::driver;
namespace reg = kickos::rtusb::reg;
namespace usb = kickos::usb;
namespace rtirq = kickos::imxrt1062::irq;

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    constexpr uint32_t BLOCK_SIZE = 4096u;

    // One packet per dTD, and the largest is the 64-byte EP0 packet.
    constexpr uint32_t MAX_XFER = KOS_USB_CDC_EP0_MAX_PACKET;

    // RM 42.5.6.6.5 warns the ENDPTFLUSH wait can be long under bus activity; every spin
    // here is capped so a wedged controller does not wedge the IRQ thread with it.
    constexpr uint32_t SPIN_MAX = 200000u;
    constexpr uint32_t SUTW_TRIES = 16u;

    // How far the IRQ thread got, read back by ROOT after drv::bring_up gives up. The thread
    // cannot report anything itself: kos_console_publish runs before it is spawned, so its
    // console has no receiver until the unwind. stage_reason() spells each value out.
    enum stage
    {
        STAGE_NONE = 0,
        STAGE_ENTERED = 1,
        // Every value below STAGE_RST_WRITE must mean probe_id was never read: ROOT prints the
        // clock verdict from that comparison alone.
        STAGE_DECLINED = 2,
        STAGE_PROBE = 3,
        STAGE_RST_WRITE = 4,
        STAGE_RST_WAIT = 5,
        STAGE_MODE = 6,
        STAGE_PORT = 7,
        STAGE_ARMED = 8,
        STAGE_FLUSHED = 9,
        STAGE_EP_OPEN = 10,
        STAGE_ATTACHED = 11
    };

    // Bounded spins that EXPIRED, one BIT POSITION each in Block::stalls. A spin that gives up
    // still reaches the loop, so without this a wait that is silently too short looks like
    // success. STALL_COUNT sizes stall_reason's text table, so a slot added here without a
    // string fails the build, and the order below is report priority: the first slot set wins.
    enum stall_slot
    {
        STALL_RESET,
        STALL_UNSAFE,
        STALL_PRIME,
        STALL_FLUSH,
        STALL_SUTW,
        STALL_COUNT
    };

    // Memory the CONTROLLER reads, laid out so the dQH list lands on the block base.
    struct alignas(BLOCK_SIZE) Block
    {
        reg::Dqh qh[reg::QH_COUNT]; // MUST BE FIRST: this address goes in ENDPTLISTADDR
        reg::Dtd td[reg::QH_COUNT]; // one per queue head; only five are ever primed
        uint8_t ep0_in_buf[KOS_USB_CDC_EP0_MAX_PACKET];
        uint8_t ep0_out_buf[KOS_USB_CDC_EP0_MAX_PACKET];
        uint8_t bulk_in_buf[KOS_USB_CDC_BULK_MAX_PACKET];
        uint8_t bulk_out_buf[KOS_USB_CDC_BULK_MAX_PACKET];
        Atomic<uint32_t, Order::RELAXED> stage;
        Atomic<uint32_t, Order::RELAXED> stalls;
        // What the window answered to the ID read, valid from STAGE_RST_WRITE.
        Atomic<uint32_t, Order::RELAXED> probe_id;
        usb::Shared sh;
    };

    static_assert(offsetof(Block, qh) == 0,
                  "the dQH list must sit at the block base or ENDPTLISTADDR cannot name it");
    static_assert((BLOCK_SIZE & (BLOCK_SIZE - 1u)) == 0u,
                  "the RAM arm of grant_region_admissible takes only a power of two");
    static_assert(BLOCK_SIZE % 2048u == 0u,
                  "ENDPTLISTADDR reads bits 10:0 as zero, so the list base is 2 KiB aligned "
                  "(RM 42.7.25)");
    static_assert(alignof(Block) % 64u == 0u and sizeof(reg::Dqh) % 64u == 0u,
                  "every dQH must be 64 B aligned (RM 42.5.5.1)");
    static_assert(offsetof(Block, td) % 32u == 0u and sizeof(reg::Dtd) % 32u == 0u,
                  "every dTD must be 32 B aligned (RM 42.5.6.6.2)");
    // alignas pads sizeof(Block) UP to 4096, so the CONTENT end is what has to be checked.
    static_assert(offsetof(Block, sh) + sizeof(usb::Shared) <= BLOCK_SIZE,
                  "the descriptor block plus the rings must fit one 4 KiB grant");
    // block_init zeroes [0, offsetof(sh)) and shared_init owns the rest, so a breadcrumb
    // moved after sh would start as arena garbage.
    static_assert(offsetof(Block, probe_id) < offsetof(Block, sh),
                  "the breadcrumbs must precede sh to be zeroed by block_init");
    static_assert(BLOCK_SIZE <= 4096u,
                  "a naturally aligned block this size cannot span a 4 KiB page boundary");
    static_assert(KOS_USB_CDC_BULK_MAX_PACKET <= MAX_XFER
                      and KOS_USB_CDC_EP0_MAX_PACKET <= MAX_XFER,
                  "a transfer longer than MAX_XFER could walk past the dTD's page 0");

    constexpr uint16_t READY_OFFSET =
        static_cast<uint16_t>(offsetof(Block, sh) + usb::KOS_USB_READY_OFFSET);

    // Every method touches the granted window, which only the IRQ thread holds (k_desc below).
    struct RtUsb
    {
        Block* blk;
        uintptr_t regs;

        usb::Shared* stats_block() { return &blk->sh; }

        void set_stage(uint32_t s) { blk->stage = s; }

        // Read then write, not a fetch_or: that is a libcall in a freestanding link.
        void note_stall(stall_slot which)
        {
            uint32_t const seen = blk->stalls;
            blk->stalls = seen | (1u << static_cast<uint32_t>(which));
        }

        // A descriptor (Normal memory) must be visible before the prime (Device) doorbell.
        static void order_before_doorbell() { __asm volatile("dsb" ::: "memory"); }

        // The mirror: ENDPTCOMPLETE is a Device read, the retired dTD is Normal memory.
        static void order_after_doorbell() { __asm volatile("dsb" ::: "memory"); }

        static uint32_t addr_of(void const* p)
        {
            return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
        }

        // USBCMD carries ITC in 23:16 and RS in bit 0, so no write to it may be absolute.
        void cmd_set(uint32_t bits)
        {
            r32(regs + reg::USBCMD) = r32(regs + reg::USBCMD) | bits;
        }

        void cmd_clear(uint32_t bits)
        {
            r32(regs + reg::USBCMD) = r32(regs + reg::USBCMD) & ~bits;
        }

        // RM 42.5.6.3.2: an ENDPTCTRLx write must preserve the endpoint type field.
        void epctrl_update(uint8_t ep, uint32_t clear, uint32_t set)
        {
            uintptr_t const a = regs + reg::endptctrl(ep);
            r32(a) = (r32(a) & ~clear) | set;
        }

        // ------------------------------------------------------------------------------
        // Descriptor construction.

        // RM 42.5.6.5.1: legal only while the endpoint is un-primed with no dTD outstanding.
        void qh_init(uint8_t qi, uint32_t max_packet, bool ios)
        {
            reg::Dqh& q = blk->qh[qi];
            // Mult stays 00: RM 42.5.6.4.2.2 requires it of control, bulk and interrupt.
            // ZLT disabled, or the controller appends a zero-length packet of its own on top
            // of the ones the class layer sends.
            uint32_t caps = reg::DQH_ZLT_DISABLE | (max_packet << reg::DQH_MAXLEN_SHIFT);
            if (ios)
            {
                caps |= reg::DQH_IOS;
            }
            q.caps = caps;
            q.current_td = 0;
            q.next_td = reg::DTD_TERMINATE;
            q.token = 0; // Active and Halted clear
            for (uint32_t i = 0; i < 5u; i++)
            {
                q.buf[i] = 0;
            }
            q.setup[0] = 0;
            q.setup[1] = 0;
        }

        static uint32_t ep_bit(uint8_t qi)
        {
            uint8_t const ep = static_cast<uint8_t>(qi / 2u);
            if ((qi & 1u) != 0u)
            {
                return reg::ep_bit_in(ep);
            }
            return reg::ep_bit_out(ep);
        }

        // An endpoint whose ENDPTSTAT or ENDPTPRIME bit is set owns its queue head's transfer
        // overlay: writing it corrupts a transfer the bus master is executing.
        bool own_overlay(uint32_t bit)
        {
            uint32_t const live =
                (r32(regs + reg::ENDPTSTAT) | r32(regs + reg::ENDPTPRIME)) & bit;
            if (live == 0u)
            {
                return true;
            }
            return flush(bit);
        }

        // RM 42.5.6.6.2, then Case 1 of RM 42.5.6.6.3: the guard above makes an un-quiesced
        // endpoint a refusal rather than an append, so the list is empty at every add.
        void queue(uint8_t qi, uint8_t const* buf, uint32_t len)
        {
            uint32_t const bit = ep_bit(qi);
            if (not own_overlay(bit))
            {
                return; // flush() counted it; the host retries a transfer this one drops
            }
            reg::Dtd& t = blk->td[qi];
            t.next = reg::DTD_TERMINATE;
            t.token = ((len & reg::DTD_TOTAL_MASK) << reg::DTD_TOTAL_SHIFT) | reg::DTD_IOC
                      | reg::DTD_ACTIVE;
            t.buf[0] = addr_of(buf);
            // RM 42.5.6.6.2 step 7 sets pages 1..4 one page above each predecessor. They get
            // page 0 of this block instead: MAX_XFER keeps every transfer inside page 0, and
            // a page outside the grant would aim a bus master, which no MPU checks, at
            // memory this driver does not own.
            uint32_t const page0 = addr_of(blk);
            for (uint32_t i = 1; i < 5u; i++)
            {
                t.buf[i] = page0;
            }
            t.spare = 0;

            reg::Dqh& q = blk->qh[qi];
            q.next_td = addr_of(&t) & reg::DTD_PTR_MASK; // terminate bit clear, one DWord
            q.token = 0;                                 // clears a stale Active or Halted
            prime(qi);
        }

        void prime(uint8_t qi)
        {
            order_before_doorbell();
            // Write-to-set: a read-modify-write would race the bits hardware is clearing.
            r32(regs + reg::ENDPTPRIME) = ep_bit(qi);
        }

        // Bytes actually moved: the controller decrements Total Bytes by what it
        // transferred (RM 42.5.6.6.4), so the residual is what is left of the request.
        uint32_t retired_len(uint8_t qi, uint32_t requested)
        {
            order_after_doorbell();
            uint32_t const tok = blk->td[qi].token;
            // ACTIVE is part of this test: an unretired dTD's residual means nothing yet, and
            // a FLUSHED one keeps ACTIVE set for ever, RM 42.7.36 de-priming the buffer
            // without touching the descriptor.
            if ((tok & (reg::DTD_ACTIVE | reg::DTD_ERR_MASK)) != 0u)
            {
                if ((tok & reg::DTD_ERR_MASK) != 0u)
                {
                    // The wire ABI carries no USB error counter, so a halted or errored
                    // transfer lands under framing.
                    kos_counter_increment(&stats_block()->stats.rx_framing, 1u);
                }
                return 0;
            }
            uint32_t const left = (tok >> reg::DTD_TOTAL_SHIFT) & reg::DTD_TOTAL_MASK;
            if (left > requested)
            {
                return 0; // a token this driver did not build; trust neither number
            }
            return requested - left;
        }

        // RM 42.5.6.6.5. True means the named directions are neither primed nor pending a
        // prime, so their transfer overlays are software's again. ENDPTPRIME is drained
        // FIRST: a prime the controller has accepted but not yet acted on sets no ENDPTSTAT
        // bit, so testing ENDPTSTAT alone would report clean and the controller would arm the
        // old descriptor a microframe later.
        bool flush(uint32_t bits)
        {
            for (uint32_t round = 0; round < 2u; round++)
            {
                uint32_t spin = 0;
                while ((r32(regs + reg::ENDPTPRIME) & bits) != 0u and ++spin < SPIN_MAX)
                {
                }
                if (spin >= SPIN_MAX)
                {
                    note_stall(STALL_PRIME);
                }
                r32(regs + reg::ENDPTFLUSH) = bits;
                spin = 0;
                while ((r32(regs + reg::ENDPTFLUSH) & bits) != 0u and ++spin < SPIN_MAX)
                {
                }
                if (spin >= SPIN_MAX)
                {
                    note_stall(STALL_FLUSH);
                }
                uint32_t const live =
                    (r32(regs + reg::ENDPTSTAT) | r32(regs + reg::ENDPTPRIME)) & bits;
                if (live == 0u)
                {
                    return true;
                }
            }
            // RM 42.5.6.6.5: the controller refuses a flush while a packet is in progress.
            // Two rounds did not clear it, so the caller must not touch the overlay.
            note_stall(STALL_UNSAFE);
            kos_counter_increment(&stats_block()->stats.rx_overrun, 1u);
            return false;
        }

        // ------------------------------------------------------------------------------
        // The UsbDev interface.

        int bring_up()
        {
            // Every access below is refused at the bridge until this returns: the AIPS slot
            // resets supervisor-protected and RM 32.8.2 terminates the access.
            if (kos_periph_enable(regs) != 0)
            {
                // The refusal reaches ROOT as a stalled ready latch, so the reason has to be
                // in the block: nothing this thread prints has a receiver yet.
                set_stage(STAGE_DECLINED);
                return -KOS_EPERM;
            }

            set_stage(STAGE_PROBE);
            uint32_t const id = r32(regs + reg::ID);
            blk->probe_id = id;

            set_stage(STAGE_RST_WRITE);
            // The boot ROM drives OTG1 for serial download (RM 9.9.1), so this block can
            // arrive already in device mode against the ROM's queue heads, and USBMODE.CM is
            // write-once after a controller reset (RM 42.7.33): the reset comes first.
            cmd_clear(reg::USBCMD_RS);
            r32(regs + reg::USBCMD) = r32(regs + reg::USBCMD) | reg::USBCMD_RST;
            set_stage(STAGE_RST_WAIT);
            uint32_t spin = 0;
            while ((r32(regs + reg::USBCMD) & reg::USBCMD_RST) != 0u and ++spin < SPIN_MAX)
            {
            }
            if (spin >= SPIN_MAX)
            {
                note_stall(STALL_RESET);
            }

            // Device mode, little endian, setup lockout OFF so the SUTW tripwire is the
            // extraction protocol (RM 42.5.6.4.2.1). USBMODE resets to 0x5000, so two bits
            // RM 42.7.33 calls reserved come up SET and an absolute write would clear them.
            uint32_t const mode =
                r32(regs + reg::USBMODE)
                & ~(reg::USBMODE_CM_MASK | reg::USBMODE_ES_BIG_ENDIAN | reg::USBMODE_SDIS);
            r32(regs + reg::USBMODE) = mode | reg::USBMODE_CM_DEVICE | reg::USBMODE_SLOM;
            set_stage(STAGE_MODE);

            // Full speed forced: the shared descriptor table declares a 32-byte bulk endpoint
            // (usb_cdc.h) and a high-speed bulk endpoint must declare 512, so a successful
            // chirp would enumerate a device whose descriptors are out of spec.
            uint32_t const port = r32(regs + reg::PORTSC1) & ~reg::PORTSC1_W1C;
            r32(regs + reg::PORTSC1) = port | reg::PORTSC1_PFSC;

            // RM 42.7.32: OT must be set in device mode, it being what pulls DM down.
            uint32_t const otg = r32(regs + reg::OTGSC) & ~reg::OTGSC_W1C;
            r32(regs + reg::OTGSC) = otg | reg::OTGSC_OT;
            set_stage(STAGE_PORT);

            r32(regs + reg::ENDPTLISTADDR) = addr_of(blk) & reg::ENDPTLISTADDR_MASK;

            // RM 42.5.6.1's recommended device set, minus SRI: a start-of-frame interrupt
            // would wake this thread every millisecond for nothing.
            r32(regs + reg::USBINTR) = reg::USBINTR_UE | reg::USBINTR_UEE | reg::USBINTR_PCE
                                       | reg::USBINTR_URE | reg::USBINTR_SLE;
            // RS is left clear: RM 42.5.6.1 wants the endpoint-zero queue heads built before
            // the device attaches, so attach() does it after ep_open_all.
            set_stage(STAGE_ARMED);
            return 0;
        }

        void attach()
        {
            cmd_set(reg::USBCMD_RS);
            set_stage(STAGE_ATTACHED);
        }

        void ep_open_all()
        {
            // QUIESCE FIRST. On a SET_CONFIGURATION re-entry bulk OUT may already be
            // primed, and qh_init would zero a live overlay's page pointers. RM 42.7.40
            // also wants an un-primed endpoint before TXR/RXR.
            (void)flush(reg::EP_BIT_ALL);
            pending_complete_ = 0; // the transfers those bits described are cancelled
            set_stage(STAGE_FLUSHED);

            // Endpoint 0 needs no ENDPTCTRL0 write (RM 42.5.6.1: always enabled, fixed
            // control), but both its queue heads must exist. Only the RX head receives
            // setup data (RM 42.5.5.1.4), so only it asks for the setup interrupt.
            qh_init(reg::qh_out(0), KOS_USB_CDC_EP0_MAX_PACKET, true);
            qh_init(reg::qh_in(0), KOS_USB_CDC_EP0_MAX_PACKET, false);

            qh_init(reg::qh_in(KOS_USB_CDC_EP_NOTIFY),
                    KOS_USB_CDC_NOTIFY_MAX_PACKET, false);
            qh_init(reg::qh_out(KOS_USB_CDC_EP_DATA),
                    KOS_USB_CDC_BULK_MAX_PACKET, false);
            qh_init(reg::qh_in(KOS_USB_CDC_EP_DATA),
                    KOS_USB_CDC_BULK_MAX_PACKET, false);
            order_before_doorbell(); // the enables below let the controller read them

            // RM 42.5.6.3.1 Table 42-66: toggle reset set, toggle inhibit clear, type, no
            // stall, enable. Absolute writes: ENDPTCTRLn resets to zero, and RM 42.5.6.3.2's
            // read-modify-write rule covers writes DURING operation, which is ep_stall.
            //
            // The notification endpoint's disabled OUT half still gets its type moved off
            // control: RM 42.7.40 warns that the default control type leaves PID tracking on
            // the active direction undefined.
            uint32_t const notify_type = reg::EPTYPE_INTERRUPT;
            r32(regs + reg::endptctrl(KOS_USB_CDC_EP_NOTIFY)) =
                reg::EPCTRL_TXE | reg::EPCTRL_TXR | (notify_type << reg::EPCTRL_TXT_SHIFT)
                | (notify_type << reg::EPCTRL_RXT_SHIFT);
            uint32_t const data_type = reg::EPTYPE_BULK;
            r32(regs + reg::endptctrl(KOS_USB_CDC_EP_DATA)) =
                reg::EPCTRL_TXE | reg::EPCTRL_TXR | (data_type << reg::EPCTRL_TXT_SHIFT)
                | reg::EPCTRL_RXE | reg::EPCTRL_RXR | (data_type << reg::EPCTRL_RXT_SHIFT);
            // Nothing is primed here; an un-primed endpoint NAKs (RM 42.7.35).
            set_stage(STAGE_EP_OPEN);
        }

        uint32_t take_events()
        {
            uint32_t const sts = r32(regs + reg::USBSTS);
            constexpr uint32_t WATCHED = reg::USBSTS_UI | reg::USBSTS_UEI | reg::USBSTS_PCI
                                         | reg::USBSTS_SEI | reg::USBSTS_URI
                                         | reg::USBSTS_SLI;
            uint32_t const act = sts & WATCHED;
            if (act != 0u)
            {
                // Clear exactly what was read: a source asserting in between must not be
                // thrown away.
                r32(regs + reg::USBSTS) = act;
            }
            if ((act & reg::USBSTS_UEI) != 0u)
            {
                // The wire ABI has no USB error counter, so it lands under framing.
                kos_counter_increment(&stats_block()->stats.rx_framing, 1u);
            }
            if ((act & reg::USBSTS_SEI) != 0u)
            {
                // A bus-master read of the descriptor block got an error response.
                kos_counter_increment(&stats_block()->stats.rx_overrun, 1u);
            }

            if ((act & reg::USBSTS_URI) != 0u)
            {
                // A bus reset supersedes the rest of this pass: handling it re-initialises the
                // queue heads, so a setup reported alongside it would be decoded out of a
                // buffer on_bus_reset had just zeroed.
                return usb::KOS_USB_EV_BUS_RESET;
            }
            uint32_t ev = 0;
            // A setup and a completion are found in their own registers, not in USBSTS: one
            // USBSTS_UI can carry both, and either can be pending with UI already cleared.
            if (r32(regs + reg::ENDPTSETUPSTAT) != 0u)
            {
                ev |= usb::KOS_USB_EV_SETUP;
            }
            // Latched and cleared HERE, not re-read in take_buff_status: on_setup() runs
            // between the two calls and primes new EP0 descriptors, so a later sample would
            // carry the previous transfer's completion into the state the setup just created.
            // A completion landing after this read re-asserts USBSTS.UI for the next pass.
            uint32_t const done = r32(regs + reg::ENDPTCOMPLETE);
            if (done != 0u)
            {
                r32(regs + reg::ENDPTCOMPLETE) = done; // write-1-to-clear exactly what was read
                pending_complete_ |= done;
            }
            if (pending_complete_ != 0u)
            {
                ev |= usb::KOS_USB_EV_BUFFER;
            }
            return ev;
        }

        // The class layer reads bit 2n as EPn IN and bit 2n+1 as EPn OUT; ENDPTCOMPLETE
        // puts OUT at bit n and IN at bit 16+n (RM 42.7.38).
        uint32_t take_buff_status()
        {
            uint32_t const done = pending_complete_;
            pending_complete_ = 0;
            uint32_t bits = 0;
            for (uint8_t ep = 0; ep < reg::EP_COUNT; ep++)
            {
                if ((done & reg::ep_bit_in(ep)) != 0u)
                {
                    bits |= 1u << (2u * ep);
                }
                if ((done & reg::ep_bit_out(ep)) != 0u)
                {
                    bits |= 1u << (2u * ep + 1u);
                }
            }
            return bits;
        }

        void bus_reset_recover()
        {
            // RM 42.5.6.2.1 step 1 names ENDPTSTAT as the register holding the setup token
            // semaphores, but ENDPTSTAT is read-only (RM 42.7.37) and the semaphores are in
            // ENDPTSETUPSTAT (RM 42.7.34), which is what is acknowledged here.
            uint32_t const setup = r32(regs + reg::ENDPTSETUPSTAT);
            if (setup != 0u)
            {
                r32(regs + reg::ENDPTSETUPSTAT) = setup;
            }
            uint32_t const done = r32(regs + reg::ENDPTCOMPLETE);
            if (done != 0u)
            {
                r32(regs + reg::ENDPTCOMPLETE) = done;
            }
            (void)flush(reg::EP_BIT_ALL); // drains ENDPTPRIME itself
            pending_complete_ = 0;        // every transfer those bits described is cancelled
            // DEVICEADDR is not written: the controller resets the address to zero itself on
            // a bus reset (RM 42.5.6.2.1, RM 42.7.23).
        }

        void setup_read(struct kos_usb_setup* out)
        {
            uint32_t const pending = r32(regs + reg::ENDPTSETUPSTAT);
            reg::Dqh& q = blk->qh[reg::qh_out(0)];
            // The acknowledge precedes the copy, per RM 42.5.6.4.2.1 and not the opposite
            // ordering of RM 42.5.6.5.2: SUTW cannot report a setup arriving mid-copy unless
            // the status bit is already clear.
            r32(regs + reg::ENDPTSETUPSTAT) = pending;
            uint32_t w[2] = { 0, 0 };
            bool intact = false;
            for (uint32_t tries = 0; tries < SUTW_TRIES and not intact; tries++)
            {
                cmd_set(reg::USBCMD_SUTW);
                order_after_doorbell();
                w[0] = q.setup[0];
                w[1] = q.setup[1];
                // Hardware clears the tripwire when a new setup arrives mid-copy, so the
                // bit still being up is what says the eight bytes are one packet.
                intact = (r32(regs + reg::USBCMD) & reg::USBCMD_SUTW) != 0u;
            }
            cmd_clear(reg::USBCMD_SUTW);
            if (intact)
            {
                mem_copy(out, w, sizeof(*out));
            }
            else
            {
                // A torn packet decodes to whatever the two halves spell, so it is reported as
                // a VENDOR request, which on_setup stalls outright; the host retries.
                mem_zero(out, sizeof(*out));
                out->bmRequestType = KOS_USB_REQ_TYPE_VENDOR;
                note_stall(STALL_SUTW);
                kos_counter_increment(&stats_block()->stats.rx_framing, 1u);
            }

            // RM 42.5.6.5.2 step 3: a status or data stage from the previous control
            // transfer may still be primed, and it must go before a new one is linked.
            uint32_t const ep0_bits = reg::ep_bit_out(0) | reg::ep_bit_in(0);
            (void)flush(ep0_bits); // a refusal is re-tested by queue()'s own guard
            // Its completion goes with it, from the register AND from this pass's latch: a
            // stage that retired just before the setup would otherwise be delivered after the
            // setup has moved the EP0 state machine on.
            r32(regs + reg::ENDPTCOMPLETE) = ep0_bits;
            pending_complete_ &= ~ep0_bits;
            ep0_out_req_ = 0;
        }

        void set_address(uint8_t addr)
        {
            // USBADRA left clear, so the address takes effect at once (RM 42.7.23), which
            // chapter 9's 2 ms status-stage budget requires.
            r32(regs + reg::DEVICEADDR) =
                static_cast<uint32_t>(addr) << reg::DEVICEADDR_USBADR_SHIFT;
        }

        // `pid` is ignored on every queue site below: the controller owns the data toggle
        // (RM 42.7.40 TXI/RXI), reset with TXR/RXR in ep_open_all and ep_stall.
        void ep0_in(uint8_t const* p, uint32_t n, uint8_t)
        {
            if (n > KOS_USB_CDC_EP0_MAX_PACKET)
            {
                n = KOS_USB_CDC_EP0_MAX_PACKET;
            }
            if (p != nullptr)
            {
                // `p` may point at the descriptor tables in XIP flash or at a caller's
                // stack, and the controller may be sent to read neither.
                mem_copy(blk->ep0_in_buf, p, n);
            }
            queue(reg::qh_in(0), blk->ep0_in_buf, n);
        }

        void ep0_out_arm(uint32_t n, uint8_t)
        {
            if (n > KOS_USB_CDC_EP0_MAX_PACKET)
            {
                n = KOS_USB_CDC_EP0_MAX_PACKET;
            }
            ep0_out_req_ = n;
            queue(reg::qh_out(0), blk->ep0_out_buf, n);
        }

        uint32_t ep0_out_read(uint8_t* out, uint32_t max)
        {
            uint32_t n = retired_len(reg::qh_out(0), ep0_out_req_);
            if (n > max)
            {
                n = max;
            }
            mem_copy(out, blk->ep0_out_buf, n);
            return n;
        }

        void ep0_stall()
        {
            // A protocol stall: RM 42.5.6.3.2 says to set both directions as a pair in one
            // write, and hardware clears them itself at the next setup.
            epctrl_update(0, 0, reg::EPCTRL_TXS | reg::EPCTRL_RXS);
        }

        void ep_in(uint8_t ep, uint8_t const* p, uint32_t n, uint8_t)
        {
            if (ep != KOS_USB_CDC_EP_DATA)
            {
                return; // the notification endpoint is declared but never queued
            }
            if (n > KOS_USB_CDC_BULK_MAX_PACKET)
            {
                n = KOS_USB_CDC_BULK_MAX_PACKET;
            }
            mem_copy(blk->bulk_in_buf, p, n);
            queue(reg::qh_in(ep), blk->bulk_in_buf, n);
        }

        void ep_out_arm(uint8_t ep, uint8_t)
        {
            if (ep != KOS_USB_CDC_EP_DATA)
            {
                return;
            }
            queue(reg::qh_out(ep), blk->bulk_out_buf, KOS_USB_CDC_BULK_MAX_PACKET);
        }

        uint32_t ep_out_read(uint8_t ep, uint8_t* out, uint32_t max)
        {
            if (ep != KOS_USB_CDC_EP_DATA)
            {
                return 0;
            }
            uint32_t n = retired_len(reg::qh_out(ep), KOS_USB_CDC_BULK_MAX_PACKET);
            if (n > max)
            {
                n = max;
            }
            mem_copy(out, blk->bulk_out_buf, n);
            return n;
        }

        void ep_stall(uint8_t addr, bool on)
        {
            uint8_t const num = static_cast<uint8_t>(addr & 0x0Fu);
            if (num == 0u)
            {
                if (on)
                {
                    ep0_stall();
                }
                return;
            }
            uint32_t stall = reg::EPCTRL_RXS;
            uint32_t toggle_reset = reg::EPCTRL_RXR;
            uint32_t bit = reg::ep_bit_out(num);
            if ((addr & 0x80u) != 0u)
            {
                stall = reg::EPCTRL_TXS;
                toggle_reset = reg::EPCTRL_TXR;
                bit = reg::ep_bit_in(num);
            }
            // Setting the stall bit neither un-primes the endpoint nor produces a completion,
            // so the dTD armed before the halt stays outstanding and the class layer's re-arm
            // after a clear would write an overlay the controller still owns.
            (void)flush(bit);
            pending_complete_ &= ~bit;
            if (on)
            {
                epctrl_update(num, 0, stall);
                return;
            }
            // Chapter 9: clearing a halt restarts the data toggle at DATA0, which is
            // RM 42.5.6.3.3.1's toggle-reset bit, in the SAME write that drops the stall.
            epctrl_update(num, stall, toggle_reset);
        }

        // What the last ep0_out_arm asked for, so a retired token's residual becomes a count.
        uint32_t ep0_out_req_ = 0;
        // ENDPTCOMPLETE, sampled by take_events and consumed by take_buff_status.
        uint32_t pending_complete_ = 0;
    };

    void rtusb_irq_thread(void* arg)
    {
        Block* blk = static_cast<Block*>(arg);
        // FIRST statement, before anything that can fault: it separates a thread that never
        // ran from one that ran and died.
        blk->stage = STAGE_ENTERED;
#ifdef KICKOS_RTUSB_UNGRANTED_PROBE
        // USBPHY1 is kernel-only and outside this service's grant, so an enforcing MPU must
        // refuse this read. It does NOT separate an MPU refusal from a bridge one: USBPHY1 is
        // an off-platform AIPS peripheral too.
        uint32_t const phy = r32(kickos::imxrt1062::mmap::USBPHY1_BASE);
        (void)phy;
        kos::print("[rtusb] CONTROL: the ungranted read RETURNED; this thread is unrestricted\n");
#endif
        RtUsb dev;
        dev.blk = blk;
        dev.regs = reg::USB1_BASE;
        usb::Cdc<RtUsb> cdc(dev, &blk->sh);
        if (cdc.bring_up() != 0)
        {
            // Leaving the ready latch clear is the report: drv::bring_up gives up on it,
            // unwinds, and rtusb_console_start prints the stage this thread left behind.
            exit(0);
        }
        usb::irq_loop(cdc, &blk->sh); // parks in irq_wait; never returns
    }

    void rtusb_service_thread(void* arg)
    {
        usb::console_serve_loop(&static_cast<Block*>(arg)->sh);
    }

    // The block drv::bring_up allocated: it does not hand the pointer back, so block_init is
    // where this TU sees it.
    Block* g_blk = nullptr;

    int block_init(void* raw, struct kos_service_cfg const*)
    {
        // A bus master pointed at a mis-aligned list corrupts memory rather than faulting,
        // so the allocator's alignment is checked rather than assumed.
        if ((reinterpret_cast<uintptr_t>(raw) & (BLOCK_SIZE - 1u)) != 0u)
        {
            return -1;
        }
        Block* blk = static_cast<Block*>(raw);
        mem_zero(blk, offsetof(Block, sh)); // the descriptor half; shared_init owns the rest
        usb::shared_init(&blk->sh);
        g_blk = blk;
        return 0;
    }

    char const* stage_reason(uint32_t s)
    {
        switch (s)
        {
        case STAGE_NONE:
        {
            return "the thread never ran (spawn or scheduling, not the device)\n";
        }
        case STAGE_ENTERED:
        {
            return "died before touching the window at all (thread prologue, in RAM)\n";
        }
        case STAGE_DECLINED:
        {
            return "declined at the bridge: kos_periph_enable refused, the window stayed off\n";
        }
        case STAGE_PROBE:
        {
            return "died on the FIRST window READ: the window is unreachable or dead\n";
        }
        case STAGE_RST_WRITE:
        {
            return "read the window, then died on the first WRITE (USBCMD)\n";
        }
        case STAGE_RST_WAIT:
        {
            return "died waiting out the controller reset (USBCMD.RST)\n";
        }
        case STAGE_MODE:
        {
            return "died after USBMODE, at PORTSC1 or OTGSC\n";
        }
        case STAGE_PORT:
        {
            return "died at the list address or the interrupt enables\n";
        }
        case STAGE_ARMED:
        {
            return "died quiescing the endpoints (ENDPTFLUSH)\n";
        }
        case STAGE_FLUSHED:
        {
            return "died building the queue heads or enabling the endpoints\n";
        }
        case STAGE_EP_OPEN:
        {
            return "died attaching (USBCMD.RS)\n";
        }
        case STAGE_ATTACHED:
        {
            return "attached, then died before the ready latch\n";
        }
        default:
        {
            return "left an unrecognised stage in the block\n";
        }
        }
    }

    // The VALUE is the clock verdict: the controller cannot return its own identification
    // constant without a bus clock.
    char const* probe_reason(uint32_t id)
    {
        if (id == reg::ID_RESET)
        {
            return "  window answered ID correctly: mapped, clocked and alive\n";
        }
        if (id == 0u)
        {
            return "  window read back ZERO: address decodes, nothing drives it "
                   "(CCGR6 usboh3 gate did not take)\n";
        }
        if (id == 0xFFFFFFFFu)
        {
            return "  window read back ALL-ONES: nothing is answering on the bus\n";
        }
        return "  window answered an UNEXPECTED id (see the hex below)\n";
    }

    void print_hex(uint32_t v)
    {
        static char const digits[] = "0123456789abcdef";
        char buf[11];
        buf[0] = '0';
        buf[1] = 'x';
        for (uint32_t i = 0; i < 8u; i++)
        {
            buf[2u + i] = digits[(v >> (28u - 4u * i)) & 0xFu];
        }
        buf[10] = '\0';
        kos::print(buf);
    }

    // Indexed by stall_slot, so the enum is the only list; the assert below is what keeps the
    // mapping total.
    constexpr char const* STALL_TEXT[] = {
        "  a bounded wait EXPIRED: USBCMD.RST never cleared\n",
        "  the endpoints would not QUIESCE: two flush rounds left them primed\n",
        "  a bounded wait EXPIRED: ENDPTPRIME never drained\n",
        "  a bounded wait EXPIRED: ENDPTFLUSH never cleared\n",
        "  a TORN setup: the SUTW tripwire never held for one extraction\n"
    };

    constexpr uint32_t STALL_SLOTS = static_cast<uint32_t>(STALL_COUNT);

    static_assert(sizeof(STALL_TEXT) / sizeof(STALL_TEXT[0]) == STALL_SLOTS,
                  "every stall_slot needs its own line, or a stall would print another's reason");

    char const* stall_reason(uint32_t bits)
    {
        for (uint32_t slot = 0; slot < STALL_SLOTS; slot++)
        {
            if ((bits & (1u << slot)) != 0u)
            {
                return STALL_TEXT[slot];
            }
        }
        return nullptr;
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[rtusb] ",
        // The register map is hard-wired to USB1 and the vector is claimed by number, so a
        // cfg naming another window would grant one block and poke another.
        .expected_base = reg::USB1_BASE,
        .block_size = BLOCK_SIZE,
        // The controller reads its dQH/dTD lists and writes transfer results out of this
        // block as a BUS MASTER, and this tree has no cache-maintenance primitive.
        .block_flags = KOS_MEM_NOCACHE,
        .ready_offset = READY_OFFSET,
        // The publish blinds the kernel console, which is LPUART6 on pins 0/1 and a
        // different peripheral from the one taken here.
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 1,
        .thread_count = 2,
        // irq_loop latches ready before enumeration, so the poll does not wait for a cable.
        .barrier_after = 1,
        // LEVEL: USBSTS is an OR of sources cleared at the peripheral, and the NVIC line
        // follows it.
        .lines = {{rtirq::USB_OTG1_IRQ, KOS_IRQ_LEVEL}},
        .threads = {{.entry = rtusb_irq_thread,
                     .name = "rtusbirq",
                     .prio_delta = 1,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT}}},
                    {.entry = rtusb_service_thread,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_BLOCK,
                     .window_grant = false,
                     .cap_count = 2,
                     // The SAME line as the doorbell, SIGNAL only: a pure post on the
                     // binding, not a raise at the controller.
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_SIGNAL}}}},
        .block_init = block_init
    };

    static_assert(drv::valid(k_desc), "the rtusb descriptor is not a well-formed driver shape");
    // usb::desc_ok is the same check spelled for a 2 KiB block with Shared at offset 0,
    // which this driver cannot use: the dQH list has to sit at offset 0.
    static_assert(drv::ring_doorbell_shape_ok(k_desc, READY_OFFSET, BLOCK_SIZE),
                  "the rtusb cap positions do not match KOS_USB_CAP_*");
}

extern "C"
{

int rtusb_console_start(struct kos_service_cfg const* cfg)
{
    int const rc = drv::bring_up(k_desc, cfg, nullptr);
    if (rc == 0)
    {
        return 0;
    }
    // The unwind reclaimed the console, so a print reaches the wire again from here. How far
    // the IRQ thread got is only in the block; what it printed had no receiver.
    if (g_blk != nullptr)
    {
        uint32_t const stage = g_blk->stage;
        uint32_t const stalls = g_blk->stalls;
        kos::print(k_desc.tag);
        kos::print("  irq thread: ");
        kos::print(stage_reason(stage));
        // Before STAGE_RST_WRITE the field is still zero, and printing it would invent a
        // clock verdict out of an unread register.
        if (stage >= STAGE_RST_WRITE)
        {
            uint32_t const id = g_blk->probe_id;
            kos::print(k_desc.tag);
            kos::print(probe_reason(id));
            kos::print(k_desc.tag);
            kos::print("  id=");
            print_hex(id);
            kos::print("\n");
        }
        char const* stall = stall_reason(stalls);
        if (stall != nullptr)
        {
            kos::print(k_desc.tag);
            kos::print(stall);
        }
    }
    return rc;
}

}
