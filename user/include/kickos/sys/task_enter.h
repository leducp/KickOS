// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The entry thread of a task kos_task_create started, its argument struct kos_task_args.

#ifndef KICKOS_SYS_TASK_ENTER_H
#define KICKOS_SYS_TASK_ENTER_H

#include <kickos/sys/abi.h> // kos_task_args
#include <kickos/sys/table.h>

// Records the declaration index `decl` as the one kos_self answers for this image.
extern "C" void kickos_self_enter(uintptr_t decl) __attribute__((visibility("hidden")));

// The image's start for the declaration of `args`, its drivers' bring-up included, and that
// declaration's table record.
extern "C" kos_table_task const* kickos_task_enter(kos_task_args const* args)
    __attribute__((visibility("hidden")));

// The entry thread of a task whose image holds a packaged driver, the decl_start of such an
// image: runs its start as kickos_task_enter, which hands a packaged driver's record to
// kickos_driver_task.
extern "C" [[noreturn]] void kickos_task_entry(void* args) __attribute__((visibility("hidden")));

// The entry thread of a program's task, the decl_start of a program's own image: runs its start
// as kickos_task_enter, brings up the drivers it hosts, then kickos_program_run.
extern "C" [[noreturn]] void kickos_program_entry(void* args) __attribute__((visibility("hidden")));

// Runs the program's main, `t`'s entry, with the argv of `args`, or the system's where the create
// passed none, and passes its return to exit().
extern "C" [[noreturn]] void kickos_program_run(kos_table_task const* t, kos_task_args* args)
    __attribute__((visibility("hidden")));

// A packaged driver task's entry thread: its START with the instance its table record `t` and its
// shared `block` make, a return from it ending the task. Defined by the walk's driver path.
extern "C" [[noreturn]] void kickos_driver_task(kos_table_task const* t, uintptr_t block);

// Brings up each driver whose table record names the declaration of `args` its host, as threads of
// the caller's task, the one with a ring block taking the task's shared block, ending the task
// with kos_exit(1) where one fails. Defined by the walk's driver path.
extern "C" void kickos_driver_hosted(kos_task_args const* args);

#endif
