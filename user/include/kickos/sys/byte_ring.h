// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Single-producer / single-consumer byte ring for USERSPACE drivers: two instances (TX and
// RX) in memory a driver shares with its own second thread. The kernel's own ring
// (lib/include/kickos/console_tx.h) is file-static and privileged-side.
//
// LOCK-FREE ONLY UNDER THE SPSC CONTRACT: `head` is written by the producer alone and
// `tail` by the consumer alone. A second writer of either index breaks it; there is no
// lock here and no atomic RMW. For the two-thread UART driver the ring direction fixes
// which thread owns which index (design-m4.6-irq-driver.md section 7.2), and it is why the
// service thread rings a doorbell instead of priming the peripheral itself.

#ifndef KICKOS_SYS_BYTE_RING_H
#define KICKOS_SYS_BYTE_RING_H

#include <stdint.h>

// Publication barrier between a payload store and the index update that exposes it.
// Compiler-only by default, which is correct on the in-order single-core M-class parts
// today. Producer and consumer are two THREADS that may be preempted between the store and
// the index update, so on a weakly-ordered core -DKOS_RING_BARRIER=... must supply a real
// RELEASE fence and not merely a compiler barrier. That is a STRONGER requirement than the
// kernel ring's KICKOS_CONSOLE_TX_BARRIER, which publishes its head under IrqLock.
#ifndef KOS_RING_BARRIER
#define KOS_RING_BARRIER() __asm volatile("" ::: "memory")
#endif

#ifdef __cplusplus
extern "C"
{
#endif

// `size` MUST be a power of two; usable capacity is size-1, because one slot is reserved
// so that head == tail means EMPTY unambiguously rather than either empty or full.
struct kos_byte_ring
{
    unsigned char* buf;
    uint32_t size;
    uint32_t mask;
    volatile uint32_t head; // producer only
    volatile uint32_t tail; // consumer only
};

// A non-power-of-two size would make the mask wrap wrong and silently corrupt the ring, so
// it is refused: size stays 0 and every later call reports empty-and-full rather than
// scribbling.
static inline void kos_byte_ring_init(struct kos_byte_ring* r, unsigned char* buf,
                                      uint32_t size)
{
    r->buf = buf;
    r->size = 0;
    r->mask = 0;
    r->head = 0;
    r->tail = 0;
    // Split rather than one condition: this header must stay valid in C, and the spelled
    // logical operators the house style requires are C++-only.
    if (buf == 0)
    {
        return;
    }
    if (size < 2u)
    {
        return;
    }
    if ((size & (size - 1u)) != 0u)
    {
        return;
    }
    r->size = size;
    r->mask = size - 1u;
}

static inline uint32_t kos_byte_ring_used(struct kos_byte_ring const* r)
{
    // ONE read of each index: re-reading could see the other side move between reads and
    // yield a count that was never true. Unsigned wrap makes the subtraction correct
    // across the uint32 rollover without a branch.
    uint32_t const head = r->head;
    uint32_t const tail = r->tail;
    return (head - tail) & r->mask;
}

static inline uint32_t kos_byte_ring_space(struct kos_byte_ring const* r)
{
    if (r->size == 0u)
    {
        return 0u;
    }
    return r->mask - kos_byte_ring_used(r);
}

// Producer side. Returns the bytes ACCEPTED, which may be less than n on a full ring; the
// policy for a short accept belongs to the caller.
static inline uint32_t kos_byte_ring_push(struct kos_byte_ring* r,
                                          unsigned char const* src, uint32_t n)
{
    uint32_t const space = kos_byte_ring_space(r);
    if (n > space)
    {
        n = space;
    }
    uint32_t idx = r->head;
    for (uint32_t i = 0; i < n; i++)
    {
        r->buf[idx] = src[i];
        idx = (idx + 1u) & r->mask;
    }
    KOS_RING_BARRIER(); // every payload byte is visible before the head that exposes it
    r->head = idx;
    return n;
}

// Consumer side. Returns the bytes REMOVED.
static inline uint32_t kos_byte_ring_pop(struct kos_byte_ring* r, unsigned char* dst,
                                         uint32_t n)
{
    uint32_t const used = kos_byte_ring_used(r);
    if (n > used)
    {
        n = used;
    }
    uint32_t idx = r->tail;
    for (uint32_t i = 0; i < n; i++)
    {
        dst[i] = r->buf[idx];
        idx = (idx + 1u) & r->mask;
    }
    KOS_RING_BARRIER(); // the payload is consumed before the tail frees the slots
    r->tail = idx;
    return n;
}

// Consumer side, one byte: the drain loop has to re-check the peripheral's slot_free
// between bytes, so it cannot pop a block. 1 on success, 0 on an empty ring.
static inline int kos_byte_ring_pop_one(struct kos_byte_ring* r, unsigned char* out)
{
    if (kos_byte_ring_used(r) == 0u)
    {
        return 0;
    }
    *out = r->buf[r->tail];
    KOS_RING_BARRIER();
    r->tail = (r->tail + 1u) & r->mask;
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif
