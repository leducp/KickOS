// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The map editor's FOURTH transition, in its own binary because it ends the process: the
// kernel maps a page into an address space of its own, makes that space the running one,
// reads the page THROUGH THE HARDWARE WALK, unmaps it, and reads it again. The last read
// must be a translation fault.
//
// The whole point of running it here rather than as a TAP arm is that a fault is not a
// return value: the selftest reaches the first three transitions through the acquire pair
// and stays alive, and only a dying image can witness the fourth. A backend whose unmap
// left the translation standing answers a VALUE to that last read instead, which is what
// the ERROR lines below exist to make loud.

#include <kickos/kos.h>
#include <kickos/sys.h>

int main(int, char**)
{
    kos_print("[aspacefault] mapping, activating, unmapping, then reading\n");
    uintptr_t const rc = kos_aspace_probe(KOS_ASPACE_OP_TOUCH_UNMAPPED, 0);
    if (rc == 0)
    {
        kos_print("[aspacefault] ERROR: the scenario could not be set up\n");
        return 1;
    }
    if (rc == 1)
    {
        kos_print("[aspacefault] ERROR: the running translation never reached the frame\n");
        return 1;
    }
    kos_print("[aspacefault] ERROR: the unmapped page did not fault\n");
    return 1;
}
