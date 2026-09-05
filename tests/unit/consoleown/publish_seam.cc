// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "publish_seam.h"

#include <kickos/irq_route.h>
#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irq.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
    int g_mask_depth = 0;
    uint32_t g_cur_masked = 0;
    uint32_t g_max_masked = 0;
    uint32_t g_gaps = 0;
    uint32_t g_seat_gap = 0;
    uint32_t g_pushes_after_commit = 0;
    bool g_committed = false;
    bool g_isr_runs = true;
    bool g_tx_irq_on = false;
    bool g_in_gap = false;
    bool g_seat_fired = false;
    void (*g_seat_fn)(void) = nullptr;
    std::string g_wire;
    char g_storage[4096];

    int mock_slot_free(void)
    {
        return 1;
    }

    void mock_push(uint8_t b)
    {
        if (g_mask_depth > 0)
        {
            g_cur_masked++;
            if (g_cur_masked > g_max_masked)
            {
                g_max_masked = g_cur_masked;
            }
        }
        if (g_committed)
        {
            g_pushes_after_commit++;
        }
        g_wire.push_back(static_cast<char>(b));
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

    void on_gap(void)
    {
        if (g_in_gap)
        {
            return; // a function seated here masks again; it must not recurse into the drain
        }
        g_in_gap = true;
        g_gaps++;
        void (*seated)(void) = nullptr;
        if (g_seat_fn != nullptr and g_gaps == g_seat_gap)
        {
            seated = g_seat_fn;
            g_seat_fn = nullptr;
            g_seat_fired = true;
        }
        if (g_isr_runs and g_tx_irq_on)
        {
            console_tx_isr();
        }
        if (seated != nullptr)
        {
            seated();
        }
        g_in_gap = false;
    }
}

namespace consolepub
{
    std::string const& wire()
    {
        return g_wire;
    }

    uint32_t pushes_after_commit()
    {
        return g_pushes_after_commit;
    }

    uint32_t max_masked_pushes()
    {
        return g_max_masked;
    }

    uint32_t gap_count()
    {
        return g_gaps;
    }

    void reset(uint32_t ring_size)
    {
        g_mask_depth = 0;
        g_cur_masked = 0;
        g_max_masked = 0;
        g_gaps = 0;
        g_seat_gap = 0;
        g_pushes_after_commit = 0;
        g_committed = false;
        g_isr_runs = true;
        g_tx_irq_on = false;
        g_in_gap = false;
        g_seat_fired = false;
        g_seat_fn = nullptr;
        g_wire.clear();
        console_tx_init(&g_backend, g_storage, ring_size, 3);
    }

    void set_isr_runs_in_gap(bool runs)
    {
        g_isr_runs = runs;
    }

    void run_in_gap(uint32_t ordinal, void (*fn)(void))
    {
        g_seat_gap = ordinal;
        g_seat_fn = fn;
        g_seat_fired = false;
    }

    bool seat_fired()
    {
        return g_seat_fired;
    }

    void note_commit()
    {
        g_committed = true;
    }
}

extern "C"
{
    arch_irq_state_t arch_irq_save(void)
    {
        arch_irq_state_t const prior = static_cast<arch_irq_state_t>(g_mask_depth);
        g_mask_depth++;
        return prior;
    }

    void arch_irq_restore(arch_irq_state_t state)
    {
        g_mask_depth = static_cast<int>(state);
        if (g_mask_depth == 0)
        {
            g_cur_masked = 0;
            on_gap();
        }
    }

    int arch_in_isr(void)
    {
        return 0;
    }

    // Every buffered chip's arch_console_write is exactly this, so the routing decision in
    // console.cc reaches the real ring producer.
    void arch_console_write(char const* buf, size_t n)
    {
        console_tx_write(buf, n);
    }

    void arch_console_write_sync(char const* buf, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            mock_push(static_cast<uint8_t>(buf[i]));
        }
    }

    void arch_console_flush_sync(void)
    {
    }

    void arch_console_reclaim(void)
    {
    }

    void arch_console_reclaim_window(uintptr_t* base, size_t* size)
    {
        *base = 0x40000000u;
        *size = 0x100u;
    }

    // Reached only through the irq_line_op stub below; no arm here masks.
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

    int kvsnprintf(char* buf, size_t size, char const* fmt, va_list ap)
    {
        return vsnprintf(buf, size, fmt, ap);
    }

    // Distinct statuses: an arm that expects the panic path must be able to tell it from an
    // ordinary failed expectation.
    void arch_shutdown(int status)
    {
        printf("SEAM: arch_shutdown(%d)\n", status);
        exit(43);
    }

    int arch_reboot(void)
    {
        return 0;
    }

    void kfault_terminate(void)
    {
        printf("SEAM: kfault_terminate\n");
        exit(42);
    }
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t)
    {
        return true;
    }

    int32_t cap_console_deliver(char const*, size_t)
    {
        return 0;
    }

    namespace sched
    {
        Thread* current()
        {
            return nullptr;
        }
    }

    bool irq_attach(int, IrqHandler, void*)
    {
        return true;
    }

    void irq_detach(int)
    {
    }
}

namespace kickos
{
    // One core, so every line is local. Forwarded to the arch stubs in this file, which is
    // what keeps each arm's recorded trace unchanged.
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
