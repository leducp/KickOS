// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch-independent syscall table + dispatch. The arch entry (sim
// trampoline / ARM SVC handler) reads the number + args and calls
// syscall_dispatch(); the result is delivered back to the caller frame.
// Kernel objects addressable from userspace (semaphores, threads) live in
// static pools referenced by small integer handles -- no pointers cross the
// boundary.

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/time.h>
#include <kickos/kernel.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/ktrace.h>

#include <kickos/sys/abi.h>

namespace kickos
{
    namespace
    {
        // --- Semaphore registry (generational slot pool, see slotpool.h) -----------
        // The handle is opaque to userspace; sem_resolve() is the single validate-and-
        // resolve chokepoint the capability model later swaps for a unified
        // handle table. (ABA/generation mechanics live in slotpool.h.)
        Semaphore* sem_resolve(int handle) { return kernel().sems.resolve(handle); }

        int sem_create(int initial)
        {
            IrqLock lock;
            int const i = kernel().sems.alloc();
            if (i < 0)
            {
                return -1;
            }
            sem_init(kernel().sems.at(i), initial);
            return kernel().sems.handle_for(i);
        }

        int sem_destroy(int handle)
        {
            IrqLock lock;
            Semaphore* s = sem_resolve(handle);
            if (s == nullptr)
            {
                return -1;
            }
            // Quiescent-only: refuse while waiters are parked (waking them with an
            // error needs the wait_result channel timed wait adds -- Later).
            if (not s->waiters.empty())
            {
                return -1;
            }
            kernel().sems.free(handle);
            return 0;
        }

        // Privileged in-kernel IRQ handler bound by KOS_SYS_irq_attach: posts a
        // semaphore from ISR context, driving the interrupt-exit switch (trigger #4).
        void irq_sem_post(void* arg)
        {
            int handle = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            Semaphore* s = sem_resolve(handle);
            if (s != nullptr)
            {
                sem_post(s);
            }
        }

        // --- Thread pool (slot allocation lives in ThreadPool, thread.h) -----------
        // Reuse is safe because thread_create re-inits the TCB + re-fabricates the arch
        // context from scratch, so a reclaimed slot's privilege posture is a clean reset
        // (the sim's mid-syscall `raised`, the ARM CONTROL.nPRIV, the RX PSW). No syscall
        // resolves a thread by handle yet; when join/kill-by-id adds one it must also
        // reject state==EXITED (the generation bumps at reclaim, not exit -- see ThreadPool).

        // Confused-deputy floor: syscall_dispatch runs privileged (it bypasses the
        // MPU), so it must never dereference a user pointer the CALLER could not
        // itself reach. A range passes iff it lies within one region the current
        // thread is granted -- its domain data + its own stack (thread.cc composition)
        // -- with the required access. Privileged callers (kernel domain, trusted)
        // bypass. LIMIT: only kernel-modelled regions are checked. On hardware a
        // domain also owns the app's code/rodata/.data regions (M2 fan-out), but the
        // host sim does not model those, so bounds on buffers/strings that may point
        // into them (kconsole_write, the thread name) wait for the hardware backends.
        // Struct + out-pointer args are always caller STACK locals, so they are in a
        // modelled region and safe to check now.
        bool user_range_ok(uintptr_t ptr, size_t len, uint32_t need)
        {
            Thread* c = sched::current();
            if (c == nullptr)
            {
                return false;
            }
            if (c->privileged)
            {
                return true;
            }
            if (len == 0)
            {
                return true;
            }
            uintptr_t const end = ptr + len;
            if (end < ptr)
            {
                return false; // address-space wrap
            }
            for (size_t i = 0; i < c->region_count; i++)
            {
                arch_mpu_region const& r = c->regions[i];
                if ((r.attr & need) != need)
                {
                    continue;
                }
                uintptr_t const rend = r.base + r.size;
                if (rend >= r.base and ptr >= r.base and end <= rend)
                {
                    return true;
                }
            }
            return false;
        }

        int thread_spawn(kos_thread_params const* p)
        {
            IrqLock lock;
            if (p == nullptr)
            {
                return -1;
            }
            // Copy the caller's params into kernel memory through a checked read: an
            // unprivileged caller must not hand the kernel a pointer it could not read
            // (a kernel address would otherwise be dereferenced privileged). The struct
            // is a caller stack local (kos::thread::spawn), so it lies in the stack
            // region. Read the fields from the kernel-owned copy hereafter. (The name
            // pointer inside is still user memory; thread_create copies it bounded --
            // validating IT against app rodata is the hardware-backend pass.)
            if (not user_range_ok(reinterpret_cast<uintptr_t>(p), sizeof(*p), ARCH_MPU_R))
            {
                return -1;
            }
            kos_thread_params params = *p;
            p = &params;
            // Validate the user-supplied priority: it indexes the ready lists and
            // drives a 1u<<prio bitmap shift, so an out-of-range value is an OOB write / UB.
            // Priority 0 is reserved for the idle thread.
            if (p->prio < KICKOS_PRIO_MIN or p->prio > KICKOS_PRIO_MAX)
            {
                return -1;
            }
            // No privilege escalation: only a privileged thread may spawn one (a
            // privileged thread is granted the whole arena). The granted domain
            // region's geometry is validated arch-side in arch_mpu_apply.
            if (p->privileged != 0 and not sched::current()->privileged)
            {
                return -1;
            }
            // Caller-provided stack (optional): validate BEFORE allocating a slot, so a bad
            // one is a clean spawn error, not a leaked slot / silent overflow. stack_base==0
            // means "use the kernel default". A provided stack must clear the floor and be
            // aligned (base AND size KICKOS_STACK_ALIGN-aligned, so the initial top is aligned).
            if (p->stack_base != nullptr)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(p->stack_base);
                if (p->stack_size < KICKOS_MIN_STACK_SIZE
                    or (base & (KICKOS_STACK_ALIGN - 1)) != 0
                    or (p->stack_size & (KICKOS_STACK_ALIGN - 1)) != 0
                    or base + p->stack_size < base) // base+size must not wrap the address space
                {
                    return -1;
                }
            }
            Kernel& k = kernel();
            // Resolve the memory domain BEFORE claiming a slot, so a domain-pool
            // exhaustion is a clean spawn failure, not a leaked thread slot. domain_for
            // does not take a reference (thread_create does); a domain it creates but
            // we never reference stays refcount 0 == a free slot.
            Domain* const dom = domain_for(p->privileged != 0, p->mem_base, p->mem_size);
            if (dom == nullptr)
            {
                return -1;
            }
            // Reclaim an EXITED slot or bump-allocate (ThreadPool::alloc). Single-core: an
            // EXITED thread is guaranteed off-CPU by the time any other thread reaches here
            // -- it parked in exit_current until its switch-away committed -- and is off
            // every ready/wait/timer list, so reinit is safe. current() is RUNNING, never
            // EXITED, so it is never picked.
            int const i = k.threads.alloc();
            if (i < 0)
            {
                return -1;
            }

            ThreadAttr attr;
            attr.name = "user";
            if (p->name != nullptr)
            {
                attr.name = p->name;
            }
            attr.prio = p->prio;
            attr.policy = Policy::FIFO;
            if (p->policy == KOS_POLICY_RR)
            {
                attr.policy = Policy::RR;
            }
            attr.quantum_ns = p->quantum_ns;
            attr.privileged = (p->privileged != 0);
            attr.mem_base = p->mem_base;
            attr.mem_size = p->mem_size;
            attr.domain = dom;

            // Caller's stack if given (a thread's stack is a userspace concern), else the
            // kernel's default per-thread slab.
            void* stack = k.threads.stacks[i];
            size_t stack_size = KICKOS_USER_STACK_SIZE;
            if (p->stack_base != nullptr)
            {
                stack = p->stack_base;
                stack_size = p->stack_size;
            }
            thread_create(&k.threads.slots[i], p->entry, p->arg, stack, stack_size, attr);
            return k.threads.handle_for(i);
        }

    }
}

using namespace kickos;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
namespace
{
    uint16_t syscall_tid()
    {
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return static_cast<uint16_t>(trace::TRACE_NO_THREAD);
        }
        return c->id;
    }

    // RAII SYSCALL_ENTER/EXIT bracket. The EXIT fires in the destructor on EVERY
    // ordinary return path -- but KOS_SYS_exit switches away permanently inside the
    // dispatch (never returns to this frame), so its destructor never runs and it
    // is recorded as ENTER-only (the decoder handles the missing EXIT).
    struct SyscallTrace
    {
        uint16_t tid;
        uint16_t nr;
        SyscallTrace(uint16_t t, uint16_t n) : tid(t), nr(n)
        {
            ktrace_syscall_enter(tid, nr);
        }
        ~SyscallTrace()
        {
            ktrace_syscall_exit(tid, nr);
        }
    };
}
#define KTRACE_SYSCALL_SCOPE(nr) SyscallTrace _kt_syscall(syscall_tid(), static_cast<uint16_t>(nr))
#else
#define KTRACE_SYSCALL_SCOPE(nr) do { } while (0)
#endif

extern "C" uintptr_t syscall_dispatch(uintptr_t nr,
                                      uintptr_t a0, uintptr_t a1,
                                      uintptr_t a2, uintptr_t a3)
{
    (void)a2; // unused by the current syscalls (all take <= 2 args or an out-ptr)
    (void)a3;
    KTRACE_SYSCALL_SCOPE(nr);
    switch (nr)
    {
        case KOS_SYS_kconsole_write:
        {
            // Explicit (buf, len): the kernel must never strlen a user pointer.
            // Bound-checking buf against the caller's regions is M2 (item 12), but
            // len is a plain scalar: clamp it now so a garbage/huge value can't
            // walk off RAM (HardFault -> whole board, no MPU to contain it) or
            // monopolize the UART for an unbounded stretch.
            constexpr size_t kMaxConsoleWrite = 4096;
            char const* buf = reinterpret_cast<char const*>(a0);
            size_t len = static_cast<size_t>(a1);
            if (len > kMaxConsoleWrite)
            {
                len = kMaxConsoleWrite;
            }
            kconsole_write(buf, len); // fan-out (chip + RTT), not the raw transport
            return len;
        }
        case KOS_SYS_yield:
        {
            sched::yield();
            return 0;
        }
        case KOS_SYS_sleep_ns:
        {
            ktime_sleep_ns(kos_u64_join(static_cast<uint32_t>(a0),
                                        static_cast<uint32_t>(a1)));
            return 0;
        }
        case KOS_SYS_sem_create:
        {
            return static_cast<uintptr_t>(sem_create(static_cast<int>(a0)));
        }
        case KOS_SYS_sem_destroy:
        {
            return static_cast<uintptr_t>(sem_destroy(static_cast<int>(a0)));
        }
        case KOS_SYS_sem_wait:
        {
            // Resolve and use under one lock (sem_wait/sem_post nest their own):
            // otherwise a concurrent sem_destroy could free the slot between
            // resolve and use, defeating the quiescent-only guarantee.
            IrqLock lock;
            Semaphore* s = sem_resolve(static_cast<int>(a0));
            if (s == nullptr)
            {
                return static_cast<uintptr_t>(-1);
            }
            sem_wait(s);
            return 0;
        }
        case KOS_SYS_sem_post:
        {
            IrqLock lock;
            Semaphore* s = sem_resolve(static_cast<int>(a0));
            if (s == nullptr)
            {
                return static_cast<uintptr_t>(-1);
            }
            sem_post(s);
            return 0;
        }
        case KOS_SYS_thread_spawn:
        {
            return static_cast<uintptr_t>(
                thread_spawn(reinterpret_cast<kos_thread_params const*>(a0)));
        }
        case KOS_SYS_exit:
        {
            sched::exit_current(static_cast<int>(a0)); // noreturn
            return 0;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_irq_inject:
        {
            // Test scaffolding only (real IRQs come from devices), so compiled out
            // of the production ABI -- like guard_addr below. The line is
            // unprivileged-user-reachable: validate it at the boundary and reject a
            // bad value with -1 rather than passing it to the controller. (Never
            // KICKOS_UNREACHABLE a user-supplied number -- that would let a user
            // halt the kernel.)
            // Deliberately NOT privilege-gated (unlike irq_unmask/irq_attach): this
            // simulates a DEVICE firing, not an arm of the controller, and the tier-1
            // model has unprivileged drivers receive IRQs (selftest injects from an
            // unprivileged thread). Test-only + one-owner attach already bound the line.
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-1);
            }
            arch_irq_inject(irq);
            return 0;
        }
        case KOS_SYS_guard_addr:
        {
            return arch_mpu_probe_addr();
        }
        case KOS_SYS_irq_spurious:
        {
            return static_cast<uintptr_t>(irq_spurious_count());
        }
        case KOS_SYS_irq_unmask:
        {
            // Test scaffolding: enable an UNBOUND line so an injected raise reaches
            // the default (spurious) handler on masked-by-default controllers (ARM
            // NVIC, RX), which else drop it. Privileged-only (it arms a controller
            // line), like irq_attach.
            if (not sched::current()->privileged)
            {
                return static_cast<uintptr_t>(-1);
            }
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-1);
            }
            arch_irq_unmask(irq);
            return 0;
        }
#endif
        case KOS_SYS_irq_attach:
        {
            // Tier-2 installs a privileged in-kernel handler: privileged-only, so
            // an unprivileged thread cannot bind (or steal) a line's dispatch.
            if (not sched::current()->privileged)
            {
                return static_cast<uintptr_t>(-1);
            }
            // Resolve + attach + unmask under one lock (like sem_wait/post): otherwise a
            // concurrent sem_destroy between the resolve check and the attach could bind
            // the line to an already-dead handle.
            IrqLock lock;
            int irq = static_cast<int>(a0);
            int sem_handle = static_cast<int>(a1);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ or sem_resolve(sem_handle) == nullptr)
            {
                return static_cast<uintptr_t>(-1);
            }
            // Store the handle (not a pointer): irq_sem_post re-resolves each fire,
            // so a since-destroyed sem fails safe instead of poking a stale slot.
            // irq_attach fails (-1) if the line is already owned -- no stealing.
            if (not irq_attach(irq, irq_sem_post,
                               reinterpret_cast<void*>(static_cast<intptr_t>(sem_handle))))
            {
                return static_cast<uintptr_t>(-1);
            }
            // Enable the line: a userspace tier-2 binding has no separate unmask
            // syscall (tier-1 unmasks via register/irq_ack), so attach must arm it.
            // Required on default-masked controllers (ARM NVIC, RX); sim/riscv were
            // unmasked-by-default and only worked by that leniency. In-kernel
            // irq_attach (console) still unmasks on its own schedule -- untouched.
            arch_irq_unmask(irq);
            return 0;
        }
        case KOS_SYS_clock_now:
        {
            // Out-pointer for a 64-bit store: reject null and misalignment (an unaligned u64
            // write faults / is UB on ARM and RISC-V), then bound it against the caller's
            // writable regions -- the kernel writes it privileged, so an unprivileged caller
            // must own it. The stub passes a stack local (in its stack region); privileged
            // callers bypass. Closes the privileged-kernel-writes-a-user-pointer hole.
            if (a0 == 0 or (a0 & 0x7u) != 0)
            {
                return static_cast<uintptr_t>(-1);
            }
            if (not user_range_ok(a0, sizeof(uint64_t), ARCH_MPU_W))
            {
                return static_cast<uintptr_t>(-1);
            }
            *reinterpret_cast<uint64_t*>(a0) = arch_clock_now();
            return 0;
        }
        case KOS_SYS_ram_alloc:
        {
            // Privileged-only: domains are carved by the privileged setup path,
            // not by arbitrary user threads (avoids a DoS on the shared pool and
            // matches static-allocation-first). IrqLock: arch_ram_alloc does an
            // unguarded read-modify-write of the bump pointer.
            IrqLock lock;
            if (not sched::current()->privileged)
            {
                return static_cast<uintptr_t>(-1);
            }
            return reinterpret_cast<uintptr_t>(
                arch_ram_alloc(static_cast<size_t>(a0)));
        }
        case KOS_SYS_irq_register:
        {
            return static_cast<uintptr_t>(irq_register(static_cast<int>(a0)));
        }
        case KOS_SYS_irq_wait:
        {
            return static_cast<uintptr_t>(irq_wait(static_cast<int>(a0)));
        }
        case KOS_SYS_irq_ack:
        {
            return static_cast<uintptr_t>(irq_ack(static_cast<int>(a0)));
        }
        case KOS_SYS_diag_led_set:
        {
            // Benign single LED (the kernel's diagnostic pin, borrowed): left
            // unprivileged like the console. A no-op on boards with no LED.
            kdiag_led_set(a0 != 0);
            return 0;
        }
        case KOS_SYS_diag_led_toggle:
        {
            kdiag_led_toggle();
            return 0;
        }
        default:
        {
            // Unknown syscall from userspace is a caller error, not a kernel
            // invariant violation: fault the caller (-1), never panic the kernel.
            return static_cast<uintptr_t>(-1);
        }
    }
}
