// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The address-space boundary of the K-seam, for the one gate that compiles the kernel with
// KICKOS_HAVE_ASPACE=1. No board the host arch answers for translates, so the layer under
// kernel/mem is not compiled at all: this records the calls exit_current makes across it and
// keeps one installed-root cell per core, which is the whole of what the ordering claim needs.
//
// Keep this header GTEST-FREE, for the reason kfixture.h states.

#ifndef KICKOS_TESTS_UNIT_DEATHSPACE_ASPACE_SEAM_H
#define KICKOS_TESTS_UNIT_DEATHSPACE_ASPACE_SEAM_H

#include <stdint.h>

namespace kickos
{
    struct Domain;

    namespace testfix
    {
        // The seam's per-core translation base, written by the two installers below. Null is
        // "nothing installed yet", which no core is in once an arm has switched.
        struct arch_aspace* installed_on(uint32_t core);

        // Write THIS core's translation base.
        void install_here(struct arch_aspace* space);

        // The one boot root every core falls back to.
        struct arch_aspace* boot_space();

        // The space the seam answers domain_space with for `d`. Distinct per domain slot, so
        // an arm can tell one task's space from another's.
        struct arch_aspace* space_of(Domain const* d);

        // Puts every core back on the boot root and clears the counters below. Called by the
        // gate's own SetUp, after the fixture's reset().
        void aspace_seam_reset();

        // ustack_free calls, and the bytes the last one gave back.
        extern uint32_t g_ustack_frees;
        extern uintptr_t g_ustack_free_base;
    }
}

#endif
