// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sim: struct arch_context is opaque storage for a ucontext_t plus the entry
// trampoline payload. Kept as a raw aligned buffer so the freestanding kernel
// TUs that embed it in the TCB never pull in <ucontext.h>. The sim backend
// casts this to its private layout and static_asserts the size fits.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stddef.h>

#define ARCH_CONTEXT_SIZE 2048

struct arch_context {
  alignas(16) unsigned char opaque[ARCH_CONTEXT_SIZE];
};

#endif // KICKOS_ARCH_CONTEXT_H
