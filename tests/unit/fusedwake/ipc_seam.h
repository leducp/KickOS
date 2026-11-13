// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_TESTS_UNIT_FUSEDWAKE_IPC_SEAM_H
#define KICKOS_TESTS_UNIT_FUSEDWAKE_IPC_SEAM_H

#include <stdint.h>

namespace kickos::testfix
{
    // Address to reject during validation; zero rejects nothing. This lets
    // tests reject buf while accepting opts. Tests must clear it themselves;
    // KSeam reset does not clear this state.
    extern uintptr_t g_ipc_seam_refuse_rw;    // user_readable_and_writable_ok
    extern uintptr_t g_ipc_seam_refuse_write; // kaccess_to_user, by destination
    // Becomes g_ipc_seam_refuse_write at the next ep_copy: a word that turns unwritable mid-call.
    extern uintptr_t g_ipc_seam_refuse_write_from_copy;
}

#endif
