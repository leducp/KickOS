// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ring side of a published console. The contract for each function is stated at its
// declaration in <kickos/sys/console_ring.h>.

#include <kickos/sys/console_ring.h>

#include <kickos/sys/bytes.h> // mem_copy

namespace kickos::console
{

int32_t mode_apply(Atomic<uint32_t, Order::RELAXED>* mode, uint32_t flags, uint32_t required)
{
    if (mode == nullptr)
    {
        return -KOS_ENOSYS;
    }
    if ((flags & ~static_cast<uint32_t>(KOS_CONSOLE_MODE_MASK)) != 0u)
    {
        return -KOS_EINVAL;
    }
    if ((flags & required) != required)
    {
        return -KOS_ENOTSUP;
    }
    *mode = flags;
    return 0;
}

// The word array is the wire image: same order, same width.
constexpr uint32_t KOS_UART_STATS_WORDS = 9;
static_assert(KOS_UART_STATS_WORDS * sizeof(uint32_t) == sizeof(struct kos_uart_stats),
              "kos_uart_stats gained or lost a field; pack and unpack must follow");

void stats_pack(uint8_t* wire, struct kos_uart_stats const* live, uint32_t tx_lost)
{
    uint32_t f[KOS_UART_STATS_WORDS];
    f[0] = live->tx_bytes.load(std::memory_order_relaxed);
    f[1] = live->rx_bytes.load(std::memory_order_relaxed);
    f[2] = live->tx_dropped.load(std::memory_order_relaxed) + tx_lost;
    f[3] = live->rx_dropped.load(std::memory_order_relaxed);
    f[4] = live->rx_overrun.load(std::memory_order_relaxed);
    f[5] = live->rx_framing.load(std::memory_order_relaxed);
    f[6] = live->rx_parity.load(std::memory_order_relaxed);
    f[7] = live->irq_wakes.load(std::memory_order_relaxed);
    f[8] = live->irq_spurious.load(std::memory_order_relaxed);
    mem_copy(wire, f, sizeof(f));
}

void stats_unpack(struct kos_uart_stats* dst, uint8_t const* wire)
{
    uint32_t f[KOS_UART_STATS_WORDS];
    mem_copy(f, wire, sizeof(f));
    dst->tx_bytes.store(f[0], std::memory_order_relaxed);
    dst->rx_bytes.store(f[1], std::memory_order_relaxed);
    dst->tx_dropped.store(f[2], std::memory_order_relaxed);
    dst->rx_dropped.store(f[3], std::memory_order_relaxed);
    dst->rx_overrun.store(f[4], std::memory_order_relaxed);
    dst->rx_framing.store(f[5], std::memory_order_relaxed);
    dst->rx_parity.store(f[6], std::memory_order_relaxed);
    dst->irq_wakes.store(f[7], std::memory_order_relaxed);
    dst->irq_spurious.store(f[8], std::memory_order_relaxed);
}

uint32_t tx_write(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                         uint8_t const* p, uint32_t n)
{
    uint32_t const took = kos_byte_ring_push(tx, p, n);
    kos_counter_increment(&stats->tx_bytes, took);
    (void)kos_irq_notify(KOS_CONSOLE_CAP_DOORBELL);
    return took;
}

uint32_t flush(struct kos_byte_ring* tx, Atomic<uint32_t, Order::RELAXED> const* inflight)
{
    for (uint32_t i = 0; i < KOS_CONSOLE_FLUSH_MAX; i++)
    {
        uint32_t left = kos_byte_ring_used(tx);
        if (inflight != nullptr)
        {
            left += *inflight;
        }
        if (left == 0u)
        {
            return 0;
        }
        (void)kos_irq_notify(KOS_CONSOLE_CAP_DOORBELL);
        kos_sleep_ns(KOS_CONSOLE_FLUSH_SLEEP_NS);
    }
    uint32_t left = kos_byte_ring_used(tx);
    if (inflight != nullptr)
    {
        left += *inflight;
    }
    return left;
}

uint32_t push_all(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                         uint8_t const* p, uint32_t n)
{
    uint32_t off = tx_write(tx, stats, p, n);
    while (off < n)
    {
        kos_sleep_ns(KOS_CONSOLE_FLUSH_SLEEP_NS);
        off += tx_write(tx, stats, p + off, n - off);
    }
    return off;
}

uint32_t push_some(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                          uint8_t const* p, uint32_t n, uint32_t mode)
{
    if ((mode & static_cast<uint32_t>(KOS_UART_F_NONBLOCK)) != 0u)
    {
        return tx_write(tx, stats, p, n);
    }
    return push_all(tx, stats, p, n);
}

uint32_t cook_crlf(uint8_t const* in, uint32_t in_n, uint8_t* out,
                          uint32_t out_cap, uint32_t* taken)
{
    uint32_t c = 0;
    uint32_t t = 0;
    while (t < in_n)
    {
        uint8_t const b = in[t];
        uint32_t need = 1;
        if (b == '\n')
        {
            need = 2;
        }
        if (c + need > out_cap)
        {
            break;
        }
        if (b == '\n')
        {
            out[c] = '\r';
            c++;
        }
        out[c] = b;
        c++;
        t++;
    }
    *taken = t;
    return c;
}

uint32_t write_console(struct kos_byte_ring* tx, struct kos_uart_stats* stats,
                             uint8_t const* p, uint32_t n, uint32_t mode)
{
#if KICKOS_CONSOLE_CRLF
    // n == 0 must reach the pump in BOTH postures: the doorbell count must not depend on
    // the CRLF posture.
    if (n == 0u)
    {
        return push_some(tx, stats, p, 0u, mode);
    }
    uint8_t cooked[KOS_CONSOLE_COOK_CHUNK];
    uint32_t in_done = 0;
    while (in_done < n)
    {
        uint32_t take = 0;
        uint32_t const c =
            cook_crlf(p + in_done, n - in_done, cooked, KOS_CONSOLE_COOK_CHUNK, &take);
        if (push_some(tx, stats, cooked, c, mode) < c)
        {
            return in_done;
        }
        in_done += take;
    }
    return n;
#else
    return push_some(tx, stats, p, n, mode);
#endif
}

}
