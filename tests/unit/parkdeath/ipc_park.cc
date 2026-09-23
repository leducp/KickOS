// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The IPC park, over the real kernel/syscall/syscall_ipc.cc at two kernel cores: a cancel its
// prologue honours must leave the capability sweep exactly as preemptible as an ordinary exit
// leaves it, the kernel lock free in every gap.

#include <kickos/sys/abi.h>

#include "park_sweep.h"
#include "syscall_internal.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            using namespace parksweep;

            class CancelledIpcPark : public KSeam
            {
            };

            uint8_t g_msg[8] = {};

            void send_a_message()
            {
                (void)endpoint_send(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(g_msg),
                                    sizeof(g_msg), KOS_TIMEOUT_NONE);
            }
        }

        TEST_F(CancelledIpcPark, a_send_sweeps_as_an_exit_does)
        {
            Sweep const reference = reference_sweep();
            Thread* const victim = cancelled_thread();
            Sweep const parked = sweep_of(send_a_message);
            expect_same_sweep(reference, parked, victim);
        }
    }
}
