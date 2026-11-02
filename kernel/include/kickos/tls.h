// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-thread carve off the LOW end of the thread's own stack, so it costs no MPU
// descriptor of its own: a pool thread's libc state first (KICKOS_REENT_CARVE bytes, whose
// last two words are the cell that ends where the TLS block starts), then the TLS block, which
// the thread pointer names less the image's offset (tls_thread_pointer). Idle's stack carries
// the TLS block alone.
//
// Whether the thread pointer is derived by masking SP down to the stride (plus the libc
// state) or SEATED from the thread's own context is per arch (ARCH_TLS_FROM_SP); a seating
// backend owes the stack no alignment past the ABI's.

#ifndef KICKOS_TLS_H
#define KICKOS_TLS_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{

struct Thread;

// Each image's TLS block is its own span of the link's one thread-local segment (<kickos/sys/
// image.h>), at the image's offset in it, `image` naming the image of the thread's task
// (task_image).

// Image `image`'s TLS block: zero where the board compiles no TLS, and where the image declares
// no thread_local.
size_t tls_block_size(size_t image);

// The thread pointer of a thread of image `image` whose TLS block starts at `block`: below it
// by the image's offset in the segment, so that the image's own offsets land in its block.
uintptr_t tls_thread_pointer(size_t image, uintptr_t block);

// What a pool thread's stack pays: its libc state and the TLS block.
size_t tls_pool_carve_size(size_t image);

// What `t`'s stack pays: tls_pool_carve_size(), or the TLS block alone for an idle TCB.
size_t tls_carve_size(Thread const* t);

// `block` is the TLS block's start, copied from image `image`'s template. Safe with a zero block
// size.
void tls_seat(void* block, size_t image);

// True iff a pool thread's stack block at (base, size) can carry its carve: larger than the
// carve, and where the thread pointer is SP masked and a thread_local is declared, also
// strided and EXACTLY one stride wide.
bool tls_stack_admissible(uintptr_t base, size_t size, size_t image);

// Answers the stack base as the kernel reaches it; null where nothing was carved. Panics on a
// stack the carve does not fit unless `t` is one of this kernel's idle TCBs.
// A pointer and no aggregate: a returned struct costs thread_create a stack slot on the
// kstacks-0 syscall path, whose red zone has four bytes to spare.
void* tls_carve(Thread const* t, void* stack_base, size_t stack_size);

}

#endif
