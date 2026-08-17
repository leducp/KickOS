// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Single-producer / single-consumer byte ring for USERSPACE drivers: two instances (TX and
// RX) in memory a driver shares with its own second thread. No lock and no atomic RMW, so a
// second writer of either index loses updates.

#ifndef KICKOS_SYS_BYTE_RING_H
#define KICKOS_SYS_BYTE_RING_H

#include <stdint.h>

#include <kickos/sys/atomic.h>

// Publication barrier between a payload store and the index update that exposes it.
// Compiler-only by default: on a weakly-ordered core -DKOS_RING_BARRIER=... must supply a
// real RELEASE fence. The -D reaches only the TUs it is compiled into, never the ring
// operations already built into libkickos_user.
#ifndef KOS_RING_BARRIER
#define KOS_RING_BARRIER() __asm volatile("" ::: "memory")
#endif

// `size` MUST be a power of two; usable capacity is size-1, because one slot is reserved
// so that head == tail means EMPTY unambiguously rather than either empty or full.
struct kos_byte_ring
{
    unsigned char* buf;
    uint32_t size;
    uint32_t mask;
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> head;
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> tail;
};

// A non-power-of-two size would make the mask wrap wrong and silently corrupt the ring, so
// it is refused: size stays 0 and every later call reports empty-and-full instead.
static inline void kos_byte_ring_init(struct kos_byte_ring* r, unsigned char* buf,
                                      uint32_t size)
{
    r->buf = buf;
    r->size = 0;
    r->mask = 0;
    r->head.store(0u);
    r->tail.store(0u);
    if (buf == nullptr or size < 2u or (size & (size - 1u)) != 0u)
    {
        return;
    }
    r->size = size;
    r->mask = size - 1u;
}

static inline uint32_t kos_byte_ring_used(struct kos_byte_ring const* r)
{
    // ONE read of each index: re-reading could see the other side move between reads and
    // yield a count that was never true.
    uint32_t const head = r->head.load();
    uint32_t const tail = r->tail.load();
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
    uint32_t idx = r->head.load();
    for (uint32_t i = 0; i < n; i++)
    {
        r->buf[idx] = src[i];
        idx = (idx + 1u) & r->mask;
    }
    KOS_RING_BARRIER(); // every payload byte is visible before the head that exposes it
    r->head.store(idx);
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
    uint32_t idx = r->tail.load();
    for (uint32_t i = 0; i < n; i++)
    {
        dst[i] = r->buf[idx];
        idx = (idx + 1u) & r->mask;
    }
    KOS_RING_BARRIER(); // the payload is consumed before the tail frees the slots
    r->tail.store(idx);
    return n;
}

// Consumer side, non-destructive: copies up to n bytes from the tail WITHOUT advancing it.
// Pair it with kos_byte_ring_drop once the sink has said how many it took; a pop into a
// device that then refuses them has nowhere to put them back.
static inline uint32_t kos_byte_ring_peek(struct kos_byte_ring const* r, unsigned char* dst,
                                          uint32_t n)
{
    uint32_t const used = kos_byte_ring_used(r);
    if (n > used)
    {
        n = used;
    }
    uint32_t idx = r->tail.load();
    for (uint32_t i = 0; i < n; i++)
    {
        dst[i] = r->buf[idx];
        idx = (idx + 1u) & r->mask;
    }
    return n;
}

// Consumer side: release n bytes a preceding peek copied out. Clamped to what the ring
// holds, so a caller that over-reports cannot drive the tail past the head.
static inline void kos_byte_ring_drop(struct kos_byte_ring* r, uint32_t n)
{
    uint32_t const used = kos_byte_ring_used(r);
    if (n > used)
    {
        n = used;
    }
    KOS_RING_BARRIER(); // the peeked payload is consumed before the tail frees the slots
    uint32_t const tail = r->tail.load();
    r->tail.store((tail + n) & r->mask);
}

// Consumer side, one byte. 1 on success, 0 on an empty ring.
static inline int kos_byte_ring_pop_one(struct kos_byte_ring* r, unsigned char* out)
{
    if (kos_byte_ring_used(r) == 0u)
    {
        return 0;
    }
    uint32_t const tail = r->tail.load();
    *out = r->buf[tail];
    KOS_RING_BARRIER();
    r->tail.store((tail + 1u) & r->mask);
    return 1;
}

#endif
