// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "tx_seam.h"

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irq.h>

namespace
{
    int g_mask_depth = 0;
    uint32_t g_cur_masked = 0;
    uint32_t g_max_masked = 0;
    uint32_t g_gaps = 0;
    int g_slot_free = 1;
    bool g_isr_runs = true;
    bool g_tx_irq_on = false;
    bool g_in_gap = false;
    void (*g_first_gap_fn)(void) = nullptr;
    std::string g_wire;
    char g_storage[4096];

    int mock_slot_free(void)
    {
        return g_slot_free;
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
            return; // a producer seated by run_in_first_gap must not recurse into the drain
        }
        g_in_gap = true;
        g_gaps++;
        void (*seated)(void) = g_first_gap_fn;
        g_first_gap_fn = nullptr;
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

namespace consoletxfix
{
    std::string const& wire()
    {
        return g_wire;
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
        g_slot_free = 1;
        g_isr_runs = true;
        g_tx_irq_on = false;
        g_in_gap = false;
        g_first_gap_fn = nullptr;
        g_wire.clear();
        console_tx_init(&g_backend, g_storage, ring_size, 3);
    }

    void set_slot_free(int free)
    {
        g_slot_free = free;
    }

    void set_isr_runs_in_gap(bool runs)
    {
        g_isr_runs = runs;
    }

    bool tx_irq_enabled()
    {
        return g_tx_irq_on;
    }

    void run_in_first_gap(void (*fn)(void))
    {
        g_first_gap_fn = fn;
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

    // The disarm fallback pushes here, unmasked, so these bytes land on the wire without
    // counting against the masked-push metric.
    void arch_console_write_sync(char const* buf, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            mock_push(static_cast<uint8_t>(buf[i]));
        }
    }

    // Both ownership reads are pinned kernel-owned: this seam measures the RING, and the
    // publish sequence that moves them is gated in tests/unit/consoleown/publish_handoff.cc
    // against the real console.cc.
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

    void arch_irq_unmask(int)
    {
    }

    void arch_irq_clear_pending(int)
    {
    }

    console_tx_backend const* arch_console_tx_backend(char**, uint32_t*, int*)
    {
        return nullptr;
    }
}

namespace kickos
{
    void kpanic(char const*) __attribute__((noreturn));
    void kpanic(char const*)
    {
        __builtin_trap();
    }

    // console_buffer_init is never called here (the gate arms the ring through
    // console_tx_init), but the deinit cases do reach irq_detach; the drain ISR it nulls is
    // modelled by set_isr_runs_in_gap, not by this stub.
    bool irq_attach(int, IrqHandler, void*)
    {
        return true;
    }

    void irq_detach(int)
    {
    }
}
