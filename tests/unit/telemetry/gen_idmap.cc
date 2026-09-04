// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// CI gate 3: trace-metadata de-drift. Emits the id->number maps from
// kickos::trace::ArchId (include/kickos/trace/record.h) and enum kos_syscall_nr
// (user/include/kickos/sys/abi.h): one "arch <n>" / "syscall <n>" line each.
// check_idmap.py cross-checks these against abi.h and tools/kicktrace.py.
//
// The lists below reference each enumerator BY NAME, so a removed or renamed enum value fails
// to compile here. An enumerator MISSING from the syscall list is caught by check_idmap.py,
// which parses abi.h itself.

#include <kickos/sys/abi.h>
#include <kickos/trace/record.h>

#include <cstdio>

using namespace kickos::trace;

int main()
{
    ArchId const archs[] = {
        ARCH_SIM, ARCH_ARMV7M, ARCH_ARMV6M, ARCH_XTENSA, ARCH_RX, ARCH_RISCV,
    };
    for (ArchId a : archs)
    {
        printf("arch %d\n", static_cast<int>(a));
    }

    kos_syscall_nr const calls[] = {
        KOS_SYS_KCONSOLE_WRITE, KOS_SYS_YIELD, KOS_SYS_SLEEP_NS, KOS_SYS_SEM_CREATE,
        KOS_SYS_SEM_WAIT, KOS_SYS_SEM_POST, KOS_SYS_THREAD_CREATE, KOS_SYS_EXIT,
        KOS_SYS_IRQ_INJECT, KOS_SYS_GUARD_ADDR, KOS_SYS_IRQ_ATTACH, KOS_SYS_CLOCK_NOW,
        KOS_SYS_RAM_ALLOC, KOS_SYS_IRQ_CLAIM, KOS_SYS_IRQ_WAIT, KOS_SYS_IRQ_ACK,
        KOS_SYS_HANDLE_CLOSE, KOS_SYS_IRQ_SPURIOUS, KOS_SYS_DIAG_LED_SET,
        KOS_SYS_DIAG_LED_TOGGLE, KOS_SYS_IRQ_UNMASK, KOS_SYS_CPU_CLOCK_HZ,
        KOS_SYS_MUTEX_CREATE, KOS_SYS_MUTEX_LOCK, KOS_SYS_MUTEX_UNLOCK,
        KOS_SYS_ENDPOINT_CREATE, KOS_SYS_SEND, KOS_SYS_RECV, KOS_SYS_CONSOLE_PUBLISH,
        KOS_SYS_CPU_CLOCK_SET, KOS_SYS_GRANT_PROBE, KOS_SYS_PERIPH_CLOCK_HZ,
        KOS_SYS_PINMUX_SET, KOS_SYS_CALL, KOS_SYS_REPLY, KOS_SYS_SHUTDOWN,
        KOS_SYS_MEM_SELF_GRANT, KOS_SYS_REBOOT, KOS_SYS_PERIPH_ENABLE,
        KOS_SYS_CAP_NARROW, KOS_SYS_PANIC, KOS_SYS_PERIPH_REG_WRITE,
        KOS_SYS_IRQ_NOTIFY, KOS_SYS_IRQ_DISCARD, KOS_SYS_THREAD_KILL,
        KOS_SYS_CALL_TIMED, KOS_SYS_RECV_TIMED, KOS_SYS_THREAD_JOIN,
        KOS_SYS_WAIT_LAST, KOS_SYS_SEND_TIMED, KOS_SYS_TASK_CREATE,
        KOS_SYS_TASK_KILL, KOS_SYS_THREAD_SLAY, KOS_SYS_TASK_SLAY, KOS_SYS_BENCH,
        KOS_SYS_CALL_REG, KOS_SYS_IPC_FAST_TAKEN, KOS_SYS_NEST_WITNESS,
        KOS_SYS_ASPACE_PROBE,
    KOS_SYS_FRAME_MAP,
    KOS_SYS_FRAME_UNMAP,
    KOS_SYS_AMP_ENDPOINT_CREATE,
    KOS_SYS_THREAD_SET_AFFINITY,
    KOS_SYS_TASK_SCHED_GRANT,
    KOS_SYS_SCHED_PROBE,
    };
    for (kos_syscall_nr s : calls)
    {
        printf("syscall %d\n", static_cast<int>(s));
    }
    return 0;
}
