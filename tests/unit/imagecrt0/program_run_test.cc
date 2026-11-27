// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/init.h>
#include <kickos/sys/task_enter.h>

#include <gtest/gtest.h>

#include <stdint.h>

extern "C" kos_table_task const* kickos_task_enter(kos_task_args const*)
{
    return nullptr;
}

namespace
{
    // argc, plus 100 where argv[argc] is not the vector's terminator.
    int vector_status(int argc, char** argv)
    {
        int status = argc;
        if (argv == nullptr or argv[argc] != nullptr)
        {
            status = status + 100;
        }
        return status;
    }

    void run(uintptr_t argc, char* arg1)
    {
        char arg0[] = "host";
        static char* host[] = {arg0, nullptr};
        kickos_init_args.argc = 1;
        kickos_init_args.argv = host;
        kos_table_task t{};
        t.entry.program = vector_status;
        alignas(kos_task_args) static unsigned char storage[sizeof(kos_task_args) + 3u * sizeof(char*)];
        auto const a = reinterpret_cast<kos_task_args*>(storage);
        a->argc = argc;
        a->argv[0] = arg0;
        a->argv[1] = arg1;
        a->argv[2] = nullptr;
        if (argc == 0)
        {
            a->argv[0] = nullptr;
        }
        kickos_program_run(&t, a);
    }

    TEST(ProgramRun, an_empty_vector_reaches_main_empty)
    {
        EXPECT_EXIT(run(0, nullptr), ::testing::ExitedWithCode(0), "");
    }

    TEST(ProgramRun, the_create_s_vector_reaches_main)
    {
        char arg1[] = "role";
        EXPECT_EXIT(run(2, arg1), ::testing::ExitedWithCode(2), "");
    }
}
