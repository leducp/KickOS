// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A program's task entry, apart from kickos_task_entry so that only a program's image links
// exit() and what it runs.

#include <kickos/sys/task_enter.h>

#include <stdlib.h>

extern "C" void kickos_program_run(kos_table_task const* t, kos_task_args* a)
{
    exit(t->entry.program(static_cast<int>(a->argc), a->argv));
}

extern "C" void kickos_program_entry(void* args)
{
    auto const a = static_cast<kos_task_args*>(args);
    kickos_program_run(kickos_task_enter(a), a);
}
