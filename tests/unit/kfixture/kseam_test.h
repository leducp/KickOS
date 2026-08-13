// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The GoogleTest layer over the K-seam fixture. Separate from kfixture.h for a link reason
// stated there: the fixture library is built -fno-exceptions -fno-rtti and gtest's headers
// configure themselves from those flags.

#ifndef KICKOS_KSEAM_TEST_H
#define KICKOS_KSEAM_TEST_H

#include <string>

#include <gtest/gtest.h>

#include "kfixture.h"

namespace kickos
{
    namespace testfix
    {
        // A gate derives its own named subclass, because TEST_F takes the fixture CLASS name as
        // the ctest suite name.
        class KSeam : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                reset();
            }
        };
    }
}

// `msg` is a diag:: catalogue entry, never a sentence: KICKOS_DIAG_TERSE picks the column and a
// hardcoded sentence would gate the verbose build only.
#define KICKOS_EXPECT_PANIC(stmt, msg)                                                            \
    EXPECT_DEATH(                                                                                 \
        {                                                                                         \
            ::kickos::testfix::fold_stdout_into_stderr();                                          \
            stmt;                                                                                 \
        },                                                                                        \
        std::string("KERNEL PANIC: ") + (msg))

// The other way an arm ends: a fixture self-diagnostic, which exits rather than returning a
// state no assertion could interpret. `msg` is a substring of the refusal, so a diagnostic
// naming a thread id can still be matched.
#define KICKOS_EXPECT_FIXTURE_REFUSAL(stmt, msg)                                                  \
    EXPECT_DEATH(                                                                                 \
        {                                                                                         \
            ::kickos::testfix::fold_stdout_into_stderr();                                          \
            stmt;                                                                                 \
        },                                                                                        \
        std::string("FIXTURE FAIL: ") + (msg))

#endif
