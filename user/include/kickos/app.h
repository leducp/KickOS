// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The OS-agnostic application entry contract (dependency inversion, invariant #8). The app writes
// a plain `int main(int argc, char** argv)`; linking KickOS::kernel, or the `kickos` and
// `kickos_cxx` leaves, compiles it with -Dmain=kickos_app_main. Under a system target,
// kickos_main (KickOS::main), the entry a composition names as `entry: kickos_main` and the
// default composition names for its `main` task, calls kickos_app_main as that task's entry
// thread and passes its return to exit(), so the system ends with it where the composition
// `ends` on that task. Under the `kickos` and `kickos_cxx` leaves the default init calls it on
// root's thread, and its return becomes the process exit status on the sim.
//
// App and library C++ global ctors run on MCU targets from the kernel's root thread before
// kickos_app_main runs, on ONE thread in sequence, and before the init starts any task. An app
// global ctor MUST NOT block (sleep/wait), or a higher-priority thread it already spawned may run
// before the remaining globals are constructed. Do blocking work inside main.
//
// This declaration is force-included into every app TU by the build. extern "C" gives the
// -Dmain-renamed C++ `main` C language linkage, so it resolves to the unmangled symbol the
// kernel calls.

#ifndef KICKOS_APP_H
#define KICKOS_APP_H

#include <iso646.h> // and / or / not are macros in C, not keywords

#ifdef __cplusplus
extern "C"
{
#endif

int kickos_app_main(int argc, char** argv);

// Per-app build stamp: the app's OWN source compile time, distinct from the banner's `build` line.
// Weak, and allowlisted in tests/static/weak_allowlist.txt: the definition below is emitted by
// EVERY app TU, so weak is the C-compatible vague linkage that merges the duplicates.
//
// Must stay DATA. The banner reads it before any address space is activated, and a kernel-text
// reference to app text is what tests/static/check_riscv_kernel_apphalf.sh refuses.
//
// The banner prints these bytes as they stand, so the layout is C's own spelling:
// "Mmm dd yyyy HH:MM:SS", with the zone appended when the build supplies one.
//
// Only the DEFINITION below is weak. The kernel's reference states its own linkage
// (KICKOS_LINK_OPTIONAL), and a weak one here would override it: ld -m i386pep emits no base
// relocation for a weak reference to a weak definition, which leaves the kernel's pointer at
// the link address whenever firmware loads the image elsewhere.
extern char const kickos_app_build_time[];

// The two languages spell a PUBLIC const definition differently: `extern` on a definition with an
// initialiser is a C diagnostic under -Wextra, and without `extern` a const at namespace scope is
// internal in C++, which a weak symbol may not be.
#ifdef main
#ifdef __cplusplus
#define KOS_APP_STAMP extern __attribute__((weak)) char const
#else
#define KOS_APP_STAMP __attribute__((weak)) char const
#endif
#ifdef KICKOS_APP_TZ
/* KICKOS_APP_TZ arrives as a bare token (e.g. +0200); stringize it here so the CMake define
   carries no quotes to double-escape on a build-dir reconfigure. */
#define KOS_TZ_STR2(x) #x
#define KOS_TZ_STR(x) KOS_TZ_STR2(x)
KOS_APP_STAMP kickos_app_build_time[] = __DATE__ " " __TIME__ " " KOS_TZ_STR(KICKOS_APP_TZ);
#undef KOS_TZ_STR
#undef KOS_TZ_STR2
#else
KOS_APP_STAMP kickos_app_build_time[] = __DATE__ " " __TIME__;
#endif
#undef KOS_APP_STAMP
#endif

#ifdef __cplusplus
}
#endif

#endif
