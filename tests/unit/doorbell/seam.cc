// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "seam.h"

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_protocol.h>

namespace kickos::doorbellfix
{
    bool g_masked = false;
    std::string g_log;

    void reset(void)
    {
        g_masked = false;
        g_log.clear();
    }
}

namespace kickos::doorbell
{
    void host_note(char step)
    {
        doorbellfix::g_log.push_back(step);
    }
}

extern "C"
{
    arch_irq_state_t arch_irq_save(void)
    {
        bool const was = kickos::doorbellfix::g_masked;
        kickos::doorbellfix::g_masked = true;
        return static_cast<arch_irq_state_t>(was);
    }

    void arch_irq_restore(arch_irq_state_t state)
    {
        kickos::doorbellfix::g_masked = state != 0;
    }

    uint32_t arch_cpu_id(void)
    {
        return 1u;
    }
}
