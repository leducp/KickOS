// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The I2C driver class: the contract every I2C backend implements. It must keep compiling as
// C, because the same code links from the kernel and from unprivileged userspace unchanged.
//
// ONE DEFINITION OF THESE PUBLIC NAMES PER IMAGE, chosen per executable: a local engine, the
// proxy that marshals onto a service endpoint, or a mock. A SERVICE driver renames its four
// symbols at its CMake target (kos_i2c_bus_open=rxriic_bus_open ...) so its own engine stays
// private. Skipping that rename on ALL FOUR is NOT reported at link time: an archive member is
// extracted only to satisfy a name nothing on the link line already defines, so the engine is
// silently shadowed. tests/static/check_class_backend.sh is what catches it.
//
// FOUR THINGS THIS CLASS REFUSES TO PROMISE, because at least one of the three target
// controllers cannot honour them (docs/design-m5-i2c-seam.md section 3). Three of them are
// absences, so a comment is the only place they can be recorded:
//
//   1. NO LATE ACK DECISION. There is no hook that inspects a received byte and then chooses
//      ACK or NACK for it. A controller that fixes ACK at enqueue time cannot express one.
//   2. NO HARDWARE TIMEOUT. No backend arms a controller watchdog; see the deadline argument
//      of kos_i2c_transfer, which is software and mandatory.
//   3. NO ADDRESS-NACK VERSUS DATA-NACK DISTINCTION, AND THIS ONE IS CONDITIONAL. No
//      controller here has a second status bit for it, so the discriminator is the POSITION
//      of the NACK and the caller derives the meaning from the segment layout it submitted.
//      That derivation only works while the position is CARRIED: it is the `xferred` count
//      of kos_i2c_transfer below, and an implementation that reports 0 there on every error
//      withdraws the grounds this refusal stands on: an absent device and a device that
//      refused a data byte become one answer, with nothing to catch it. There is no bit;
//      there is a count, and it is not optional (docs/design-m5-i2c-seam.md section 9.1).
//   4. NO BUS-BUSY CALL. One of the three controllers has no such bit at all, so a
//      kos_i2c_bus_busy would be a promise two chips keep and one fakes.

#ifndef KICKOS_DRIVER_I2C_H
#define KICKOS_DRIVER_I2C_H

#include <kickos/sys/abi.h> // kos_cap_t, KOS_EP_MSG_MAX
#include <kickos/sys/bus.h> // kos_bus_seg / kos_bus_mode / KOS_BUS_SEG_RD / KOS_BUS_SEG_STOP

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        KOS_I2C_SEG_MAX = KOS_BUS_SEG_MAX,

        // Controller words one device's configuration folds down to, kept in the handle.
        KOS_I2C_PROG_WORDS = 2,

        // The LARGEST transfer this API accepts, in bytes: the inline endpoint budget, not
        // a property of any controller.
        KOS_I2C_XFER_MAX = KOS_EP_MSG_MAX - (int)sizeof(struct kos_bus_req)
                           - KOS_I2C_SEG_MAX * (int)sizeof(struct kos_bus_seg),

        // Ceiling on the transaction deadline below. A held SCL is the whole bus stopped for
        // every device on it, so an arbitrarily long deadline is the denial of service the
        // deadline exists to bound. The slowest rate any backend here programs moves
        // KOS_I2C_XFER_MAX bytes in well under a millisecond of this.
        KOS_I2C_TIMEOUT_MAX_US = 1000000
    };

    // A consumer fills the field it holds; the linked implementation reads its own and
    // ignores the other. There is no discriminator.
    struct kos_i2c_bus_config
    {
        uintptr_t base; // the granted register window; a local engine's authority
        kos_cap_t ep;   // a SIGNAL-bearing cap on a bus service endpoint; the proxy's
        kos_cap_t irq;  // the pacing IRQ cap, KOS_CAP_NONE when the engine polls
    };

    struct kos_i2c_bus
    {
        uintptr_t base;
        kos_cap_t ep;
        kos_cap_t irq;
    };

    struct kos_i2c_device_config
    {
        uint32_t hz;    // 0 = adopt the rate the bus is already programmed to
        uint16_t addr;  // 7-bit address, or 10-bit with KOS_BUS_ADDR_10BIT in mode
        uint8_t slot;   // this device's slot on the bus, < KOS_BUS_DEV_MAX
        uint8_t mode;   // enum kos_bus_mode; only KOS_BUS_ADDR_10BIT is an I2C bit
        uint8_t rsv[4]; // reserved zero
    };

    struct kos_i2c_device
    {
        struct kos_i2c_bus* bus;           // the bus this handle was issued against
        uint32_t hz;                       // the rate kos_i2c_device_open reported
        uint32_t prog[KOS_I2C_PROG_WORDS]; // IMPLEMENTATION-PRIVATE controller words
        uint16_t addr;
        uint8_t slot;
        uint8_t mode;
    };

    // Bind the object to a controller and bring the bus up to the point where a device can
    // be opened against it. Returns 0, or a negative kos_errno. Nothing may transfer before
    // it returns 0.
    //
    // A LOCAL ENGINE DOES ITS WHOLE PERIPHERAL BRING-UP HERE, so the caller must already hold
    // the window grant and, where the engine is interrupt-paced, the line cap.
    //
    // THE BUS IS LEFT RELEASED OR THE CALL FAILS. A bus found with SDA or SCL held low by a
    // peer that lost sync is recovered by whatever primitive this controller has, and one
    // still held after that is -KOS_EBUSY.
    int32_t kos_i2c_bus_open(struct kos_i2c_bus* b, struct kos_i2c_bus_config const* cfg);

    // Issue a device handle against an open bus. Returns THE SCL FREQUENCY THIS DEVICE WILL
    // ACTUALLY BE CLOCKED AT, rounded DOWN, or a negative kos_errno. NEVER an echo of
    // cfg->hz; a backend that can neither program the rate nor read the divider back REFUSES.
    // cfg->hz == 0 asks for no rate change and reports the rate already programmed, which is
    // not a licence to return an unknown rate.
    //
    // THE REPORTED RATE CARRIES A BOARD ASSUMPTION. SCL rise and fall are set by the bus
    // capacitance and the pull-ups, which no controller can measure; a backend whose rate
    // formula names those terms substitutes the I2C-bus specification maxima for the mode it
    // is programming. A slower bus than the specification allows therefore runs slower than
    // this number, never faster.
    //
    // A well-formed request this controller cannot express is -KOS_ENOTSUP, never something
    // quietly ignored.
    //
    // THE HANDLE IS NOT REVALIDATED ON EVERY TRANSFER: it holds a pointer to its bus and the
    // folded controller words, so it must not outlive the bus and must not be copied to
    // another bus by hand.
    int32_t kos_i2c_device_open(struct kos_i2c_device* d, struct kos_i2c_bus* b,
                                struct kos_i2c_device_config const* cfg);

    // The segment arithmetic every backend must apply. Returns 0, or the negative kos_errno
    // kos_i2c_transfer owes its caller.
    //
    // THE LAST SEGMENT MUST CARRY KOS_BUS_SEG_STOP. The wire ABI can express a list that ends
    // without one, and this class refuses it: the transaction would return with the bus still
    // owned and SCL held, which stops every unrelated device on it until some later call
    // happens to release it.
    //
    // A ZERO-LENGTH WRITE SEGMENT IS LEGAL and is the address-only presence probe: the address
    // byte is clocked and its acknowledgement is the whole result. A zero-length READ segment
    // is not, there being no way to address a device for reading and then clock nothing.
    static inline int32_t kos_i2c_seg_check(struct kos_bus_seg const* seg, uint8_t nseg,
                                            uint32_t len)
    {
        uint32_t sum = 0u;
        uint8_t const known = (uint8_t)(KOS_BUS_SEG_RD | KOS_BUS_SEG_STOP);

        if (seg == 0)
        {
            return -KOS_EINVAL;
        }
        if (nseg < 1u)
        {
            return -KOS_EINVAL;
        }
        if (nseg > (uint8_t)KOS_I2C_SEG_MAX)
        {
            return -KOS_EINVAL;
        }
        if (len > (uint32_t)KOS_I2C_XFER_MAX)
        {
            return -KOS_EINVAL;
        }
        for (unsigned i = 0u; i < nseg; i++)
        {
            if ((seg[i].flags & (uint8_t)~known) != 0u)
            {
                return -KOS_EINVAL;
            }
            if (seg[i].rsv != 0u)
            {
                return -KOS_EINVAL;
            }
            if ((seg[i].flags & (uint8_t)KOS_BUS_SEG_RD) != 0u)
            {
                if (seg[i].len == 0u)
                {
                    return -KOS_EINVAL;
                }
            }
            sum += seg[i].len;
        }
        if ((seg[nseg - 1u].flags & (uint8_t)KOS_BUS_SEG_STOP) == 0u)
        {
            return -KOS_EINVAL;
        }
        if (sum != len)
        {
            return -KOS_EINVAL;
        }
        return 0;
    }

    // Run one transaction against `d`: a START, then each segment in turn, then the STOP the
    // last segment carries. Returns `len`, or a negative kos_errno.
    //
    // `buf` IS THE WHOLE SEGMENT STREAM LAID END TO END, `len` bytes, one region per segment
    // in order. A write segment SOURCES its bytes from its region; a read segment STORES into
    // its region. It is not full duplex: nothing is written back over a write segment's bytes.
    //
    // KOS_BUS_SEG_STOP ON A SEGMENT ENDS THAT TRANSACTION; its ABSENCE is a repeated START
    // into the next segment with the bus never released. A list may therefore carry several
    // transactions, and the bus is held for as long as the whole list takes.
    //
    // `timeout_us` IS A DEADLINE ON THE WHOLE LIST, IT IS MANDATORY, AND IT IS ENFORCED IN
    // SOFTWARE. 0 is -KOS_EINVAL and so is anything above KOS_I2C_TIMEOUT_MAX_US: no
    // controller here has a usable hardware timeout, and held SCL is not a local stall but
    // the whole bus stopped for every device on it. On expiry the transaction is ABANDONED,
    // the bus is recovered as far as this controller can, and -KOS_ETIMEDOUT is returned.
    // Unlike a timed syscall this does leave state behind: the peer saw part of a
    // transaction.
    //
    // SIZE IT FOR THE DEVICE, NOT FOR THE BYTE COUNT. Held SCL has two causes and they are
    // indistinguishable from the bus: this driver was late, or the addressed part is
    // stretching the clock on purpose. Stretching is a documented, normal data path on real
    // sensors and can last for a conversion, so a deadline derived from the transfer length
    // alone will time out a healthy device.
    //
    // THE BUS IS RELEASED BEFORE EVERY RETURN, success or failure. On any error the backend
    // issues a STOP, and where a STOP alone cannot free the lines it runs whatever recovery
    // its controller has, before returning the ORIGINAL error rather than the recovery's.
    //
    // -KOS_EIO IS A NACK and `*xferred` IS WHERE IT HAPPENED. The code does not say address
    // versus data, per refusal 3 above; the count does, against the segment list the caller
    // submitted. -KOS_EBUSY is arbitration lost or a bus no recovery could free.
    //
    // `xferred` IS MANDATORY AND IS WRITTEN BEFORE EVERY RETURN, failures included; a null
    // one is -KOS_EINVAL rather than a permitted "do not care", because a caller that drops
    // it drops the only discriminator a NACK has. It counts PAYLOAD bytes of `buf`, running
    // across the whole segment list, and the rule is the one in docs/reference/bus-service.md:
    // bytes actually transferred BEFORE the failure. So a write segment contributes the bytes
    // the device acknowledged, a read segment the bytes it stored, the address byte of a
    // segment is never counted because it is not payload, and the byte a NACK refused is not
    // counted either. An address NACK is therefore 0 bytes into that segment; a device that
    // acknowledged two register-address bytes and refused the first data byte of the same
    // segment is 2. On success it is `len`.
    //
    // THERE IS NO SHORT TRANSFER. `len` above KOS_I2C_XFER_MAX is -KOS_EINVAL in EVERY
    // implementation. A consumer moving more splits it into transactions the target accepts
    // as separate. A return below `len` is an error return, never a partial success.
    int32_t kos_i2c_transfer(struct kos_i2c_device* d, struct kos_bus_seg const* seg, uint8_t nseg,
                             unsigned char* buf, uint32_t len, uint32_t timeout_us,
                             uint32_t* xferred);

    // Quiesce the bus: the controller stopped, every interrupt source this backend armed
    // disarmed, both lines left released. Returns 0, or a negative kos_errno. Idempotent.
    //
    // RELEASES NOTHING. The window grant, the line cap and the endpoint cap belong to the
    // consumer that acquired them and are still held after this returns. Device handles
    // issued against this bus are dead once it returns and must not be transferred through.
    int32_t kos_i2c_bus_close(struct kos_i2c_bus* b);

    // WHERE `xferred` LIVES ON THE WIRE, AND THE CONTRADICTION A SERVICE MUST NOT SPLIT THE
    // DIFFERENCE ON. bus.h names kos_bus_rsp.len "rx bytes following"; the NACK rule in
    // docs/reference/bus-service.md names it "bytes actually transferred before the NACK".
    // A write-only transaction makes the two disagree outright (three bytes transferred and
    // no rx byte to follow), so the sign of `status` is the discriminator:
    //
    //   status >= 0   len is rx bytes following, and they follow. Unchanged.
    //   status <  0   NO reply payload follows at all, and len is `xferred`.
    //
    // A reader that sizes its copy by len without first reading status reads bytes that are
    // not there. An aborted transaction DISCARDS whatever an earlier read segment had already
    // stored: the transaction, not the segment, is the unit the client asked for.
    //
    // BLOCKING, AND `timeout_us` DOES NOT CROSS THE WIRE. struct kos_bus_req has no field for
    // a deadline, so a proxy implementation of kos_i2c_transfer cannot forward its caller's:
    // the deadline a SERVICE enforces is the service's own policy, and the caller's argument
    // bounds only its local work. A caller parked in kos_call has no timeout of its own
    // either; what saves it is the endpoint dying, which wakes it with -KOS_EPIPE.

#ifdef __cplusplus
}
#endif

#endif
