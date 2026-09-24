// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Emit architecture and syscall IDs for comparison with abi.h and kicktrace.py.
// Named enumerators catch removals at compile time; check_idmap.py also
// parses abi.h to detect IDs missing from this list.

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
        KOS_SYS_IRQ_INJECT, KOS_SYS_GUARD_ADDR, KOS_SYS_NOTIFY_CREATE, KOS_SYS_CLOCK_NOW,
        KOS_SYS_RAM_ALLOC, KOS_SYS_IRQ_CLAIM, KOS_SYS_NOTIFY_WAIT, KOS_SYS_IRQ_ACK,
        KOS_SYS_HANDLE_CLOSE, KOS_SYS_IRQ_SPURIOUS, KOS_SYS_DIAG_LED_SET,
        KOS_SYS_DIAG_LED_TOGGLE, KOS_SYS_IRQ_UNMASK, KOS_SYS_CPU_CLOCK_HZ,
        KOS_SYS_MUTEX_CREATE, KOS_SYS_MUTEX_LOCK, KOS_SYS_MUTEX_UNLOCK,
        KOS_SYS_ENDPOINT_CREATE, KOS_SYS_SEND, KOS_SYS_CONSOLE_PUBLISH,
        KOS_SYS_CPU_CLOCK_SET, KOS_SYS_GRANT_PROBE, KOS_SYS_PERIPH_CLOCK_HZ,
        KOS_SYS_PINMUX_SET, KOS_SYS_CALL, KOS_SYS_REPLY, KOS_SYS_SHUTDOWN,
        KOS_SYS_MEM_SELF_GRANT, KOS_SYS_REBOOT, KOS_SYS_PERIPH_ENABLE,
        KOS_SYS_CAP_NARROW, KOS_SYS_PANIC, KOS_SYS_PERIPH_REG_WRITE,
        KOS_SYS_NOTIFY, KOS_SYS_IRQ_DISCARD, KOS_SYS_THREAD_KILL,
        KOS_SYS_CALL_TIMED, KOS_SYS_THREAD_JOIN,
        KOS_SYS_WAIT_LAST, KOS_SYS_SEND_TIMED, KOS_SYS_TASK_CREATE,
        KOS_SYS_TASK_KILL, KOS_SYS_THREAD_SLAY, KOS_SYS_TASK_SLAY, KOS_SYS_BENCH,
        KOS_SYS_CALL_REG, KOS_SYS_IPC_FAST_TAKEN, KOS_SYS_NEST_WITNESS,
        KOS_SYS_ASPACE_PROBE,
    KOS_SYS_REPLY_RECV,
    KOS_SYS_NOTIFY_BADGE,
    KOS_SYS_NOTIFY_BIND,
    KOS_SYS_NOTIFY_UNBIND,
    KOS_SYS_IRQ_BIND_NOTIFY,
    KOS_SYS_FRAME_MAP,
    KOS_SYS_FRAME_UNMAP,
    KOS_SYS_AMP_ENDPOINT_CREATE,
    KOS_SYS_THREAD_SET_AFFINITY,
    KOS_SYS_TASK_SCHED_GRANT,
    KOS_SYS_SCHED_PROBE,
    KOS_SYS_AMP_PROBE,
    KOS_SYS_DOORBELL_PROBE,
    KOS_SYS_THREAD_SELF,
    };
    for (kos_syscall_nr s : calls)
    {
        printf("syscall %d\n", static_cast<int>(s));
    }
    return 0;
}
