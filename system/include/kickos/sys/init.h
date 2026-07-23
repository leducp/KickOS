// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace init-service seam. The kernel's root thread calls
// kickos_init_entry once kernel init is complete; whatever CMake target the build
// selected as KICKOS_INIT_PROVIDER supplies that symbol (default: kickos_default_init,
// a thin passthrough to the app's kickos_app_main).
//
// Lifecycle: RETURNING from kickos_init_entry is a single-shot system shutdown with
// that int status. A persistent init never returns (it parks or loops).
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
