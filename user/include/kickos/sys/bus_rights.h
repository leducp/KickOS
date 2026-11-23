// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The rights a bus service request needs of its caller's badge (<kickos/sys/rights.h>), which every
// bus service checks before it acts on the request's op.

#ifndef KICKOS_SYS_BUS_RIGHTS_H
#define KICKOS_SYS_BUS_RIGHTS_H

#include <kickos/sys/bus.h>
#include <kickos/sys/rights.h>

#include <stdint.h>

namespace kickos::bus
{

// CONFIG needs `config`; an SPI transfer `transfer`; an I2C transfer `write` for any segment that
// writes and `read` for any that reads. An unknown op or protocol needs none, and is refused as
// unknown.
inline uint32_t request_rights(struct kos_bus_req const& req, struct kos_bus_seg const* seg, unsigned nseg)
{
    if (req.op == KOS_BUS_OP_CONFIG)
    {
        return KOS_RIGHT_CONFIG;
    }
    if (req.op != KOS_BUS_OP_XFER)
    {
        return KOS_RIGHT_NONE;
    }
    if (req.proto == KOS_BUS_SPI)
    {
        return KOS_RIGHT_TRANSFER;
    }
    if (req.proto != KOS_BUS_I2C)
    {
        return KOS_RIGHT_NONE;
    }
    uint32_t rights = KOS_RIGHT_NONE;
    for (unsigned s = 0; s < nseg; s++)
    {
        if ((seg[s].flags & KOS_BUS_SEG_RD) != 0u)
        {
            rights = rights | KOS_RIGHT_READ;
        }
        else
        {
            rights = rights | KOS_RIGHT_WRITE;
        }
    }
    return rights;
}

}

#endif
