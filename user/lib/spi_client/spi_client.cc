// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Chip-neutral SPI client wrapper (see <kickos/driver/spi_client.h>): frame a
// kos_bus_req in a stack buffer, one kos_call, split rx out of the kos_bus_rsp.
// kos_call is IN-PLACE (the reply overwrites the request buffer), so recv_cap is
// the full buffer size and the reply is read back from the same buffer after the
// request bytes have been delivered.

#include <kickos/driver/spi_client.h>

#include <kickos/sys.h>       // kos_call, KOS_EP_MSG_MAX, KOS_E* (via abi.h -> errno.h)
#include <kickos/sys/bytes.h> // mem_copy / mem_zero

namespace
{
    // Frame a 1- or 2-segment SPI XFER on slot `device`, kos_call, and copy the rx window
    // back. tx bytes for both segments are `wr` (wr_len bytes) followed by rd_len
    // dummy 0x00 (the read phase). The caller wants `rx_len` bytes starting at
    // `rx_skip` of the full-duplex reply (single segment: skip 0, len = wr_len; two
    // segments: skip the write phase, len = rd_len). Returns rx bytes (>= 0) or
    // -KOS_E*.
    int32_t frame_call(kos_cap_t ep, uint8_t device, uint8_t nseg, size_t wr_len, void const* wr,
                       size_t rd_len, void* rx, size_t rx_skip, size_t rx_len)
    {
        size_t const total = wr_len + rd_len;
        if (total == 0)
        {
            return 0;
        }

        unsigned char buf[KOS_EP_MSG_MAX];
        size_t const framing =
            sizeof(struct kos_bus_req) + static_cast<size_t>(nseg) * sizeof(struct kos_bus_seg);
        if (framing + total > sizeof(buf))
        {
            return -KOS_EINVAL;
        }

        struct kos_bus_req* req = reinterpret_cast<struct kos_bus_req*>(buf);
        req->proto = KOS_BUS_SPI;
        req->op = KOS_BUS_OP_XFER;
        req->device = device;
        req->nseg = nseg;
        req->region_cap = -1;
        req->offset = 0;

        struct kos_bus_seg* seg =
            reinterpret_cast<struct kos_bus_seg*>(buf + sizeof(struct kos_bus_req));
        if (nseg == 2)
        {
            seg[0].len = static_cast<uint16_t>(wr_len);
            seg[1].len = static_cast<uint16_t>(rd_len);
            seg[1].flags = 0;
            seg[1].rsv = 0;
        }
        else
        {
            seg[0].len = static_cast<uint16_t>(total);
        }
        seg[0].flags = 0;
        seg[0].rsv = 0;

        unsigned char* payload = buf + framing;
        if (wr_len != 0 and wr != nullptr)
        {
            mem_copy(payload, wr, wr_len);
        }
        else
        {
            mem_zero(payload, wr_len);
        }
        mem_zero(payload + wr_len, rd_len); // read phase shifts dummy 0x00 out

        int32_t const rc = kos_call(ep, buf, framing + total, sizeof(buf));
        if (rc < 0)
        {
            return rc;
        }
        if (static_cast<size_t>(rc) < sizeof(struct kos_bus_rsp))
        {
            return -KOS_EINVAL; // reply too short to carry a status header
        }

        struct kos_bus_rsp const* rsp = reinterpret_cast<struct kos_bus_rsp const*>(buf);
        if (rsp->status < 0)
        {
            return rsp->status; // service-level error
        }

        size_t got = rsp->len;
        size_t const avail = static_cast<size_t>(rc) - sizeof(struct kos_bus_rsp);
        if (got > avail)
        {
            got = avail; // reply clamped in transit
        }
        unsigned char const* rxb = buf + sizeof(struct kos_bus_rsp);

        size_t out = 0;
        if (got > rx_skip)
        {
            out = got - rx_skip;
        }
        if (out > rx_len)
        {
            out = rx_len;
        }
        if (rx != nullptr and out != 0)
        {
            mem_copy(rx, rxb + rx_skip, out);
        }
        return static_cast<int32_t>(out);
    }
}

extern "C"
{
    int32_t spi_transfer(kos_cap_t ep, uint8_t device, void const* tx, void* rx, size_t len)
    {
        // One segment: rx is the whole full-duplex result (skip 0, len bytes).
        return frame_call(ep, device, /*nseg=*/1, /*wr_len=*/len, tx, /*rd_len=*/0, rx,
                          /*rx_skip=*/0, /*rx_len=*/len);
    }

    int32_t spi_transact(kos_cap_t ep, uint8_t device, void const* wr, size_t wlen, void* rd,
                         size_t rlen)
    {
        if (wlen == 0)
        {
            // No write phase: a single-segment read of rlen bytes.
            return frame_call(ep, device, /*nseg=*/1, /*wr_len=*/rlen, nullptr, /*rd_len=*/0,
                              rd, /*rx_skip=*/0, /*rx_len=*/rlen);
        }
        // Two segments in one CS bracket: rd is the read phase (skip the write phase).
        return frame_call(ep, device, /*nseg=*/2, /*wr_len=*/wlen, wr, /*rd_len=*/rlen, rd,
                          /*rx_skip=*/wlen, /*rx_len=*/rlen);
    }

    int spi_config(kos_cap_t ep, uint8_t device, struct kos_bus_cfg const* cfg,
                   uint32_t* achieved_hz)
    {
        if (cfg == nullptr)
        {
            return -KOS_EINVAL;
        }

        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_bus_req* req = reinterpret_cast<struct kos_bus_req*>(buf);
        req->proto = KOS_BUS_SPI;
        req->op = KOS_BUS_OP_CONFIG;
        req->device = device;
        req->nseg = 0; // CONFIG carries a kos_bus_cfg inline, no segments
        req->region_cap = -1;
        req->offset = 0;

        unsigned char* payload = buf + sizeof(struct kos_bus_req);
        mem_copy(payload, cfg, sizeof(struct kos_bus_cfg));

        int32_t const rc =
            kos_call(ep, buf, sizeof(struct kos_bus_req) + sizeof(struct kos_bus_cfg), sizeof(buf));
        if (rc < 0)
        {
            return static_cast<int>(rc);
        }
        if (static_cast<size_t>(rc) < sizeof(struct kos_bus_rsp))
        {
            return -KOS_EINVAL;
        }

        struct kos_bus_rsp const* rsp = reinterpret_cast<struct kos_bus_rsp const*>(buf);
        if (rsp->status < 0)
        {
            return rsp->status;
        }
        if (achieved_hz != nullptr)
        {
            uint32_t hz = 0;
            size_t const avail = static_cast<size_t>(rc) - sizeof(struct kos_bus_rsp);
            if (rsp->len >= sizeof(uint32_t) and avail >= sizeof(uint32_t))
            {
                mem_copy(&hz, buf + sizeof(struct kos_bus_rsp), sizeof(uint32_t));
            }
            *achieved_hz = hz;
        }
        return 0;
    }
}
