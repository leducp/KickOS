// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The real-hardware dead end: the diagnostic LED's pin may be a task's GPIO until the panic, so
// kfault_terminate takes it back before its first edge.

#include <kickos/arch/arch.h>
#include <kickos/kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kseam_test.h"

extern "C"
{
    void kpanic_enter(void)
    {
    }

    void arch_diag_led_init(void)
    {
        printf("SEAM: arch_diag_led_init\n");
    }

    void arch_diag_led_set(int on)
    {
        printf("SEAM: arch_diag_led_set(%d)\n", on);
        exit(0);
    }
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class DeadEndLed : public KSeam
            {
            };

            TEST_F(DeadEndLed, the_dead_end_takes_the_led_back_before_its_first_edge)
            {
                EXPECT_EXIT(
                    {
                        dup2(STDERR_FILENO, STDOUT_FILENO);
                        kfault_terminate();
                    },
                    ::testing::ExitedWithCode(0), "^SEAM: arch_diag_led_init\nSEAM: arch_diag_led_set\\(1\\)");
            }
        }
    }
}
