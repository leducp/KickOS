// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The APIC timer is the only interrupt source this port arms.

#ifndef KICKOS_ARCH_APIC_H
#define KICKOS_ARCH_APIC_H

#include <stdint.h>

namespace kickos::x86_64
{
    // Above the 32 architecturally defined exception vectors.
    constexpr unsigned vector_timer = 0x30;
    constexpr unsigned vector_doorbell = 0x31;
    constexpr unsigned vector_spurious = 0xf0;

    void apic_init(void);

    // A fixed-delivery interrupt to this core alone, carrying the software interrupt
    // controller's injected lines, taken as soon as the interrupt flag is set.
    void apic_doorbell(void);

    // End of interrupt. NOT owed for the spurious vector (Intel SDM Vol 3).
    void apic_eoi(void);

    // Zero before apic_init.
    uint64_t apic_timer_hz(void);
    uint64_t apic_tsc_hz(void);

    bool apic_is_x2(void);

    // The register window the xAPIC path reaches; zero in x2APIC mode.
    uintptr_t apic_mmio_base(void);

    uint64_t tsc_now(void);

    uint64_t clock_now(void);
    void timer_arm(uint64_t deadline_ns);
    void timer_disarm(void);

    // The one-shot has expired, so the next arm must reprogram whatever deadline it holds.
    void timer_expired(void);
}

// The chip's reference timebase, which apic_init measures against. kickos_x86_ref_spin blocks
// for about `want` of its ticks and returns how many it observed, 0 for a reference that did
// not move.
extern "C" uint32_t kickos_x86_ref_hz(void);
extern "C" uint32_t kickos_x86_ref_spin(uint32_t want);

#endif
