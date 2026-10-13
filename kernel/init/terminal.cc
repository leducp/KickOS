// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ampwindow.h>
#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/atomic.h>

namespace kickos
{
    constinit Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_terminal_outcome{0u};

    namespace
    {
        constexpr uint32_t terminal_word(uint32_t outcome)
        {
            uint32_t const v = outcome + 1u;
            return v | ((~v & 0xFFFFu) << 16);
        }
    }

    bool reset_owned_here(void)
    {
#if KICKOS_AMP_NODE
        return amp::self() == KICKOS_AMP_RESET_OWNER;
#else
        return true;
#endif
    }

    int terminal_outcome_set(Thread* caller, uint32_t outcome)
    {
        if (not cap_check_authority(caller, AUTH_SYSTEM | AUTH_BOOTLOADER))
        {
            return -KOS_EPERM;
        }
        if (outcome > KOS_SHUTDOWN_BOOTLOADER)
        {
            return -KOS_EINVAL;
        }
        if (outcome != KOS_SHUTDOWN_HALT and not reset_owned_here())
        {
            return -KOS_EPERM;
        }
        IrqLock lock;
        if (g_terminal_outcome.load() != 0u)
        {
            return -KOS_EALREADY;
        }
        g_terminal_outcome.store(terminal_word(outcome));
        return 0;
    }
}

extern "C" void kickos_terminal_outcome_apply(void)
{
    uint32_t const word = ::kickos::g_terminal_outcome.load();
    uint32_t const v = word & 0xFFFFu;
    if (word != ::kickos::terminal_word(v - 1u) or v <= KOS_SHUTDOWN_HALT + 1u
        or not ::kickos::reset_owned_here())
    {
        return;
    }
    (void)arch_reboot(v - 1u);
}
