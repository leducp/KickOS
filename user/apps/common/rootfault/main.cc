// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ROOT-confinement gate, in its own binary because the run ends in a trap: the child's
// write is the control that must succeed, and ROOT's write is the one that must fault.
// Root holds only [app code RX, app static data RW, its own stack], so region A
// is in no region of root's: under enforcement the write traps. What that DOES depends
// on the posture (KICKOS_FAULT_OUTCOME): with no fault isolation the kernel reports
// "MPU FAULT: thread 'root' attempted write at <A>" and shuts down; where the arch opted
// in, root itself is killed ("=== THREAD FAULT === thread 'root' killed") and the parked
// child keeps the system alive. Without enforcement the write completes, and the run ends
// with the "not confined" line and a clean exit.
//
// Region A is genuinely another DOMAIN's, not merely unmapped: the child is still
// alive and parked when root writes it, so its region descriptor is live.
//
// The fault report survives a console handover either way: the panic arm goes through
// kickos_isr_fault, whose kpanic_enter reclaims the UART from the userspace driver before
// printing; the thread-kill arm prints with kprintf_fault, whose kernel-path write runs
// FIRST and unconditionally (the chip backend DROPS it while a driver owns the UART, RTT
// does not), then delivers to the published endpoint and forces the chip path back open
// only when that delivery reaches nobody.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>   // emit: kos_print is dropped once the console is published

using kickos::emit;

namespace
{
    // The child's only cap (delegated cap 0 lands at child table index 1). It posts,
    // then waits again and parks: nobody posts twice, so its domain is still
    // referenced when root makes the write below.
    constexpr int CH_DONE = 1;

    void confined_child(void* arg)
    {
        volatile int* own = static_cast<volatile int*>(arg); // -> region A (granted)
        *own = 0x1111;                                       // granted -> must succeed
        // The marker below is the gate's CONTROL, so it must witness the write's EFFECT
        // and not merely its position in program order: read the cell back first.
        if (*own != 0x1111)
        {
            emit("[rootfault] ERROR: the child's own write did not stick\n");
            kos_sem_post(CH_DONE);
            return;
        }
        emit("[rootfault] child: wrote my own granted region\n");
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_DONE); // park: keep A's domain alive under root's write
        emit("[rootfault] ERROR: child unparked\n");
    }
}

int main(int, char**)
{
    // A refusal here means root's AUTH_MEMORY seat is missing, a different bug, so it
    // is reported distinctly.
    void* rA = kos_ram_alloc(4096);
    kos_cap_t done = KOS_CAP_NONE;
    int const done_rc = kos_sem_create(0, &done);
    if (rA == nullptr or done_rc != 0)
    {
        emit("[rootfault] ERROR: ram_alloc / sem_create refused (authority seat?)\n");
        return 1;
    }

    // Hand A to an UNPRIVILEGED child: A becomes a live foreign domain's region, not
    // a stray arena page. Root has not touched A at this point.
    kos_cap_grant caps[] = {
        { done, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER },
    };
    auto const child = kos::thread::create_caps(confined_child, rA, "confined", 10,
                                             caps, 1, KOS_POLICY_FIFO, 0,
                                             /*privileged=*/false, rA, 4096);
    if (not child.valid())
    {
        emit("[rootfault] ERROR: child spawn refused\n");
        return 1;
    }
    kos_sem_wait(done); // the child wrote A and parked: the control half passed

    // Announce BEFORE the poke, with the address: the armv7m dump reports MMFAR but
    // no thread name (kickos_armv7m_fault_report), so a capture cross-checks this line
    // against the kernel's fault line. %p, not %x: the sim is a 64-bit host, and a
    // truncated pointer would not match the kernel's address.
    char msg[96];
    ksnprintf(msg, sizeof(msg),
              "[rootfault] root: writing the child's granted region at %p (expect fault)\n",
              rA);
    emit(msg);
    *static_cast<volatile int*>(rA) = 0x2222;

    // Reached ONLY where nothing is enforced; "NOT confined" is the gate's FAIL marker.
    emit("[rootfault] cross-domain write completed: root is NOT confined "
         "(no enforcement)\n");
    return 0;
}
