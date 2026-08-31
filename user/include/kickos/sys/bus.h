// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SPI/I2C bus-service wire ABI: the request/reply/config structs a client
// and a bus driver exchange 1:1 over a kos_call endpoint. Offset-based from day
// one (region_cap == -1 selects the inline path; the region path is DEFERRED, so
// a driver returns a service-level error for region_cap != -1 and this header only
// defines the field). Fixed-width, naturally aligned, no compiler pragmas; native
// endianness is fine, the payload is kernel-copied within one machine and never
// crosses a link.

#ifndef KICKOS_SYS_BUS_H
#define KICKOS_SYS_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// proto (kos_bus_req.proto): sanity tag, the driver rejects a mismatch.
enum kos_bus_proto
{
    KOS_BUS_SPI = 0,
    KOS_BUS_I2C = 1
};

// op (kos_bus_req.op): a data transfer, or a per-device config.
enum kos_bus_op
{
    KOS_BUS_OP_XFER = 0,
    KOS_BUS_OP_CONFIG = 1
};

// Max segments in one request (bounds the inline framing arithmetic below).
enum
{
    KOS_BUS_SEG_MAX = 8
};

// Device slots a bus service tracks: kos_bus_req.device names one of
// 0 .. KOS_BUS_DEV_MAX-1. A service keeps one folded profile per slot; the ceiling
// bounds that storage, it is not part of the wire format (the field stays 8-bit).
enum
{
    KOS_BUS_DEV_MAX = 4
};

// kos_bus_seg.flags. SPI clocks full duplex both ways with CS spanning the whole
// message, so no SPI flag is defined yet; these are the I2C phase controls.
enum kos_bus_seg_flags
{
    KOS_BUS_SEG_RD = 1 << 0,  // I2C: this segment reads (absent = writes)
    KOS_BUS_SEG_STOP = 1 << 1 // I2C: STOP after this segment (absent = repeated START)
};

// kos_bus_cfg.cs_policy. Driver-internal choice; the client never sees CS.
enum kos_bus_cs_policy
{
    KOS_BUS_CS_NONE = 0,
    KOS_BUS_CS_HW = 1,  // the controller's own chip-select line
    KOS_BUS_CS_GPIO = 2 // a software GPIO the driver owns across the transaction
};

// kos_bus_cfg.mode bits. SPI: clock polarity/phase + bit order. I2C: address width.
enum kos_bus_mode
{
    KOS_BUS_MODE_CPOL = 1 << 0,      // SPI clock idle level
    KOS_BUS_MODE_CPHA = 1 << 1,      // SPI sample edge
    KOS_BUS_MODE_LSB_FIRST = 1 << 2, // SPI shift LSB first
    KOS_BUS_ADDR_10BIT = 1 << 0      // I2C 10-bit addressing (overlaps CPOL: distinct protos)
};

// Every call to a bus service starts with this header. nseg kos_bus_seg follow,
// then the inline payload bytes (SPI: tx bytes for every segment concatenated,
// full duplex; I2C: wr-segment bytes only, rd segments carry no request bytes).
struct kos_bus_req
{
    uint8_t proto;      // enum kos_bus_proto
    uint8_t op;         // enum kos_bus_op
    uint8_t device;     // device slot on this bus, < KOS_BUS_DEV_MAX (CS index / I2C address slot)
    uint8_t nseg;       // 1 .. KOS_BUS_SEG_MAX
    int32_t region_cap; // -1 = inline payload follows; else granted-region cap (DEFERRED)
    uint32_t offset;    // byte offset into the region (region path); 0 inline
};

struct kos_bus_seg
{
    uint16_t len;  // bytes clocked in this segment
    uint8_t flags; // enum kos_bus_seg_flags
    uint8_t rsv;
};

// Reply payload: the SERVICE-level result. The kernel-level status is kos_call's
// return; this status is the driver's own outcome. rx bytes follow (SPI: full-duplex
// data for all segments concatenated; I2C: rd-segment data concatenated).
struct kos_bus_rsp
{
    int16_t status; // 0, or a negative KOS_E*-taxonomy service error
    uint16_t len;   // rx bytes following
};

// Per-device config (op == KOS_BUS_OP_CONFIG): stored against req.device and
// re-applied before every transfer naming that slot. A transfer naming a slot with
// no config yet is refused.
struct kos_bus_cfg
{
    uint32_t hz;       // target clock; driver rounds down, replies achieved hz in rsp
    uint16_t addr;     // I2C 7/10-bit address (width flag in mode); SPI 0
    uint8_t mode;      // enum kos_bus_mode
    uint8_t word_bits; // SPI frame size (8 default)
    uint8_t cs_policy; // enum kos_bus_cs_policy
    // HW PCS/SELO line index, or the driver's GPIO pin slot. The two shipped SPI engines
    // (k64dspi, xmcssc) REFUSE a non-zero value with -KOS_ENOTSUP: each has exactly one CS
    // line. The selftest mock takes and ignores it.
    uint8_t cs_index;
    uint8_t rsv[2];
};

// Inline budget: 12 B header + 8 * 4 B segs = 44 B worst-case framing, leaving
// ~212 B inline data under KOS_EP_MSG_MAX (256). Covers every first consumer.
#ifdef __cplusplus
static_assert(sizeof(struct kos_bus_req) == 12, "kos_bus_req must stay 12 bytes (wire ABI)");
static_assert(sizeof(struct kos_bus_seg) == 4, "kos_bus_seg must stay 4 bytes (wire ABI)");
static_assert(sizeof(struct kos_bus_rsp) == 4, "kos_bus_rsp must stay 4 bytes (wire ABI)");
static_assert(sizeof(struct kos_bus_cfg) == 12, "kos_bus_cfg must stay 12 bytes (wire ABI)");
#else
_Static_assert(sizeof(struct kos_bus_req) == 12, "kos_bus_req must stay 12 bytes (wire ABI)");
_Static_assert(sizeof(struct kos_bus_seg) == 4, "kos_bus_seg must stay 4 bytes (wire ABI)");
_Static_assert(sizeof(struct kos_bus_rsp) == 4, "kos_bus_rsp must stay 4 bytes (wire ABI)");
_Static_assert(sizeof(struct kos_bus_cfg) == 12, "kos_bus_cfg must stay 12 bytes (wire ABI)");
#endif

#ifdef __cplusplus
}
#endif

#endif
