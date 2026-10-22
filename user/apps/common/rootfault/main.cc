// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Task-confinement gate on the default system, in its own binary because the run ends in a
// trap: the child's write is the control that must succeed, and MAIN's write is the one that
// must fault. Main holds only [app code RX, app static data RW, its own stack], so region A,
// the domain of the task its child runs in, is in no region of main's: under enforcement the
// write traps. What that DOES depends on the posture (KICKOS_FAULT_OUTCOME): with no fault
// isolation the kernel reports "MPU FAULT: thread 'main' attempted write at <A>" and shuts
// down; where the arch opted in, main is killed ("=== THREAD FAULT === thread 'main' killed"),
// which ends its task, and the init ends the system with KOS_EXIT_FAULT. Without enforcement
// the write completes, and the run ends with the "not confined" line and a clean exit.
//
// Region A is genuinely another DOMAIN's, not merely unmapped: the child is still
// alive and parked when main writes it, so its region descriptor is live.
//
// The fault report survives a console handover either way: the panic arm goes through
// kickos_isr_fault, whose kpanic_enter reclaims the UART from the userspace driver before
// printing; the thread-kill arm prints with kprintf_fault, whose kernel-path write runs
// unconditionally (the chip backend DROPS it while a driver owns the UART, RTT does not), and
// whose record is held whole for the published endpoint's driver, which writes it.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

namespace
{
    // The child reports down main's endpoint, its only delegated capability, then parks on
    // g_hold, which nobody posts, so its domain is still referenced when main makes the write
    // below. A word the child shares with main would be its own copy on a translating board.
    constexpr kos_cap_t CH_DONE = KOS_SPAWN_DELEGATED_CAP0;
    kos_sem_t g_hold = KOS_SEM_INIT(0);

    void report_done()
    {
        uint8_t const mark = 1;
        (void)kos_send(CH_DONE, &mark, sizeof(mark), KOS_TIMEOUT_NONE);
    }

    void confined_child(void* arg)
    {
        volatile int* own = static_cast<volatile int*>(arg); // -> region A (granted)
        *own = 0x1111;                                       // granted -> must succeed
        // The marker below is the gate's CONTROL, so it must witness the write's EFFECT
        // and not merely its position in program order: read the cell back first.
        if (*own != 0x1111)
        {
            kos::print("[rootfault] ERROR: the child's own write did not stick\n");
            report_done();
            return;
        }
        kos::print("[rootfault] child: wrote my own granted region\n");
        report_done();
        kos_sem_wait(&g_hold, KOS_TIMEOUT_NONE);
        kos::print("[rootfault] ERROR: child unparked\n");
    }
}

int main(int, char**)
{
    // A refusal here means main's AUTH_MEMORY seat is missing, a different bug, so it
    // is reported distinctly.
    void* rA = kos_ram_alloc(4096);
    kos_cap_t done = KOS_CAP_NONE;
    int const done_rc = kos_endpoint_create(&done);
    if (rA == nullptr or done_rc != 0)
    {
        kos::print("[rootfault] ERROR: ram_alloc / endpoint_create refused (authority seat?)\n");
        return 1;
    }

    // Hand A to an UNPRIVILEGED child in a task of its own: A becomes a live foreign domain's
    // region, not a stray arena page. Main has not touched A at this point.
    kos_cap_grant caps[] = {
        { done, KOS_CAP_SIGNAL },
    };
    // The child outranks main on main's own core, so the yield below runs it until it blocks
    // in its send before main receives.
    uint32_t core_mask = 0;
#if KICKOS_KERNEL_CORES > 1
    if (kos::thread::pin(kos_thread_self(), 0) < 0)
    {
        kos::print("[rootfault] ERROR: main could not pin itself\n");
        return 1;
    }
    core_mask = 1u;
#endif
    auto const child = kos::thread::create_caps(confined_child, rA, "confined", 10,
                                             caps, 1, KOS_POLICY_FIFO, 0,
                                             rA, 4096, 0, nullptr,
                                             KOS_TASK_NONE, nullptr, 0, core_mask);
    if (not child.valid())
    {
        kos::print("[rootfault] ERROR: child spawn refused\n");
        return 1;
    }
    kos_yield();
    // The child wrote A: the control half passed.
    uint8_t mark = 0;
    struct kos_reply_recv_opts opts;
    kos_reply_recv_opts_init(&opts, done, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
    (void)kos_reply_recv(KOS_CAP_NONE, &mark, kos_call_lens_pack(0, sizeof(mark)), &opts);

    // Announce BEFORE the poke, with the address: the armv7m dump reports MMFAR but
    // no thread name (kickos_armv7m_fault_report), so a capture cross-checks this line
    // against the kernel's fault line. %p, not %x: the sim is a 64-bit host, and a
    // truncated pointer would not match the kernel's address.
    char msg[96];
    ksnprintf(msg, sizeof(msg),
              "[rootfault] main: writing the child's granted region at %p (expect fault)\n",
              rA);
    kos::print(msg);
    *static_cast<volatile int*>(rA) = 0x2222;

    // Reached ONLY where nothing is enforced; "NOT confined" is the gate's FAIL marker.
    kos::print("[rootfault] cross-domain write completed: main is NOT confined "
               "(no enforcement)\n");
    return 0;
}
