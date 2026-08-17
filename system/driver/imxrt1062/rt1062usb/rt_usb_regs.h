// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 USB device-controller register map and descriptor layout, from the
// i.MX RT1060 Processor Reference Manual, Rev. 3 (IMXRT1060RM) chapter 42. Offsets and
// layout only. The PHY, PLL_USB1 and the CCGR6 gate belong to the kernel
// (arch/arm/chip/imxrt1062/) and are not named here.
//
//   - The controller is a BUS MASTER, so a wrong field corrupts memory at DMA time instead
//     of faulting at the poke site. Coherency rests on KICKOS_IMXRT_DCACHE=OFF; no cache
//     maintenance exists.
//   - Two per-endpoint bitmap conventions appear below. ENDPTPRIME / ENDPTFLUSH /
//     ENDPTSTAT / ENDPTCOMPLETE put OUT endpoint n at bit n and IN at bit 16 + n
//     (RM 42.7.35, 42.7.38); the dQH list puts OUT at index 2n and IN at 2n + 1
//     (RM 42.5.5); the class layer's buff_status has IN at bit 2n and OUT at 2n + 1.
//   - The data toggle is HARDWARE-owned (ENDPTCTRLn TXI/RXI default to PID sequencing
//     enabled, RM 42.7.40). Software resets it with TXR/RXR and never sets a PID.
//   - No structure the controller reads may span a 4 KiB page boundary (RM 42.5.5).

#ifndef KICKOS_DRIVER_IMXRT1062_RT_USB_REGS_H
#define KICKOS_DRIVER_IMXRT1062_RT_USB_REGS_H

#include <stddef.h>
#include <stdint.h>

namespace kickos::rtusb::reg
{
    // USB1 (OTG1). RM Table 3-1 gives the peripheral 402E_0000-402E_3FFF, of which the CORE
    // is 512 B (RM 42.7 addresses every register as "base + off + 512d * i"; OTG2 starts at
    // +0x200, USBNC at +0x800).
    constexpr uintptr_t USB1_BASE = 0x402E0000u;
    constexpr uint32_t USB1_WINDOW = 0x200u;

    // Identification register (RM 42.7.1), read-only, reset E4A1_FA05.
    constexpr uintptr_t ID = 0x000u;
    constexpr uint32_t ID_RESET = 0xE4A1FA05u;

    // Capability registers (RM 42.7.17).
    constexpr uintptr_t DCCPARAMS = 0x124u;
    constexpr uint32_t DCCPARAMS_DEN_MASK = 0x1Fu; // reset 0x188 -> DEN = 8 endpoints

    // Operational registers (RM 42.7.18 to 42.7.46). RM 42.5.1: DWORD access only.
    constexpr uintptr_t USBCMD = 0x140u;
    constexpr uintptr_t USBSTS = 0x144u;
    constexpr uintptr_t USBINTR = 0x148u;
    constexpr uintptr_t DEVICEADDR = 0x154u;    // the device alias of PERIODICLISTBASE
    constexpr uintptr_t ENDPTLISTADDR = 0x158u; // the device alias of ASYNCLISTADDR
    constexpr uintptr_t BURSTSIZE = 0x160u;
    constexpr uintptr_t PORTSC1 = 0x184u;
    constexpr uintptr_t OTGSC = 0x1A4u;
    constexpr uintptr_t USBMODE = 0x1A8u;
    constexpr uintptr_t ENDPTSETUPSTAT = 0x1ACu;
    constexpr uintptr_t ENDPTPRIME = 0x1B0u;
    constexpr uintptr_t ENDPTFLUSH = 0x1B4u;
    constexpr uintptr_t ENDPTSTAT = 0x1B8u; // read-only
    constexpr uintptr_t ENDPTCOMPLETE = 0x1BCu;
    constexpr uintptr_t endptctrl(uint8_t ep) { return 0x1C0u + 4u * ep; } // EP0..EP7

    // USBCMD (RM 42.7.18). ITC at 23:16 resets to 8, so an absolute write here would also
    // retune the interrupt threshold.
    constexpr uint32_t USBCMD_RS = 1u << 0;    // run/stop: the D+ pull-up, so the attach
    constexpr uint32_t USBCMD_RST = 1u << 1;   // controller reset, self-clearing
    constexpr uint32_t USBCMD_SUTW = 1u << 13; // setup tripwire, device mode only
    constexpr uint32_t USBCMD_ATDTW = 1u << 14;

    // USBSTS (RM 42.7.19), write-1-to-clear except the read-only schedule bits.
    constexpr uint32_t USBSTS_UI = 1u << 0;  // a dTD with IOC retired, or a short packet
    constexpr uint32_t USBSTS_UEI = 1u << 1; // USB error
    constexpr uint32_t USBSTS_PCI = 1u << 2; // port change: the port reached FS or HS
    constexpr uint32_t USBSTS_SEI = 1u << 4; // system error on a bus-master read
    constexpr uint32_t USBSTS_URI = 1u << 6; // USB reset received
    constexpr uint32_t USBSTS_SRI = 1u << 7; // SOF received
    constexpr uint32_t USBSTS_SLI = 1u << 8; // DCSuspend
    constexpr uint32_t USBSTS_NAKI = 1u << 16;

    // USBINTR (RM 42.7.20). Every enable sits at the bit position of the USBSTS bit it
    // gates, so one mask serves both.
    constexpr uint32_t USBINTR_UE = USBSTS_UI;
    constexpr uint32_t USBINTR_UEE = USBSTS_UEI;
    constexpr uint32_t USBINTR_PCE = USBSTS_PCI;
    constexpr uint32_t USBINTR_URE = USBSTS_URI;
    constexpr uint32_t USBINTR_SLE = USBSTS_SLI;

    // DEVICEADDR (RM 42.7.23). USBADR is the TOP seven bits, not the bottom seven.
    constexpr uint32_t DEVICEADDR_USBADR_SHIFT = 25;
    constexpr uint32_t DEVICEADDR_USBADRA = 1u << 24;

    // ENDPTLISTADDR (RM 42.7.25): EPBASE is 31:11 and bits 10:0 read as zero, so the dQH
    // list base is 2 KiB aligned. The 42.7.25 prose calls the structure 64-byte aligned and
    // the NOTE in RM 42.5.5 says 2k; 2048 is the one the register enforces.
    constexpr uint32_t ENDPTLISTADDR_MASK = 0xFFFFF800u;

    // PORTSC1 (RM 42.7.31). PFSC forces a full-speed connect by suppressing the
    // high-speed chirp.
    constexpr uint32_t PORTSC1_CCS = 1u << 0;
    constexpr uint32_t PORTSC1_CSC = 1u << 1; // write-1-to-clear in host mode, undefined here
    constexpr uint32_t PORTSC1_PEC = 1u << 3; // likewise
    constexpr uint32_t PORTSC1_OCC = 1u << 5;
    constexpr uint32_t PORTSC1_PFSC = 1u << 24;
    constexpr uint32_t PORTSC1_PSPD_SHIFT = 26; // read-only: 0 FS, 1 LS, 2 HS
    constexpr uint32_t PORTSC1_PSPD_MASK = 3u << 26;
    // Every write-1-to-clear bit in the register, so a read-modify-write can mask the set
    // and acknowledge nothing by accident.
    constexpr uint32_t PORTSC1_W1C = PORTSC1_OCC | PORTSC1_CSC | PORTSC1_PEC;

    // OTGSC (RM 42.7.32). OT must be set in device mode: it controls the DM pulldown.
    // Bits 22:16 are the write-1-to-clear interrupt statuses.
    constexpr uint32_t OTGSC_OT = 1u << 3;
    constexpr uint32_t OTGSC_W1C = 0x007F0000u;

    // USBMODE (RM 42.7.33). CM is write-once after a controller reset, so USBCMD.RST must
    // precede this write on a warm boot. SLOM = 1 turns setup lockout OFF, which makes the
    // USBCMD.SUTW tripwire the extraction protocol (RM 42.5.6.4.2.1).
    constexpr uint32_t USBMODE_CM_MASK = 3u << 0;
    constexpr uint32_t USBMODE_CM_DEVICE = 2u << 0;
    constexpr uint32_t USBMODE_ES_BIG_ENDIAN = 1u << 2;
    constexpr uint32_t USBMODE_SLOM = 1u << 3;
    constexpr uint32_t USBMODE_SDIS = 1u << 4;

    // ENDPTPRIME / ENDPTFLUSH / ENDPTSTAT / ENDPTCOMPLETE share one bit layout
    // (RM 42.7.35 to 42.7.38): receive (OUT) endpoint n is bit n, transmit (IN) endpoint
    // n is bit 16 + n. PRIME and FLUSH are write-to-set, COMPLETE is write-1-to-clear,
    // STAT is read-only.
    constexpr uint32_t ep_bit_out(uint8_t ep) { return 1u << ep; }
    constexpr uint32_t ep_bit_in(uint8_t ep) { return 1u << (16u + ep); }
    constexpr uint32_t EP_BIT_ALL = 0x00FF00FFu;

    // ENDPTCTRLn, n >= 1 (RM 42.7.40). ENDPTCTRL0 (RM 42.7.39) implements only the STALL
    // bits: endpoint 0 is always enabled and fixed as a control endpoint. RM 42.5.6.3.2: a
    // write during operational mode must PRESERVE the endpoint type field. TXR/RXR are
    // write-to-set and read back as zero, so a read-modify-write never resets a toggle.
    constexpr uint32_t EPCTRL_RXS = 1u << 0;
    constexpr uint32_t EPCTRL_RXT_SHIFT = 2;
    constexpr uint32_t EPCTRL_RXT_MASK = 3u << 2;
    constexpr uint32_t EPCTRL_RXI = 1u << 5;
    constexpr uint32_t EPCTRL_RXR = 1u << 6; // data toggle reset, write-to-set
    constexpr uint32_t EPCTRL_RXE = 1u << 7;
    constexpr uint32_t EPCTRL_TXS = 1u << 16;
    constexpr uint32_t EPCTRL_TXT_SHIFT = 18;
    constexpr uint32_t EPCTRL_TXT_MASK = 3u << 18;
    constexpr uint32_t EPCTRL_TXI = 1u << 21;
    constexpr uint32_t EPCTRL_TXR = 1u << 22; // data toggle reset, write-to-set
    constexpr uint32_t EPCTRL_TXE = 1u << 23;
    // Endpoint type, both halves (RM 42.7.40). Same encoding as the class layer's
    // kos_usb_ep_type, so the descriptors' bmAttributes can be used directly.
    constexpr uint32_t EPTYPE_CONTROL = 0u;
    constexpr uint32_t EPTYPE_ISOCHRONOUS = 1u;
    constexpr uint32_t EPTYPE_BULK = 2u;
    constexpr uint32_t EPTYPE_INTERRUPT = 3u;

    // ------------------------------------------------------------------------------
    // Device data structures (RM 42.5.5). The controller reads AND writes both as a bus
    // master, so every field is volatile.

    // Endpoint queue head (RM 42.5.5.1, Table 42-57). DWords 2..8 are the TRANSFER
    // OVERLAY: the controller copies the primed dTD into them and owns them until the
    // transfer expires, so software must not write them while a transfer is live.
    struct Dqh
    {
        volatile uint32_t caps;       // DWord 0: Mult 31:30, zlt 29, max packet 26:16, ios 15
        volatile uint32_t current_td; // DWord 1: hardware-owned, never written here
        volatile uint32_t next_td;    // DWord 2: overlay, terminate bit 0
        volatile uint32_t token;      // DWord 3: overlay
        volatile uint32_t buf[5];     // DWords 4..8: overlay
        volatile uint32_t reserved;   // DWord 9: free for software link pointers
        volatile uint32_t setup[2];   // DWords 10..11: the 8 setup bytes, RX queue head only
        volatile uint32_t pad[4];     // to the 64-byte stride
    };
    static_assert(sizeof(Dqh) == 64, "the dQH stride must be the 64 B RM 42.5.5.1 aligns to");
    static_assert(offsetof(Dqh, setup) == 40 and offsetof(Dqh, pad) == 48,
                  "the controller reads the setup bytes at DWords 10..11");

    constexpr uint32_t DQH_MULT_SHIFT = 30;
    constexpr uint32_t DQH_ZLT_DISABLE = 1u << 29; // 1 SUPPRESSES the controller's own ZLP
    constexpr uint32_t DQH_MAXLEN_SHIFT = 16;      // 11 bits, max 1024
    constexpr uint32_t DQH_IOS = 1u << 15;         // raise USBINT on a received setup

    // Endpoint transfer descriptor (RM 42.5.5.2, Table 42-61): seven DWords, which
    // RM 42.5.6.6.2 says to allocate as eight on an eight-DWord boundary.
    struct Dtd
    {
        volatile uint32_t next;   // DWord 0: next pointer 31:5, terminate bit 0
        volatile uint32_t token;  // DWord 1: total bytes 30:16, ioc 15, MultO 11:10, status 7:0
        volatile uint32_t buf[5]; // DWords 2..6: page pointers, current offset in buf[0] 11:0
        volatile uint32_t spare;  // DWord 7: allocated by RM 42.5.6.6.2, unused by hardware
    };
    static_assert(sizeof(Dtd) == 32, "RM 42.5.6.6.2 allocates a dTD as 8 DWords on an 8-DWord boundary");
    static_assert(offsetof(Dtd, buf) == 8 and offsetof(Dtd, spare) == 28,
                  "the controller reads the page pointers at DWords 2..6");

    constexpr uint32_t DTD_TERMINATE = 1u << 0;
    constexpr uint32_t DTD_PTR_MASK = 0xFFFFFFE0u; // bits 31:5
    constexpr uint32_t DTD_TOTAL_SHIFT = 16;       // 15 bits, 30:16
    constexpr uint32_t DTD_TOTAL_MASK = 0x7FFFu;
    constexpr uint32_t DTD_IOC = 1u << 15;
    // dTD status (RM 42.5.5.2, Table 42-63). Bits 4, 2 and 0 are reserved.
    constexpr uint32_t DTD_ACTIVE = 1u << 7;
    constexpr uint32_t DTD_HALTED = 1u << 6;
    constexpr uint32_t DTD_BUFFER_ERR = 1u << 5;
    constexpr uint32_t DTD_XACT_ERR = 1u << 3;
    constexpr uint32_t DTD_ERR_MASK = DTD_HALTED | DTD_BUFFER_ERR | DTD_XACT_ERR;

    // The dQH list (RM 42.5.5, RM 42.5.6.3). DCCPARAMS.DEN is 8 on this part, and the
    // list holds one head per endpoint per direction: EVEN index receives (OUT/SETUP),
    // ODD index transmits (IN/INTERRUPT).
    constexpr uint8_t EP_COUNT = 8;
    constexpr uint8_t QH_COUNT = 2u * EP_COUNT;
    constexpr uint8_t qh_out(uint8_t ep) { return static_cast<uint8_t>(2u * ep); }
    constexpr uint8_t qh_in(uint8_t ep) { return static_cast<uint8_t>(2u * ep + 1u); }
}

#endif
