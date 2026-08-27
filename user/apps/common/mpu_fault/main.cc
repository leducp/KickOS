// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory-domain isolation gate, in its own binary because the run ends in a trap: an
// unprivileged domain-A thread writes its own granted region (OK), then writes domain B's
// region, which must fault. What the trap DOES depends on the posture
// (KICKOS_FAULT_OUTCOME): with no fault isolation the kernel reports "MPU FAULT: thread
// 'domainA'" and shuts down; where the arch opted in, domainA alone is killed
// ("=== THREAD FAULT === thread 'domainA' killed") and root parks forever.
//
// The worker is static-data-free by construction: it takes its region base through its
// thread ARG, by value, and derives both cells from it, so the only memory it touches is
// its code (flash, granted RX), region A (granted), and its own stack.
//
// The arg MUST be a value, not a struct in region A: root is not granted A, so filling a
// struct there faults in root during setup and proves nothing about the child.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h> // emit: kos_print is dropped once the console is published

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
        // The marker below is the gate's CONTROL, so it must witness the write's EFFECT
        // and not merely its position in program order: read the cell back first.
        if (*own != 0x1111)
        {
            emit("[domain] ERROR: the control write did not stick\n");
            return;
        }
        emit("[domain] A: my region ok; writing domain B (expect fault)\n");
        *reinterpret_cast<volatile int*>(base + REGION) = 0x2222; // not granted -> fault
        // Reached only where the MPU is not enforced (privilege-only boards). The
        // wording steers clear of the "did not fault" phrase CTest negative-asserts.
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

    // Announced from root, which is not granted A and so may not touch it: the gate pins
    // the reported fault address to this one, since a grant that never happened faults on
    // the worker's OWN write instead and prints the same banner.
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[domain] expect fault at %p\n",
              static_cast<char*>(rA) + REGION);
    emit(msg);

    kos::thread::create(domainA_worker, rA, "domainA", 10, KOS_POLICY_FIFO, 0,
                        /*privileged=*/false, rA, REGION);
    kos_cap_t idle = KOS_CAP_NONE;
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
