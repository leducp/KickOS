// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The counting harness of a self-asserting app, judged by tests/integration/check_app_arms.sh:
// `[<prefix>] ok - <arm>` per arm, `[<prefix>] ERROR: <arm>` per failure, and a closing
// `PASS (<n> arms)` or `FAIL (<n> failed)`.

#ifndef KICKOS_APPS_ARM_COUNT_H
#define KICKOS_APPS_ARM_COUNT_H

#include <stdint.h>

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace kickos::apps
{
    class ArmCount
    {
    public:
        explicit constexpr ArmCount(char const* prefix)
            : prefix_(prefix)
        {
        }

        // The arm string must fit the buffer with its prefix and newline: a truncated arm loses
        // its '\n', merges two arms onto one line and undercounts the judge's per-line tally.
        void check(bool ok, char const* what)
        {
            char msg[128];
            if (ok)
            {
                arms_ = arms_ + 1;
                ksnprintf(msg, sizeof(msg), "[%s] ok - %s\n", prefix_, what);
            }
            else
            {
                failures_ = failures_ + 1;
                ksnprintf(msg, sizeof(msg), "[%s] ERROR: %s\n", prefix_, what);
            }
            kos::print(msg);
        }

        void note_rc(char const* what, int64_t rc)
        {
            char msg[128];
            ksnprintf(msg, sizeof(msg), "[%s]   %s rc=%d\n", prefix_, what, static_cast<int>(rc));
            kos::print(msg);
        }

        void skip(char const* what)
        {
            char msg[128];
            ksnprintf(msg, sizeof(msg), "[%s] skip - %s\n", prefix_, what);
            kos::print(msg);
        }

        // The count comes from a counter and the `ok -` lines from one emit per arm: the judge
        // cross-checks the two, so output lost between them cannot read as a clean run.
        int verdict()
        {
            char msg[64];
            if (failures_ != 0)
            {
                ksnprintf(msg, sizeof(msg), "[%s] FAIL (%d failed)\n", prefix_, failures_);
                kos::print(msg);
                return 1;
            }
            ksnprintf(msg, sizeof(msg), "[%s] PASS (%d arms)\n", prefix_, arms_);
            kos::print(msg);
            return 0;
        }

    private:
        char const* prefix_;
        int arms_ = 0;
        int failures_ = 0;
    };
}

#endif
