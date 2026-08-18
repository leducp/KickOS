// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The UART service wire ABI: what a client and a buffered userspace UART driver exchange
// over a kos_call endpoint. Same shape and same rules as <kickos/sys/bus.h>: fixed-width
// fields, no pointers, no padding holes, payload inline in the KOS_EP_MSG_MAX buffer, sizes
// pinned by the asserts at the bottom.
//
// The driver replies immediately out of ring state and never parks its client on hardware.

#ifndef KICKOS_SYS_UART_H
#define KICKOS_SYS_UART_H

#include <stdint.h>

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
    // KOS_UART_READ: a blocking read. A driver that does not implement it MUST refuse the
    // flag with -KOS_ENOSYS rather than silently returning 0 bytes, so a client cannot
    // mistake "not supported" for "no data".
    KOS_UART_F_BLOCK = 1 << 0,
    // KOS_UART_SET_MODE: O_NONBLOCK for the unframed console arm. Clear, a write waits for
    // ring room and does not give up; set, it takes what fits and returns.
    //
    // The unframed arm reports NO per-call count: every short accept adds its remainder to
    // stats.tx_dropped, read with the framed KOS_UART_STATS. A writer can pace itself on
    // that but cannot learn WHICH bytes went missing, so retry needs KOS_UART_WRITE, which
    // reports a short accept in rsp.len.
    //
    // A transport whose ring may have no consumer at all REQUIRES this flag and refuses a
    // request to clear it with -KOS_ENOTSUP, a blocking write there being unbounded.
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

typedef struct
{
    uint32_t v;
} kos_counter_t;

// EVERY counter below has exactly ONE writer: a second writer of any field loses updates,
// and nothing here can check that. The READER is a different thread, so the access is
// RELAXED-atomic rather than plain. Both helpers must also compile as C, which is what the
// __atomic builtins are doing here in place of <kickos/sys/atomic.h>.
static inline __attribute__((always_inline)) void kos_counter_increment(kos_counter_t* c,
                                                                        uint32_t delta)
{
    __atomic_store_n(&c->v, __atomic_load_n(&c->v, __ATOMIC_RELAXED) + delta,
                     __ATOMIC_RELAXED);
}

static inline __attribute__((always_inline)) uint32_t kos_counter_load(kos_counter_t const* c)
{
    return __atomic_load_n(&c->v, __ATOMIC_RELAXED);
}

// Read by KOS_UART_STATS, and ALSO the driver's own live counters: they live in the shared
// ring block, which is arena memory the bring-up allocated, so they OUTLIVE the driver
// thread and a supervisor can read the final tally after a restart.
struct kos_uart_stats
{
    kos_counter_t tx_bytes;
    kos_counter_t rx_bytes;
    kos_counter_t tx_dropped;   // lost on driver death, or by a client that gave up retrying
    kos_counter_t rx_dropped;   // RX ring full at IRQ time (a SOFTWARE overrun)
    kos_counter_t rx_overrun;   // hardware overrun flag seen (ORER / OR / RXFIFO_OVF)
    kos_counter_t rx_framing;   // FER / FE / FRM_ERR
    kos_counter_t rx_parity;    // PER / PF / PARITY_ERR
    kos_counter_t irq_wakes;    // irq_wait returns, hardware raises AND doorbell notifies
    kos_counter_t irq_spurious; // of those, the ones that found nothing asserted
};

// 12 B of request framing leaves 244 B of inline payload under KOS_EP_MSG_MAX (256).
//
// kos_counter_t is pinned to the word it wraps: nine of them ARE the 36-byte wire image, and
// stats_pack / stats_unpack copy the struct whole.
#ifdef __cplusplus
static_assert(sizeof(kos_counter_t) == 4, "kos_counter_t must stay one uint32_t wide");
static_assert(alignof(kos_counter_t) == 4, "kos_counter_t must stay 4-aligned");
static_assert(sizeof(struct kos_uart_req) == 12, "kos_uart_req must stay 12 bytes (wire ABI)");
static_assert(sizeof(struct kos_uart_rsp) == 8, "kos_uart_rsp must stay 8 bytes (wire ABI)");
static_assert(sizeof(struct kos_uart_stats) == 36, "kos_uart_stats must stay 36 bytes (wire ABI)");
#else
_Static_assert(sizeof(kos_counter_t) == 4, "kos_counter_t must stay one uint32_t wide");
_Static_assert(_Alignof(kos_counter_t) == 4, "kos_counter_t must stay 4-aligned");
_Static_assert(sizeof(struct kos_uart_req) == 12, "kos_uart_req must stay 12 bytes (wire ABI)");
_Static_assert(sizeof(struct kos_uart_rsp) == 8, "kos_uart_rsp must stay 8 bytes (wire ABI)");
_Static_assert(sizeof(struct kos_uart_stats) == 36, "kos_uart_stats must stay 36 bytes (wire ABI)");
#endif

#ifdef __cplusplus
}
#endif

#endif
