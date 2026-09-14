// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "console_seam.h"

#include <kickos/irq_route.h>
#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irq.h>

namespace
{
    int g_mask_depth = 0;
    uint32_t g_cur_masked = 0;
    uint32_t g_max_masked = 0;
    uint32_t g_gaps = 0;
    uint32_t g_seat_gap = 0;
    uint32_t g_pushes_after_commit = 0;
    int g_slot_free = 1;
    uint32_t g_gap_budget = 0;
    uint32_t g_gap_left = 0;
    bool g_in_isr = false;
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
        if (g_in_isr and g_gap_budget != 0 and g_gap_left == 0)
        {
            return 0;
        }
        return g_slot_free;
    }

    void mock_push(uint8_t b)
    {
        if (g_in_isr and g_gap_left != 0)
        {
            g_gap_left--;
        }
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

    // The mask just fell to zero: this is where a real TX-empty ISR would land.
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
            g_gap_left = g_gap_budget;
            g_in_isr = true;
            console_tx_isr();
            g_in_isr = false;
        }
        if (seated != nullptr)
        {
            seated();
        }
        g_in_gap = false;
    }
}

namespace consoleseam
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
        g_slot_free = 1;
        g_gap_budget = 0;
        g_gap_left = 0;
        g_in_isr = false;
        g_committed = false;
        g_isr_runs = true;
        g_tx_irq_on = false;
        g_in_gap = false;
        g_seat_fired = false;
        g_seat_fn = nullptr;
        g_wire.clear();
        console_tx_init(&g_backend, g_storage, ring_size, 3);
    }

    void set_slot_free(int free)
    {
        g_slot_free = free;
    }

    void set_gap_budget(uint32_t bytes)
    {
        g_gap_budget = bytes;
    }

    void set_isr_runs_in_gap(bool runs)
    {
        g_isr_runs = runs;
    }

    bool tx_irq_enabled()
    {
        return g_tx_irq_on;
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

    // Unmasked, so these bytes land on the wire without counting against the masked-push
    // metric.
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
}

namespace kickos
{
    // What irq_detach nulls is modelled by set_isr_runs_in_gap; these only have to link.
    bool irq_attach(int, IrqHandler, void*)
    {
        return true;
    }

    void irq_detach(int)
    {
    }

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
