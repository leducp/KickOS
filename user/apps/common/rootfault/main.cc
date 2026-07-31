// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ROOT-confinement gate, in its own binary because it ends the process. The
// complement of apps/mpu_fault (which confines a spawned CHILD): here the child's
// write is the control that must succeed, and ROOT's write is the one that must
// fault. Root holds only [app code RX, app static data RW, its own stack], so region A
// is in no region of root's: under enforcement the write traps and the kernel reports
// "MPU FAULT: task 'root' attempted write at <A>". Without enforcement it completes,
// and the run ends with the "not confined" line and a clean exit.
//
// Region A is genuinely another DOMAIN's, not merely unmapped: the child is still
// alive and parked when root writes it, so its region descriptor is live.
//
// The fault report survives a console handover: kickos_isr_fault funnels through
// kpanic_enter, which reclaims the UART from the userspace driver before printing.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>   // ksnprintf: print region A's address for the MMFAR check
#include <kickos/sys/emit.h>   // publish-aware write (kos_print is dropped once published)

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
        // Read back: the marker below is the gate's CONTROL, so it must witness the
        // write's EFFECT. Emitted after the store in program order, it would otherwise
        // print with the grant machinery never exercised.
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
    int done = kos_sem_create(0);
    if (rA == nullptr or done < 0)
    {
        emit("[rootfault] ERROR: ram_alloc / sem_create refused (authority seat?)\n");
        return 1;
    }

    // Hand A to an UNPRIVILEGED child: A becomes a live foreign domain's region, not
    // a stray arena page. Root has not touched A at this point.
    kos_cap_grant caps[] = {
        { done, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER },
    };
    int const child = kos::thread::spawn_caps(confined_child, rA, "confined", 10,
                                             caps, 1, KOS_POLICY_FIFO, 0,
                                             /*privileged=*/false, rA, 4096);
    if (child < 0)
    {
        emit("[rootfault] ERROR: child spawn refused\n");
        return 1;
    }
    kos_sem_wait(done); // the child wrote A and parked: the control half passed

    // Announce BEFORE the poke, with the address: the armv7m dump reports MMFAR but
    // no task name (kickos_armv7m_fault_report), so a capture cross-checks this line
    // against the kernel's fault line. %p, not %x: the sim is a 64-bit host, and a
    // truncated pointer would not match the kernel's address.
    char msg[96];
    ksnprintf(msg, sizeof(msg),
              "[rootfault] root: writing the child's granted region at %p (expect fault)\n",
              rA);
    emit(msg);
    *static_cast<volatile int*>(rA) = 0x2222;

    // Reached ONLY where nothing is enforced. The "NOT confined" substring is the FAIL
    // marker in tests/check_rootfault.sh.
    emit("[rootfault] cross-domain write completed: root is NOT confined "
         "(no enforcement)\n");
    return 0;
}
