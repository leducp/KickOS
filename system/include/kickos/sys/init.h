// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace init-service seam. The kernel's root thread calls
// kickos_init_entry once kernel init is complete; whatever CMake target the build
// selected as KICKOS_INIT_PROVIDER supplies that symbol (default: kickos_default_init,
// a thin passthrough to the app's kickos_app_main).
//
// Lifecycle. Two postures, chosen by whether the entry RETURNS:
//   * DEFAULT / single-shot. The default init first walks the board's service list
//     (kickos_service_list_run: on a board with a userspace console driver the console
//     is the list's first KOS_SVC_CONSOLE entry, so the app's stdout reaches the wire
//     through the driver; on a board with an empty list the kernel console stays), then
//     runs the plain app's kickos_app_main: that main IS the program, and its return is
//     the process exit. A nonzero bring-up result short-circuits: the entry returns it
//     WITHOUT running the app, so the app never runs against a dark console. RETURNING
//     from kickos_init_entry tears the system down (root_entry flushes the console, then
//     arch_shutdown(status)). Fine for a batch/demo/self-test app.
//   * REAL service-spawning init. An init that brings up services (owns the
//     console-endpoint chain, respawns drivers, performs future well-known publishes)
//     must PERSIST: it parks (a sleep loop or a wait on a semaphore nobody posts)
//     and NEVER returns. Returning would exit, taking down every service it spawned.
//     Persistence, not any registration act, is what keeps the system alive.
//
// Privilege posture: the app main / init body currently runs in the PRIVILEGED kernel
// root thread. A future unprivileged-main init (dropping the root thread to user mode
// before calling the entry) must be OPT-IN build config, never a silent flip: existing
// apps assume privileged main (e.g. kos_console_publish is privileged-only), so an
// implicit demotion would break them.
//
// Ordering: app and libstdc++ global constructors run in the kernel root thread
// BEFORE kickos_init_entry is entered, so a custom init must not assume they can
// depend on anything that init itself brings up.
//
// Overriding: a power user names their own target in KICKOS_INIT_PROVIDER and may
// call kickos_default_init_run to reuse the default body (run the app's main).

#ifndef KICKOS_SYS_INIT_H
#define KICKOS_SYS_INIT_H

#ifdef __cplusplus
extern "C"
{
#endif

// The symbol the kernel boot path calls after kernel init.
int kickos_init_entry(int argc, char** argv);

// The kernel -> init argument handoff. kmain fills it; the root thread reads it
// immediately before calling kickos_init_entry above.
//
// It lives HERE, app-side, because of where the reader will run. The object is
// defined in libkickos_user.a, and every enforcement linker script captures only the
// closed KickOS-owned set (kernel/arch/chip/lib) into kernel .data/.bss and routes
// everything else into the .appdata/.appbss grant -- which is a region of every
// unprivileged thread (arch_domain_static_regions). The handoff used to be a kmain
// stack local instead, and the boot stack is OUTSIDE the arena: an unprivileged root
// would fault reading it, on every enforcing board, before its first statement.
//
// argv is null (argc 0) on MCU. On the hosted sim it points into the host process's
// own argv, which no grant covers -- the sim does not enforce non-arena regions, so
// this is exactly the case its coverage misses.
struct kos_init_args
{
    int argc;
    char** argv;
};

extern struct kos_init_args kickos_init_args;

// The default init body (runs the app's kickos_app_main). Exposed so a custom init
// provider can delegate to it. This is JUST the app-main step: it does NOT bring the
// service list up (that is kickos_service_list_run below), so a custom init composes
// the two in whatever order it needs.
int kickos_default_init_run(int argc, char** argv);

// Run the selected board's service list (see <kickos/sys/service.h>): walk each
// entry's start() in array order. On a board with a userspace console driver the
// console is the first KOS_SVC_CONSOLE entry, so this is the sole userspace-console
// bring-up path. Returns 0 on success (or empty list), or the first failing entry's
// negative code. The default kickos_init_entry runs this AFTER the pin map and BEFORE
// the app main, aborting the app on a nonzero result. MUST NOT use libc stdio (bring-up
// self-deadlock rule).
int kickos_service_list_run(void);

// Apply the selected board's pin map (see <kickos/sys/pinmap.h>) BEFORE the service
// list: the DAG middle of the clock->pinmux->service->app chain. A board with an
// empty map (count = 0) is a no-op. Returns 0 on success, or the first failing
// entry's negative rc. The default kickos_init_entry runs this FIRST and aborts the
// app on a nonzero result. MUST NOT use libc stdio (same bring-up rule as above).
int kickos_pinmux_run(void);

#ifdef __cplusplus
}
#endif

#endif
