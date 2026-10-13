// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One arm in a process of its own, for state no reset returns to its start (the console's
// ownership among it). The arm's failures fail the calling case.

#ifndef KICKOS_TESTS_UNIT_COMMON_ISOLATED_ARM_H
#define KICKOS_TESTS_UNIT_COMMON_ISOLATED_ARM_H

#include <gtest/gtest.h>

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

inline void run_isolated(void (*body)())
{
    fflush(nullptr);
    pid_t const pid = fork();
    ASSERT_NE(pid, -1) << "fork failed, the arm did not run";
    if (pid == 0)
    {
        body();
        fflush(nullptr);
        if (::testing::Test::HasFailure())
        {
            _exit(1);
        }
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "the arm died on a signal";
    ASSERT_EQ(WEXITSTATUS(status), 0) << "see the arm's own failure text above";
}

#endif
