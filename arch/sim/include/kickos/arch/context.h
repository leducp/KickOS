// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A raw aligned buffer, so the freestanding kernel TUs that embed it in the TCB never pull in
// <ucontext.h>. The sim backend casts it to its private layout and static_asserts the fit.

#ifndef KICKOS_ARCH_CONTEXT_H
#define KICKOS_ARCH_CONTEXT_H

#include <stddef.h>

#define ARCH_CONTEXT_SIZE 2048

struct arch_context
{
    alignas(16) unsigned char opaque[ARCH_CONTEXT_SIZE];
};

#endif
