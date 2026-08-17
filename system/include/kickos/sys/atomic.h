// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A single-word cross-thread field whose memory ordering is part of its TYPE.
//
// There is NO read-modify-write surface: no fetch_add, no operator++, no operator+=, no
// compare_exchange. A counter with a single writer is a load, an add and a store.

#ifndef KICKOS_SYS_ATOMIC_H
#define KICKOS_SYS_ATOMIC_H

#include <atomic>

#define KICKOS_ATOMIC_INLINE inline __attribute__((always_inline))

namespace kickos
{

// No default: every declaration names its ordering.
enum class Order
{
    RELAXED
};

constexpr std::memory_order std_order(Order order)
{
    std::memory_order mo = std::memory_order_seq_cst; // an unmapped ordering must not weaken
    switch (order)
    {
        case Order::RELAXED:
        {
            mo = std::memory_order_relaxed;
            break;
        }
    }
    return mo;
}

template <typename T, Order ORDER>
class Atomic
{
public:
    Atomic() = default;
    constexpr Atomic(T v) : v_{v} {}

    // always_inline is load-bearing, not a hint: at -Os GCC emits an out-of-line copy and
    // every access becomes a call.
    KICKOS_ATOMIC_INLINE operator T() const { return v_.load(MO); }

    KICKOS_ATOMIC_INLINE Atomic& operator=(T v)
    {
        v_.store(v, MO);
        return *this;
    }

    KICKOS_ATOMIC_INLINE T load() const { return v_.load(MO); }
    KICKOS_ATOMIC_INLINE void store(T v) { v_.store(v, MO); }

private:
    static_assert(sizeof(T) <= 4, "wider than a word: keep the field volatile, not atomic");
    // Descriptors reach these fields by byte offset (KOS_UART_READY_OFFSET), so the wrapper
    // must not grow or realign what it wraps.
    static_assert(sizeof(std::atomic<T>) == sizeof(T)
                      and alignof(std::atomic<T>) == alignof(T),
                  "std::atomic must be a drop-in for the plain field");

    static constexpr std::memory_order MO = std_order(ORDER);

    std::atomic<T> v_;
};

}

#endif
