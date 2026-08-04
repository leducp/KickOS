// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Shared SPI bus-service choreography, templated over a concrete transaction
// ENGINE (the per-chip Bus class): the request parse+run (serve_one), the
// error reply, and the recv/dispatch loop. Every SPI driver defines its own
// Bus (the register access + CS framing, i.e. the silicon), then runs
// spi_serve_loop() on it. No MMIO and no chip knowledge live here; only the
// wire-ABI framing over <kickos/sys/bus.h> plus the per-device slot store. The
// Bus supplies a nested profile type and the two members the shared code calls
// (the implicit interface):
//     struct Profile;  // POD: the chip words one device's config folds down to
//     uint32_t fold(struct kos_bus_cfg const& cfg, Profile& out);   // -> achieved hz
//     void     transfer(Profile const& p, unsigned char* buf, size_t len);
// A controller has ONE live profile register set, so a transfer cannot be issued
// without naming the profile it must (re-)apply first, so the profile is an
// argument of transfer() rather than a separate select() a future path could forget.
// Instantiated only on an anonymous-namespace Bus, so each instantiation is
// TU-local (internal linkage, no COMDAT).

#ifndef KICKOS_SYS_SPI_SERVICE_H
#define KICKOS_SYS_SPI_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/bus.h>   // kos_bus_req/seg/rsp/cfg wire ABI
#include <kickos/sys/bytes.h> // mem_copy

#include <stdint.h>
#include <stddef.h>

namespace kickos
{
namespace spi
{

// Build a service-level error reply (no rx) and complete the call. ALWAYS
// consumes the reply cap.
inline void reply_error(kos_cap_t reply_cap, int16_t status)
{
    struct kos_bus_rsp rsp;
    rsp.status = status;
    rsp.len = 0;
    kos_reply(reply_cap, &rsp, sizeof(rsp));
}

// The per-device profile store: one folded Bus::Profile per kos_bus_req.device.
// Slots are indexed by the CALLER's own device byte, which is sound only while a
// single client can reach the endpoint: a client holds a SIGNAL-only cap, and
// spawn-time delegation refuses a source cap without CAP_TRANSFER, so a client
// cannot pass its copy on: the reachable set is exactly what the bring-up handed
// out. That is the one-client-several-devices case (a flash and a sensor on one
// bus). Mutually-untrusting clients sharing a bus would need badged endpoints and
// are NOT supported here.
template <typename Bus>
struct SlotTable
{
    typename Bus::Profile profile[KOS_BUS_DEV_MAX];
    bool folded[KOS_BUS_DEV_MAX];

    SlotTable()
    {
        for (unsigned d = 0; d < KOS_BUS_DEV_MAX; d++)
        {
            folded[d] = false;
        }
    }
};

// Parse + run one request; ALWAYS completes the call (consumes reply_cap on every
// path, success or error; a leaked reply cap parks the client forever).
template <typename Bus>
void serve_one(Bus& bus, SlotTable<Bus>& slots, unsigned char const* msg, size_t n,
               kos_cap_t reply_cap)
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
        uint32_t const achieved = bus.fold(cfg, slots.profile[req.device]);
        slots.folded[req.device] = true;

        unsigned char rbuf[sizeof(struct kos_bus_rsp) + sizeof(uint32_t)];
        struct kos_bus_rsp* rsp = reinterpret_cast<struct kos_bus_rsp*>(rbuf);
        rsp->status = 0;
        rsp->len = static_cast<uint16_t>(sizeof(uint32_t));
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
    if (not slots.folded[req.device])
    {
        reply_error(reply_cap, -KOS_EINVAL); // no profile for this slot: nothing to apply
        return;
    }
    size_t const framing =
        sizeof(struct kos_bus_req) + static_cast<size_t>(req.nseg) * sizeof(struct kos_bus_seg);
    if (n < framing)
    {
        reply_error(reply_cap, -KOS_EINVAL);
        return;
    }

    // Concatenated full-duplex bytes = the sum of every segment length; the CS
    // spans all of them (one coherent transaction, no per-segment CS toggle).
    size_t total = 0;
    for (unsigned s = 0; s < req.nseg; s++)
    {
        struct kos_bus_seg seg;
        mem_copy(&seg, msg + sizeof(struct kos_bus_req) + s * sizeof(struct kos_bus_seg),
                 sizeof(seg));
        total += seg.len;
    }
    if (framing + total > n)
    {
        reply_error(reply_cap, -KOS_EINVAL); // segment lengths exceed the message
        return;
    }
    if (total + sizeof(struct kos_bus_rsp) > KOS_EP_MSG_MAX)
    {
        reply_error(reply_cap, -KOS_EINVAL); // reply would not fit the wire
        return;
    }

    // Gather tx into the reply buffer past its header, run in place (rx
    // overwrites tx), then complete the call from the same buffer.
    unsigned char rbuf[KOS_EP_MSG_MAX];
    struct kos_bus_rsp* rsp = reinterpret_cast<struct kos_bus_rsp*>(rbuf);
    unsigned char* work = rbuf + sizeof(struct kos_bus_rsp);
    mem_copy(work, msg + framing, total);
    bus.transfer(slots.profile[req.device], work, total);
    rsp->status = 0;
    rsp->len = static_cast<uint16_t>(total);
    kos_reply(reply_cap, rbuf, sizeof(struct kos_bus_rsp) + total);
}

// The driver's recv/dispatch loop: block on the delegated WAIT recv cap, drop
// plain sends (no reply cap), hand every call to serve_one. Returns when the
// endpoint dies (n < 0 -> EPIPE), so the driver thread can exit and let root
// respawn.
template <typename Bus>
void serve_loop(Bus& bus)
{
    kos_cap_t const ep = KOS_SPAWN_DELEGATED_CAP0; // delegated {E | WAIT} recv cap
    SlotTable<Bus> slots;
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

} // namespace spi
} // namespace kickos

#endif
