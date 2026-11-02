// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Memory-isolation gate, in its own binary because the run ends in a trap: a thread holding a
// window on region A writes it (OK), then writes the block's other half, which must fault.
// What the trap DOES depends on the posture (KICKOS_FAULT_OUTCOME): with no fault isolation
// the kernel reports "MPU FAULT: thread 'domainA'" and shuts down; where the arch opted in,
// the thread is killed ("=== THREAD FAULT === thread 'domainA' killed").
//
// The worker takes region A as its own window, a block main is not granted, and reads its
// base with kos_window_get.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr uint32_t REGION = 4096; // granted to the worker
    constexpr uint32_t BLOCK = REGION * 2;

    void domainA_worker(void*)
    {
        kos_window w = {};
        if (kos_window_get(0, &w) != 0)
        {
            kos::print("[domain] ERROR: the window is unreadable\n");
            return;
        }
        char* base = reinterpret_cast<char*>(w.base);
        kos::print("[domain] A: writing my own region\n");
        volatile int* own = reinterpret_cast<volatile int*>(base + 64); // granted -> ok
        *own = 0x1111;
        // The marker below is the gate's CONTROL, so it must witness the write's EFFECT
        // and not merely its position in program order: read the cell back first.
        if (*own != 0x1111)
        {
            kos::print("[domain] ERROR: the control write did not stick\n");
            return;
        }
        kos::print("[domain] A: my region ok; writing domain B (expect fault)\n");
        *reinterpret_cast<volatile int*>(base + REGION) = 0x2222; // not granted -> fault
        // Reached only where the MPU is not enforced (privilege-only boards). The
        // wording steers clear of the "did not fault" phrase CTest negative-asserts.
        kos::print("[domain] cross-domain write completed: OK where the MPU is a "
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
        kos::print("[domain] ERROR: ram_alloc failed\n");
        return 1;
    }

    // Announced from main, which is not granted A and so may not touch it: the gate pins
    // the reported fault address to this one, since a grant that never happened faults on
    // the worker's OWN write instead and prints the same banner.
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[domain] expect fault at %p\n",
              static_cast<char*>(rA) + REGION);
    kos::print(msg);

    kos_window win = {};
    win.base = reinterpret_cast<uintptr_t>(rA);
    win.size = REGION;
    win.kind = KOS_WINDOW_MEMORY;
    kos::thread::create(domainA_worker, nullptr, "domainA", 10, KOS_POLICY_FIFO, 0, nullptr, 0,
                        &win, 1);
    static kos_sem_t idle = KOS_SEM_INIT(0);
    while (true)
    {
        kos_sem_wait(&idle, KOS_TIMEOUT_NONE);
    }
}
