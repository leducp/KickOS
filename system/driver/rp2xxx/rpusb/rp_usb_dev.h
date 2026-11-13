// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2040 / RP2350 USB device controller behind usb::Cdc: every register access of the
// rpusb backend. Every register write is an absolute value, which absorbs the two parts'
// reset-value differences. Three chip facts bind:
//
//   - The data PID is SOFTWARE-OWNED (buffer control bit 13); the controller never
//     toggles it. Every queue site here therefore carries the toggle, and a missed
//     toggle is a silently dropped packet the host retries forever.
//   - A buffer control word is written in TWO steps: length/PID/FULL, then AVAILABLE at
//     least one clk_usb cycle later. Mandatory on the RP2040; the RP2350's double-read
//     fix makes it redundant but harmless.
//   - EP_ABORT must never be set, on either part: RP2040-E2 makes the device NAK forever on
//     ALL endpoints once it is.

#ifndef KICKOS_DRIVER_RP2XXX_RP_USB_DEV_H
#define KICKOS_DRIVER_RP2XXX_RP_USB_DEV_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/io/mmio.h>
#include <kickos/sys/bytes.h>
#include <kickos/sys/usb_cdc_service.h>

#include "rp_usb_regs.h"

#include <stdint.h>

namespace kickos::rpusb
{
    // 64-byte slots: the endpoint control word carries only bits 15:6 of the offset.
    constexpr uint32_t BULK_IN_BUF = reg::DP_DATA_BASE;       // 0x180
    constexpr uint32_t BULK_OUT_BUF = reg::DP_DATA_BASE + 64; // 0x1C0
    constexpr uint32_t NOTIFY_BUF = reg::DP_DATA_BASE + 128;  // 0x200

    // Every method touches the granted window: IRQ thread only.
    struct RpUsb
    {
        usb::Shared* sh;
        uintptr_t dpram; // the granted base; the register block sits 0x10000 above it
        uintptr_t regs;

        // The AVAILABLE / STALL bits go in a SECOND write, at least one clk_usb cycle
        // after the rest of the word. Three nops covers the documented worst case at
        // 125 MHz clk_sys against 48 MHz clk_usb.
        static void avail_delay()
        {
            __asm volatile("nop");
            __asm volatile("nop");
            __asm volatile("nop");
        }

        void arm_buffer(uintptr_t buf_ctrl, uint32_t word)
        {
            r32(buf_ctrl) = word;
            avail_delay();
            r32(buf_ctrl) = word | reg::BUF_AVAILABLE;
        }

        static void dpram_write(uintptr_t dst, uint8_t const* src, uint32_t n)
        {
            for (uint32_t i = 0; i < n; i++)
            {
                r8(dst + i) = src[i];
            }
        }

        static void dpram_read(uint8_t* dst, uintptr_t src, uint32_t n)
        {
            for (uint32_t i = 0; i < n; i++)
            {
                dst[i] = r8(src + i);
            }
        }

        int bring_up()
        {
            // RESETS and CLOCKS are the kernel's. If chip init refused (the crystal did not
            // come up) the block is still in reset, and the first DPRAM access below faults
            // this thread.
            for (uint32_t off = 0; off < reg::DPRAM_SIZE; off += 4u)
            {
                r32(dpram + off) = 0;
            }
            r32(regs + reg::USB_MUXING) = reg::USB_MUXING_TO_PHY | reg::USB_MUXING_SOFTCON;
            // Write the override value, then the override enable.
            r32(regs + reg::USB_PWR) = reg::USB_PWR_VBUS_DETECT;
            r32(regs + reg::USB_PWR) =
                reg::USB_PWR_VBUS_DETECT | reg::USB_PWR_VBUS_DETECT_OVERRIDE_EN;
            // Absolute, not a set-alias: this write is also what clears the RP2350's
            // MAIN_CTRL.PHY_ISO, which resets SET and is the one mandatory chip delta.
            r32(regs + reg::MAIN_CTRL) = reg::MAIN_CTRL_CONTROLLER_EN;
            r32(regs + reg::SIE_CTRL) = reg::SIE_CTRL_EP0_INT_1BUF;
            r32(regs + reg::INTE) = reg::INT_BUFF_STATUS | reg::INT_BUS_RESET
                                    | reg::INT_SETUP_REQ | reg::INT_DEV_SUSPEND
                                    | reg::INT_DEV_RESUME_FROM_HOST;
            return 0;
        }

        void attach()
        {
            r32(regs + reg::SIE_CTRL) =
                reg::SIE_CTRL_EP0_INT_1BUF | reg::SIE_CTRL_PULLUP_EN;
        }

        void ep_open_all()
        {
            uint32_t const notify =
                reg::EPC_ENABLE | reg::EPC_INT_1BUF
                | (static_cast<uint32_t>(KOS_USB_EP_INTERRUPT) << reg::EPC_TYPE_SHIFT)
                | (NOTIFY_BUF & reg::EPC_ADDR_MASK);
            uint32_t const bulk_in =
                reg::EPC_ENABLE | reg::EPC_INT_1BUF
                | (static_cast<uint32_t>(KOS_USB_EP_BULK) << reg::EPC_TYPE_SHIFT)
                | (BULK_IN_BUF & reg::EPC_ADDR_MASK);
            uint32_t const bulk_out =
                reg::EPC_ENABLE | reg::EPC_INT_1BUF
                | (static_cast<uint32_t>(KOS_USB_EP_BULK) << reg::EPC_TYPE_SHIFT)
                | (BULK_OUT_BUF & reg::EPC_ADDR_MASK);
            r32(dpram + reg::dp_ep_ctrl_in(KOS_USB_CDC_EP_NOTIFY)) = notify;
            r32(dpram + reg::dp_ep_ctrl_in(KOS_USB_CDC_EP_DATA)) = bulk_in;
            r32(dpram + reg::dp_ep_ctrl_out(KOS_USB_CDC_EP_DATA)) = bulk_out;
            // Every buffer starts un-armed: an armed buffer on an endpoint the class
            // layer has not primed would answer a host with stale DPRAM.
            r32(dpram + reg::dp_buf_ctrl_in(KOS_USB_CDC_EP_NOTIFY)) = 0;
            r32(dpram + reg::dp_buf_ctrl_in(KOS_USB_CDC_EP_DATA)) = 0;
            r32(dpram + reg::dp_buf_ctrl_out(KOS_USB_CDC_EP_DATA)) = 0;
        }

        uint32_t take_events()
        {
            uint32_t const ints = r32(regs + reg::INTS);
            uint32_t const st = r32(regs + reg::SIE_STATUS);

            // Line errors: counted and dropped at the source. The SIE gives no per-byte
            // attribution, so an erroneous packet is a lost packet the host retries.
            uint32_t err_clr = 0;
            if ((st & reg::SIE_STATUS_CRC_ERROR) != 0u)
            {
                kos_counter_increment(&sh->stats.rx_framing, 1u);
                err_clr |= reg::SIE_STATUS_CRC_ERROR;
            }
            if ((st & reg::SIE_STATUS_BIT_STUFF_ERROR) != 0u)
            {
                kos_counter_increment(&sh->stats.rx_framing, 1u);
                err_clr |= reg::SIE_STATUS_BIT_STUFF_ERROR;
            }
            if ((st & reg::SIE_STATUS_RX_OVERFLOW) != 0u)
            {
                kos_counter_increment(&sh->stats.rx_overrun, 1u);
                err_clr |= reg::SIE_STATUS_RX_OVERFLOW;
            }
            if ((st & reg::SIE_STATUS_DATA_SEQ_ERROR) != 0u)
            {
                // A data-PID mismatch, counted under framing because the wire ABI has no
                // PID counter.
                kos_counter_increment(&sh->stats.rx_framing, 1u);
                err_clr |= reg::SIE_STATUS_DATA_SEQ_ERROR;
            }
            if (err_clr != 0u)
            {
                r32(regs + reg::SIE_STATUS) = err_clr;
            }

            uint32_t ev = 0;
            if ((ints & reg::INT_BUS_RESET) != 0u)
            {
                ev |= usb::KOS_USB_EV_BUS_RESET;
            }
            if ((ints & reg::INT_SETUP_REQ) != 0u)
            {
                ev |= usb::KOS_USB_EV_SETUP;
            }
            if ((ints & reg::INT_BUFF_STATUS) != 0u)
            {
                ev |= usb::KOS_USB_EV_BUFFER;
            }
            // VBUS is forced detected, so an unplug is a suspend here (RP2350 DS 12.7.3.8.4 /
            // RP2040 DS 4.1.2.8.4).
            uint32_t edge_clr = 0;
            if ((ints & reg::INT_DEV_SUSPEND) != 0u)
            {
                edge_clr |= reg::SIE_STATUS_SUSPENDED;
            }
            if ((ints & reg::INT_DEV_RESUME_FROM_HOST) != 0u)
            {
                edge_clr |= reg::SIE_STATUS_RESUME;
            }
            if (edge_clr != 0u)
            {
                // Clear, THEN read the state: `st` predates the clear, and an edge landing
                // between the two would be acknowledged unseen.
                r32(regs + reg::SIE_STATUS) = edge_clr;
                if ((r32(regs + reg::SIE_STATUS) & reg::SIE_STATUS_SUSPENDED) != 0u)
                {
                    ev |= usb::KOS_USB_EV_SUSPEND;
                }
                else
                {
                    ev |= usb::KOS_USB_EV_RESUME;
                }
            }
            return ev;
        }

        uint32_t take_buff_status()
        {
            uint32_t const bits = r32(regs + reg::BUFF_STATUS);
            // Clear EXACTLY what was observed: a buffer completing between the read and
            // the write would otherwise have its notification thrown away.
            r32(regs + reg::BUFF_STATUS) = bits;
            return bits;
        }

        void bus_reset_recover()
        {
            r32(regs + reg::ADDR_ENDP) = 0;
            r32(regs + reg::SIE_STATUS) = reg::SIE_STATUS_BUS_RESET;
            r32(dpram + reg::dp_buf_ctrl_in(0)) = 0;
            r32(dpram + reg::dp_buf_ctrl_out(0)) = 0;
        }

        void setup_read(struct kos_usb_setup* out)
        {
            // Cleared BEFORE the copy, which is the datasheet's order. The reverse loses
            // a SETUP that arrives mid-copy: the clear would drop its status bit too.
            r32(regs + reg::SIE_STATUS) = reg::SIE_STATUS_SETUP_REC;
            uint32_t const w[2] = { r32(dpram + reg::DP_SETUP),
                                    r32(dpram + reg::DP_SETUP + 4u) };
            mem_copy(out, w, sizeof(*out));
        }

        void set_address(uint8_t addr)
        {
            r32(regs + reg::ADDR_ENDP) = addr;
        }

        void ep0_in(uint8_t const* p, uint32_t n, uint8_t pid)
        {
            if (n > KOS_USB_CDC_EP0_MAX_PACKET)
            {
                n = KOS_USB_CDC_EP0_MAX_PACKET;
            }
            if (p != nullptr)
            {
                dpram_write(dpram + reg::DP_EP0_BUF, p, n);
            }
            uint32_t word = reg::BUF_FULL | reg::BUF_LAST | (n & reg::BUF_LEN_MASK);
            if (pid != 0u)
            {
                word |= reg::BUF_PID_DATA1;
            }
            arm_buffer(dpram + reg::dp_buf_ctrl_in(0), word);
        }

        void ep0_out_arm(uint32_t n, uint8_t pid)
        {
            if (n > KOS_USB_CDC_EP0_MAX_PACKET)
            {
                n = KOS_USB_CDC_EP0_MAX_PACKET;
            }
            uint32_t word = reg::BUF_LAST | (n & reg::BUF_LEN_MASK);
            if (pid != 0u)
            {
                word |= reg::BUF_PID_DATA1;
            }
            arm_buffer(dpram + reg::dp_buf_ctrl_out(0), word);
        }

        uint32_t ep0_out_read(uint8_t* out, uint32_t max)
        {
            uint32_t n = r32(dpram + reg::dp_buf_ctrl_out(0)) & reg::BUF_LEN_MASK;
            if (n > max)
            {
                n = max;
            }
            // EP0's single buffer is shared between the two directions, so an OUT lands
            // at the same DPRAM offset an IN was sent from.
            dpram_read(out, dpram + reg::DP_EP0_BUF, n);
            return n;
        }

        void ep0_stall()
        {
            // EP0 needs BOTH the buffer control STALL bit and its EP_STALL_ARM bit; the
            // controller clears EP_STALL_ARM itself when the next SETUP arrives, which is
            // what makes the stall protocol-correct rather than sticky.
            r32(regs + reg::EP_STALL_ARM) =
                reg::STALL_ARM_EP0_IN | reg::STALL_ARM_EP0_OUT;
            r32(dpram + reg::dp_buf_ctrl_in(0)) = 0;
            avail_delay();
            r32(dpram + reg::dp_buf_ctrl_in(0)) = reg::BUF_STALL;
            r32(dpram + reg::dp_buf_ctrl_out(0)) = 0;
            avail_delay();
            r32(dpram + reg::dp_buf_ctrl_out(0)) = reg::BUF_STALL;
        }

        void ep_in(uint8_t ep, uint8_t const* p, uint32_t n, uint8_t pid)
        {
            if (ep != KOS_USB_CDC_EP_DATA)
            {
                return;
            }
            if (n > KOS_USB_CDC_BULK_MAX_PACKET)
            {
                n = KOS_USB_CDC_BULK_MAX_PACKET;
            }
            dpram_write(dpram + BULK_IN_BUF, p, n);
            uint32_t word = reg::BUF_FULL | reg::BUF_LAST | (n & reg::BUF_LEN_MASK);
            if (pid != 0u)
            {
                word |= reg::BUF_PID_DATA1;
            }
            arm_buffer(dpram + reg::dp_buf_ctrl_in(ep), word);
        }

        void ep_out_arm(uint8_t ep, uint8_t pid)
        {
            uint32_t word = reg::BUF_LAST
                            | (KOS_USB_CDC_BULK_MAX_PACKET & reg::BUF_LEN_MASK);
            if (pid != 0u)
            {
                word |= reg::BUF_PID_DATA1;
            }
            arm_buffer(dpram + reg::dp_buf_ctrl_out(ep), word);
        }

        uint32_t ep_out_read(uint8_t ep, uint8_t* out, uint32_t max)
        {
            uint32_t n = r32(dpram + reg::dp_buf_ctrl_out(ep)) & reg::BUF_LEN_MASK;
            if (n > max)
            {
                n = max;
            }
            dpram_read(out, dpram + BULK_OUT_BUF, n);
            return n;
        }

        void ep_stall(uint8_t addr, bool on)
        {
            uint8_t const num = static_cast<uint8_t>(addr & 0x0Fu);
            if (num == 0u)
            {
                // Only the SET direction reaches EP0. Nothing un-stalls the control
                // endpoint here, and with EP_ABORT forbidden by RP2040-E2 a stall this
                // path armed by mistake would need a bus reset to clear.
                if (on)
                {
                    ep0_stall();
                }
                return;
            }
            uintptr_t buf_ctrl = dpram + reg::dp_buf_ctrl_out(num);
            if ((addr & 0x80u) != 0u)
            {
                buf_ctrl = dpram + reg::dp_buf_ctrl_in(num);
            }
            r32(buf_ctrl) = 0;
            if (on)
            {
                avail_delay();
                r32(buf_ctrl) = reg::BUF_STALL;
            }
        }
    };
}

#endif
