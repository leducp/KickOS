// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SPI bus service: a thin transport over the SPI class <kickos/driver/spi.h>, whose
// implementation is chosen by the LINK. Every request op is one class call and there is no
// third op: KOS_BUS_OP_CONFIG is kos_spi_device_open, KOS_BUS_OP_XFER is kos_spi_transfer. The
// bus lifecycle is NOT on the wire; the driver thread that holds the window grant performs it
// before serve_loop. Adding a call to the class means adding an op here in the same change.
//
// THE SLOT TABLE IS INDEXED BY THE CALLER'S OWN device BYTE, which is sound only while a
// single client can reach the endpoint: a client holds a SIGNAL-only cap, and spawn-time
// delegation refuses a source cap without CAP_TRANSFER, so a client cannot pass its copy on.
// Mutually-untrusting clients sharing a bus would need badged endpoints and are NOT supported.

#ifndef KICKOS_SYS_SPI_SERVICE_H
#define KICKOS_SYS_SPI_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/spi.h>
#include <kickos/sys/bus.h>   // kos_bus_req/seg/rsp/cfg wire ABI
#include <kickos/sys/bytes.h> // mem_copy
#include <kickos/sys/driver_service.h>

#include <stdint.h>
#include <stddef.h>

namespace kickos::spi
{

// Child cap indices the driver thread reads. A bus whose engine polls its FIFOs is delegated
// the endpoint alone.
enum
{
    KOS_SPI_CAP_EP = KOS_SPAWN_DELEGATED_CAP0,      // the request endpoint (WAIT)
    KOS_SPI_CAP_LINE = KOS_SPAWN_DELEGATED_CAP0 + 1 // the tier-1 line, when the bus has one
};

// THE BODIES BELOW STAY IN THIS HEADER. A SPI service target renames the class symbols it
// calls with a private -D (kos_spi_device_open=k64dspi_device_open), so the rename applies
// only where the caller is compiled: one body in libkickos_user.a would call the PUBLIC name,
// which such a service never defines.

// Build a service-level error reply (no rx) and complete the call. ALWAYS consumes the reply
// cap.
inline void reply_error(kos_cap_t reply_cap, int16_t status)
{
    struct kos_bus_rsp rsp;
    rsp.status = status;
    rsp.len = 0;
    kos_reply(reply_cap, &rsp, sizeof(rsp));
}

// One device handle per kos_bus_req.device. A slot with no CONFIG yet holds no handle, and a
// transfer naming it is refused rather than run against a zeroed profile.
struct SlotTable
{
    struct kos_spi_device dev[KOS_BUS_DEV_MAX];
    bool open[KOS_BUS_DEV_MAX];

    SlotTable()
    {
        for (unsigned d = 0; d < KOS_BUS_DEV_MAX; d++)
        {
            open[d] = false;
        }
    }
};

// Parse + run one request; ALWAYS completes the call (consumes reply_cap on every path,
// success or error; a leaked reply cap parks the client forever).
inline void serve_one(struct kos_spi_bus* bus, SlotTable& slots, unsigned char const* msg,
                      size_t n, kos_cap_t reply_cap)
{
    if (n < sizeof(struct kos_bus_req))
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }
    struct kos_bus_req req;
    mem_copy(&req, msg, sizeof(req)); // msg may be unaligned for the 32-bit fields

    if (req.proto != KOS_BUS_SPI)
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }
    if (req.region_cap != -1)
    {
        reply_error(reply_cap, -KOS_ENOSYS); // region path is DEFERRED (inline only)
        return;
    }
    if (req.offset != 0u)
    {
        reply_error(reply_cap, -KOS_EINVAL); // offset belongs to the region path
        return;
    }
    if (req.device >= KOS_BUS_DEV_MAX)
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }

    if (req.op == KOS_BUS_OP_CONFIG)
    {
        size_t const need = sizeof(struct kos_bus_req) + sizeof(struct kos_bus_cfg);
        if (req.nseg != 0 or n < need)
        {
            reply_error(reply_cap, -KOS_EINVAL);
            return;
        }
        struct kos_bus_cfg cfg;
        mem_copy(&cfg, msg + sizeof(struct kos_bus_req), sizeof(cfg));
        // The wire's reserved bytes have no class field to land in, so a client setting one
        // is REFUSED rather than silently dropped by the mapping below.
        if (cfg.rsv[0] != 0u or cfg.rsv[1] != 0u)
        {
            reply_error(reply_cap, -KOS_EINVAL);
            return;
        }

        // The device slot is the caller's own request byte; cfg.addr is I2C's and has no
        // class field to land in.
        struct kos_spi_device_config dcfg;
        dcfg.hz = cfg.hz;
        dcfg.slot = req.device;
        dcfg.mode = cfg.mode;
        dcfg.word_bits = cfg.word_bits;
        dcfg.cs_policy = cfg.cs_policy;
        dcfg.cs_index = cfg.cs_index;
        dcfg.rsv[0] = 0u;
        dcfg.rsv[1] = 0u;
        dcfg.rsv[2] = 0u;

        int32_t const rc = kos_spi_device_open(&slots.dev[req.device], bus, &dcfg);
        if (rc < 0)
        {
            reply_error(reply_cap, static_cast<int16_t>(rc));
            return;
        }
        slots.open[req.device] = true;

        uint32_t const achieved = static_cast<uint32_t>(rc);
        unsigned char rbuf[sizeof(struct kos_bus_rsp) + sizeof(uint32_t)];
        struct kos_bus_rsp rsp;
        rsp.status = 0;
        rsp.len = static_cast<uint16_t>(sizeof(uint32_t));
        mem_copy(rbuf, &rsp, sizeof(rsp));
        mem_copy(rbuf + sizeof(struct kos_bus_rsp), &achieved, sizeof(achieved));
        kos_reply(reply_cap, rbuf, sizeof(rbuf));
        return;
    }

    if (req.op != KOS_BUS_OP_XFER)
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }

    if (req.nseg < 1u or req.nseg > KOS_BUS_SEG_MAX)
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }
    if (not slots.open[req.device])
    {
        reply_error(reply_cap, -KOS_EINVAL); // no device handle for this slot
        return;
    }
    size_t const framing =
        sizeof(struct kos_bus_req) + static_cast<size_t>(req.nseg) * sizeof(struct kos_bus_seg);
    if (n < framing)
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }

    struct kos_bus_seg seg[KOS_BUS_SEG_MAX];
    uint32_t total = 0u;
    for (unsigned s = 0; s < req.nseg; s++)
    {
        mem_copy(&seg[s], msg + sizeof(struct kos_bus_req) + s * sizeof(struct kos_bus_seg),
                 sizeof(seg[s]));
        total += seg[s].len;
    }
    if (framing + total > n)
    {
        reply_error(reply_cap, -KOS_EINVAL); // segment lengths exceed the message
        return;
    }
    // The class's own ceiling, applied BEFORE the gather: the reply buffer below is sized for
    // it, so an oversized total must be refused rather than copied and then refused.
    if (total > static_cast<uint32_t>(KOS_SPI_XFER_MAX))
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }

    // Gathered into the reply buffer past its header and run IN PLACE: rx overwrites tx.
    unsigned char rbuf[KOS_EP_MSG_MAX];
    unsigned char* work = rbuf + sizeof(struct kos_bus_rsp);
    mem_copy(work, msg + framing, total);

    int32_t const moved = kos_spi_transfer(&slots.dev[req.device], seg,
                                           static_cast<uint8_t>(req.nseg), work, total);
    if (moved < 0)
    {
        reply_error(reply_cap, static_cast<int16_t>(moved));
        return;
    }
    struct kos_bus_rsp rsp;
    rsp.status = 0;
    rsp.len = static_cast<uint16_t>(moved);
    mem_copy(rbuf, &rsp, sizeof(rsp));
    kos_reply(reply_cap, rbuf, sizeof(struct kos_bus_rsp) + static_cast<size_t>(moved));
}

// The driver's recv/dispatch loop. Returns only when the endpoint dies, so the driver thread
// can exit and let root respawn.
inline void serve_loop(struct kos_spi_bus* bus)
{
    kos_cap_t const ep = KOS_SPI_CAP_EP; // delegated {E | WAIT} recv cap
    SlotTable slots;
    unsigned char msg[KOS_EP_MSG_MAX];
    while (true)
    {
        struct kos_recv_info info = {0u, KOS_CAP_NONE};
        long const n = kos_recv(ep, msg, sizeof(msg), &info);
        if (n < 0)
        {
            break; // EPIPE / dead endpoint: exit, let root respawn
        }
        if (info.reply_cap == KOS_CAP_NONE)
        {
            continue; // plain send: not part of the bus call/reply protocol
        }
        serve_one(bus, slots, msg, static_cast<size_t>(n), info.reply_cap);
    }
}

// ---------------------------------------------------------------------------------
// The class-side half of the descriptor check. The generic validator cannot know that
// serve_loop receives on KOS_SPI_CAP_EP == 1 and that a bus config naming a line names
// KOS_SPI_CAP_LINE == 2, so a descriptor granting the right caps in the wrong ORDER passes
// valid() and stalls silently.
constexpr bool desc_ok(driver::Descriptor const& d)
{
    // The bus handle and its slot table live on the driver thread's own stack, so a second
    // thread would serve against state it cannot reach.
    if (d.thread_count != 1u)
    {
        return false;
    }
    // kos_spi_bus_config carries ONE irq cap and KOS_SPI_CAP_LINE is the only line index this
    // substrate names, so a second line would be claimed and never waited on.
    if (d.line_count > 1u)
    {
        return false;
    }
    // No Shared, no ring, no doorbell: this substrate lays out no block, so there is no latch
    // for a barrier to poll.
    if (d.block_size != 0u or d.ready_offset != driver::KOS_DRV_READY_NONE)
    {
        return false;
    }
    // One cap per resource this bus has, and nothing spare.
    if (d.threads[0].cap_count != 1u + d.line_count)
    {
        return false;
    }
    if (d.threads[0].caps[0].resource != driver::KOS_DRV_RES_EP
        or (d.threads[0].caps[0].rights & KOS_CAP_WAIT) == 0u)
    {
        return false;
    }
    // WAIT, never SIGNAL: the driver services the line, it does not ring its own doorbell.
    if (d.line_count != 0u
        and (d.threads[0].caps[1].resource != driver::KOS_DRV_RES_LINE0
             or (d.threads[0].caps[1].rights & KOS_CAP_WAIT) == 0u))
    {
        return false;
    }
    return true;
}

}

#endif
