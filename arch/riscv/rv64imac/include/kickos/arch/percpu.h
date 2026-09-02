// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RV64 supervisor mode: the state the trap entry and the interrupt controller must reach
// without a TCB, one row per hart.
//
// THE ROW'S BLOCK STARTS WHERE ITS TRAP STACK ENDS, so sscratch's existing value, the
// trap-stack top, IS this hart's block address, and switch.S keeps every csrrw against
// sscratch it already had. The stack grows DOWN from the block, so no frame the entry builds
// can reach a field, and an sp ABOVE the top is already refused by the entry's range test.
//
// S-mode has no hart-identity CSR: mhartid is machine-mode only, tp is parked at 0 and is
// U-mode writable. sscratch is neither readable nor writable from U-mode, so the identity a
// row carries cannot be forged from below.

#ifndef KICKOS_ARCH_PERCPU_H
#define KICKOS_ARCH_PERCPU_H

#include <kickos/arch/arch.h>
#include <kickos/arch/rv64_frame.h>

#include <stdint.h>

// C++ ONLY, as arch.h itself is: the extern "C" below is UNGUARDED, so a C includer breaks
// here. Nothing but this arch's own C++ backend includes it, switch.S reaching the block by
// displacement off sscratch.
extern "C"
{

// The type and the accessor may not share a name: a function shadowing a struct hides that
// struct's constructor, which is -Wshadow and an error here.
struct alignas(KICKOS_RV64_PERCPU_BLOCK_SIZE) rv64_percpu_block
{
    // The context PHYSICALLY on the CPU, which is not arch_switch's `from`. switch.S spells
    // this displacement as a literal, so a field added AHEAD of it is a silent wrong offset.
    struct arch_context* ctx_current;

    // The deferred switch's target, consumed at the interrupt leg's exit.
    struct arch_context* switch_to;

    // Nonzero exactly while an interrupt dispatch runs on this hart.
    uint32_t isr_depth;

    // The dense index this hart answers arch_cpu_id with, seated by kickos_rv64_init.
    uint32_t id;

    // THE SOFTWARE INTERRUPT CONTROLLER'S CELLS ARE NOT HERE, and that is a ruling rather than
    // an omission: this board has no interrupt controller, so those cells mirror no per-hart
    // hardware and a logical line is one system-wide resource. Keyed per hart, a driver that
    // unmasks on one and an injector that raises on another never meet, and the raise sits
    // latched on the injector's hart until an unmask that only the driver's hart will make.
    // They live at file scope in arch_rv64imac.cc, under the kernel lock every caller holds.
};

// The trap stack FIRST: its top is the block's address, which is what sscratch holds.
struct alignas(KICKOS_RV64_PERCPU_BLOCK_SIZE) rv64_percpu_row
{
    uint8_t trap_stack[KICKOS_RV64_TRAP_STACK_SIZE];
    struct rv64_percpu_block block;
};

extern struct rv64_percpu_row kickos_rv64_percpu[KICKOS_NUM_CORES];

// At one core the row is the array's first element and the accessor is a FOLD, so the image
// carries no CSR read to find it. The multi-core arm reads sscratch, which the machine-mode
// prologue seats on every hart before its mret (chip startup.S).
//
// The SEAT is the one call that validates: it derives the dense index from the row sscratch
// names and refuses a pointer that names none, so no later reader repeats the check.
#if KICKOS_NUM_CORES > 1
struct rv64_percpu_block* rv64_percpu(void);
struct rv64_percpu_block* rv64_percpu_seat(void);
#else
#define rv64_percpu() (&kickos_rv64_percpu[0].block)
#define rv64_percpu_seat() rv64_percpu()
#endif

}

#endif // KICKOS_ARCH_PERCPU_H
