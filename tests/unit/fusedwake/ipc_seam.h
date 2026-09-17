// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_TESTS_UNIT_FUSEDWAKE_IPC_SEAM_H
#define KICKOS_TESTS_UNIT_FUSEDWAKE_IPC_SEAM_H

#include <stdint.h>

namespace kickos::testfix
{
    // The address layer answers by fiat in ipc_seam.cc, so an arm that needs a REFUSAL names
    // the one address to refuse. Keyed by address and not by a one-shot count: one call
    // validates `opts` and then `buf`, and one arm has to refuse the second while the first
    // still passes. Zero refuses nothing.
    //
    // The state survives the KSeam fixture's reset(), which knows nothing about this seam, so
    // an arm that sets one clears it before it returns.
    extern uintptr_t g_ipc_seam_refuse_rw;    // user_readable_and_writable_ok
    extern uintptr_t g_ipc_seam_refuse_write; // kaccess_to_user, by destination
}

#endif
