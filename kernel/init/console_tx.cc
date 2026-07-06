// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered, IRQ-drained console TX ring (see console_tx.h). Lives in the kernel
// rather than lib/ because the producer runs under IrqLock (arch-coupled); lib/
// stays a leaf. The producer (console_tx_write) runs the whole enqueue under
// IrqLock: that serializes multiple thread producers (concurrent kos_print) AND
// keeps it atomic against the single consumer (console_tx_isr). The routing guard
// in console.cc keeps the producer out of ISR context. Storage, the per-chip TX
// edge, and the TX IRQ line come from the chip via arch_console_tx_backend.

#include <kickos/console_tx.h>

#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>

namespace
{
    console_tx_backend const* g_be = nullptr;
    char* g_buf = nullptr;
    uint32_t g_size = 0; // power of two; usable capacity = g_size - 1
    uint32_t g_mask = 0;
    volatile uint32_t g_head = 0; // producer advances (bytes queued)
    volatile uint32_t g_tail = 0; // ISR advances (bytes drained)
    bool g_armed = false;

    // Indices stay in [0, g_size); power-of-two size makes (head - tail) & mask the
    // used count (unsigned wrap reduces mod g_size). One slot reserved so head==tail
    // is unambiguously empty.
    inline uint32_t used() { return (g_head - g_tail) & g_mask; }
    inline uint32_t space() { return g_size - 1u - used(); }

    // A dead/misconfigured TX channel must NEVER hang panic/fault/boot, so every
    // synchronous poll is bounded (matches the chips' own TX_POLL_TIMEOUT) and bails
    // rather than spinning forever. Cap dwarfs a real per-byte wait at any baud.
    constexpr uint32_t DRAIN_POLL_CAP = 1000000u;

    bool wait_slot()
    {
        for (uint32_t i = 0; i < DRAIN_POLL_CAP; i++)
        {
            if (g_be->slot_free() != 0)
            {
                return true;
            }
        }
        return false;
    }

    // Poll-push [tail, head) straight to the peripheral with the TX IRQ already
    // disabled. Shared by the overflow and panic-flush paths. Bounded per byte; on a
    // stuck channel it gives up and resets the ring (drops the undrained bytes)
    // rather than hang.
    void drain_sync()
    {
        uint32_t tail = g_tail;
        uint32_t const head = g_head;
        while (tail != head)
        {
            if (not wait_slot())
            {
                break; // stuck TX: give up rather than hang the caller
            }
            g_be->push(static_cast<uint8_t>(g_buf[tail]));
            tail = (tail + 1u) & g_mask;
        }
        g_tail = head;
    }

    void console_tx_isr_trampoline(void*) { console_tx_isr(); }
}

extern "C"
{

void console_tx_init(console_tx_backend const* be, char* storage, uint32_t size)
{
    g_be = be;
    g_buf = storage;
    g_size = size;
    g_mask = size - 1u;
    g_head = 0;
    g_tail = 0;
    g_armed = true;
}

int console_tx_armed(void) { return static_cast<int>(g_armed); }

void console_tx_write(char const* buf, size_t n)
{
    if (not g_armed)
    {
        arch_console_write_sync(buf, n);
        return;
    }

    // The whole enqueue runs under IrqLock: it serializes concurrent thread
    // producers and is atomic against the drain ISR. The fast-path copy is bounded
    // (<= ring) -- microseconds, not the transmission the buffering moves off-caller.
    kickos::IrqLock lock;

    // Fast path: the burst fits.
    if (n <= space())
    {
        bool const was_empty = (used() == 0);
        uint32_t idx = g_head;
        for (size_t i = 0; i < n; i++)
        {
            g_buf[idx] = buf[i];
            idx = (idx + 1u) & g_mask;
        }
        KICKOS_CONSOLE_TX_BARRIER();
        g_head = idx;
        g_be->irq_enable();
        // Prime the pump: an edge/event-triggered TX interrupt (XMC USIC TBIEN)
        // raises nothing when enabled on an idle channel, so push the first byte
        // directly to start the transfer -- its completion event then drives the
        // drain ISR. Only on the idle->busy transition; a harmless immediate send on
        // a level-triggered part (K64F TDRE).
        if (was_empty and g_head != g_tail and g_be->slot_free() != 0)
        {
            g_be->push(static_cast<uint8_t>(g_buf[g_tail]));
            g_tail = (g_tail + 1u) & g_mask;
        }
        return;
    }

    // Overflow (rare: sustained > line-rate output): drain the ring + send the burst
    // synchronously, TX IRQ off, bounded so a stuck channel cannot hang. Still under
    // the lock -- a full drain can mask IRQs for up to the drain time, an accepted
    // debug-console-flooding tradeoff in exchange for no producer/ISR race. Dropping
    // kernel debug output would be worse than the stall.
    g_be->irq_disable();
    drain_sync();
    for (size_t i = 0; i < n; i++)
    {
        if (not wait_slot())
        {
            return; // stuck TX: give up rather than hang
        }
        g_be->push(static_cast<uint8_t>(buf[i]));
    }
}

void console_tx_isr(void)
{
    uint32_t tail = g_tail;
    uint32_t const head = g_head; // producer cannot run during this ISR (priority)
    while (tail != head and g_be->slot_free() != 0)
    {
        g_be->push(static_cast<uint8_t>(g_buf[tail]));
        tail = (tail + 1u) & g_mask;
    }
    g_tail = tail;
    if (tail == head)
    {
        g_be->irq_disable();
    }
}

void console_tx_flush_sync(void)
{
    if (not g_armed)
    {
        return;
    }
    g_be->irq_disable();
    drain_sync();
}

// Weak default: chips without a buffered console leave the console synchronous.
__attribute__((weak)) console_tx_backend const* arch_console_tx_backend(char**, uint32_t*, int*)
{
    return nullptr;
}

// Called once from kmain after irq_init(). If the chip offers a backend, bind the
// drain ISR, unmask the line (priority lands in the IrqLock-maskable band), and
// arm the ring. No-op on sim / polled-only chips.
void console_buffer_init(void)
{
    char* buf = nullptr;
    uint32_t size = 0;
    int line = -1;
    console_tx_backend const* be = arch_console_tx_backend(&buf, &size, &line);
    if (be == nullptr or buf == nullptr or size == 0 or line < 0)
    {
        return;
    }
    kickos::irq_attach(line, console_tx_isr_trampoline, nullptr);
    arch_irq_unmask(line);
    console_tx_init(be, buf, size);
}

} // extern "C"
