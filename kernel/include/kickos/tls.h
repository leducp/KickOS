// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-thread TLS block, carved off the LOW end of the thread's own stack, so it costs
// no MPU descriptor of its own.
//
// Whether the thread pointer is derived by masking SP down to that base or SEATED from the
// thread's own context is per arch (ARCH_TLS_FROM_SP); a seating backend owes the stack no
// alignment past the ABI's.

#ifndef KICKOS_TLS_H
#define KICKOS_TLS_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{

// Bytes to carve off the bottom of a thread's stack. Zero where the board compiles no TLS, and
// where the image declares no thread_local unless KICKOS_REENT_IN_TCB keeps the control block.
size_t tls_block_size();

// Copy the .tdata template into a freshly carved block and zero its .tbss. `base` is the
// stack block's base, which is the thread pointer. Safe to call with a zero block size.
void tls_seat(void* base);

// True iff a stack block at (base, size) can carry a TLS block: larger than the block itself,
// and where the thread pointer is SP masked, also strided and EXACTLY one stride wide. A
// masking arch's idle block is not, and it reaches no thread_local.
bool tls_stack_admissible(uintptr_t base, size_t size);

struct Thread;

// Carves the block off the low end of (stack_base, stack_size) and seats it, answering its base
// as the kernel reaches it; null where nothing was carved and the stack is used whole. Panics
// on a stack tls_stack_admissible refuses unless `t` is one of this kernel's idle TCBs.
// A pointer and no aggregate: a returned struct costs thread_create a stack slot on the
// kstacks-0 syscall path, whose red zone has four bytes to spare.
void* tls_carve(Thread const* t, void* stack_base, size_t stack_size);

}

#endif
