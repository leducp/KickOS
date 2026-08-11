// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SPI driver class: the contract every SPI backend implements. It must keep compiling as
// C, because the same code links from the kernel and from unprivileged userspace unchanged.
//
// ONE DEFINITION OF THESE PUBLIC NAMES PER IMAGE, chosen per executable: a local engine, the
// proxy that marshals onto a service endpoint, or the selftest's spi_mock.cc. A SERVICE driver
// renames its four symbols at its CMake target (kos_spi_bus_open=k64dspi_bus_open ...) so its
// own engine stays private. Skipping that rename on ALL FOUR is NOT reported at link time: an
// archive member is extracted only to satisfy a name nothing on the link line already defines,
// so the engine is silently shadowed. tests/check_class_backend.sh is what catches it.

#ifndef KICKOS_DRIVER_SPI_H
#define KICKOS_DRIVER_SPI_H

#include <kickos/sys/abi.h> // kos_cap_t, KOS_EP_MSG_MAX
#include <kickos/sys/bus.h> // kos_bus_seg / kos_bus_mode / kos_bus_cs_policy

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        KOS_SPI_SEG_MAX = KOS_BUS_SEG_MAX,

        // Controller words one device's configuration folds down to, kept in the handle.
        KOS_SPI_PROG_WORDS = 2,

        // The LARGEST transfer this API accepts, in bytes: the inline endpoint budget, not
        // a property of any controller.
        KOS_SPI_XFER_MAX = KOS_EP_MSG_MAX - (int)sizeof(struct kos_bus_req)
                           - KOS_SPI_SEG_MAX * (int)sizeof(struct kos_bus_seg)
    };

    // A consumer fills the field it holds; the linked implementation reads its own and
    // ignores the other. There is no discriminator.
    struct kos_spi_bus_config
    {
        uintptr_t base; // the granted register window; a local engine's authority
        kos_cap_t ep;   // a SIGNAL-bearing cap on a bus service endpoint; the proxy's
        kos_cap_t irq;  // the pacing IRQ cap, KOS_CAP_NONE when the engine polls
    };

    struct kos_spi_bus
    {
        uintptr_t base;
        kos_cap_t ep;
        kos_cap_t irq;
    };

    struct kos_spi_device_config
    {
        uint32_t hz;       // 0 = adopt the rate the bus is already programmed to
        uint8_t slot;      // this device's slot on the bus, < KOS_BUS_DEV_MAX
        uint8_t mode;      // enum kos_bus_mode
        uint8_t word_bits; // 0 = the 8-bit default
        uint8_t cs_policy; // enum kos_bus_cs_policy
        uint8_t cs_index;  // the controller CS line, or the GPIO pin slot
        uint8_t rsv[3];    // reserved zero
    };

    struct kos_spi_device
    {
        struct kos_spi_bus* bus;           // the bus this handle was issued against
        uint32_t hz;                       // the rate kos_spi_device_open reported
        uint32_t prog[KOS_SPI_PROG_WORDS]; // IMPLEMENTATION-PRIVATE controller words
        uint8_t slot;
        uint8_t mode;
        uint8_t word_bits;
        uint8_t cs_policy;
        uint8_t cs_index;
        uint8_t rsv[3];
    };

    // Bind the object to a controller and bring the bus up to the point where a device can
    // be opened against it. Returns 0, or a negative kos_errno. Nothing may transfer before
    // it returns 0.
    //
    // A LOCAL ENGINE DOES ITS WHOLE PERIPHERAL BRING-UP HERE, so the caller must already hold
    // the window grant and, where the engine is interrupt-paced, the line cap. A bring-up
    // store the platform silently discards is -KOS_EPERM here, not a dead bus later.
    int32_t kos_spi_bus_open(struct kos_spi_bus* b, struct kos_spi_bus_config const* cfg);

    // Issue a device handle against an open bus. Returns THE BIT CLOCK THE DEVICE WILL
    // ACTUALLY BE CLOCKED AT, rounded DOWN, or a negative kos_errno. NEVER an echo of
    // cfg->hz; a backend that can neither program the rate nor read the divider back REFUSES.
    // cfg->hz == 0 asks for no rate change and reports the rate already programmed, which is
    // not a licence to return an unknown rate.
    //
    // A well-formed request this controller cannot express is -KOS_ENOTSUP rather than
    // something quietly ignored, so cs_policy and mode are worth setting deliberately.
    //
    // THE HANDLE IS NOT REVALIDATED ON EVERY TRANSFER: it holds a pointer to its bus and the
    // folded controller words, so it must not outlive the bus and must not be copied to
    // another bus by hand.
    int32_t kos_spi_device_open(struct kos_spi_device* d, struct kos_spi_bus* b,
                                struct kos_spi_device_config const* cfg);

    // The segment arithmetic every backend must apply. Returns 0, or the negative kos_errno
    // kos_spi_transfer owes its caller.
    static inline int32_t kos_spi_seg_check(struct kos_bus_seg const* seg, uint8_t nseg,
                                           uint32_t len)
    {
        uint32_t sum = 0u;

        if (seg == 0)
        {
            return -KOS_EINVAL;
        }
        if (nseg < 1u)
        {
            return -KOS_EINVAL;
        }
        if (nseg > (uint8_t)KOS_SPI_SEG_MAX)
        {
            return -KOS_EINVAL;
        }
        if (len == 0u)
        {
            return -KOS_EINVAL;
        }
        if (len > (uint32_t)KOS_SPI_XFER_MAX)
        {
            return -KOS_EINVAL;
        }
        for (unsigned i = 0u; i < nseg; i++)
        {
            if (seg[i].flags != 0u)
            {
                return -KOS_EINVAL;
            }
            if (seg[i].rsv != 0u)
            {
                return -KOS_EINVAL;
            }
            sum += seg[i].len;
        }
        if (sum != len)
        {
            return -KOS_EINVAL;
        }
        return 0;
    }

    // Clock one transaction to `d`: apply its profile, assert its chip select, clock `len`
    // bytes full duplex across `nseg` segments, release the chip select. Returns `len`, or a
    // negative kos_errno.
    //
    // `buf` IS IN PLACE AND FULL DUPLEX: it holds the bytes to send on entry and the bytes
    // received on return, one byte per frame. A caller that wants its outgoing bytes back
    // must keep its own copy.
    //
    // ONE CHIP-SELECT BRACKET SPANS ALL SEGMENTS: nothing deasserts between them. `len` must
    // equal the sum of the segment lengths, or -KOS_EINVAL.
    //
    // kos_bus_seg.flags are the I2C phase controls; a non-zero flags or rsv here is
    // -KOS_EINVAL rather than something quietly dropped.
    //
    // THERE IS NO SHORT TRANSFER. `len` above KOS_SPI_XFER_MAX is -KOS_EINVAL in EVERY
    // implementation: a half-clocked transaction cannot be resumed, its chip select having
    // already been released. A consumer moving more splits it into transactions the target
    // accepts as separate.
    int32_t kos_spi_transfer(struct kos_spi_device* d, struct kos_bus_seg const* seg, uint8_t nseg,
                             unsigned char* buf, uint32_t len);

    // Quiesce the bus: the controller stopped, every interrupt source this backend armed
    // disarmed, the chip select left at its idle level. Returns 0, or a negative kos_errno.
    // Idempotent.
    //
    // RELEASES NOTHING. The window grant, the line cap and the endpoint cap belong to the
    // consumer that acquired them and are still held after this returns. Device handles
    // issued against this bus are dead once it returns and must not be transferred through.
    int32_t kos_spi_bus_close(struct kos_spi_bus* b);

    // BLOCKING, WITH NO DEADLINE ANY ARGUMENT HERE CAN EXPRESS. A caller parked in kos_call
    // has no timeout, so a proxy transfer against a service that stopped replying blocks that
    // thread forever. What saves it is the endpoint dying: when the last receiver goes, the
    // parked caller wakes with -KOS_EPIPE. An image that keeps a receive-bearing cap alive
    // outside the service thread defeats that.

#ifdef __cplusplus
}
#endif

#endif
