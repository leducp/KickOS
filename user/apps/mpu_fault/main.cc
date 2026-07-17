// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory-domain isolation gate, isolated in its own binary because it ends the
// process: an unprivileged domain-A thread writes its own granted region (OK),
// then writes domain B's region -- which must fault. The kernel reports "MPU
// FAULT" and shuts down. CTest asserts the marker appears (and no "did not fault").
//
// Static-data-free by construction: the worker takes the two region pointers
// through its thread ARG (a struct placed in region A, which it is granted), NOT
// through file-scope globals. Under REAL MPU enforcement an unprivileged thread
// has no access to .data/.bss, so a globals-based test would fault reading its
// own globals before it could exercise the cross-domain write. This way the only
// memory the worker touches is its code (flash, granted RX), region A (granted),
// and its own stack -- exactly the isolation the reference-pair backends enforce.
//
// Enforced in the sim (mprotect) and on HW where the MPU backend is active
// (KICKOS_HAVE_MPU). Where the MPU is a no-op (privilege-only boards), the
// cross-domain write COMPLETES and the app ends via the "no enforcement" path --
// expected there, not a failure.

#include <kickos/kos.h>
#include <kickos/sys.h>

namespace
{
    // Placed at the base of region A (granted to the worker); the worker reads it
    // from there, so no global is dereferenced.
    struct DomainArg
    {
        volatile int* own;   // a cell inside region A  -> writable
        volatile int* other; // a cell inside region B  -> NOT granted -> faults
    };

    void domainA_worker(void* arg)
    {
        DomainArg* d = static_cast<DomainArg*>(arg); // -> region A (granted)
        kos_print("[domain] A: writing my own region\n");
        *d->own = 0x1111; // granted -> ok
        kos_print("[domain] A: my region ok; writing domain B (expect fault)\n");
        *d->other = 0x2222; // not granted -> fault (sim / enforced HW)
        // Reached only where the MPU is NOT enforced (privilege-only boards). NOT
        // the "did not fault" wording CTest negative-asserts.
        kos_print("[domain] cross-domain write completed: OK where the MPU is a "
                  "no-op; an enforced backend traps this\n");
    }
}

int main(int, char**)
{
    void* rA = kos_ram_alloc(4096);
    void* rB = kos_ram_alloc(4096);
    if (rA == nullptr or rB == nullptr)
    {
        kos_print("[domain] ERROR: ram_alloc failed\n");
        return 1;
    }
    // The arg struct lives at the base of region A; point `own` past it (still in
    // A) and `other` into region B. main is the privileged root, so it may write A.
    DomainArg* d = static_cast<DomainArg*>(rA);
    d->own = reinterpret_cast<volatile int*>(static_cast<char*>(rA) + 64);
    d->other = static_cast<volatile int*>(rB);

    // Domain-A worker is unprivileged and granted only region A.
    kos::thread::spawn(domainA_worker, d, "domainA", 10, KOS_POLICY_FIFO, 0,
                       /*privileged=*/false, rA, 4096);
    // Park: the worker runs, writes A (ok), faults writing B, kernel shuts down.
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
