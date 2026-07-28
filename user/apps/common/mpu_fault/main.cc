// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory-domain isolation gate, isolated in its own binary because it ends the
// process: an unprivileged domain-A thread writes its own granted region (OK),
// then writes domain B's region -- which must fault. The kernel reports "MPU
// FAULT" and shuts down. CTest asserts the marker appears (and no "did not fault").
//
// Static-data-free by construction: the worker takes its region base through its
// thread ARG, by value, and derives both cells from it. The only memory it touches is
// its code (flash, granted RX), region A (granted), and its own stack.
//
// The arg is a value, not a struct in region A: under KICKOS_ROOT_PRIVILEGED=0 root is
// not granted A, so filling a struct there would fault in root during setup and prove
// nothing about the child.
//
// Enforced in the sim (mprotect) and on HW where the MPU backend is active
// (KICKOS_HAVE_MPU). Where the MPU is a no-op (privilege-only boards), the
// cross-domain write COMPLETES and the app ends via the "no enforcement" path --
// expected there, not a failure.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h> // publish-aware write (kos_print is dropped once published)

using kickos::emit;

namespace
{
    constexpr uint32_t REGION = 4096; // granted to the worker
    constexpr uint32_t BLOCK = REGION * 2;

    void domainA_worker(void* arg)
    {
        char* base = static_cast<char*>(arg); // region A base, by value
        emit("[domain] A: writing my own region\n");
        *reinterpret_cast<volatile int*>(base + 64) = 0x1111; // granted -> ok
        emit("[domain] A: my region ok; writing domain B (expect fault)\n");
        *reinterpret_cast<volatile int*>(base + REGION) = 0x2222; // not granted -> fault
        // Reached only where the MPU is NOT enforced (privilege-only boards). NOT
        // the "did not fault" wording CTest negative-asserts.
        emit("[domain] cross-domain write completed: OK where the MPU is a "
             "no-op; an enforced backend traps this\n");
    }
}

int main(int, char**)
{
    // One block, low half granted. kos_ram_alloc returns a base naturally aligned to the
    // rounded 8 KiB block, so it is also 4 KiB-aligned and the grant encodes as one
    // descriptor. base + REGION is outside it.
    void* rA = kos_ram_alloc(BLOCK);
    if (rA == nullptr)
    {
        emit("[domain] ERROR: ram_alloc failed\n");
        return 1;
    }

    // Domain-A worker is unprivileged and granted only the low half.
    kos::thread::spawn(domainA_worker, rA, "domainA", 10, KOS_POLICY_FIFO, 0,
                       /*privileged=*/false, rA, REGION);
    // Park: the worker runs, writes A (ok), faults writing B, kernel shuts down.
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
