// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The user half seen from ring 0: the kernel touches a word of this app's own data, which the
// app has just written, through KOS_SYS_WITNESS_UACCESS. Only the uaccess witness's kernel
// answers it. Where the hardware keeps ring 0 out of user pages the access faults into the
// panic dump; a return is the guard missing, and the ERROR line says what the kernel saw or did.
//
// The address is announced before the call, so the gate can hold the dump's fault address
// against the word this image named.

#include <kickos/arch/arch.h> // arch_syscall
#include <kickos/kos.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/trap.h>
#include <kickos/sys/witness.h>

#include <kickos/libc/fmt.h>

#include <stdint.h>

#ifndef KICKOS_UACCESS_OP
#error "KICKOS_UACCESS_OP names the access this image asks for"
#endif

namespace
{
    // Its own granule, so no other object's traffic is on the page the kernel touches.
    alignas(4096) uint8_t volatile g_word[4096] = {0x5A};

    constexpr uint64_t PARK_NS = 2000000u;
    // Far enough that the timer arm's interrupt finds the flags this image sets after the call.
    constexpr uint64_t TIMER_NS = 50000000u;
    // Iterations the image spins waiting for that interrupt, many times what it takes.
    constexpr uint64_t SPIN = 2000000000u;
}

// The fetch arm's target: user text that answers the word the read arms see.
extern "C" __attribute__((noinline)) uint8_t uaccess_fetch_target(void)
{
    return 0x5A;
}

int main(int, char**)
{
    g_word[0] = 0x5A;
    uintptr_t at = reinterpret_cast<uintptr_t>(&g_word[0]);
    uint64_t arg = PARK_NS;
    if (KICKOS_UACCESS_OP == KOS_WITNESS_UFETCH_NOW)
    {
        at = reinterpret_cast<uintptr_t>(&uaccess_fetch_target);
    }
    if (KICKOS_UACCESS_OP == KOS_WITNESS_UREAD_IN_TIMER_IRQ)
    {
        arg = TIMER_NS;
    }
    char msg[112];
    ksnprintf(msg, sizeof(msg), "[uaccess] op %d names 0x%llx\n",
              static_cast<int>(KICKOS_UACCESS_OP), static_cast<unsigned long long>(at));
    kos_print(msg);
    long long const rc = static_cast<long long>(static_cast<int64_t>(
        arch_syscall(KOS_SYS_WITNESS_UACCESS, KICKOS_UACCESS_OP, at, arg, 0)));
    if (KICKOS_UACCESS_OP == KOS_WITNESS_UREAD_AT_FAULT_EXIT and rc == 0)
    {
        // The kill's redirect runs the fault-exit stub, where the armed read waits.
        KOS_TRAP_ILLEGAL();
    }
    if (KICKOS_UACCESS_OP == KOS_WITNESS_UREAD_IN_TIMER_IRQ and rc == 0)
    {
#if defined(__x86_64__)
        // RFLAGS.AC set lifts SMAP, and interrupt delivery keeps it: the entry has to clear it.
        __asm__ volatile("pushfq\n\torq $0x40000, (%%rsp)\n\tpopfq" ::: "memory", "cc");
#endif
        for (uint64_t volatile i = 0; i < SPIN; i = i + 1)
        {
        }
#if defined(__x86_64__)
        __asm__ volatile("pushfq\n\tandq $~0x40000, (%%rsp)\n\tpopfq" ::: "memory", "cc");
#endif
        kos_print("[uaccess] ERROR: no timer interrupt took the armed read\n");
        return 1;
    }
    ksnprintf(msg, sizeof(msg), "[uaccess] ERROR: ring 0 reached 0x%llx: rc %lld, word now 0x%x\n",
              static_cast<unsigned long long>(at), rc, static_cast<unsigned>(g_word[0]));
    kos_print(msg);
    return 1;
}
