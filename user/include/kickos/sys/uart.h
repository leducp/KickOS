// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The UART service wire ABI: what a client and a buffered userspace UART driver exchange
// over a kos_call endpoint. Same shape and same rules as <kickos/sys/bus.h>: fixed-width
// fields, no pointers, no padding holes, payload inline in the KOS_EP_MSG_MAX buffer.
// Sized by asserts at the bottom, because a silent layout change is a wire break.
//
// A 1:1 serialization of the Uart class methods (design-m4-driver-model.md): four DEVICE
// ops, four methods, no device op that is not a method and no method that is not an op.
// SET_MODE is the one op that is NOT a device method: it sets ring-side write policy and
// touches no register.
//
// Call/reply rather than a bare rendezvous because a WRITE must return HOW MANY bytes were
// accepted (the ring may be full) and a READ must return bytes plus a count. No BLOCKING
// device transaction is involved, unlike SPI: the driver replies immediately out of ring
// state and never parks its client waiting on hardware.

#ifndef KICKOS_SYS_UART_H
#define KICKOS_SYS_UART_H

#include <stdint.h>

#include <kickos/sys/byte_ring.h> // KOS_ATOMIC_U32, kos_counter_increment

#ifdef __cplusplus
extern "C"
{
#endif

enum kos_uart_op
{
    KOS_UART_CONFIGURE = 0, // baud + frame format
    KOS_UART_WRITE = 1,     // queue bytes for TX; may accept fewer than offered
    KOS_UART_READ = 2,      // take up to len bytes from RX; may return 0
    KOS_UART_STATS = 3,     // the driver's counters, including what it dropped
    // Set write policy for the UNFRAMED console arm, from kos_uart_flags. Refused with
    // -KOS_ENOSYS by a service that has no unframed arm, so a mode can never be accepted
    // and then not applied.
    KOS_UART_SET_MODE = 4
};

// Frame format for KOS_UART_CONFIGURE. Chip-neutral by SHAPE: the driver maps these onto
// its own register encoding.
enum kos_uart_parity
{
    KOS_UART_PARITY_NONE = 0,
    KOS_UART_PARITY_EVEN = 1,
    KOS_UART_PARITY_ODD = 2
};

enum kos_uart_flags
{
    // KOS_UART_READ: reserved for a future blocking read. A driver that does not
    // implement it MUST refuse the flag with -KOS_ENOSYS rather than silently returning
    // 0 bytes, so a client cannot mistake "not supported" for "no data". Blocking needs a
    // receive-from-either-of-two-sources primitive the kernel does not have
    // (design-m4.6-irq-driver.md section 7.5).
    KOS_UART_F_BLOCK = 1 << 0,
    // KOS_UART_SET_MODE: O_NONBLOCK for the unframed console arm. Clear, a write waits for
    // ring room and does not give up; set, it takes what fits and returns.
    //
    // WHERE THE LOST COUNT IS REPORTED, and why it is not a return value. The unframed arm
    // is a plain send, and a plain send is released the moment the receiver TAKES the
    // message (endpoint_send copies into the parked receiver and wakes it, in the sender's
    // own context). The ring accept happens afterwards, in the driver's context, so the
    // accepted count does not yet exist when the sender resumes. No per-call report is
    // possible on this path without making the hot console write a call/reply.
    // The loss is therefore reported through the COUNTERS: every unframed short accept adds
    // its remainder to stats.tx_dropped, which any holder of the endpoint reads with the
    // framed KOS_UART_STATS, off the hot path. A writer can see its own loss and pace
    // itself; it CANNOT learn which bytes went missing, so this path supports pacing and
    // not retry. A caller needing an exact per-call count with retry uses KOS_UART_WRITE,
    // which is framed and already reports a short accept in rsp.len.
    //
    // A transport whose ring may have no consumer at all REQUIRES this flag and refuses a
    // request to clear it with -KOS_ENOTSUP, since a blocking write there is unbounded.
    KOS_UART_F_NONBLOCK = 1 << 1
};

struct kos_uart_req
{
    uint8_t op;    // enum kos_uart_op
    uint8_t flags; // enum kos_uart_flags
    uint16_t len;  // WRITE: payload bytes that follow; READ: bytes requested
    uint32_t baud; // CONFIGURE only
    uint8_t data_bits;
    uint8_t parity; // enum kos_uart_parity
    uint8_t stop_bits;
    uint8_t rsv;
};

struct kos_uart_rsp
{
    int32_t status; // 0, or a negative KOS_E*-taxonomy service error
    uint16_t len;   // bytes accepted (WRITE) / bytes returned (READ)
    uint16_t rsv;
};

// Read by KOS_UART_STATS, and ALSO the driver's own live counters: they live in the shared
// ring block, which is arena memory the bring-up allocated, so they OUTLIVE the driver
// thread and a supervisor can read the final tally after a restart.
//
// The live copy is read and written by two threads, so the STATS reply is built field by
// field: a block copy of the struct is not a read of its atomics at all.
//
// KOS_ATOMIC_U32, not <kickos/sys/atomic.h>: a pure C main linking libkickos names this
// struct to read the STATS reply.
struct kos_uart_stats
{
    KOS_ATOMIC_U32 tx_bytes;
    KOS_ATOMIC_U32 rx_bytes;
    KOS_ATOMIC_U32 tx_dropped;   // lost on driver death, or by a client that gave up retrying
    KOS_ATOMIC_U32 rx_dropped;   // RX ring full at IRQ time (a SOFTWARE overrun)
    KOS_ATOMIC_U32 rx_overrun;   // hardware overrun flag seen (ORER / OR / RXFIFO_OVF)
    KOS_ATOMIC_U32 rx_framing;   // FER / FE / FRM_ERR
    KOS_ATOMIC_U32 rx_parity;    // PER / PF / PARITY_ERR
    KOS_ATOMIC_U32 irq_wakes;    // irq_wait returns, hardware raises AND doorbell notifies
    KOS_ATOMIC_U32 irq_spurious; // of those, the ones that found nothing asserted
};

// 12 B of request framing leaves 244 B of inline payload under KOS_EP_MSG_MAX (256). The
// region-cap path for larger transfers stays reserved and -KOS_ENOSYS as in bus.h, so no
// raw pointer belongs in these structs.
#ifdef __cplusplus
static_assert(sizeof(struct kos_uart_req) == 12, "kos_uart_req must stay 12 bytes (wire ABI)");
static_assert(sizeof(struct kos_uart_rsp) == 8, "kos_uart_rsp must stay 8 bytes (wire ABI)");
static_assert(sizeof(struct kos_uart_stats) == 36, "kos_uart_stats must stay 36 bytes (wire ABI)");
#else
_Static_assert(sizeof(struct kos_uart_req) == 12, "kos_uart_req must stay 12 bytes (wire ABI)");
_Static_assert(sizeof(struct kos_uart_rsp) == 8, "kos_uart_rsp must stay 8 bytes (wire ABI)");
_Static_assert(sizeof(struct kos_uart_stats) == 36, "kos_uart_stats must stay 36 bytes (wire ABI)");
#endif

#ifdef __cplusplus
}
#endif

#endif
