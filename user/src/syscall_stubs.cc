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
    return static_cast<long>(arch_syscall(KOS_SYS_KCONSOLE_WRITE,
                                          reinterpret_cast<uintptr_t>(buf),
                                          static_cast<uintptr_t>(len), 0, 0));
}

void kos_print(char const* s)
{
    kos_kconsole_write(s, strlen(s));
}

void kos_yield(void)
{
    arch_syscall(KOS_SYS_YIELD, 0, 0, 0, 0);
}

void kos_sleep_ns(uint64_t ns)
{
    arch_syscall(KOS_SYS_SLEEP_NS, kos_u64_lo(ns), kos_u64_hi(ns), 0, 0);
}

int kos_sem_create(int initial)
{
    return static_cast<int>(arch_syscall(KOS_SYS_SEM_CREATE,
                                         static_cast<uintptr_t>(initial), 0, 0, 0));
}

int kos_sem_wait(int sem)
{
    return static_cast<int>(arch_syscall(KOS_SYS_SEM_WAIT, static_cast<uintptr_t>(sem), 0, 0, 0));
}

int kos_sem_post(int sem)
{
    return static_cast<int>(arch_syscall(KOS_SYS_SEM_POST, static_cast<uintptr_t>(sem), 0, 0, 0));
}

int kos_mutex_create(void)
{
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_CREATE, 0, 0, 0, 0));
}

int kos_mutex_lock(int mtx)
{
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_LOCK,
                                         static_cast<uintptr_t>(mtx), 0, 0, 0));
}

int kos_mutex_unlock(int mtx)
{
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_UNLOCK,
                                         static_cast<uintptr_t>(mtx), 0, 0, 0));
}

int kos_endpoint_create(void)
{
    return static_cast<int>(arch_syscall(KOS_SYS_ENDPOINT_CREATE, 0, 0, 0, 0));
}

long kos_send(int ep, void const* buf, size_t len)
{
    return static_cast<long>(arch_syscall(KOS_SYS_SEND,
                                          static_cast<uintptr_t>(ep),
                                          reinterpret_cast<uintptr_t>(buf),
                                          static_cast<uintptr_t>(len), 0));
}

long kos_recv(int ep, void* buf, size_t cap_len, uint32_t* badge)
{
    return static_cast<long>(arch_syscall(KOS_SYS_RECV,
                                          static_cast<uintptr_t>(ep),
                                          reinterpret_cast<uintptr_t>(buf),
                                          static_cast<uintptr_t>(cap_len),
                                          reinterpret_cast<uintptr_t>(badge)));
}

int kos_console_publish(int ep)
{
    return static_cast<int>(arch_syscall(KOS_SYS_CONSOLE_PUBLISH,
                                         static_cast<uintptr_t>(ep), 0, 0, 0));
}

int kos_handle_close(int cap)
{
    return static_cast<int>(arch_syscall(KOS_SYS_HANDLE_CLOSE,
                                         static_cast<uintptr_t>(cap), 0, 0, 0));
}

int kos_sem_destroy(int cap)
{
    return kos_handle_close(cap);
}

int kos_thread_spawn(struct kos_thread_params const* params)
{
    return static_cast<int>(arch_syscall(KOS_SYS_THREAD_SPAWN,
                                         reinterpret_cast<uintptr_t>(params), 0, 0, 0));
}

void kos_exit(int code)
{
    arch_syscall(KOS_SYS_EXIT, static_cast<uintptr_t>(code), 0, 0, 0);
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
    arch_syscall(KOS_SYS_IRQ_INJECT, static_cast<uintptr_t>(irq), 0, 0, 0);
}

#if defined(KICKOS_ENABLE_SELFTEST)
void* kos_guard_addr(void)
{
    return reinterpret_cast<void*>(arch_syscall(KOS_SYS_GUARD_ADDR, 0, 0, 0, 0));
}

uint32_t kos_irq_spurious_count(void)
{
    return static_cast<uint32_t>(arch_syscall(KOS_SYS_IRQ_SPURIOUS, 0, 0, 0, 0));
}

uintptr_t kos_grant_probe(uintptr_t op, uintptr_t base, uintptr_t size)
{
    return arch_syscall(KOS_SYS_GRANT_PROBE, op, base, size, 0);
}
#endif

int kos_irq_attach(int irq, int sem_id)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_ATTACH, static_cast<uintptr_t>(irq),
                     static_cast<uintptr_t>(sem_id), 0, 0));
}

int kos_irq_register(int line)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_REGISTER, static_cast<uintptr_t>(line), 0, 0, 0));
}

int kos_irq_wait(int handle)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_WAIT, static_cast<uintptr_t>(handle), 0, 0, 0));
}

int kos_irq_ack(int handle)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_ACK, static_cast<uintptr_t>(handle), 0, 0, 0));
}

#if defined(KICKOS_ENABLE_SELFTEST)
int kos_irq_unmask(int line)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_UNMASK, static_cast<uintptr_t>(line), 0, 0, 0));
}
#endif

uint64_t kos_clock_now(void)
{
    uint64_t out = 0;
    // Surface the syscall status instead of discarding it: on a reject (bad/misaligned
    // out-ptr -- impossible for this well-formed stack local, so purely defensive) the
    // out value is never written, so report 0 rather than an uninitialized time.
    long const rc = static_cast<long>(
        arch_syscall(KOS_SYS_CLOCK_NOW, reinterpret_cast<uintptr_t>(&out), 0, 0, 0));
    if (rc < 0)
    {
        return 0;
    }
    return out;
}

uint32_t kos_cpu_clock_hz(void)
{
    return static_cast<uint32_t>(arch_syscall(KOS_SYS_CPU_CLOCK_HZ, 0, 0, 0, 0));
}

uint32_t kos_cpu_clock_set(kos_pstate_t pstate)
{
    return static_cast<uint32_t>(
        arch_syscall(KOS_SYS_CPU_CLOCK_SET, static_cast<uintptr_t>(pstate), 0, 0, 0));
}

void* kos_ram_alloc(size_t size)
{
    return reinterpret_cast<void*>(
        arch_syscall(KOS_SYS_RAM_ALLOC, static_cast<uintptr_t>(size), 0, 0, 0));
}

void kos_kernel_diag_led_set(int on)
{
    arch_syscall(KOS_SYS_DIAG_LED_SET, static_cast<uintptr_t>(on), 0, 0, 0);
}

void kos_kernel_diag_led_toggle(void)
{
    arch_syscall(KOS_SYS_DIAG_LED_TOGGLE, 0, 0, 0, 0);
}
}
