// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32's half of the LX6 cross-core doorbell: which trigger raises it, which one this
// core clears, and where this core's matrix bank points them. The cells, the service body and
// the kernel lock live in arch/xtensa/lx6/klock_lx6.cc.
//
// THE PARTITION IS TARGET-KEYED: trigger n wakes core n, so the register name's "FROM" reads
// as "for". Two writers per trigger is sound because both only ever SET it.
//
// TRIGGERS 2 AND 3 ARE RESERVED. They sink in both banks so no source reaches either core, and
// they are the room a higher-priority second doorbell class would need.
//
// NO DPORT READ APPEARS ON THIS PATH. The part's known DPORT/APB concurrency hazard is a
// read-side one and no errata for it is on hand, so the raise and the clear are writes and the
// map registers are written once at bring-up.

#include <kickos/arch/lx6_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/arch/arch.h>

#include <stdint.h>

#include <kickos/chip_mmap.h>
#include "irq.h"
#include "regs/dport.h"

namespace reg = kickos::esp32::reg;
namespace irq = kickos::esp32::irq;

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Orders this core's prior stores ahead of the trigger write, and the trigger write ahead
    // of the caller's next load. MEMW performs every earlier load and store before any later
    // one (ISA summary 8.3.164, p.490); the ISA carves out no peripheral space from it.
    inline void memory_wait(void)
    {
        __asm volatile("memw" ::: "memory");
    }
}

extern "C"
{

void kickos_lx6_doorbell_send(uint32_t cores)
{
    // BEFORE ANY TRIGGER WRITE: a receiver woken by the trigger must not be able to read the
    // caller's request cell before that store lands.
    memory_wait();
    for (uint32_t to = 0; to < KICKOS_NUM_CORES; to++)
    {
        if ((cores & (1u << to)) != 0)
        {
            r32(reg::dport::cpu_intr_from_cpu(to)) = reg::dport::CPU_INTR_FROM_CPU_TRIGGER;
        }
    }
}

void kickos_lx6_doorbell_clear(void)
{
    // Bit 0 is the register's only field, so nothing is preserved and no read is owed. The
    // input is LEVEL: this write IS the acknowledgement and no INTCLEAR follows it.
    r32(reg::dport::cpu_intr_from_cpu(arch_cpu_id())) = 0u;
    // Orders the clear ahead of the cell loads the service makes; without it a lost wake
    // becomes a lost request.
    memory_wait();
}

void kickos_lx6_doorbell_route(void)
{
    uint32_t const me = arch_cpu_id();
    for (uint32_t n = 0; n < 4u; n++)
    {
        uint32_t const source = reg::dport::CPU_INTR_FROM_CPU_SOURCE_0 + n;
        uint32_t target = reg::dport::INTR_MAP_SINK;
        if (n == me)
        {
            target = irq::cpu_int::DOORBELL_CPU_INT;
        }
        // Each core writes its OWN bank, so the two never write one register and this needs
        // no exclusion of its own.
        if (me == 0u)
        {
            r32(reg::dport::pro_intr_map(source)) = target;
        }
        else
        {
            r32(reg::dport::app_intr_map(source)) = target;
        }
    }
}

uint32_t kickos_lx6_doorbell_cpu_int(void)
{
    return irq::cpu_int::DOORBELL_CPU_INT;
}

}

#endif
