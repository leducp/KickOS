// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Userspace syscall stubs. Identical source across arches; only arch_syscall() differs.

#include <kickos/sys.h>
#include <kickos/libc/string.h>
#include <kickos/arch/arch.h>

extern "C"
{

// The byte-count returns are pinned at 4 bytes on every target (see sys.h). sizeof on a call
// expression is unevaluated, so these pin the DECLARED return type without emitting a trap.
static_assert(sizeof(kos_kconsole_write(nullptr, 0)) == 4, "must be exactly 4 bytes");
static_assert(sizeof(kos_send(0, nullptr, 0)) == 4, "must be exactly 4 bytes");
static_assert(sizeof(kos_send_timed(0, nullptr, 0, 0)) == 4, "must be exactly 4 bytes");
static_assert(sizeof(kos_recv(0, nullptr, 0, nullptr)) == 4, "must be exactly 4 bytes");
static_assert(sizeof(kos_call(0, nullptr, 0, 0)) == 4, "must be exactly 4 bytes");
static_assert(sizeof(kos_call_generic(0, nullptr, 0, 0)) == 4, "must be exactly 4 bytes");
static_assert(sizeof(kos_call_timed(0, nullptr, 0, 0, 0)) == 4, "must be exactly 4 bytes");
static_assert(sizeof(kos_recv_timed(0, nullptr, 0, nullptr)) == 4, "must be exactly 4 bytes");

// arch_syscall returns at REGISTER width, so the narrowing casts below truncate. They are
// exact: a transferred count is bounded by KOS_EP_MSG_MAX / the kernel's 4096-byte console
// clamp, and a refusal is a small negated errno whose sign extension survives.
int32_t kos_kconsole_write(void const* buf, size_t len)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_KCONSOLE_WRITE,
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

// Holds the "always written" guarantee on the paths the kernel never reaches: a null or
// unwritable out-pointer is refused at the boundary.
static void cap_out_clear(kos_cap_t* out_cap)
{
    if (out_cap != nullptr)
    {
        *out_cap = KOS_CAP_NONE;
    }
}

int kos_sem_create(int initial, kos_cap_t* out_cap)
{
    cap_out_clear(out_cap);
    return static_cast<int>(arch_syscall(KOS_SYS_SEM_CREATE,
                                         static_cast<uintptr_t>(initial),
                                         reinterpret_cast<uintptr_t>(out_cap), 0, 0));
}

int kos_sem_wait(kos_cap_t sem)
{
    return static_cast<int>(arch_syscall(KOS_SYS_SEM_WAIT, static_cast<uintptr_t>(sem), 0, 0, 0));
}

int kos_sem_post(kos_cap_t sem)
{
    return static_cast<int>(arch_syscall(KOS_SYS_SEM_POST, static_cast<uintptr_t>(sem), 0, 0, 0));
}

int kos_mutex_create(kos_cap_t* out_cap)
{
    cap_out_clear(out_cap);
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_CREATE,
                                         reinterpret_cast<uintptr_t>(out_cap), 0, 0, 0));
}

int kos_mutex_lock(kos_cap_t mtx)
{
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_LOCK,
                                         static_cast<uintptr_t>(mtx), 0, 0, 0));
}

int kos_mutex_unlock(kos_cap_t mtx)
{
    return static_cast<int>(arch_syscall(KOS_SYS_MUTEX_UNLOCK,
                                         static_cast<uintptr_t>(mtx), 0, 0, 0));
}

int kos_endpoint_create(kos_cap_t* out_cap)
{
    cap_out_clear(out_cap);
    return static_cast<int>(arch_syscall(KOS_SYS_ENDPOINT_CREATE,
                                         reinterpret_cast<uintptr_t>(out_cap), 0, 0, 0));
}

int32_t kos_send(kos_cap_t ep, void const* buf, size_t len)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_SEND,
                                             static_cast<uintptr_t>(ep),
                                             reinterpret_cast<uintptr_t>(buf),
                                             static_cast<uintptr_t>(len), 0));
}

int32_t kos_send_timed(kos_cap_t ep, void const* buf, size_t len, uint32_t timeout_us)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_SEND_TIMED,
                                             static_cast<uintptr_t>(ep),
                                             reinterpret_cast<uintptr_t>(buf),
                                             static_cast<uintptr_t>(len),
                                             static_cast<uintptr_t>(timeout_us)));
}

int32_t kos_recv(kos_cap_t ep, void* buf, size_t cap_len, struct kos_recv_info* info)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_RECV,
                                             static_cast<uintptr_t>(ep),
                                             reinterpret_cast<uintptr_t>(buf),
                                             static_cast<uintptr_t>(cap_len),
                                             reinterpret_cast<uintptr_t>(info)));
}

int32_t kos_recv_timed(kos_cap_t ep, void* buf, size_t cap_len,
                       struct kos_recv_timed_opts* opts)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_RECV_TIMED,
                                             static_cast<uintptr_t>(ep),
                                             reinterpret_cast<uintptr_t>(buf),
                                             static_cast<uintptr_t>(cap_len),
                                             reinterpret_cast<uintptr_t>(opts)));
}

int32_t kos_call(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap)
{
#if KICKOS_ARCH_HAS_IPC_FASTPATH
    // The caller-side selection is SIZE only; the kernel's refusals are about STATE and
    // live in the fastpath.
    if (send_len <= (size_t)KOS_CALL_REG_BYTES and recv_cap <= (size_t)KOS_CALL_REG_BYTES)
    {
        uint32_t io[KOS_CALL_REG_WORDS + 3];
        unsigned char* const payload = reinterpret_cast<unsigned char*>(&io[3]);
        unsigned char const* const src = static_cast<unsigned char const*>(buf);
        for (size_t i = 0; i < (size_t)KOS_CALL_REG_BYTES; i++)
        {
            unsigned char b = 0;
            if (i < send_len)
            {
                b = src[i];
            }
            payload[i] = b; // a byte loop: `buf` carries no alignment guarantee
        }
        io[0] = static_cast<uint32_t>(KOS_SYS_CALL_REG);
        io[1] = static_cast<uint32_t>(ep);
        io[2] = static_cast<uint32_t>(kos_call_lens_pack(send_len, recv_cap));
        int32_t const rc = arch_syscall_reg(io);
        if (rc != KOS_CALL_REG_FALLBACK)
        {
            if (rc > 0)
            {
                unsigned char const* const reply = reinterpret_cast<unsigned char const*>(&io[1]);
                unsigned char* const dst = static_cast<unsigned char*>(buf);
                for (int32_t i = 0; i < rc; i++)
                {
                    dst[i] = reply[i];
                }
            }
            return rc;
        }
    }
#endif
    return kos_call_generic(ep, buf, send_len, recv_cap);
}

int32_t kos_call_generic(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_CALL,
                                             static_cast<uintptr_t>(ep),
                                             reinterpret_cast<uintptr_t>(buf),
                                             static_cast<uintptr_t>(send_len),
                                             static_cast<uintptr_t>(recv_cap)));
}

int32_t kos_call_timed(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap,
                       uint32_t timeout_us)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_CALL_TIMED,
                                             static_cast<uintptr_t>(ep),
                                             reinterpret_cast<uintptr_t>(buf),
                                             kos_call_lens_pack(send_len, recv_cap),
                                             static_cast<uintptr_t>(timeout_us)));
}

int kos_reply(kos_cap_t reply_cap, void const* buf, size_t len)
{
    return static_cast<int>(arch_syscall(KOS_SYS_REPLY,
                                         static_cast<uintptr_t>(reply_cap),
                                         reinterpret_cast<uintptr_t>(buf),
                                         static_cast<uintptr_t>(len), 0));
}

int kos_console_publish(kos_cap_t ep)
{
    return static_cast<int>(arch_syscall(KOS_SYS_CONSOLE_PUBLISH,
                                         static_cast<uintptr_t>(ep), 0, 0, 0));
}

int kos_thread_kill(kos_thread_t thread)
{
    return static_cast<int>(arch_syscall(KOS_SYS_THREAD_KILL,
                                         static_cast<uintptr_t>(thread), 0, 0, 0));
}

int kos_thread_slay(kos_thread_t thread, uint32_t timeout_us)
{
    return static_cast<int>(arch_syscall(KOS_SYS_THREAD_SLAY,
                                         static_cast<uintptr_t>(thread),
                                         static_cast<uintptr_t>(timeout_us), 0, 0));
}

int kos_task_create(void* mem_base, uint32_t mem_size, uint32_t mem_flags,
                    kos_task_t* out_task)
{
    // Seated BEFORE the trap, so the "always written" guarantee holds even for a refusal the
    // kernel answers without touching the out-pointer.
    if (out_task != NULL)
    {
        *out_task = KOS_TASK_NONE;
    }
    return static_cast<int>(arch_syscall(KOS_SYS_TASK_CREATE,
                                         reinterpret_cast<uintptr_t>(mem_base),
                                         static_cast<uintptr_t>(mem_size),
                                         reinterpret_cast<uintptr_t>(out_task),
                                         static_cast<uintptr_t>(mem_flags)));
}

int kos_task_kill(kos_task_t task)
{
    return static_cast<int>(arch_syscall(KOS_SYS_TASK_KILL,
                                         static_cast<uintptr_t>(task), 0, 0, 0));
}

int kos_task_slay(kos_task_t task, uint32_t timeout_us)
{
    return static_cast<int>(arch_syscall(KOS_SYS_TASK_SLAY,
                                         static_cast<uintptr_t>(task),
                                         static_cast<uintptr_t>(timeout_us), 0, 0));
}

int kos_thread_join(kos_thread_t thread, uint32_t timeout_us)
{
    return static_cast<int>(arch_syscall(KOS_SYS_THREAD_JOIN,
                                         static_cast<uintptr_t>(thread),
                                         static_cast<uintptr_t>(timeout_us), 0, 0));
}

int kos_wait_last(void)
{
    return static_cast<int>(arch_syscall(KOS_SYS_WAIT_LAST, 0, 0, 0, 0));
}

int kos_cap_narrow(kos_cap_t cap, uint8_t mask)
{
    return static_cast<int>(arch_syscall(KOS_SYS_CAP_NARROW,
                                         static_cast<uintptr_t>(cap),
                                         static_cast<uintptr_t>(mask), 0, 0));
}

int kos_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    return static_cast<int>(arch_syscall(KOS_SYS_PINMUX_SET,
                                         static_cast<uintptr_t>(port),
                                         static_cast<uintptr_t>(pin),
                                         static_cast<uintptr_t>(func), 0));
}

int kos_handle_close(kos_cap_t cap)
{
    return static_cast<int>(arch_syscall(KOS_SYS_HANDLE_CLOSE,
                                         static_cast<uintptr_t>(cap), 0, 0, 0));
}

int kos_sem_destroy(kos_cap_t cap)
{
    return kos_handle_close(cap);
}

int kos_thread_spawn(struct kos_thread_params const* params, kos_thread_t* out_thread)
{
    if (out_thread != nullptr)
    {
        *out_thread = KOS_THREAD_NONE; // defined on the paths the kernel never reaches
    }
    return static_cast<int>(arch_syscall(KOS_SYS_THREAD_SPAWN,
                                         reinterpret_cast<uintptr_t>(params),
                                         reinterpret_cast<uintptr_t>(out_thread), 0, 0));
}

void kos_exit(int code)
{
    arch_syscall(KOS_SYS_EXIT, static_cast<uintptr_t>(code), 0, 0, 0);
    __builtin_unreachable();
}

// NOT noreturn, unlike kos_exit: the privilege gate can refuse.
int kos_shutdown(int status)
{
    return static_cast<int>(arch_syscall(KOS_SYS_SHUTDOWN,
                                         static_cast<uintptr_t>(status), 0, 0, 0));
}

void kos_panic(char const* msg)
{
    arch_syscall(KOS_SYS_PANIC, reinterpret_cast<uintptr_t>(msg), 0, 0, 0);
    __builtin_unreachable();
}

// Planted by the arch as the return address of an UNPRIVILEGED thread's entry: such a worker
// must reach the kernel exit path through the syscall trap, never by calling
// kickos_thread_return, which is the privileged threads' path.
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
// NOT noreturn, like kos_shutdown: the privilege gate can refuse, and a chip with no
// bootloader entry declines.
int kos_reboot(void)
{
    return static_cast<int>(arch_syscall(KOS_SYS_REBOOT, 0, 0, 0, 0));
}

void* kos_guard_addr(void)
{
    return reinterpret_cast<void*>(arch_syscall(KOS_SYS_GUARD_ADDR, 0, 0, 0, 0));
}

uint32_t kos_irq_spurious_count(void)
{
    return static_cast<uint32_t>(arch_syscall(KOS_SYS_IRQ_SPURIOUS, 0, 0, 0, 0));
}

uint32_t kos_ipc_fast_taken(void)
{
    return static_cast<uint32_t>(arch_syscall(KOS_SYS_IPC_FAST_TAKEN, 0, 0, 0, 0));
}

uint32_t kos_nest_witness(int which)
{
    return static_cast<uint32_t>(
        arch_syscall(KOS_SYS_NEST_WITNESS, static_cast<uintptr_t>(which), 0, 0, 0));
}

uintptr_t kos_grant_probe(uintptr_t op, uintptr_t base, uintptr_t size)
{
    return arch_syscall(KOS_SYS_GRANT_PROBE, op, base, size, 0);
}

uintptr_t kos_aspace_probe(uintptr_t op, uintptr_t a1)
{
    return arch_syscall(KOS_SYS_ASPACE_PROBE, op, a1, 0, 0);
}
#endif

int kos_irq_attach(int irq, kos_cap_t sem_cap)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_ATTACH, static_cast<uintptr_t>(irq),
                     static_cast<uintptr_t>(sem_cap), 0, 0));
}

int kos_irq_claim(int line, unsigned int flags, kos_cap_t* out_cap)
{
    cap_out_clear(out_cap);
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_CLAIM, static_cast<uintptr_t>(line),
                     static_cast<uintptr_t>(flags),
                     reinterpret_cast<uintptr_t>(out_cap), 0));
}

int kos_irq_wait(kos_cap_t irq_cap)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_WAIT, static_cast<uintptr_t>(irq_cap), 0, 0, 0));
}

int kos_irq_ack(kos_cap_t irq_cap)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_ACK, static_cast<uintptr_t>(irq_cap), 0, 0, 0));
}

int kos_irq_notify(kos_cap_t irq_cap)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_NOTIFY, static_cast<uintptr_t>(irq_cap), 0, 0, 0));
}

int kos_irq_discard(kos_cap_t irq_cap)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_IRQ_DISCARD, static_cast<uintptr_t>(irq_cap), 0, 0, 0));
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
    return arch_syscall64(KOS_SYS_CLOCK_NOW, 0, 0, 0, 0);
}

uint32_t kos_cpu_clock_hz(void)
{
    return static_cast<uint32_t>(arch_syscall(KOS_SYS_CPU_CLOCK_HZ, 0, 0, 0, 0));
}

uint32_t kos_periph_clock_hz(uintptr_t base)
{
    return static_cast<uint32_t>(
        arch_syscall(KOS_SYS_PERIPH_CLOCK_HZ, base, 0, 0, 0));
}

int kos_periph_enable(uintptr_t base)
{
    return static_cast<int>(arch_syscall(KOS_SYS_PERIPH_ENABLE, base, 0, 0, 0));
}

int kos_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value)
{
    return static_cast<int>(arch_syscall(KOS_SYS_PERIPH_REG_WRITE, base, offset,
                                         static_cast<uintptr_t>(value), 0));
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

int kos_mem_self_grant(void* base, size_t size, uint32_t flags)
{
    return static_cast<int>(
        arch_syscall(KOS_SYS_MEM_SELF_GRANT, reinterpret_cast<uintptr_t>(base),
                     static_cast<uintptr_t>(size), static_cast<uintptr_t>(flags), 0));
}

void kos_kernel_diag_led_set(int on)
{
    arch_syscall(KOS_SYS_DIAG_LED_SET, static_cast<uintptr_t>(on), 0, 0, 0);
}

void kos_kernel_diag_led_toggle(void)
{
    arch_syscall(KOS_SYS_DIAG_LED_TOGGLE, 0, 0, 0, 0);
}

#if defined(KICKOS_BENCH) && KICKOS_BENCH
static_assert(sizeof(kos_bench(0, 0, 0)) == 4, "must be exactly 4 bytes");

int32_t kos_bench(uint32_t op, uint32_t a0, uint32_t a1)
{
    return static_cast<int32_t>(arch_syscall(KOS_SYS_BENCH, static_cast<uintptr_t>(op),
                                             static_cast<uintptr_t>(a0),
                                             static_cast<uintptr_t>(a1), 0));
}
#endif
}
