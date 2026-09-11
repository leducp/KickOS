// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "line_seam.h"

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irq.h>
#include <kickos/irq_route.h>
#include <kickos/irqlock.h>

namespace
{
    int g_mask_depth = 0;
    int g_slot_free = 1;
    bool g_tx_irq_on = false;
    bool g_tx_event = false;
    uint32_t g_pushes = 0;
    uint32_t g_push_depth = 0;
    uint32_t g_max_push_depth = 0;
    uint32_t g_nested_pushes = 0;
    uint32_t g_isr_entries = 0;
    uint32_t g_push_seat_ordinal = 0;
    void (*g_push_seat_fn)(void) = nullptr;
    bool g_push_seat_fired = false;
    void (*g_barrier_seat_fn)(void) = nullptr;
    bool g_barrier_seat_fired = false;
    std::string g_wire;
    char g_storage[4096];

    int mock_slot_free(void)
    {
        return g_slot_free;
    }

    void mock_push(uint8_t b)
    {
        g_pushes++;
        g_push_depth++;
        if (g_push_depth > g_max_push_depth)
        {
            g_max_push_depth = g_push_depth;
        }
        if (g_push_depth > 1u)
        {
            g_nested_pushes++;
        }
        g_wire.push_back(static_cast<char>(b));
        g_tx_event = true;
        if (g_push_seat_fn != nullptr and g_pushes == g_push_seat_ordinal)
        {
            void (*fn)(void) = g_push_seat_fn;
            g_push_seat_fn = nullptr;
            g_push_seat_fired = true;
            fn();
        }
        g_push_depth--;
    }

    void mock_irq_enable(void)
    {
        g_tx_irq_on = true;
    }

    void mock_irq_disable(void)
    {
        g_tx_irq_on = false;
    }

    console_tx_backend const g_backend = {mock_slot_free, mock_push, mock_irq_enable,
                                          mock_irq_disable};

    // The bounded synchronous line writer a chip takes for a line the ring refused: one mask
    // across the device writes, with '\n' lowered in the writer rather than in a cooked
    // caller buffer.
    void write_line_sync(char const* buf, size_t n, int crlf)
    {
        kickos::IrqLock lock;
        if (crlf == 0)
        {
            arch_console_write_sync(buf, n);
            return;
        }
        static char const CRLF[2] = {'\r', '\n'};
        size_t start = 0;
        for (size_t i = 0; i < n; i++)
        {
            if (buf[i] != '\n')
            {
                continue;
            }
            if (i > start)
            {
                arch_console_write_sync(buf + start, i - start);
            }
            arch_console_write_sync(CRLF, sizeof(CRLF));
            start = i + 1;
        }
        if (n > start)
        {
            arch_console_write_sync(buf + start, n - start);
        }
    }
}

namespace consoleline
{
    void reset(uint32_t ring_size, int irq_line)
    {
        g_mask_depth = 0;
        g_slot_free = 1;
        g_tx_irq_on = false;
        g_tx_event = false;
        g_pushes = 0;
        g_push_depth = 0;
        g_max_push_depth = 0;
        g_nested_pushes = 0;
        g_isr_entries = 0;
        g_push_seat_ordinal = 0;
        g_push_seat_fn = nullptr;
        g_push_seat_fired = false;
        g_barrier_seat_fn = nullptr;
        g_barrier_seat_fired = false;
        g_wire.clear();
        for (size_t i = 0; i < sizeof(g_storage); i++)
        {
            g_storage[i] = '.';
        }
        console_tx_init(&g_backend, g_storage, ring_size, irq_line);
    }

    std::string const& wire()
    {
        return g_wire;
    }

    void set_slot_free(int free)
    {
        g_slot_free = free;
    }

    void pump_tx_isr()
    {
        while (g_tx_irq_on and g_tx_event)
        {
            g_tx_event = false;
            g_isr_entries++;
            console_tx_isr();
        }
    }

    uint32_t isr_entries()
    {
        return g_isr_entries;
    }

    void run_in_push(uint32_t ordinal, void (*fn)(void))
    {
        g_push_seat_ordinal = ordinal;
        g_push_seat_fn = fn;
        g_push_seat_fired = false;
    }

    bool push_seat_fired()
    {
        return g_push_seat_fired;
    }

    uint32_t nested_pushes()
    {
        return g_nested_pushes;
    }

    uint32_t max_push_depth()
    {
        return g_max_push_depth;
    }

    void run_at_publish_barrier(void (*fn)(void))
    {
        g_barrier_seat_fn = fn;
        g_barrier_seat_fired = false;
    }

    bool barrier_seat_fired()
    {
        return g_barrier_seat_fired;
    }

    // Every chip's arch_console_write, which is one call with no branch: a line the ring
    // cannot take does not go out. A fallback here would put a second writer at the device
    // and split a line, which is the defect the drop rule removes.
    int write_line(char const* buf, size_t n, int crlf)
    {
        return console_tx_insert_line(buf, n, crlf);
    }
}

extern "C"
{

// Disarmed before the seated call so a line inserted from inside the copy reaches the barrier
// without seating itself again.
void consoleline_publish_barrier(void)
{
    __asm volatile("" ::: "memory");
    if (g_barrier_seat_fn == nullptr)
    {
        return;
    }
    void (*fn)(void) = g_barrier_seat_fn;
    g_barrier_seat_fn = nullptr;
    g_barrier_seat_fired = true;
    fn();
}

arch_irq_state_t arch_irq_save(void)
{
    arch_irq_state_t const prior = static_cast<arch_irq_state_t>(g_mask_depth);
    g_mask_depth++;
    return prior;
}

void arch_irq_restore(arch_irq_state_t state)
{
    g_mask_depth = static_cast<int>(state);
}

int arch_in_isr(void)
{
    return 0;
}

// Reaches the same mock edge as the ring's own pushes, so a fallback byte and a drained byte
// sit on one wire in the order they were written.
void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        mock_push(static_cast<uint8_t>(buf[i]));
    }
}

void arch_irq_mask(int)
{
}

void arch_irq_unmask(int)
{
}

void arch_irq_clear_pending(int)
{
}

console_tx_backend const* arch_console_tx_backend(char**, uint32_t*, int*)
{
    return nullptr; // the fixture arms the ring through console_tx_init
}

// Both ownership reads are pinned kernel-owned: these arms measure the RING, and the publish
// sequence is gated in tests/unit/consoleown.
int console_owner_is_kernel(void)
{
    return 1;
}

int console_chip_writable(void)
{
    return 1;
}

void console_chip_writer_enter(void)
{
}

void console_chip_writer_leave(void)
{
}

// console.cc's, and the insert reaches it for the UNARMED ring alone: before console_tx_init
// there is no ring to interleave with, so that line goes straight at the device. The arms
// that exercise it read the wire, so it lands there like every other push.
void console_write_line_sync(char const* buf, size_t n)
{
    write_line_sync(buf, n, KICKOS_CONSOLE_CRLF);
}
}

namespace kickos
{
    void kpanic(char const*) __attribute__((noreturn));
    void kpanic(char const*)
    {
        __builtin_trap();
    }

    bool irq_attach(int, IrqHandler, void*)
    {
        return true;
    }

    void irq_detach(int)
    {
    }

    void irq_line_op(int line, LineOp op)
    {
        switch (op)
        {
            case LineOp::MASK:
            {
                arch_irq_mask(line);
                break;
            }
            case LineOp::UNMASK:
            {
                arch_irq_unmask(line);
                break;
            }
            case LineOp::CLEAR:
            {
                arch_irq_clear_pending(line);
                break;
            }
        }
    }

    void irq_line_op_local(int line, LineOp op)
    {
        irq_line_op(line, op);
    }
}
