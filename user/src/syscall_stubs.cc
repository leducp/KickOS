// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Userspace syscall stubs: pack arguments and issue the arch syscall trap.
// Identical source across arches; only arch_syscall() differs (sim trampoline
// vs SVC on ARM).

#include <kickos/sys.h>
#include <kickos/libc/string.h>
#include <kickos/arch/arch.h>

extern "C"
{

long kos_write(int fd, void const* buf, size_t len)
{
    return static_cast<long>(arch_syscall(KOS_SYS_write,
                                          static_cast<uintptr_t>(fd), reinterpret_cast<uintptr_t>(buf),
                                          static_cast<uintptr_t>(len), 0));
}

void kos_yield(void)
{
    arch_syscall(KOS_SYS_yield, 0, 0, 0, 0);
}

void kos_sleep_ns(uint64_t ns)
{
    arch_syscall(KOS_SYS_sleep_ns, kos_u64_lo(ns), kos_u64_hi(ns), 0, 0);
}

int kos_sem_create(int initial)
{
    return static_cast<int>(arch_syscall(KOS_SYS_sem_create,
                                         static_cast<uintptr_t>(initial), 0, 0, 0));
}

void kos_sem_wait(int id)
{
    arch_syscall(KOS_SYS_sem_wait, static_cast<uintptr_t>(id), 0, 0, 0);
}

void kos_sem_post(int id)
{
    arch_syscall(KOS_SYS_sem_post, static_cast<uintptr_t>(id), 0, 0, 0);
}

int kos_thread_spawn(const struct kos_thread_params* params)
{
    return static_cast<int>(arch_syscall(KOS_SYS_thread_spawn,
                                         reinterpret_cast<uintptr_t>(params), 0, 0, 0));
}

void kos_exit(int code)
{
    arch_syscall(KOS_SYS_exit, static_cast<uintptr_t>(code), 0, 0, 0);
    __builtin_unreachable();
}

void kos_irq_inject(int irq)
{
    arch_syscall(KOS_SYS_irq_inject, static_cast<uintptr_t>(irq), 0, 0, 0);
}

void* kos_guard_addr(void)
{
    return reinterpret_cast<void*>(arch_syscall(KOS_SYS_guard_addr, 0, 0, 0, 0));
}

void kos_irq_attach(int irq, int sem_id)
{
    arch_syscall(KOS_SYS_irq_attach, static_cast<uintptr_t>(irq),
                 static_cast<uintptr_t>(sem_id), 0, 0);
}

uint64_t kos_clock_now(void)
{
    uint64_t out = 0;
    arch_syscall(KOS_SYS_clock_now, reinterpret_cast<uintptr_t>(&out), 0, 0, 0);
    return out;
}

long kos_puts(char const* s)
{
    return kos_write(1, s, strlen(s));
}
}
