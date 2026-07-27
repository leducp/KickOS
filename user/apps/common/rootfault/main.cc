// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ROOT-confinement gate: the stage-2 witness that the root thread itself is inside
// the memory boundary, in its own binary because it ends the process.
//
// This is NOT the same test as apps/mpu_fault, and neither one implies the other.
// mpu_fault proves a spawned CHILD is confined to its granted region -- true on every
// enforcing board since M2, and true whether or not root is privileged. What no test
// covered is ROOT, which is the thread that runs the ctors, the board bring-up and
// main, and which holds the whole arena until KICKOS_ROOT_PRIVILEGED=OFF takes it
// away. So the run here is the reverse of mpu_fault's: the CHILD's write is the
// control that must succeed, and ROOT's write is the one that must fault.
//
// Deliberately DISCRIMINATING, in both postures, because a witness that fails the
// same way in a configuration where the property does not hold proves nothing:
//   KICKOS_ROOT_PRIVILEGED=1 -> root is in the kernel domain (whole arena), the
//                               cross-domain write COMPLETES, and the run ends with
//                               the "not confined" line and a clean exit.
//   KICKOS_ROOT_PRIVILEGED=0 -> root holds only [app code RX, app static data RW,
//     + enforcement            its own stack]; region A belongs to the child's
//                               domain and to no region of root's, so the write
//                               traps and the kernel reports
//                               "MPU FAULT: task 'root' attempted write at <A>".
// Running it in both postures is what turns the fault into evidence: the same
// binary source, the same address, one knob, two outcomes.
//
// Region A is genuinely another DOMAIN's, not merely unmapped: the child is still
// alive and parked when root writes it, so its domain is referenced and its region
// descriptor live. Root faults on memory that exists, is granted, and is not its.
//
// The fault report survives a console handover: kickos_isr_fault funnels through
// kpanic_enter, which reclaims the UART from the userspace driver before printing,
// so this works on a service-list board whose console belongs to xmcuart.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h> // ksnprintf: report region A's address, so a capture can be
                             // checked against the armv7m dump's MMFAR rather than trusted

namespace
{
    // The child's only cap (delegated cap 0 -> child table index 1). It posts it to
    // hand control back to root, then waits on it again to park forever -- nobody
    // posts a second time, so the child is still blocked, and its domain still
    // referenced, at the instant root makes the write below.
    constexpr int CH_DONE = 1;

    void confined_child(void* arg)
    {
        volatile int* own = static_cast<volatile int*>(arg); // -> region A (granted)
        *own = 0x1111;                                       // granted -> must succeed
        kos_print("[rootfault] child: wrote my own granted region\n");
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_DONE); // park: keep A's domain alive under root's write
        kos_print("[rootfault] ERROR: child unparked\n");
    }
}

int main(int, char**)
{
    // AUTH_MEMORY either way: implicit for a privileged root, and carried on the
    // authority cap kmain seats when the knob is OFF. A refusal here would mean the
    // authority seat is missing, which is a different bug from the one under test --
    // so it is reported distinctly rather than left to look like a fault that did not
    // happen.
    void* rA = kos_ram_alloc(4096);
    int done = kos_sem_create(0);
    if (rA == nullptr or done < 0)
    {
        kos_print("[rootfault] ERROR: ram_alloc / sem_create refused (authority seat?)\n");
        return 1;
    }

    // Hand A to an UNPRIVILEGED child, which makes A a live foreign domain's region
    // rather than a stray arena page. Root itself has not touched A at this point --
    // under the flip it could not, which is the whole point.
    kos_cap_grant caps[] = {
        { done, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER },
    };
    int const child = kos::thread::spawn_caps(confined_child, rA, "confined", 10,
                                             caps, 1, KOS_POLICY_FIFO, 0,
                                             /*privileged=*/false, rA, 4096);
    if (child < 0)
    {
        kos_print("[rootfault] ERROR: child spawn refused\n");
        return 1;
    }
    kos_sem_wait(done); // the child wrote A and parked: the control half passed

    // Announce BEFORE the poke, so a capture distinguishes "faulted here" from
    // "never got here" -- the two look identical in a log that only shows silence.
    // The ADDRESS is printed because the armv7m dump reports MMFAR but no task name
    // (kickos_armv7m_fault_report), so this line is what makes the two halves of the
    // capture check against each other instead of being read as a coincidence.
    // %p, not %x: the sim is a 64-bit host, and a truncated pointer would not match the
    // address the kernel's own fault line reports.
    char msg[96];
    ksnprintf(msg, sizeof(msg),
              "[rootfault] root: writing the child's granted region at %p (expect fault)\n",
              rA);
    kos_print(msg);
    *static_cast<volatile int*>(rA) = 0x2222;

    // Reached ONLY where root is privileged, or where the backend enforces nothing.
    // Deliberately not worded as a failure: it is the correct outcome for
    // KICKOS_ROOT_PRIVILEGED=1, and it is the control that proves the faulting run
    // faulted because of the flip.
    kos_print("[rootfault] cross-domain write completed: root is NOT confined "
              "(expected with KICKOS_ROOT_PRIVILEGED=1 or no enforcement)\n");
    return 0;
}
