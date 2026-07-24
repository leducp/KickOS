// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Chip-NEUTRAL SPI client wrapper over the bus-service call/reply ABI
// (<kickos/sys/bus.h>). A client holds a SIGNAL-bearing cap on an SPI service
// endpoint and issues transactions through these helpers; each builds a
// kos_bus_req frame in a stack buffer, does one kos_call, and splits the rx bytes
// out of the kos_bus_rsp reply. No chip register, no CS knowledge, no MMIO -- the
// same object links against ANY bus driver (K64F DSPI, XMC USIC, ...); the driver
// owns the controller and the chip-select. So a caller MUST be a spawned pool
// thread (the root/init thread cannot kos_call: -KOS_EPERM).
//
// Byte budget: a request frames as kos_bus_req(12) + nseg*kos_bus_seg + inline tx
// bytes, capped by KOS_EP_MSG_MAX (256). spi_transfer clocks one segment;
// spi_transact clocks a write phase then a read phase in ONE CS bracket (two
// segments, one call) -- the register/CSR access shape.

#ifndef KICKOS_DRIVER_SPI_CLIENT_H
#define KICKOS_DRIVER_SPI_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/bus.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // Full-duplex transfer of `len` bytes over SPI service endpoint `ep` (a
    // SIGNAL-bearing cap). tx == NULL shifts dummy 0x00; rx == NULL discards. One
    // segment, CS bracketed by the driver across the whole transfer. Returns rx
    // bytes (>= 0), or a negative -KOS_E* (kos_call failure or the service status).
    long spi_transfer(int ep, void const* tx, void* rx, size_t len);

    // Two-phase transaction in ONE CS bracket: clock `wlen` write bytes then `rlen`
    // read bytes (dummy 0x00 out), all under a single held CS -- the coherent
    // command+payload shape length-sensitive targets (e.g. LAN9252) require. rd
    // receives the read-phase bytes (may be NULL to discard). Returns `rlen` (>= 0),
    // or a negative -KOS_E*.
    long spi_transact(int ep, void const* wr, size_t wlen, void* rd, size_t rlen);

    // Apply a per-device config (baud/mode/word/CS) at the service. On success
    // *achieved_hz (if non-NULL) gets the driver's rounded-DOWN bit clock. Returns
    // 0, or a negative -KOS_E*.
    int spi_config(int ep, struct kos_bus_cfg const* cfg, uint32_t* achieved_hz);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_SPI_CLIENT_H
