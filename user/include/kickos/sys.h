// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Userspace C syscall API. Plain-C ergonomic wrappers over the arch syscall
// trap. A C++ RAII layer sits on top in <kickos/kos.hpp>.

#ifndef KICKOS_SYS_H
#define KICKOS_SYS_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

long  kos_write(int fd, const void* buf, size_t len);
void  kos_yield(void);
void  kos_sleep_ns(uint64_t ns);

int   kos_sem_create(int initial);
void  kos_sem_wait(int id);
void  kos_sem_post(int id);

int   kos_thread_spawn(const struct kos_thread_params* params);
void  kos_exit(int code) __attribute__((noreturn));
void  kos_irq_inject(int irq);
void* kos_guard_addr(void);

// Bind device line `irq` so that firing it posts semaphore `sem_id` from ISR
// context (userspace irq-as-event precursor).
void     kos_irq_attach(int irq, int sem_id);
uint64_t kos_clock_now(void);   // monotonic nanoseconds

// Convenience: write a NUL-terminated string to fd 1.
long  kos_puts(const char* s);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_SYS_H
