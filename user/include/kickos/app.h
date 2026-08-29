// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The OS-agnostic application entry contract (dependency inversion, invariant
// #8). The app writes a plain
//
//     int main(int argc, char** argv)
//
// and kickos_add_application() compiles it with -Dmain=kickos_app_main so the
// app source stays portable: the same file builds unchanged as a plain hosted
// program or on KickOS. The kernel's boot path calls kickos_app_main after init;
// its int return becomes the process exit status on the sim (a daemon-style app
// simply never returns).
//
// C++ global constructors of an app (and of any library it links, e.g. libstdc++)
// run on MCU targets from the kernel's root thread just before kickos_app_main.
// The kernel is live, so a ctor may use KickOS syscalls, but they run on ONE
// thread in sequence: an app global ctor MUST NOT block (sleep/wait), or a
// higher-priority thread it already spawned may run before the remaining globals
// are constructed. Do blocking work inside main, not in a global ctor.
//
// This declaration is force-included into every app TU by the build. Declaring
// it extern "C" gives the -Dmain-renamed C++ `main` C language linkage, so it
// resolves to the unmangled symbol the kernel calls (a C app already matches).

#ifndef KICKOS_APP_H
#define KICKOS_APP_H

#include <iso646.h> // and / or / not are macros in C, not keywords

#ifdef __cplusplus
extern "C"
{
#endif

int kickos_app_main(int argc, char** argv);

// Per-app build stamp, RAW: the app's OWN source compile time in C's own spellings, distinct
// from the banner's `build` line (the CMake-generated image link time). It moves only when the
// app TU itself recompiles, so it tells "did the APP change" vs "was the image relinked".
// Weak, and allowlisted in tests/static/weak_allowlist.txt: the definition below is emitted by
// EVERY app TU (the build force-includes this header with -Dmain=...), so weak is the
// C-compatible vague linkage that merges the duplicates. Not a backend seam.
//
// DATA AND NOT A FUNCTION, which is what lets the kernel read it at all. Where a translating
// backend splits the image the kernel may not CALL app text: the banner runs before any address
// space is activated, so on a backend whose app window is not identity mapped there is no
// mapping of that half yet, and a kernel-text reference to app text is what
// tests/static/check_riscv_kernel_apphalf.sh refuses (kernel/include/kickos/kruntime.h covers
// the narrower case, the runtime helpers a compiler emits). A byte of app DATA is reachable either
// way, the kernel's own half carrying a map of all physical RAM. kmain does the reformatting.
//
// Layout: "Mmm dd yyyy" then '|' then "HH:MM:SS", optionally then '|' and the zone.
extern char kickos_app_build_raw[] __attribute__((weak));

// Defined ONLY in an app TU (the build force-includes this header with
// -Dmain=kickos_app_main, so __DATE__/__TIME__ capture the APP's compile time, not the
// kernel's).
#ifdef main
#ifdef KICKOS_APP_TZ
/* KICKOS_APP_TZ arrives as a bare token (e.g. +0200); stringize it here so the CMake define
   carries no quotes to double-escape on a build-dir reconfigure. */
#define KOS_TZ_STR2(x) #x
#define KOS_TZ_STR(x) KOS_TZ_STR2(x)
/* NOT const: a const object at file scope has internal linkage in C++, and a weak symbol has
   to be public; `extern` on a definition with an initialiser is a C diagnostic. */
__attribute__((weak)) char kickos_app_build_raw[] =
    __DATE__ "|" __TIME__ "|" KOS_TZ_STR(KICKOS_APP_TZ);
#undef KOS_TZ_STR
#undef KOS_TZ_STR2
#else
__attribute__((weak)) char kickos_app_build_raw[] = __DATE__ "|" __TIME__;
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif
