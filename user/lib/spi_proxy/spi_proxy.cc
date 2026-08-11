// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PROXY backend of the SPI class <kickos/driver/spi.h>: the same four functions, marshalled
// onto a bus service endpoint (<kickos/sys/bus.h>) instead of onto registers. ONE
// implementation for every chip, holding no register knowledge at all.
//
// kos_spi_bus_open and kos_spi_bus_close send NOTHING: the peripheral lifecycle belongs to
// whoever holds its window grant, and a client holding only an endpoint cap cannot ask for it.
// A new call on the class is a new op here and a new op in the service, together or not at all.
//
// kos_call is IN-PLACE, the reply overwriting the request buffer, so recv_cap is the full
// buffer size and the reply is read back from the same buffer.

#include <kickos/driver/spi.h>

#include <kickos/sys.h>       // kos_call, KOS_EP_MSG_MAX, KOS_E*
#include <kickos/sys/bus.h>   // kos_bus_req/seg/rsp/cfg wire ABI
#include <kickos/sys/bytes.h> // mem_copy / mem_zero

#include <stdint.h>
#include <stddef.h>

namespace
{
    // The region path is specified in kos_bus_req.region_cap and DEFERRED, so region_cap is
    // always the inline selector here.
    void seat_req(unsigned char* buf, uint8_t op, uint8_t device, uint8_t nseg)
    {
        struct kos_bus_req req;
        req.proto = KOS_BUS_SPI;
        req.op = op;
        req.device = device;
        req.nseg = nseg;
        req.region_cap = -1;
        req.offset = 0u;
        mem_copy(buf, &req, sizeof(req));
    }

    // TWO statuses to split: the kernel-level one is kos_call's own return, the service-level
    // one is kos_bus_rsp.status. Returns the payload byte count, or a negative kos_errno.
    int32_t reply_payload(unsigned char const* buf, int32_t rc, size_t* off)
    {
        if (rc < 0)
        {
            return rc;
        }
        if (static_cast<size_t>(rc) < sizeof(struct kos_bus_rsp))
        {
            return -KOS_EINVAL; // reply too short to carry a status header
        }
        struct kos_bus_rsp rsp;
        mem_copy(&rsp, buf, sizeof(rsp));
        if (rsp.status < 0)
        {
            return rsp.status;
        }
        size_t got = rsp.len;
        size_t const avail = static_cast<size_t>(rc) - sizeof(struct kos_bus_rsp);
        if (got > avail)
        {
            got = avail; // reply clamped in transit
        }
        *off = sizeof(struct kos_bus_rsp);
        return static_cast<int32_t>(got);
    }
}

extern "C"
{

int32_t kos_spi_bus_open(struct kos_spi_bus* b, struct kos_spi_bus_config const* cfg)
{
    if (cfg->ep == KOS_CAP_NONE)
    {
        return -KOS_EINVAL;
    }
    // base and irq belong to the local engines: recorded so the POD still carries what it was
    // opened with, and NEVER dereferenced here.
    b->base = cfg->base;
    b->ep = cfg->ep;
    b->irq = cfg->irq;
    return 0;
}

int32_t kos_spi_device_open(struct kos_spi_device* d, struct kos_spi_bus* b,
                            struct kos_spi_device_config const* cfg)
{
    if (b->ep == KOS_CAP_NONE)
    {
        return -KOS_EINVAL; // no open bus behind this handle
    }

    unsigned char buf[sizeof(struct kos_bus_req) + sizeof(struct kos_bus_cfg)];
    seat_req(buf, KOS_BUS_OP_CONFIG, cfg->slot, /*nseg=*/0u);

    // addr is I2C's and stays 0: the class carries SPI vocabulary only, so this mapping is a
    // copy and not a cast.
    struct kos_bus_cfg wire;
    mem_zero(&wire, sizeof(wire));
    wire.hz = cfg->hz;
    wire.addr = 0u;
    wire.mode = cfg->mode;
    wire.word_bits = cfg->word_bits;
    wire.cs_policy = cfg->cs_policy;
    wire.cs_index = cfg->cs_index;
    mem_copy(buf + sizeof(struct kos_bus_req), &wire, sizeof(wire));

    int32_t const rc = kos_call(b->ep, buf, sizeof(buf), sizeof(buf));
    size_t off = 0u;
    int32_t const got = reply_payload(buf, rc, &off);
    if (got < 0)
    {
        return got;
    }
    if (static_cast<size_t>(got) < sizeof(uint32_t))
    {
        return -KOS_EINVAL; // a CONFIG reply without an achieved rate is not an answer
    }
    uint32_t hz = 0u;
    mem_copy(&hz, buf + off, sizeof(hz));
    if (hz == 0u)
    {
        return -KOS_ENOTSUP; // the far side reported no rate: never echo the request
    }

    d->bus = b;
    d->hz = hz;
    d->prog[0] = 0u; // the folded controller words live in the SERVICE's own handle
    d->prog[1] = 0u;
    d->slot = cfg->slot;
    d->mode = cfg->mode;
    d->word_bits = cfg->word_bits;
    d->cs_policy = cfg->cs_policy;
    d->cs_index = cfg->cs_index;
    d->rsv[0] = 0u;
    d->rsv[1] = 0u;
    d->rsv[2] = 0u;
    return static_cast<int32_t>(hz);
}

int32_t kos_spi_transfer(struct kos_spi_device* d, struct kos_bus_seg const* seg, uint8_t nseg,
                         unsigned char* buf, uint32_t len)
{
    // Checked HERE and not left to the far side: KOS_SPI_XFER_MAX is what the request frame
    // can hold, so an oversized transfer has no message to be refused in.
    int32_t const bad = kos_spi_seg_check(seg, nseg, len);
    if (bad != 0)
    {
        return bad;
    }
    struct kos_spi_bus* const b = d->bus;
    if (b == nullptr or b->ep == KOS_CAP_NONE)
    {
        return -KOS_EINVAL;
    }

    unsigned char msg[KOS_EP_MSG_MAX];
    size_t const framing =
        sizeof(struct kos_bus_req) + static_cast<size_t>(nseg) * sizeof(struct kos_bus_seg);
    seat_req(msg, KOS_BUS_OP_XFER, d->slot, nseg);
    mem_copy(msg + sizeof(struct kos_bus_req), seg,
             static_cast<size_t>(nseg) * sizeof(struct kos_bus_seg));
    mem_copy(msg + framing, buf, len);

    int32_t const rc = kos_call(b->ep, msg, framing + len, sizeof(msg));
    size_t off = 0u;
    int32_t const got = reply_payload(msg, rc, &off);
    if (got < 0)
    {
        return got;
    }
    if (static_cast<uint32_t>(got) != len)
    {
        // Full duplex: a transfer that clocked returns exactly as many bytes as it sent, so
        // anything else is a truncated transaction the caller cannot resume.
        return -KOS_EINVAL;
    }
    mem_copy(buf, msg + off, len);
    return static_cast<int32_t>(len);
}

int32_t kos_spi_bus_close(struct kos_spi_bus* b)
{
    // The endpoint cap belongs to the consumer that acquired it and is NOT closed here: the
    // service and its bus outlive every client. Only this handle goes dead.
    b->base = 0u;
    b->ep = KOS_CAP_NONE;
    b->irq = KOS_CAP_NONE;
    return 0;
}

}
