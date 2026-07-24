// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace init-service seam. The kernel's root thread calls
// kickos_init_entry once kernel init is complete; whatever CMake target the build
// selected as KICKOS_INIT_PROVIDER supplies that symbol (default: kickos_default_init,
// a thin passthrough to the app's kickos_app_main).
//
// Lifecycle. Two postures, chosen by whether the entry RETURNS:
//   * DEFAULT / single-shot. The default init first brings the console up
//     (kickos_console_bringup_run: on a board with a userspace console driver it runs
//     the handover so the app's stdout reaches the wire through the driver; on a board
//     with none it is a no-op and the kernel console stays), then runs the plain app's
//     kickos_app_main: that main IS the program, and its return is the process exit.
//     A nonzero bring-up result short-circuits: the entry returns it WITHOUT running
//     the app, so the app never runs against a dark console. RETURNING from
//     kickos_init_entry tears the system down (root_entry flushes the console, then
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

// The default init body (runs the app's kickos_app_main). Exposed so a custom init
// provider can delegate to it. This is JUST the app-main step: it does NOT bring the
// console up (that is kickos_console_bringup_run below), so a custom init composes the
// two in whatever order it needs.
int kickos_default_init_run(int argc, char** argv);

// Run the selected board's console bring-up (see <kickos/sys/bringup.h>): on a board
// with a userspace console driver, perform the handover so the app's stdout reaches
// the wire through the driver; on a board with none, a no-op that keeps the kernel
// console. Returns 0 on success (or no-driver), or the driver's negative failure
// code. The default kickos_init_entry runs this BEFORE the app main and aborts the
// app on a nonzero result. MUST NOT use libc stdio (bring-up self-deadlock rule).
int kickos_console_bringup_run(void);

#ifdef __cplusplus
}
#endif

#endif
