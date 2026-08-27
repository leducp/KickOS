// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The guard page below a thread stack, in its own binary because it ends the process: an
// unprivileged thread walks DOWN from its own stack one page at a time, writing a byte into
// each, and the first page below the stack must take a translation fault rather than reach
// whatever lies beneath it (docs/design-m6-mmu.md section 3.4).
//
// EVERY PROBE IS ANNOUNCED BEFORE IT IS MADE, which is what lets the gate compare the LAST
// announced address against the dump's FAR. Without that the run would only say "something
// faulted": a fault one page too high is a stack that is short by a page, and a fault one
// page too low means a page below the stack was mapped after all, which is exactly the
// neighbour-reaching this arrangement exists to prevent.
//
// The writes inside the stack MUST succeed first, and the count of announcements is what
// says so. An image that faults on its first probe witnesses nothing about a guard page.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/libc/fmt.h>

namespace
{
    // Far more pages than any stack this board configures, so a run that exhausts it has
    // found a mapping below the stack rather than run out of patience.
    constexpr unsigned PROBE_LIMIT = 64u;
}

int main(int, char**)
{
    uintptr_t const g = kos_aspace_probe(KOS_ASPACE_OP_GRANULE, 0);
    if (g == 0 or (g & (g - 1u)) != 0)
    {
        kos_print("[stackguard] ERROR: no granule to walk in\n");
        return 1;
    }
    // volatile so the address escapes and the object stays on the stack.
    volatile unsigned char local = 0;
    uintptr_t p = reinterpret_cast<uintptr_t>(&local) & ~(g - 1u);
    for (unsigned step = 0; step < PROBE_LIMIT; step++)
    {
        char msg[64];
        ksnprintf(msg, sizeof(msg), "[stackguard] touching 0x%x\n", static_cast<unsigned>(p));
        kos_print(msg);
        *reinterpret_cast<volatile unsigned char*>(p) = 0xA5u;
        p -= g;
    }
    kos_print("[stackguard] ERROR: walked past every page of the stack without faulting\n");
    return 1;
}
