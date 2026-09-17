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
#include <kickos/sys/serve.h>

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

// Build a service-level error reply (no rx) OVER the request, and answer its length. Nothing
// here replies: the loop carries the answer out on the call that takes the next request.
inline size_t reply_error(unsigned char* buf, int16_t status)
{
    struct kos_bus_rsp rsp;
    rsp.status = status;
    rsp.len = 0;
    mem_copy(buf, &rsp, sizeof(rsp));
    return sizeof(rsp);
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

// Parse + run one request IN PLACE: the reply is built over the request in `buf` and its
// length answered, on every path. A path answering 0 would leak the reply capability and park
// the client forever, so every arm below ends in one of the two builders.
//
// The gather below moves the payload DOWN over the request, which an ascending copy may do:
// the framing it skips is wider than the header it lands behind, asserted here rather than
// left to a reader of both structs.
static_assert(sizeof(struct kos_bus_req) + sizeof(struct kos_bus_seg) > sizeof(struct kos_bus_rsp),
              "the reply header is wider than the framing the gather skips, so the in-place "
              "move would be an ASCENDING overlapping copy and would read bytes it has "
              "already overwritten");
inline size_t serve_one(struct kos_spi_bus* bus, SlotTable& slots, unsigned char* buf, size_t n)
{
    if (n < sizeof(struct kos_bus_req))
    {
        return reply_error(buf, -KOS_EINVAL);
    }
    struct kos_bus_req req;
    mem_copy(&req, buf, sizeof(req)); // the request may be unaligned for the 32-bit fields

    if (req.proto != KOS_BUS_SPI)
    {
        return reply_error(buf, -KOS_EINVAL);
    }
    if (req.region_cap != -1)
    {
        return reply_error(buf, -KOS_ENOSYS); // region path is DEFERRED (inline only)
    }
    if (req.offset != 0u)
    {
        return reply_error(buf, -KOS_EINVAL); // offset belongs to the region path
    }
    if (req.device >= KOS_BUS_DEV_MAX)
    {
        return reply_error(buf, -KOS_EINVAL);
    }

    if (req.op == KOS_BUS_OP_CONFIG)
    {
        size_t const need = sizeof(struct kos_bus_req) + sizeof(struct kos_bus_cfg);
        if (req.nseg != 0 or n < need)
        {
            return reply_error(buf, -KOS_EINVAL);
        }
        struct kos_bus_cfg cfg;
        mem_copy(&cfg, buf + sizeof(struct kos_bus_req), sizeof(cfg));
        // The wire's reserved bytes have no class field to land in, so a client setting one
        // is REFUSED rather than silently dropped by the mapping below.
        if (cfg.rsv[0] != 0u or cfg.rsv[1] != 0u)
        {
            return reply_error(buf, -KOS_EINVAL);
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
            return reply_error(buf, static_cast<int16_t>(rc));
        }
        slots.open[req.device] = true;

        uint32_t const achieved = static_cast<uint32_t>(rc);
        struct kos_bus_rsp rsp;
        rsp.status = 0;
        rsp.len = static_cast<uint16_t>(sizeof(uint32_t));
        mem_copy(buf, &rsp, sizeof(rsp));
        mem_copy(buf + sizeof(struct kos_bus_rsp), &achieved, sizeof(achieved));
        return sizeof(struct kos_bus_rsp) + sizeof(uint32_t);
    }

    if (req.op != KOS_BUS_OP_XFER)
    {
        return reply_error(buf, -KOS_EINVAL);
    }

    if (req.nseg < 1u or req.nseg > KOS_BUS_SEG_MAX)
    {
        return reply_error(buf, -KOS_EINVAL);
    }
    if (not slots.open[req.device])
    {
        return reply_error(buf, -KOS_EINVAL); // no device handle for this slot
    }
    size_t const framing =
        sizeof(struct kos_bus_req) + static_cast<size_t>(req.nseg) * sizeof(struct kos_bus_seg);
    if (n < framing)
    {
        return reply_error(buf, -KOS_EINVAL);
    }

    struct kos_bus_seg seg[KOS_BUS_SEG_MAX];
    uint32_t total = 0u;
    for (unsigned s = 0; s < req.nseg; s++)
    {
        mem_copy(&seg[s], buf + sizeof(struct kos_bus_req) + s * sizeof(struct kos_bus_seg),
                 sizeof(seg[s]));
        total += seg[s].len;
    }
    if (framing + total > n)
    {
        return reply_error(buf, -KOS_EINVAL); // segment lengths exceed the message
    }
    // The class's own ceiling, applied BEFORE the gather: the reply buffer below is sized for
    // it, so an oversized total must be refused rather than copied and then refused.
    if (total > static_cast<uint32_t>(KOS_SPI_XFER_MAX))
    {
        return reply_error(buf, -KOS_EINVAL);
    }

    // Gathered over the request past the reply header and run IN PLACE: rx overwrites tx.
    unsigned char* work = buf + sizeof(struct kos_bus_rsp);
    mem_copy(work, buf + framing, total);

    int32_t const moved = kos_spi_transfer(&slots.dev[req.device], seg,
                                           static_cast<uint8_t>(req.nseg), work, total);
    if (moved < 0)
    {
        return reply_error(buf, static_cast<int16_t>(moved));
    }
    struct kos_bus_rsp rsp;
    rsp.status = 0;
    rsp.len = static_cast<uint16_t>(moved);
    mem_copy(buf, &rsp, sizeof(rsp));
    return sizeof(struct kos_bus_rsp) + static_cast<size_t>(moved);
}

// The driver's recv/dispatch loop. Returns only when the endpoint dies, so the driver thread
// can exit and let root respawn.
inline void serve_loop(struct kos_spi_bus* bus)
{
    SlotTable slots;
    unsigned char msg[KOS_EP_MSG_MAX];
    struct kos_reply_recv_opts opts;
    // KOS_SPI_CAP_EP is the delegated {E | WAIT} recv cap.
    kos_reply_recv_opts_init(&opts, KOS_SPI_CAP_EP, 0u, KOS_TIMEOUT_NONE);
    // Carried one pass forward: the answer to request k rides the call that takes k+1.
    kos_cap_t reply_cap = KOS_CAP_NONE;
    size_t reply_len = 0;
    while (true)
    {
        opts.info.reply_cap = KOS_CAP_NONE;
        long const n = kos_reply_recv(reply_cap, msg,
                                      kos_call_lens_pack(reply_len, sizeof(msg)), &opts);
        reply_cap = KOS_CAP_NONE;
        reply_len = 0;
        if (n < 0)
        {
            // A client's own buffer going away ends that transaction and not the bus.
            if (serve_transaction_failed(static_cast<int32_t>(n)))
            {
                continue;
            }
            break; // the recv cap no longer serves: exit, let root respawn
        }
        if (opts.info.reply_cap == KOS_CAP_NONE)
        {
            continue; // plain send: not part of the bus call/reply protocol
        }
        reply_len = serve_one(bus, slots, msg, static_cast<size_t>(n));
        reply_cap = opts.info.reply_cap;
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
