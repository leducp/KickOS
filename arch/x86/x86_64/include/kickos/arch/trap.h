// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The stubs push a zero error code for the vectors the hardware does not push one for, so one
// frame layout covers every vector.

#ifndef KICKOS_ARCH_TRAP_H
#define KICKOS_ARCH_TRAP_H

#include <stdint.h>

namespace kickos::x86_64
{
    // Field order IS the push order in trap_x86_64.S, lowest address first.
    struct trap_frame
    {
        uint64_t r15;
        uint64_t r14;
        uint64_t r13;
        uint64_t r12;
        uint64_t r11;
        uint64_t r10;
        uint64_t r9;
        uint64_t r8;
        uint64_t rbp;
        uint64_t rdi;
        uint64_t rsi;
        uint64_t rdx;
        uint64_t rcx;
        uint64_t rbx;
        uint64_t rax;
        uint64_t vector;
        uint64_t error;
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
    };

    // The vectors below 32 that the instruction ring 3 ran does not raise: NMI (2), double fault
    // (8, the kernel failing to deliver another), machine check (18), and the hypervisor
    // injection, VMM communication and security exceptions (28, 29, 30), which this kernel does
    // not serve (Intel SDM Vol 3 Table 6-1; AMD APM Vol 2 section 8.2).
    constexpr uint32_t trap_platform_vectors =
        (1u << 2) | (1u << 8) | (1u << 18) | (1u << 28) | (1u << 29) | (1u << 30);

    // Whether a frame on the running thread's own kernel block is a fault its ring 3 code took.
    // A kernel fault during a syscall dispatch builds its frame on that same block, and a vector
    // at or above 32 or among trap_platform_vectors names whoever was running, not what it did:
    // those reach the panic report.
    inline bool trap_frame_from_ring3(trap_frame const& f)
    {
        if (f.vector >= 32 or ((trap_platform_vectors >> f.vector) & 1u) != 0)
        {
            return false;
        }
        return (f.cs & 3u) == 3;
    }
}

// trap_x86_64.S: 256 stubs and a table of their offsets from the base. Index through the
// offsets; a stub may grow past the 16-byte pitch.
extern "C" char kickos_x86_64_vector_base[];
extern "C" uint32_t const kickos_x86_64_vector_offsets[256];

// The frame to resume from, which is `frame` itself unless a switch was taken at this exit.
extern "C" kickos::x86_64::trap_frame* kickos_x86_64_trap(kickos::x86_64::trap_frame* frame);

// Service the vector if this backend owns it, and return the frame to resume from; nullptr
// says the vector is not one of its own and the fault report is owed.
extern "C" kickos::x86_64::trap_frame* kickos_x86_64_isr(kickos::x86_64::trap_frame* frame);

#endif
