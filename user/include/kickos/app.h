// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The OS-agnostic application entry contract (dependency inversion, invariant #8). The app writes
// a plain `int main(int argc, char** argv)`. A program, the target a composition's `program:`
// names, keeps it as main: its task's entry thread calls it and passes its return to exit(), so
// the system ends with it where the composition `ends` on that task.
//
// App and library C++ global ctors run before main runs, on ONE thread in sequence:
// the kernel's root thread before the init starts any task where one image holds the app and its
// init, and otherwise each task's entry thread at every start of that task, in the task's own
// image. An app global ctor MUST NOT block (sleep/wait),
// or a higher-priority thread it already spawned may run before the remaining globals are
// constructed. Do blocking work inside main.
//
// This header is force-included into every app TU by the build.

#ifndef KICKOS_APP_H
#define KICKOS_APP_H

#include <iso646.h> // and / or / not are macros in C, not keywords

#ifdef __cplusplus
extern "C"
{
#endif

// Per-app stamp: the image's name and its OWN source compile time, distinct from the banner's
// `build` line, so two images built in the same second still differ. Weak, and allowlisted in
// tests/static/weak_allowlist.txt: the definition below is emitted by EVERY TU of an executable, so
// weak is the C-compatible vague linkage that merges the duplicates. A TU of any other target
// emits none, so the copy an image links is always its own executable's.
//
// Must stay DATA. The banner reads it before any address space is activated, and a kernel-text
// reference to app text is what tests/static/check_riscv_kernel_apphalf.sh refuses.
//
// The banner prints these bytes as they stand: "<image> Mmm dd yyyy HH:MM:SS", C's own spelling of
// the time, with the zone appended when the build supplies one.
//
// Only the DEFINITION below is weak. The kernel's reference states its own linkage
// (KICKOS_LINK_OPTIONAL), and a weak one here would override it: ld -m i386pep emits no base
// relocation for a weak reference to a weak definition, which leaves the kernel's pointer at
// the link address whenever firmware loads the image elsewhere.
extern char const kickos_app_stamp[];

// The two languages spell a PUBLIC const definition differently: `extern` on a definition with an
// initialiser is a C diagnostic under -Wextra, and without `extern` a const at namespace scope is
// internal in C++, which a weak symbol may not be.
#if defined(KICKOS_APP_IMAGE)
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
KOS_APP_STAMP kickos_app_stamp[] = KICKOS_APP_IMAGE " " __DATE__ " " __TIME__ " " KOS_TZ_STR(KICKOS_APP_TZ);
#undef KOS_TZ_STR
#undef KOS_TZ_STR2
#else
KOS_APP_STAMP kickos_app_stamp[] = KICKOS_APP_IMAGE " " __DATE__ " " __TIME__;
#endif
#undef KOS_APP_STAMP
#endif

#ifdef __cplusplus
}
#endif

#endif
