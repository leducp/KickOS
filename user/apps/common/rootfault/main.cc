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
    // Delegated cap i lands at child table index i+1. The child parks on CH_HOLD, which
    // nobody posts, so its domain is still referenced when main makes the write below. It
    // must not park on CH_DONE: until main is parked there, that wait takes back the child's
    // own post and main waits forever.
    constexpr int CH_DONE = 1;
    constexpr int CH_HOLD = 2;

    void confined_child(void* arg)
    {
        volatile int* own = static_cast<volatile int*>(arg); // -> region A (granted)
        *own = 0x1111;                                       // granted -> must succeed
        // The marker below is the gate's CONTROL, so it must witness the write's EFFECT
        // and not merely its position in program order: read the cell back first.
        if (*own != 0x1111)
        {
            kos::print("[rootfault] ERROR: the child's own write did not stick\n");
            kos_sem_post(CH_DONE);
            return;
        }
        kos::print("[rootfault] child: wrote my own granted region\n");
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_HOLD);
        kos::print("[rootfault] ERROR: child unparked\n");
    }
}

int main(int, char**)
{
    // A refusal here means main's AUTH_MEMORY seat is missing, a different bug, so it
    // is reported distinctly.
    void* rA = kos_ram_alloc(4096);
    kos_cap_t done = KOS_CAP_NONE;
    kos_cap_t hold = KOS_CAP_NONE;
    int const done_rc = kos_sem_create(0, &done);
    int const hold_rc = kos_sem_create(0, &hold);
    if (rA == nullptr or done_rc != 0 or hold_rc != 0)
    {
        kos::print("[rootfault] ERROR: ram_alloc / sem_create refused (authority seat?)\n");
        return 1;
    }

    // Hand A to an UNPRIVILEGED child in a task of its own: A becomes a live foreign domain's
    // region, not a stray arena page. Main has not touched A at this point.
    kos_cap_grant caps[] = {
        { done, KOS_CAP_SIGNAL },
        { hold, KOS_CAP_WAIT },
    };
    // The child outranks main on main's own core, so the yield below runs it until it blocks
    // before main waits: the order in which a child parked on CH_DONE would take back its own
    // post, so that mistake hangs every run instead of some.
    uint32_t core_mask = 0;
#if KICKOS_KERNEL_CORES > 1
    if (kos::thread::pin(kos_thread_self(), 0) != 0)
    {
        kos::print("[rootfault] ERROR: main could not pin itself\n");
        return 1;
    }
    core_mask = 1u;
#endif
    auto const child = kos::thread::create_caps(confined_child, rA, "confined", 10,
                                             caps, 2, KOS_POLICY_FIFO, 0,
                                             /*privileged=*/false, rA, 4096, 0, nullptr,
                                             KOS_TASK_NONE, nullptr, 0, core_mask);
    if (not child.valid())
    {
        kos::print("[rootfault] ERROR: child spawn refused\n");
        return 1;
    }
    kos_yield();
    kos_sem_wait(done); // the child wrote A: the control half passed

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
