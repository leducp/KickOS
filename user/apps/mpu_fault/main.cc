// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory-domain isolation gate, isolated in its own binary because it ends the
// process: an unprivileged domain-A thread writes its own granted region (OK),
// then writes domain B's region -- which must fault. The kernel reports "MPU
// FAULT" and shuts down. CTest asserts the marker appears (and no "did not
// fault"). Enforced in the sim via mprotect over the user-RAM arena.

#include <kickos/kos.h>
#include <kickos/sys.h>

namespace
{
    void* g_rA = nullptr;
    void* g_rB = nullptr;

    void domainA_worker(void*)
    {
        kos_print("[domain] A: writing my own region\n");
        *static_cast<volatile int*>(g_rA) = 0x1111; // granted -> ok
        kos_print("[domain] A: my region ok; writing domain B (expect fault)\n");
        *static_cast<volatile int*>(g_rB) = 0x2222; // not granted -> fault
        kos_print("[domain] ERROR: cross-domain write did not fault\n");
    }
}

int main(int, char**)
{
    g_rA = kos_ram_alloc(4096);
    g_rB = kos_ram_alloc(4096);
    if (g_rA == nullptr or g_rB == nullptr)
    {
        // Distinct marker: a null-deref would otherwise also fault as 'domainA'
        // and pass the gate without ever testing cross-domain isolation.
        kos_print("[domain] ERROR: ram_alloc failed\n");
        return 1;
    }
    // Domain-A thread is unprivileged and granted only region A.
    kos::thread::spawn(domainA_worker, nullptr, "domainA", 10, KOS_POLICY_FIFO,
                       0, /*privileged=*/false, g_rA, 4096);
    // Park (don't return -- that would exit before the worker runs): domainA
    // runs, writes A (ok), faults writing B, and the fault handler shuts down.
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
