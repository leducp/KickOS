// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace init-service seam. The kernel's root thread calls
// kickos_init_entry once kernel init is complete; whatever CMake target the build
// selected as KICKOS_INIT_PROVIDER supplies that symbol (default: kickos_default_init,
// a thin passthrough to the app's kickos_app_main).
//
// Lifecycle -- two postures, chosen by whether the entry RETURNS:
//   * DEFAULT / single-shot. The default init is a thin passthrough to the plain
//     app's kickos_app_main: that main IS the program, and its return is the process
//     exit. RETURNING from kickos_init_entry tears the system down (root_entry flushes
//     the console, then arch_shutdown(status)). Fine for a batch/demo/self-test app.
//   * REAL service-spawning init. An init that brings up services (owns the
//     console-endpoint chain, respawns drivers, performs future well-known publishes)
//     must PERSIST: it parks (a sleep loop or a wait on a semaphore nobody posts)
//     and NEVER returns. Returning would exit -- taking down every service it spawned.
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
// provider can delegate to it.
int kickos_default_init_run(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif
