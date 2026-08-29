// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The map editor's FOURTH transition, taken at the UNPRIVILEGED level: the kernel maps a page
// into this process's own space and seeds it, the process reads it through the running
// translation, the kernel unmaps it, and the process reads it again. The last read must fault.
//
// The reads are here rather than in the kernel because this port never sets sstatus.SUM: a
// supervisor load of a page carrying the unprivileged bit faults whether the leaf stands or not.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdint.h>

int main(int, char**)
{
    kos_print("[aspaceufault] mapping a page into my own space\n");
    uintptr_t const va = kos_aspace_probe(KOS_ASPACE_OP_MAP_HERE, 0);
    if (va == 0)
    {
        kos_print("[aspaceufault] ERROR: the scenario could not be set up\n");
        return 1;
    }
    volatile uint32_t* const page = reinterpret_cast<volatile uint32_t*>(va);
    // The word the kernel seeded through the acquire pair, read back here through the hardware
    // walk, then handed back below.
    uint32_t const seen = *page;
    kos_print("[aspaceufault] the mapping answers, unmapping\n");
    if (kos_aspace_probe(KOS_ASPACE_OP_UNMAP_HERE, seen) == 0)
    {
        kos_print("[aspaceufault] ERROR: the running translation never reached the frame\n");
        return 1;
    }
    uint32_t const after = *page;
    (void)after;
    kos_print("[aspaceufault] ERROR: the unmapped page did not fault\n");
    return 1;
}
