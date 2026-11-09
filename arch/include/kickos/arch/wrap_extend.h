// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A 32-bit free-running counter extended to 64 bits by its own overflow interrupt.
//
// `Ops` is the chip's counter, as three static functions:
//
//     uint32_t count();   // the counter
//     bool pending();     // the overflow is latched and its interrupt has not taken it yet;
//                         // a read only, never a clear
//     bool take();        // clears the latch and answers whether it was set
//
// Only on_overflow writes the high word. read() writes nothing and does not mask, so any
// context reads it, at any priority. The flag must already read set when the counter first
// reads its post-wrap value.
//
// Limit: the overflow interrupt must run within half a wrap period of its wrap. Later than
// that, a reader that sees it pending takes the counter for a reading from before the wrap
// and answers one wrap behind.

#ifndef KICKOS_ARCH_WRAP_EXTEND_H
#define KICKOS_ARCH_WRAP_EXTEND_H

#include <kickos/arch/arch.h>
#include <kickos/sys/atomic.h>

#include <stdint.h>

namespace kickos
{
    template <typename Ops>
    class arch_wrap_extend
    {
    public:
        inline __attribute__((always_inline)) uint64_t read() const
        {
            while (true)
            {
                uint32_t high = high_.load();
                // Relaxed loads are not ordered against the device reads without it.
                __asm volatile("" ::: "memory");
                uint32_t const low = Ops::count();
                bool const pending = Ops::pending();
                __asm volatile("" ::: "memory");
                if (high_.load() != high)
                {
                    continue;
                }
                if (pending and (low >> 31) == 0u)
                {
                    high = high + 1u;
                }
                return (static_cast<uint64_t>(high) << 32) | low;
            }
        }

        // The overflow interrupt's body. A level line can re-enter once after its latch
        // clears, so the word moves only on a latch take() saw set.
        void on_overflow()
        {
            arch_irq_state_t const s = arch_irq_save();
            if (Ops::take())
            {
                high_.store(high_.load() + 1u);
            }
            arch_irq_restore(s);
        }

    private:
        Atomic<uint32_t, Order::RELAXED> high_{0u};
    };
}

#endif
