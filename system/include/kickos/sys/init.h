// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace init-service seam. The kernel's root thread calls kickos_init_entry once
// kernel init is complete, taking it from the image's system target (KickOS::init), which walks
// the composition's table. RETURNING from kickos_init_entry tears the system down (root_entry
// flushes the console, then arch_shutdown(status)).
//
// The init runs in an UNPRIVILEGED root holding a full authority word, and grants each task the
// authority its composition declares.
//
// App and libstdc++ global constructors run in the kernel root thread BEFORE
// kickos_init_entry is entered, so a constructor cannot depend on anything init
// brings up.

#ifndef KICKOS_SYS_INIT_H
#define KICKOS_SYS_INIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// The symbol the kernel boot path calls after kernel init.
int kickos_init_entry(int argc, char** argv);

struct kos_table_task;

// KickOS::main's entry, a composition's `entry: kickos_main`: runs kickos_app_main with root's
// kickos_init_args below as the task's entry thread and passes its status to exit().
void kickos_main(struct kos_table_task const* self);

// The root thread's own entry, which kmain hands to thread_create. It walks the app's ctor window
// at KICKOS_PRIO_MAX, the priority the kernel creates root at, and then calls kickos_init_entry
// above, whose walk lowers root as its first act.
//
// MUST stay defined app-side (libkickos_user.a): root is UNPRIVILEGED from its first
// instruction, so on a translating board this text is fetched at EL0, where kernel-half text is
// unreachable at any address.
//
// Hidden visibility is required: kmain takes this function's address, and x86_64 reaches the
// address of a default-visibility external function GOT-indirect while `ld -m i386pep` builds no
// global offset table (tools/check-x86_64-no-got.sh).
void kickos_root_entry(void* arg) __attribute__((visibility("hidden")));

// The kernel -> init argument handoff. kmain fills it; the root thread reads it immediately before
// calling kickos_init_entry above.
//
// MUST stay defined app-side (libkickos_user.a), so the enforcement linker scripts route it into
// the .appdata/.appbss grant every unprivileged thread holds (arch_domain_static_regions).
// Kernel-side storage, or a kmain stack local outside the arena, faults an unprivileged root
// before its first statement on every enforcing board.
//
// argv is null (argc 0) on MCU. On the hosted sim it points into the host process's own argv,
// which no grant covers.
struct kos_init_args
{
    int argc;
    char** argv;
};

extern struct kos_init_args kickos_init_args;

#ifdef __cplusplus
}
#endif

#endif
