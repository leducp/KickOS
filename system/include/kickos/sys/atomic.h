// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A single-word cross-thread field whose memory ordering is part of its TYPE.
// No read-modify-write surface: no fetch_add, no operator++, no operator+=, no
// compare_exchange.

#ifndef KICKOS_SYS_ATOMIC_H
#define KICKOS_SYS_ATOMIC_H

#include <stdint.h>

#include <atomic>

#define KICKOS_ATOMIC_INLINE inline __attribute__((always_inline))

namespace kickos
{

// A bitmask, not a choice of one: ACQUIRE and RELEASE order OPPOSITE ACCESSES of the same
// field, so a field one thread publishes and another consumes names both. No seq_cst, and no
// per-call-site override.
enum class Order : uint8_t
{
    RELAXED = 0u,
    ACQUIRE = 1u, // the load
    RELEASE = 2u  // the store
};

constexpr Order operator|(Order a, Order b)
{
    return static_cast<Order>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr bool order_has(Order order, Order bit)
{
    return (static_cast<uint8_t>(order) & static_cast<uint8_t>(bit)) != 0u;
}

constexpr std::memory_order std_load_order(Order order)
{
    std::memory_order mo = std::memory_order_acquire;
    if (not order_has(order, Order::ACQUIRE))
    {
        mo = std::memory_order_relaxed;
    }
    return mo;
}

constexpr std::memory_order std_store_order(Order order)
{
    std::memory_order mo = std::memory_order_release;
    if (not order_has(order, Order::RELEASE))
    {
        mo = std::memory_order_relaxed;
    }
    return mo;
}

template <typename T, Order ORDER>
class Atomic
{
public:
    Atomic() = default;
    constexpr Atomic(T v) : v_{v} {}

    // always_inline is load-bearing: at -Os GCC otherwise emits an out-of-line copy and
    // every access becomes a call.
    KICKOS_ATOMIC_INLINE operator T() const { return v_.load(LOAD_MO); }

    KICKOS_ATOMIC_INLINE Atomic& operator=(T v)
    {
        v_.store(v, STORE_MO);
        return *this;
    }

    KICKOS_ATOMIC_INLINE T load() const { return v_.load(LOAD_MO); }
    KICKOS_ATOMIC_INLINE void store(T v) { v_.store(v, STORE_MO); }

private:
    static_assert(sizeof(T) <= 4, "wider than a word: keep the field volatile, not atomic");
    // Descriptors reach these fields by byte offset (KOS_UART_READY_OFFSET).
    static_assert(sizeof(std::atomic<T>) == sizeof(T)
                      and alignof(std::atomic<T>) == alignof(T),
                  "std::atomic must be a drop-in for the plain field");

    static_assert(static_cast<uint8_t>(ORDER)
                      <= (static_cast<uint8_t>(Order::ACQUIRE) | static_cast<uint8_t>(Order::RELEASE)),
                  "Order carries a bit that names no ordering");

    static constexpr std::memory_order LOAD_MO = std_load_order(ORDER);
    static constexpr std::memory_order STORE_MO = std_store_order(ORDER);

    std::atomic<T> v_;
};

}

#endif
