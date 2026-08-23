// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-thread TLS block: how big it is, and how it is seated. The block is carved off
// the LOW end of the thread's own stack, so it costs no MPU descriptor of its own and the
// thread pointer IS the stack block's base.

#ifndef KICKOS_TLS_H
#define KICKOS_TLS_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{

// Bytes to carve off the bottom of a thread's stack. ZERO when the image declares no
// thread_local at all, which is the state of the fleet until an app declares one, and zero
// where the board compiles no TLS.
size_t tls_block_size();

// Copy the .tdata template into a freshly carved block and zero its .tbss. `base` is the
// stack block's base, which is the thread pointer. Safe to call with a zero block size.
void tls_seat(void* base);

// True iff a stack block at (base, size) can carry a TLS block the thread pointer can be
// derived from: strided, one stride wide at most, and larger than the block itself.
bool tls_stack_admissible(uintptr_t base, size_t size);

// True iff the block is too small for a thread pointer to be masked out of an SP inside
// it. Idle's is, by design, and it reaches no thread_local; every stack the pool hands out
// is exactly one stride. Distinguishing this from a FAILED admissibility test is what lets
// thread_create assert the rest instead of silently skipping the carve.
bool tls_stack_below_stride(size_t size);

} // namespace kickos

#endif
