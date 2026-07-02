// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_KERNEL_H
#define KICKOS_KERNEL_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/thread.h>

namespace kickos
{

    // Kernel entry: called by the arch boot path (sim: host main) after arch_init.
    // Creates the idle + root threads and starts the scheduler. Returns a process
    // exit status if the scheduler ever unwinds to boot.
    int kmain();

    // Debug console (in-kernel, write-only, unbuffered). Routes to arch console.
    void kputs(char const* s);
    void kprintf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Unrecoverable error: report and halt the system.
    void kpanic(char const* msg) __attribute__((noreturn));

    // Create a thread. `stack_base`/`stack_size` and the TCB storage are supplied
    // by the caller (static allocation first). Adds it as READY.
    void thread_create(Thread* t, void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size, ThreadAttr const& attr);

}

// The application entry the kernel calls after init (dependency inversion).
extern "C" void kickos_app_main(void);

#define KICKOS_ASSERT(cond)                     \
    do                                          \
    {                                           \
        if (!(cond))                            \
            ::kickos::kpanic("assert: " #cond); \
    } while (0)

#endif
