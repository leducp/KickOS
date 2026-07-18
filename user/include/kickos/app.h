// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The OS-agnostic application entry contract (dependency inversion, invariant
// #8). The app writes a plain
//
//     int main(int argc, char** argv)
//
// and kickos_add_application() compiles it with -Dmain=kickos_app_main so the
// app source stays portable -- the same file builds unchanged as a plain hosted
// program or on KickOS. The kernel's boot path calls kickos_app_main after init;
// its int return becomes the process exit status on the sim (a daemon-style app
// simply never returns).
//
// C++ global constructors of an app (and of any library it links, e.g. libstdc++)
// run on MCU targets from the kernel's root thread just before kickos_app_main --
// the kernel is live, so a ctor may use KickOS syscalls -- but they run on ONE
// thread in sequence: an app global ctor MUST NOT block (sleep/wait), or a
// higher-priority thread it already spawned may run before the remaining globals
// are constructed. Do blocking work inside main, not in a global ctor.
//
// This declaration is force-included into every app TU by the build. Declaring
// it extern "C" gives the -Dmain-renamed C++ `main` C language linkage, so it
// resolves to the unmangled symbol the kernel calls (a C app already matches).

#ifndef KICKOS_APP_H
#define KICKOS_APP_H

#ifdef __cplusplus
extern "C"
{
#endif

int kickos_app_main(int argc, char** argv);

// Per-app build stamp. The kernel's root trampoline calls this just before
// kickos_app_main so every app/example prints its OWN compile time -- the KickOS
// banner shows the KERNEL build, which does not move when only the app is rebuilt.
// Weak: the kernel sees only this declaration and calls through it; the definition
// below is provided by the app.
char const* kickos_app_build_stamp(void) __attribute__((weak));

// Defined ONLY in an app TU: the build force-includes this header with
// -Dmain=kickos_app_main, so `main` is defined here exactly in app-compile context,
// and __DATE__/__TIME__ then capture the APP's compile time (not the kernel's). Weak +
// force-included into every app TU, so the linker keeps one definition.
#ifdef main
__attribute__((weak)) char const* kickos_app_build_stamp(void)
{
    return __DATE__ " " __TIME__;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
