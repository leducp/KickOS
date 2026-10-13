// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_SHUTDOWN and KOS_SYS_TERMINAL_OUTCOME over the REAL kernel/syscall/syscall.cc: each
// outcome is admitted only to a caller holding its authority, and a refusal changes nothing. A
// case that sets the terminal outcome runs in a child process: it is set once per process.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "kseam_test.h"
#include "stub_seam.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr int STATUS = 7;

            class SysShutdown : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    g_reboot_outcome = REBOOT_NONE;
                    g_console_drains = 0;
                    caller_ = seat_pool(1, 10);
                    kernel().current(kickos_kernel_core()) = caller_;
                }

                int32_t terminal(uint32_t authority, uint32_t outcome)
                {
                    cap_seat_authority(caller_, authority);
                    return static_cast<int32_t>(
                        syscall_dispatch(KOS_SYS_TERMINAL_OUTCOME, outcome, 0, 0, 0));
                }

                int32_t shutdown(uint32_t authority, uint32_t outcome)
                {
                    cap_seat_authority(caller_, authority);
                    return static_cast<int32_t>(
                        syscall_dispatch(KOS_SYS_SHUTDOWN, STATUS, outcome, 0, 0));
                }

                // A refusal reaches neither the console drain nor the chip.
                void expect_untouched()
                {
                    EXPECT_EQ(g_console_drains, 0u);
                    EXPECT_EQ(g_reboot_outcome, REBOOT_NONE);
                }

                Thread* caller_ = nullptr;
            };

            TEST_F(SysShutdown, an_unknown_outcome_is_refused_einval)
            {
                EXPECT_EQ(shutdown(CAP_AUTH_ALL, KOS_SHUTDOWN_BOOTLOADER + 1), -KOS_EINVAL);
                expect_untouched();
            }

            // Authority answers first: no outcome is examined for a caller that may not end the
            // system.
            TEST_F(SysShutdown, an_unknown_outcome_without_system_is_refused_eperm)
            {
                EXPECT_EQ(shutdown(0u, KOS_SHUTDOWN_BOOTLOADER + 1), -KOS_EPERM);
                expect_untouched();
            }

            TEST_F(SysShutdown, halt_is_refused_eperm_without_system)
            {
                EXPECT_EQ(shutdown(CAP_AUTH_ALL & ~AUTH_SYSTEM, KOS_SHUTDOWN_HALT), -KOS_EPERM);
                expect_untouched();
            }

            // kickos_terminate is the fixture's, which reports its status and exits 1.
            TEST_F(SysShutdown, halt_is_admitted_with_system)
            {
                EXPECT_EXIT(
                    {
                        dup2(STDERR_FILENO, STDOUT_FILENO);
                        shutdown(AUTH_SYSTEM, KOS_SHUTDOWN_HALT);
                    },
                    ::testing::ExitedWithCode(1), "kickos_terminate\\(7\\) ended the arm");
            }

            TEST_F(SysShutdown, reboot_is_refused_eperm_without_system)
            {
                EXPECT_EQ(shutdown(CAP_AUTH_ALL & ~AUTH_SYSTEM, KOS_SHUTDOWN_REBOOT), -KOS_EPERM);
                expect_untouched();
            }

            TEST_F(SysShutdown, reboot_is_admitted_with_system)
            {
                EXPECT_EQ(shutdown(AUTH_SYSTEM, KOS_SHUTDOWN_REBOOT), -KOS_ENOSYS);
                EXPECT_EQ(g_reboot_outcome, static_cast<uint32_t>(KOS_SHUTDOWN_REBOOT));
                EXPECT_EQ(g_console_drains, 1u);
            }

            TEST_F(SysShutdown, bootloader_is_refused_eperm_without_bootloader)
            {
                EXPECT_EQ(shutdown(CAP_AUTH_ALL & ~AUTH_BOOTLOADER, KOS_SHUTDOWN_BOOTLOADER),
                          -KOS_EPERM);
                expect_untouched();
            }

            TEST_F(SysShutdown, bootloader_is_refused_eperm_without_system)
            {
                EXPECT_EQ(shutdown(AUTH_BOOTLOADER, KOS_SHUTDOWN_BOOTLOADER), -KOS_EPERM);
                expect_untouched();
            }

            TEST_F(SysShutdown, bootloader_is_admitted_with_both)
            {
                EXPECT_EQ(shutdown(AUTH_SYSTEM | AUTH_BOOTLOADER, KOS_SHUTDOWN_BOOTLOADER),
                          -KOS_ENOSYS);
                EXPECT_EQ(g_reboot_outcome, static_cast<uint32_t>(KOS_SHUTDOWN_BOOTLOADER));
                EXPECT_EQ(g_console_drains, 1u);
            }

            constexpr uint32_t BOTH = AUTH_SYSTEM | AUTH_BOOTLOADER;

            TEST_F(SysShutdown, a_terminal_outcome_unknown_is_refused_einval)
            {
                EXPECT_EQ(terminal(CAP_AUTH_ALL, KOS_SHUTDOWN_BOOTLOADER + 1), -KOS_EINVAL);
            }

            TEST_F(SysShutdown, a_terminal_outcome_without_either_authority_is_refused_eperm)
            {
                EXPECT_EQ(terminal(CAP_AUTH_ALL & ~AUTH_BOOTLOADER, KOS_SHUTDOWN_BOOTLOADER),
                          -KOS_EPERM);
                EXPECT_EQ(terminal(AUTH_BOOTLOADER, KOS_SHUTDOWN_BOOTLOADER), -KOS_EPERM);
            }

            TEST_F(SysShutdown, a_terminal_outcome_is_set_once)
            {
                EXPECT_EXIT(
                    {
                        int32_t const first = terminal(BOTH, KOS_SHUTDOWN_HALT);
                        int32_t const second = terminal(BOTH, KOS_SHUTDOWN_BOOTLOADER);
                        fprintf(stderr, "first %d second %d\n", first, second);
                        exit(0);
                    },
                    ::testing::ExitedWithCode(0), "first 0 second -114");
            }

            // kfault_terminate is the dead-end every panic and fault ends in.
            TEST_F(SysShutdown, a_panic_with_no_terminal_outcome_halts)
            {
                EXPECT_EXIT(
                    {
                        dup2(STDERR_FILENO, STDOUT_FILENO);
                        kfault_terminate();
                    },
                    ::testing::ExitedWithCode(0), "^SEAM: arch_shutdown\\(132\\)");
            }

            TEST_F(SysShutdown, a_panic_with_the_bootloader_set_enters_it)
            {
                EXPECT_EXIT(
                    {
                        dup2(STDERR_FILENO, STDOUT_FILENO);
                        if (terminal(BOTH, KOS_SHUTDOWN_BOOTLOADER) != 0)
                        {
                            exit(3);
                        }
                        kfault_terminate();
                    },
                    ::testing::ExitedWithCode(0),
                    "SEAM: arch_reboot\\(2\\)\nSEAM: arch_shutdown\\(132\\)");
            }

            // The word seated directly, a bootloader outcome whose complement half is wrong.
            TEST_F(SysShutdown, a_word_whose_halves_disagree_halts)
            {
                EXPECT_EXIT(
                    {
                        dup2(STDERR_FILENO, STDOUT_FILENO);
                        g_terminal_outcome = 3u | (0xFFFDu << 16);
                        kfault_terminate();
                    },
                    ::testing::ExitedWithCode(0), "^SEAM: arch_shutdown\\(132\\)");
            }

            // Its control: the same outcome with the right complement reaches the chip.
            TEST_F(SysShutdown, a_word_whose_halves_agree_is_applied)
            {
                EXPECT_EXIT(
                    {
                        dup2(STDERR_FILENO, STDOUT_FILENO);
                        g_terminal_outcome = 3u | (0xFFFCu << 16);
                        kfault_terminate();
                    },
                    ::testing::ExitedWithCode(0),
                    "^SEAM: arch_reboot\\(2\\)\nSEAM: arch_shutdown\\(132\\)");
            }

            TEST_F(SysShutdown, a_halt_with_halt_set_reaches_no_reset)
            {
                EXPECT_EXIT(
                    {
                        dup2(STDERR_FILENO, STDOUT_FILENO);
                        if (terminal(BOTH, KOS_SHUTDOWN_HALT) != 0)
                        {
                            exit(3);
                        }
                        kfault_terminate();
                    },
                    ::testing::ExitedWithCode(0), "^SEAM: arch_shutdown\\(132\\)");
            }
        }
    }
}
