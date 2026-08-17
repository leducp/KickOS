// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// USB 2.0 chapter-9 request vocabulary, the CDC-ACM class requests, and the descriptor
// set a KickOS USB console publishes. Pure data and pure functions: no MMIO, no
// controller knowledge, no ring.
//
// PROVENANCE, weaker than every other register fact in this tree: the local reference set
// holds NO USB 2.0 specification and NO CDC/PSTN class document (design-m4.6.2-usb-cdc.md
// section 4.4). Every constant below is stated from the specifications as known and is NOT
// verified against a local copy; the failure mode is a host that refuses to enumerate.
//
// wMaxPacketSize on the two bulk endpoints is 32, not 64: RP2040-E15 hangs the device
// controller against a VL805 host when a full-speed bulk IN buffer exceeds 50 bytes, and
// 32 puts every buffer under that threshold on both RP parts (design section 1.3).

#ifndef KICKOS_SYS_USB_CDC_H
#define KICKOS_SYS_USB_CDC_H

#include <stdint.h>
#include <stddef.h> // NULL, spelled instead of nullptr so the bodies below compile as C
#include <iso646.h> // and / or / not are macros in C, not keywords

#ifdef __cplusplus
extern "C"
{
#endif

// --------------------------------------------------------------------------------
// Chapter 9: the SETUP packet and its request space.

// The 8 SETUP bytes, little-endian on the wire and little-endian in memory on every
// target in this fleet, so the struct is the wire format.
struct kos_usb_setup
{
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
};

enum kos_usb_reqtype
{
    KOS_USB_REQ_DIR_IN = 0x80u,   // bit 7: device -> host
    KOS_USB_REQ_TYPE_MASK = 0x60u, // bits 6:5
    KOS_USB_REQ_TYPE_STANDARD = 0x00u,
    KOS_USB_REQ_TYPE_CLASS = 0x20u,
    KOS_USB_REQ_TYPE_VENDOR = 0x40u,
    KOS_USB_REQ_RECIP_MASK = 0x1Fu, // bits 4:0
    KOS_USB_REQ_RECIP_DEVICE = 0x00u,
    KOS_USB_REQ_RECIP_INTERFACE = 0x01u,
    KOS_USB_REQ_RECIP_ENDPOINT = 0x02u
};

enum kos_usb_std_request
{
    KOS_USB_GET_STATUS = 0,
    KOS_USB_CLEAR_FEATURE = 1,
    KOS_USB_SET_FEATURE = 3,
    KOS_USB_SET_ADDRESS = 5,
    KOS_USB_GET_DESCRIPTOR = 6,
    KOS_USB_SET_DESCRIPTOR = 7,
    KOS_USB_GET_CONFIGURATION = 8,
    KOS_USB_SET_CONFIGURATION = 9,
    KOS_USB_GET_INTERFACE = 10,
    KOS_USB_SET_INTERFACE = 11
};

enum kos_usb_desc_type
{
    KOS_USB_DT_DEVICE = 1,
    KOS_USB_DT_CONFIGURATION = 2,
    KOS_USB_DT_STRING = 3,
    KOS_USB_DT_INTERFACE = 4,
    KOS_USB_DT_ENDPOINT = 5,
    KOS_USB_DT_DEVICE_QUALIFIER = 6,
    KOS_USB_DT_CS_INTERFACE = 0x24,
    KOS_USB_DT_CS_ENDPOINT = 0x25
};

enum kos_usb_ep_type
{
    KOS_USB_EP_CONTROL = 0,
    KOS_USB_EP_ISOCHRONOUS = 1,
    KOS_USB_EP_BULK = 2,
    KOS_USB_EP_INTERRUPT = 3
};

// Feature selectors, chapter 9. ENDPOINT_HALT is the only one a console must honour:
// a host clears it to recover a stalled bulk endpoint.
enum kos_usb_feature
{
    KOS_USB_FEATURE_ENDPOINT_HALT = 0,
    KOS_USB_FEATURE_DEVICE_REMOTE_WAKEUP = 1
};

// --------------------------------------------------------------------------------
// CDC-ACM class requests (PSTN subclass).

enum kos_cdc_request
{
    KOS_CDC_SET_LINE_CODING = 0x20,
    KOS_CDC_GET_LINE_CODING = 0x21,
    KOS_CDC_SET_CONTROL_LINE_STATE = 0x22,
    KOS_CDC_SEND_BREAK = 0x23
};

// GET/SET_LINE_CODING state. A console only echoes it back: there is no baud generator
// behind it.
//
// NOT the wire format, and it must never be memcpy'd onto the bus: the payload is SEVEN
// bytes and sizeof(this) is eight, because the three trailing uint8_t's are padded up to
// the uint32_t's alignment. Marshal through the two functions below.
struct kos_cdc_line_coding
{
    uint32_t dwDTERate;
    uint8_t bCharFormat; // 0 = 1 stop bit, 1 = 1.5, 2 = 2
    uint8_t bParityType; // 0 = none, 1 = odd, 2 = even, 3 = mark, 4 = space
    uint8_t bDataBits;   // 5, 6, 7, 8 or 16
};

enum
{
    KOS_CDC_LINE_CODING_LEN = 7
};

static inline void kos_cdc_line_coding_pack(uint8_t* out,
                                            struct kos_cdc_line_coding const* lc)
{
    out[0] = (uint8_t)(lc->dwDTERate & 0xFFu);
    out[1] = (uint8_t)((lc->dwDTERate >> 8) & 0xFFu);
    out[2] = (uint8_t)((lc->dwDTERate >> 16) & 0xFFu);
    out[3] = (uint8_t)((lc->dwDTERate >> 24) & 0xFFu);
    out[4] = lc->bCharFormat;
    out[5] = lc->bParityType;
    out[6] = lc->bDataBits;
}

static inline void kos_cdc_line_coding_unpack(struct kos_cdc_line_coding* lc,
                                              uint8_t const* in)
{
    lc->dwDTERate = (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16)
                    | ((uint32_t)in[3] << 24);
    lc->bCharFormat = in[4];
    lc->bParityType = in[5];
    lc->bDataBits = in[6];
}

// SET_CONTROL_LINE_STATE wValue bits.
enum kos_cdc_control_line
{
    KOS_CDC_CTRL_DTR = 1u << 0,
    KOS_CDC_CTRL_RTS = 1u << 1
};

// --------------------------------------------------------------------------------
// The console's endpoint plan. These are ABI between the class layer, the controller
// backend and (later) the panic writer, which cannot ask a dead driver where it put its
// buffer (design section 5.4): neither the class layer nor the backend may choose them.

enum kos_usb_cdc_endpoints
{
    KOS_USB_CDC_EP_NOTIFY = 1, // interrupt IN: declared and buffered, never queued
    KOS_USB_CDC_EP_DATA = 2,   // bulk IN (0x82) and bulk OUT (0x02)

    KOS_USB_CDC_EP0_MAX_PACKET = 64,
    KOS_USB_CDC_NOTIFY_MAX_PACKET = 16,
    KOS_USB_CDC_BULK_MAX_PACKET = 32 // RP2040-E15: keep every bulk buffer under 50 B
};

enum kos_usb_cdc_desc_size
{
    KOS_USB_CDC_DEVICE_DESC_LEN = 18,
    KOS_USB_CDC_CONFIG_DESC_LEN = 67 // 9 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7
};

// --------------------------------------------------------------------------------
// The descriptor tables. Byte arrays rather than packed structs: the wire format has
// 16-bit fields at odd offsets.

// 1209:0001 is pid.codes' allocated test pair for unreleased open-source hardware. A
// KickOS console must not answer to 2e8a, which is what picotool probes for.
enum kos_usb_cdc_id
{
    KOS_USB_CDC_VID = 0x1209,
    KOS_USB_CDC_PID = 0x0001
};

static uint8_t const kos_usb_cdc_device_desc[KOS_USB_CDC_DEVICE_DESC_LEN] = {
    18,                     // bLength
    KOS_USB_DT_DEVICE,      // bDescriptorType
    0x00, 0x02,             // bcdUSB 2.00
    0x02,                   // bDeviceClass: Communications (two-interface CDC, no IAD)
    0x00,                   // bDeviceSubClass
    0x00,                   // bDeviceProtocol
    KOS_USB_CDC_EP0_MAX_PACKET,
    (KOS_USB_CDC_VID & 0xFFu), (KOS_USB_CDC_VID >> 8),
    (KOS_USB_CDC_PID & 0xFFu), (KOS_USB_CDC_PID >> 8),
    0x00, 0x01,             // bcdDevice 1.00
    0x01,                   // iManufacturer
    0x02,                   // iProduct
    0x03,                   // iSerialNumber
    0x01                    // bNumConfigurations
};

static uint8_t const kos_usb_cdc_config_desc[KOS_USB_CDC_CONFIG_DESC_LEN] = {
    // Configuration
    9, KOS_USB_DT_CONFIGURATION,
    (KOS_USB_CDC_CONFIG_DESC_LEN & 0xFFu), (KOS_USB_CDC_CONFIG_DESC_LEN >> 8),
    0x02,                   // bNumInterfaces
    0x01,                   // bConfigurationValue
    0x00,                   // iConfiguration
    0x80,                   // bmAttributes: bus powered, bit 7 reserved-one
    0x32,                   // bMaxPower: 100 mA

    // Interface 0: communications, ACM subclass, no call-management protocol
    9, KOS_USB_DT_INTERFACE, 0x00, 0x00, 0x01, 0x02, 0x02, 0x00, 0x00,

    // CDC header functional, bcdCDC 1.10
    5, KOS_USB_DT_CS_INTERFACE, 0x00, 0x10, 0x01,
    // CDC call management functional: no call management, data interface 1
    5, KOS_USB_DT_CS_INTERFACE, 0x01, 0x00, 0x01,
    // CDC ACM functional: line-coding and serial-state requests supported
    4, KOS_USB_DT_CS_INTERFACE, 0x02, 0x02,
    // CDC union functional: control interface 0, subordinate interface 1
    5, KOS_USB_DT_CS_INTERFACE, 0x06, 0x00, 0x01,

    // Notification endpoint 0x81, interrupt IN, 16 ms polling
    7, KOS_USB_DT_ENDPOINT, 0x80 | KOS_USB_CDC_EP_NOTIFY, KOS_USB_EP_INTERRUPT,
    (KOS_USB_CDC_NOTIFY_MAX_PACKET & 0xFFu), (KOS_USB_CDC_NOTIFY_MAX_PACKET >> 8), 16,

    // Interface 1: CDC data
    9, KOS_USB_DT_INTERFACE, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00,

    // Bulk OUT 0x02
    7, KOS_USB_DT_ENDPOINT, KOS_USB_CDC_EP_DATA, KOS_USB_EP_BULK,
    (KOS_USB_CDC_BULK_MAX_PACKET & 0xFFu), (KOS_USB_CDC_BULK_MAX_PACKET >> 8), 0,

    // Bulk IN 0x82
    7, KOS_USB_DT_ENDPOINT, 0x80 | KOS_USB_CDC_EP_DATA, KOS_USB_EP_BULK,
    (KOS_USB_CDC_BULK_MAX_PACKET & 0xFFu), (KOS_USB_CDC_BULK_MAX_PACKET >> 8), 0
};

// String descriptors, UTF-16LE with a 2-byte header. Index 0 carries the LANGID array.
static uint8_t const kos_usb_cdc_string0[4] = { 4, KOS_USB_DT_STRING, 0x09, 0x04 };
static uint8_t const kos_usb_cdc_string1[14] = {
    14, KOS_USB_DT_STRING, 'K', 0, 'i', 0, 'c', 0, 'k', 0, 'O', 0, 'S', 0
};
static uint8_t const kos_usb_cdc_string2[30] = {
    30, KOS_USB_DT_STRING,
    'K', 0, 'i', 0, 'c', 0, 'k', 0, 'O', 0, 'S', 0, ' ', 0,
    'c', 0, 'o', 0, 'n', 0, 's', 0, 'o', 0, 'l', 0, 'e', 0
};
// A fixed serial makes two boards on one host indistinguishable in udev. The chip's
// unique ID is reachable only from privileged bring-up.
static uint8_t const kos_usb_cdc_string3[10] = {
    10, KOS_USB_DT_STRING, '0', 0, '0', 0, '0', 0, '1', 0
};

// Resolve a GET_DESCRIPTOR wValue to a table. Returns the length, or 0 when the device has
// no such descriptor, which the caller must answer with a protocol stall and never with a
// short reply: a host reads a zero-length answer as a malformed device.
static inline uint32_t kos_usb_cdc_descriptor(uint16_t wValue, uint8_t const** out)
{
    uint8_t const type = (uint8_t)(wValue >> 8);
    uint8_t const index = (uint8_t)(wValue & 0xFFu);
    if (type == KOS_USB_DT_DEVICE)
    {
        *out = kos_usb_cdc_device_desc;
        return sizeof(kos_usb_cdc_device_desc);
    }
    if (type == KOS_USB_DT_CONFIGURATION and index == 0u)
    {
        // bNumConfigurations is 1, so any other index has no descriptor and must stall
        // rather than be answered with configuration 0 under a different number.
        *out = kos_usb_cdc_config_desc;
        return sizeof(kos_usb_cdc_config_desc);
    }
    if (type == KOS_USB_DT_STRING)
    {
        if (index == 0u)
        {
            *out = kos_usb_cdc_string0;
            return sizeof(kos_usb_cdc_string0);
        }
        if (index == 1u)
        {
            *out = kos_usb_cdc_string1;
            return sizeof(kos_usb_cdc_string1);
        }
        if (index == 2u)
        {
            *out = kos_usb_cdc_string2;
            return sizeof(kos_usb_cdc_string2);
        }
        if (index == 3u)
        {
            *out = kos_usb_cdc_string3;
            return sizeof(kos_usb_cdc_string3);
        }
    }
    // DEVICE_QUALIFIER included: a full-speed-only device must STALL it, and a host that
    // asks is probing for high-speed support rather than malfunctioning.
    *out = NULL;
    return 0u;
}

#ifdef __cplusplus
static_assert(sizeof(struct kos_usb_setup) == 8, "the SETUP packet is 8 wire bytes");
static_assert(sizeof(kos_usb_cdc_config_desc) == KOS_USB_CDC_CONFIG_DESC_LEN,
              "wTotalLength in the configuration descriptor must equal the table size");
#else
_Static_assert(sizeof(struct kos_usb_setup) == 8, "the SETUP packet is 8 wire bytes");
_Static_assert(sizeof(kos_usb_cdc_config_desc) == KOS_USB_CDC_CONFIG_DESC_LEN,
               "wTotalLength in the configuration descriptor must equal the table size");
#endif

#ifdef __cplusplus
}
#endif

#endif
