// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RAW UART driver class: the in-process contract every UART backend implements. Form is
// design-m4-driver-model.md rule 4's "object in C" (a POD struct plus free functions taking
// it by pointer, no ctor, no dtor, no vtable), written to rule 3's KERNEL BAR: no
// exceptions, no STL, explicit init, never implicit lifetime. It compiles as C, because the
// same code must link from the kernel and from unprivileged userspace unchanged.
//
// FIVE CALLS: open, read, write, flush, close. Interrupt arming, status-flag clearing,
// FIFO recovery and error-latch handling are MECHANISM and stay inside these five.
//
// EXACTLY ONE DEFINITION PER IMAGE, and the build selects it: a per-chip local backend, or a
// proxy marshalling onto a service endpoint. An image carrying a SECOND definition does not
// report a duplicate symbol; it keeps the real backend's archive member out of the link and
// drives the other definition instead. That is why the host contract gate's mock
// (tests/uartclass) is kept off every target image, and tests/check_class_backend.sh is what
// enforces it.
//
// THE CLASS MOVES BYTES AND NOTHING ELSE. CRLF expansion, retry-on-full and the console's
// line discipline belong to each consumer: <kickos/sys/uart_service.h> for the service
// route, kernel/init/console.cc for the panic route.
//
// READ AND WRITE NEVER BLOCK, SPIN OR OWN A BUFFER. Each carries an explicit length and
// returns what actually moved, so a short transfer is the ordinary case and the retry policy
// is the caller's. <kickos/sys/uart.h> is the 1:1 serialization of these calls and caps an
// inline payload at KOS_EP_MSG_MAX, so a call promising an unbounded transfer here could not
// be serialized 1:1 later. flush is the ONE call that waits.

#ifndef KICKOS_DRIVER_UART_H
#define KICKOS_DRIVER_UART_H

#include <kickos/sys/errno.h> // KOS_EINVAL, for the shared cfg check below
#include <kickos/sys/uart.h>  // kos_uart_stats, kos_uart_parity: the wire contract's own

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // The instance. Rule 4: the context is EXPLICIT, so a backend granted one channel is
    // handed that base and cannot name the others.
    struct kos_uart
    {
        uintptr_t base;               // the granted register window
        struct kos_uart_stats* stats; // never null after open; see kos_uart_config
    };

    // What a channel is opened WITH. UART vocabulary only: a device class must not be able
    // to read a service's priority, kind or thread name.
    struct kos_uart_config
    {
        uintptr_t base;               // the granted register window
        struct kos_uart_stats* stats; // NOT optional; see kos_uart_open
        uint32_t baud;                // 0 = adopt the rate the channel is already running at
        uint8_t data_bits;
        uint8_t parity; // enum kos_uart_parity
        uint8_t stop_bits;
        uint8_t rsv; // reserved zero
    };

    // The cfg clauses every backend refuses IDENTICALLY, stated once in the contract's own
    // header so a backend cannot grow its own dialect of them. Returns 0, or -KOS_EINVAL.
    //
    // EVERY kos_uart_open CALLS THIS FIRST, before it touches a register or binds the
    // object. base is dereferenced as MMIO and stats is dereferenced on every read, so a
    // backend that took either as absent would store through a null base and fault on its
    // first byte. Checking them here once is what lets the transfer paths stay branch-free.
    static inline int32_t kos_uart_cfg_check(struct kos_uart_config const* cfg)
    {
        if (cfg->base == 0u)
        {
            return -KOS_EINVAL;
        }
        if (cfg->stats == 0)
        {
            return -KOS_EINVAL;
        }
        if (cfg->rsv != 0u)
        {
            return -KOS_EINVAL;
        }
        return 0;
    }

    // Bind the object to a channel, program baud and frame, and put the line live.
    // Returns THE BAUD THE CHANNEL IS ACTUALLY RUNNING AT as a positive value, or a
    // negative kos_errno.
    //
    // THE RETURN IS NEVER AN ECHO OF cfg->baud. A backend that can neither program the
    // rate nor read it back REFUSES: a consumer that cannot tell "programmed" from
    // "ignored" has no way to learn its bytes are leaving at the wrong rate. Divisor
    // rounding means the achieved rate normally differs from the request by a fraction of a
    // percent, and the value returned is the rounded-down truth rather than the request.
    //
    // cfg->baud == 0 asks for no rate change: the channel keeps whatever divisor it already
    // has and open reports that rate. It is not a licence to return an unknown rate; a
    // backend that cannot read its divisor back still refuses.
    //
    // -KOS_EINVAL is a cfg kos_uart_cfg_check rejects, and every backend runs that check
    // first. -KOS_ENOTSUP is the frame format this controller has no encoding for, or a rate
    // outside its divider's reach. -KOS_ENOSYS is a rate the chip could not report at
    // runtime. -KOS_EPERM is a register the platform will not let this caller write.
    //
    // cfg->stats is validated ONCE here and never again: a consumer that wants no counters
    // supplies a private struct rather than a null pointer. The service route points it at
    // the shared ring block, where the counters outlive the driver thread.
    //
    // Nothing may transfer before open returns non-negative. A divisor or frame change with
    // a byte in the shifter corrupts it on the wire, and several parts make the divisor
    // unwritable while the transmitter is enabled.
    int32_t kos_uart_open(struct kos_uart* u, struct kos_uart_config const* cfg);

    // Take up to n bytes from the receiver and return how many arrived, 0 when nothing is
    // pending. Also the place the chip's receive-error latches are read and cleared, which
    // is why it must be called even by a consumer that discards the bytes: on several parts
    // an uncleared framing or overrun flag inhibits all further reception.
    //
    // Counts stats.rx_bytes (bytes taken off the wire) and stats.rx_overrun / rx_framing /
    // rx_parity. rx_dropped is NOT counted here: bytes lost to a full ring are the
    // consumer's loss, and the class has no ring.
    uint32_t kos_uart_read(struct kos_uart* u, unsigned char* dst, uint32_t n);

    // Hand up to n bytes to the transmitter and return how many it took, 0 when the
    // transmit buffer or FIFO is full. NEVER spins waiting for room.
    //
    // THIS CALL OWNS THE TX INTERRUPT. A return below n arms the source, so the next
    // drain-room event wakes the consumer; a return of exactly n disarms it. A consumer
    // arming on its own guess would arm an IDLE transmitter, which on a transition-triggered
    // part raises nothing and waits forever.
    //
    // A CONSUMER THAT STOPS SHORT OF EMPTYING ITS QUEUE MUST CALL AGAIN WITH WHAT IS LEFT,
    // even if that is nothing: the disarm only happens on a call that was not refused, so
    // draining in fixed-size segments has to end on a call the device accepted whole.
    //
    // The count is what the DEVICE accepted, so a caller staging out of a ring must release
    // exactly that many (kos_byte_ring_peek then kos_byte_ring_drop) rather than popping
    // first. Does NOT touch stats.tx_bytes; the producer that queued the bytes owns it.
    uint32_t kos_uart_write(struct kos_uart* u, unsigned char const* src, uint32_t n);

    // Wait until the transmit path has drained, then return 0. -KOS_EBUSY when the backend's
    // own bound expired with bytes still in flight.
    //
    // "DRAINED" IS AS STRONG AS THE SILICON ALLOWS AND NO STRONGER. With a shifter or
    // frame-finished indication it means the last stop bit left the pin; with only FIFO
    // occupancy it means the FIFO emptied and a byte may still be shifting, so a consumer
    // that must not clip the final byte needs its own delay. Each backend states which it
    // is at its definition.
    //
    // Bytes a consumer still holds in a ring of its own are NOT covered.
    int32_t kos_uart_flush(struct kos_uart* u);

    // Quiesce the channel: transmit and receive off, every interrupt source this backend
    // armed disarmed. Returns 0, or a negative kos_errno. Idempotent.
    //
    // RELEASES NOTHING: the MMIO grant and any ring block are still held after this returns.
    // Flush FIRST, because stopping the channel truncates a frame that is still shifting.
    int32_t kos_uart_close(struct kos_uart* u);

#ifdef __cplusplus
}
#endif

#endif
