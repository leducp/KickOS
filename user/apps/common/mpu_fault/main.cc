// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory-domain isolation gate. Its own binary because it ends the process: an
// unprivileged domain-A thread writes its own granted region (OK), then writes domain B's
// region, which must fault. The kernel reports "MPU FAULT" and shuts down.
// tests/check_mpu_fault.sh owns the verdict.
//
// Static-data-free by construction: the worker takes its region base through its thread
// ARG, by value, and derives both cells from it. The only memory it touches is its code
// (flash, granted RX), region A (granted), and its own stack.
//
// The arg MUST be a value, not a struct in region A: root is not granted A, so filling a
// struct there faults in root during setup and proves nothing about the child.
//
// Where the MPU is a no-op (privilege-only boards) the cross-domain write COMPLETES and
// the app ends via the "no enforcement" path, which is expected, not a failure.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h> // ksnprintf: announce the address the gate pins the trap to
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
        volatile int* own = reinterpret_cast<volatile int*>(base + 64); // granted -> ok
        *own = 0x1111;
        // Read back: the marker below is the gate's CONTROL, so it must witness the
        // write's EFFECT. Emitted after the store in program order, it would otherwise
        // print with region A never granted at all.
        if (*own != 0x1111)
        {
            emit("[domain] ERROR: the control write did not stick\n");
            return;
        }
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
    // One block, low half granted. REGION is a multiple of every granule in the tree, so
    // the grant of (base, REGION) encodes as one descriptor on a pow2 and on a granular
    // backend alike. base + REGION is outside it.
    void* rA = kos_ram_alloc(BLOCK);
    if (rA == nullptr)
    {
        emit("[domain] ERROR: ram_alloc failed\n");
        return 1;
    }

    // Announced from root, which is not granted A and so may not touch it: the gate
    // pins the reported fault address to this one. Without the pin, a grant that never
    // happened faults on the worker's OWN write instead and prints the same banner.
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[domain] expect fault at %p\n",
              static_cast<char*>(rA) + REGION);
    emit(msg);

    kos::thread::spawn(domainA_worker, rA, "domainA", 10, KOS_POLICY_FIFO, 0,
                       /*privileged=*/false, rA, REGION);
    int idle = kos_sem_create(0);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
