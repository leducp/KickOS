// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Drives the deepest syscall descent userspace can reach on its own kernel block, then panics
// so the block's high-water is read out.
//
// The block's fill records a MAXIMUM over everything that ran on the slot, so the deep spawn
// and the panic need not be one chain to both count. What that maximum cannot contain is the
// two stacked: the panic tail on top of the spawn's frame.

#ifndef KICKOS_SD_MODE
#error "KICKOS_SD_MODE must be set by this app's CMakeLists"
#endif

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h>

using kickos::emit;

namespace
{
    constexpr size_t SD_GRANT = 1024;

#if KICKOS_SD_MODE != 0
    // tests/integration/check_stackdepth.sh greps both of these. Change one there too.
    char const SD_RAN[] = "[stackdepth] spawn arm ran\n";
    char const SD_NOT_RUN[] = "[stackdepth] SPAWN ARM DID NOT RUN\n";

    void worker(void*)
    {
    }
#endif
}

int main(int, char**)
{
#if KICKOS_SD_MODE == 0
    // The panic arm alone: the spawn arm's contribution is the difference between the two
    // images.
    emit("[stackdepth] panic arm only\n");
    kos_panic("[stackdepth] read the block out");
#else
    emit("[stackdepth] driving the grant-carrying spawn\n");

    // The grant is what reaches task_for -> domain_for -> grant_region_admissible ->
    // grant_hits_reserved. A spawn without one stops short of that subtree.
    void* const block = kos_ram_alloc(SD_GRANT);
    if (block == nullptr)
    {
        emit("[stackdepth] no RAM for the grant\n");
        emit(SD_NOT_RUN);
        return 1;
    }

    kos_thread_params p = {};
    p.entry = worker;
    p.name = "sdw";
    p.prio = 10;
    p.mem_base = block;
    p.mem_size = SD_GRANT;

    kos_thread_t t;
    int const rc = kos_thread_create(&p, &t);
    char b[64];
    ksnprintf(b, sizeof(b), "[stackdepth] spawn rc=%d\n", rc);
    emit(b);
    if (rc != 0)
    {
        // The kernel prints its high-water line whatever this arm reached, so an arm that
        // never entered the subtree has to say so itself.
        emit(SD_NOT_RUN);
        return 1;
    }
    emit(SD_RAN);

    kos_panic("[stackdepth] read the block out");
#endif
}
