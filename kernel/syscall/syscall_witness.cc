// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_WITNESS_UACCESS: a ring-0 access to a user pointer, made on purpose. Built only into
// the uaccess witness's own kernel (<kickos/sys/witness.h>). Where the hardware keeps ring 0 out
// of user pages, every arm faults into the panic dump; where it does not, the access lands and
// the caller sees it.

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/time.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/witness.h>

#include "syscall_internal.h"

#if not KICKOS_WITNESS_UACCESS
#error "syscall_witness.cc is the uaccess witness's alone"
#endif

namespace kickos
{
    namespace
    {
        // The fault-exit arm's target: the thread that armed it and the word it named.
        Thread const* g_fault_exit_thread = nullptr;
        uintptr_t g_fault_exit_addr = 0;

        // The timer arm's, and the deadline before which an interrupt is not the one it asked.
        Thread const* g_timer_thread = nullptr;
        uintptr_t g_timer_addr = 0;
        uint64_t g_timer_due = 0;

        // The address is announced first, so the gate can hold the fault's address against it.
        __attribute__((noinline)) uint8_t plant_read(uintptr_t uaddr, char const* when)
        {
            kprintf("[uaccess] ring 0 reads 0x%llx %s\n", static_cast<unsigned long long>(uaddr),
                    when);
            return *reinterpret_cast<uint8_t const volatile*>(uaddr);
        }

        __attribute__((noinline)) uint8_t plant_fetch(uintptr_t uaddr)
        {
            kprintf("[uaccess] ring 0 fetches 0x%llx on entry\n",
                    static_cast<unsigned long long>(uaddr));
            return reinterpret_cast<uint8_t (*)(void)>(uaddr)();
        }

        __attribute__((noinline)) void plant_write(uintptr_t uaddr)
        {
            kprintf("[uaccess] ring 0 writes 0x%llx on entry\n",
                    static_cast<unsigned long long>(uaddr));
            *reinterpret_cast<uint8_t volatile*>(uaddr) = 0xA5u;
        }
    }

    int64_t witness_uaccess(uintptr_t op, uintptr_t uaddr, uintptr_t ns)
    {
        // The whole authority word, which outside this witness's own composition only root holds.
        if (not cap_check_authority(sched::current(), CAP_AUTH_ALL))
        {
            return -KOS_EPERM;
        }
        switch (op)
        {
            case KOS_WITNESS_UREAD_NOW:
            {
                return plant_read(uaddr, "on entry");
            }
            case KOS_WITNESS_UWRITE_NOW:
            {
                plant_write(uaddr);
                return 0;
            }
            case KOS_WITNESS_UREAD_AFTER_PARK:
            {
                ktime_sleep_ns(ns);
                return plant_read(uaddr, "after a park");
            }
            case KOS_WITNESS_UREAD_SPACELESS:
            {
#if KICKOS_HAVE_ASPACE
                IrqLock lock;
                // Forced: at one kernel core a thread with no space runs on the last space.
                aspace_install_boot();
                uint8_t const seen = plant_read(uaddr, "with no space installed");
                (void)aspace_activate_for(sched::current());
                return seen;
#else
                return -KOS_ENOSYS;
#endif
            }
            case KOS_WITNESS_UREAD_AT_FAULT_EXIT:
            {
                g_fault_exit_thread = sched::current();
                g_fault_exit_addr = uaddr;
                return 0;
            }
            case KOS_WITNESS_UFETCH_NOW:
            {
                return plant_fetch(uaddr);
            }
            case KOS_WITNESS_UREAD_IN_TIMER_IRQ:
            {
                IrqLock lock;
                g_timer_thread = sched::current();
                g_timer_addr = uaddr;
                g_timer_due = ktime_now() + ns;
                // An earlier deadline already armed serves as well: the arm waits for the due one.
                uint64_t& armed = kernel().timer_armed_ns[kickos_kernel_core()];
                if (g_timer_due < armed)
                {
                    armed = g_timer_due;
                    arch_timer_arm(g_timer_due);
                }
                return 0;
            }
            default:
            {
                return -KOS_EINVAL;
            }
        }
    }

    void witness_uaccess_timer(Thread const* c)
    {
        if (c == nullptr or c != g_timer_thread or ktime_now() < g_timer_due)
        {
            return;
        }
        uintptr_t const at = g_timer_addr;
        g_timer_thread = nullptr;
        if (arch_witness_irq_lift_flag() == 0)
        {
            kprintf("[uaccess] INCONCLUSIVE: the timer interrupt found the flag that lifts the "
                    "guard clear\n");
            return;
        }
        uint8_t const seen = plant_read(at, "in a timer interrupt");
        kprintf("[uaccess] ERROR: ring 0 reached 0x%llx in a timer interrupt: read 0x%x\n",
                static_cast<unsigned long long>(at), static_cast<unsigned>(seen));
    }

    void witness_uaccess_fault_exit(Thread const* c)
    {
        if (c == nullptr or c != g_fault_exit_thread)
        {
            return;
        }
        uintptr_t const at = g_fault_exit_addr;
        g_fault_exit_thread = nullptr;
        uint8_t const seen = plant_read(at, "in the fault exit");
        kprintf("[uaccess] ERROR: ring 0 reached 0x%llx in the fault exit: read 0x%x\n",
                static_cast<unsigned long long>(at), static_cast<unsigned>(seen));
    }
}
