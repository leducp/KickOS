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

long kos_kconsole_write(void const* buf, size_t len)
{
    return static_cast<long>(arch_syscall(KOS_SYS_kconsole_write,
                                          reinterpret_cast<uintptr_t>(buf),
                                          static_cast<uintptr_t>(len), 0, 0));
}

void kos_print(char const* s)
{
    kos_kconsole_write(s, strlen(s));
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

void kos_sem_wait(int sem)
{
    arch_syscall(KOS_SYS_sem_wait, static_cast<uintptr_t>(sem), 0, 0, 0);
}

void kos_sem_post(int sem)
{
    arch_syscall(KOS_SYS_sem_post, static_cast<uintptr_t>(sem), 0, 0, 0);
}

int kos_sem_destroy(int sem)
{
    return static_cast<int>(arch_syscall(KOS_SYS_sem_destroy,
                                         static_cast<uintptr_t>(sem), 0, 0, 0));
}

int kos_thread_spawn(struct kos_thread_params const* params)
{
    return static_cast<int>(arch_syscall(KOS_SYS_thread_spawn,
                                         reinterpret_cast<uintptr_t>(params), 0, 0, 0));
}

void kos_exit(int code)
{
    arch_syscall(KOS_SYS_exit, static_cast<uintptr_t>(code), 0, 0, 0);
    __builtin_unreachable();
}

// Thread epilogue for UNPRIVILEGED threads: the arch plants this as the return
// address of a user thread's entry, so a worker that returns exits via the
// syscall trap (running the kernel exit path privileged) rather than calling
// kickos_thread_return directly from unprivileged mode. Privileged threads use
// kickos_thread_return; on the sim (no real privilege) this is unused.
void kickos_user_thread_return(void)
{
    kos_exit(0);
    __builtin_unreachable();
}

void kos_irq_inject(int irq)
{
    arch_syscall(KOS_SYS_irq_inject, static_cast<uintptr_t>(irq), 0, 0, 0);
}

#if defined(KICKOS_ENABLE_SELFTEST)
void* kos_guard_addr(void)
{
    return reinterpret_cast<void*>(arch_syscall(KOS_SYS_guard_addr, 0, 0, 0, 0));
}

uint32_t kos_irq_spurious_count(void)
{
    return static_cast<uint32_t>(arch_syscall(KOS_SYS_irq_spurious, 0, 0, 0, 0));
}
#endif

int kos_irq_attach(int irq, int sem_id)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_irq_attach, static_cast<uintptr_t>(irq),
                     static_cast<uintptr_t>(sem_id), 0, 0));
}

int kos_irq_register(int line)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_irq_register, static_cast<uintptr_t>(line), 0, 0, 0));
}

int kos_irq_wait(int handle)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_irq_wait, static_cast<uintptr_t>(handle), 0, 0, 0));
}

int kos_irq_ack(int handle)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_irq_ack, static_cast<uintptr_t>(handle), 0, 0, 0));
}

uint64_t kos_clock_now(void)
{
    uint64_t out = 0;
    arch_syscall(KOS_SYS_clock_now, reinterpret_cast<uintptr_t>(&out), 0, 0, 0);
    return out;
}

void* kos_ram_alloc(size_t size)
{
    return reinterpret_cast<void*>(
        arch_syscall(KOS_SYS_ram_alloc, static_cast<uintptr_t>(size), 0, 0, 0));
}
}
