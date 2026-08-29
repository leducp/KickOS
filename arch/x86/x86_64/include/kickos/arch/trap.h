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
