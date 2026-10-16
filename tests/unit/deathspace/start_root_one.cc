// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// sched::start at ONE kernel core, where a switch to a spaceless thread keeps whatever root the
// core holds: a core whose first thread has no space must still leave the root it came up on.
// The seam stands in for that bring-up root with a space installed before the start.

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "aspace_seam.h"
#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class StartRootOneCore : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    aspace_seam_reset();
                }
            };

            static_assert(KICKOS_KERNEL_CORES == 1,
                          "above one core every switch to a spaceless thread installs it");

            TEST_F(StartRootOneCore, a_core_starting_on_a_spaceless_thread_leaves_the_root_it_held)
            {
                Thread* const t = seat_pool(1, 5);
                join_task(t, task(0));
                install_for(t);
                ASSERT_NE(installed_on(0), nullptr);
                ASSERT_NE(installed_on(0), boot_space());

                // The fixture's reset starts the scheduler on its idle thread, which has no space.
                reset();
                EXPECT_EQ(installed_on(0), boot_space());
            }
        }
    }
}
