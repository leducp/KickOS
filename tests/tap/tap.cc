// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "tap.h"

#include <stdarg.h>

#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

namespace tap
{
    namespace
    {
        constexpr int kMaxTests = 64;

        struct Entry
        {
            char const* name;
            TestFn fn;
        };

        Entry g_tests[kMaxTests];
        int g_count = 0;

        bool g_failed = false;
        char g_failmsg[192];

        void emit(char const* s)
        {
            kos_print(s);
        }

        void emitf(char const* fmt, ...)
        {
            char b[224];
            va_list ap;
            va_start(ap, fmt);
            kvsnprintf(b, sizeof(b), fmt, ap);
            va_end(ap);
            emit(b);
        }
    }

    void add(char const* name, TestFn fn)
    {
        if (g_count < kMaxTests)
        {
            g_tests[g_count].name = name;
            g_tests[g_count].fn = fn;
            g_count++;
        }
    }

    void fail(char const* fmt, ...)
    {
        if (g_failed) // first failure per test wins
        {
            return;
        }
        va_list ap;
        va_start(ap, fmt);
        kvsnprintf(g_failmsg, sizeof(g_failmsg), fmt, ap);
        va_end(ap);
        g_failed = true;
    }

    int run_all()
    {
        emitf("1..%d\n", g_count);
        int failed = 0;
        for (int i = 0; i < g_count; i++)
        {
            g_failed = false;
            g_failmsg[0] = 0;
            g_tests[i].fn();
            if (g_failed)
            {
                failed++;
                emitf("not ok %d - %s # %s\n", i + 1, g_tests[i].name, g_failmsg);
            }
            else
            {
                emitf("ok %d - %s\n", i + 1, g_tests[i].name);
            }
        }
        if (failed == 0)
        {
            emit("# all tests passed\n");
        }
        else
        {
            emitf("# %d test(s) failed\n", failed);
        }
        return failed;
    }
}
