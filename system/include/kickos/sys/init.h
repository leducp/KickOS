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
// Privilege posture: root's CPU mode is the build knob KICKOS_ROOT_PRIVILEGED, decided
// once in kmain before any app code runs. Under OFF the app main / init body runs in an
// UNPRIVILEGED root holding a CAP_AUTHORITY, which is the posture every enforcement
// board is validated in. So an app must not assume ambient privilege: the acts that used
// to need it are gated on authority bits (kos_console_publish on AUTH_CONSOLE, and so
// on), and what root still holds when main is entered is what kickos_app_authority()
// below declares.
//
// Ordering: app and libstdc++ global constructors run in the kernel root thread
// BEFORE kickos_init_entry is entered, so a custom init must not assume they can
// depend on anything that init itself brings up.
//
// Overriding: a power user names their own target in KICKOS_INIT_PROVIDER and may
// call kickos_default_init_run to reuse the default body (run the app's main).

#ifndef KICKOS_SYS_INIT_H
#define KICKOS_SYS_INIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// The symbol the kernel boot path calls after kernel init.
int kickos_init_entry(int argc, char** argv);

// The kernel -> init argument handoff. kmain fills it; the root thread reads it
// immediately before calling kickos_init_entry above.
//
// App-side on purpose: the object is defined in libkickos_user.a, so the enforcement
// linker scripts route it into the .appdata/.appbss grant, a region of every
// unprivileged thread (arch_domain_static_regions). Kernel-side storage (or a kmain
// stack local, which sits outside the arena) would fault an unprivileged root before
// its first statement on every enforcing board.
//
// argv is null (argc 0) on MCU. On the hosted sim it points into the host process's
// own argv, which no grant covers; the sim does not enforce non-arena regions, so
// this is exactly the case its coverage misses.
struct kos_init_args
{
    int argc;
    char** argv;
};

extern struct kos_init_args kickos_init_args;

// The default init body: narrow root's authority to kickos_app_authority() below, then
// run the app's kickos_app_main. Exposed so a custom init provider can delegate to it.
// It does NOT bring the service list up (that is kickos_service_list_run below), so a
// custom init composes the two in whatever order it needs -- but the narrowing rides
// HERE rather than in the default kickos_init_entry precisely so that delegating cannot
// silently hand the app root's full authority. Call it LAST: any bring-up sequenced
// after it runs with the app's narrowed set and earns -KOS_EPERM.
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

// The authority the app's main needs: a mask of kos_cap_authority KOS_AUTH_* bits
// (<kickos/sys/abi.h>). The default kickos_init_entry narrows root's authority cap to
// this AFTER the pin map and the service list and BEFORE kickos_app_main, so bring-up
// keeps the pinmux / console / spawn authority it needs while the app runs with only
// what it asked for. It can only CLEAR bits: root cannot gain authority here.
//
// The weak default is KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM -- spawn worker threads, and end
// the system when main returns. An app needing more declares it in its OWN TU, at file
// scope next to main (the macro supplies the C linkage the override needs):
//
//     KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_PINMUX);
//
// An app whose main RETURNS must keep KOS_AUTH_SYSTEM: root_entry ends the system with
// kos_shutdown, and a refused shutdown panics "root: shutdown refused"
// (kernel/init/kmain.cc). A never-returning app may declare 0 and hold nothing.
//
// This bites only where root is UNPRIVILEGED. cap_check_authority short-circuits on
// Thread::privileged, and kmain seats root an authority cap only under
// KICKOS_ROOT_PRIVILEGED=OFF -- so on a privileged-root board the narrow finds an empty
// slot, answers -KOS_EBADF, and changes nothing.
//
// NO weak symbol is involved, deliberately. Both this and the fallback are STRONG, and
// the override works by archive-member-on-demand: the fallback is alone in
// system/init/app_authority_default.cc, so an app that defines the symbol resolves it
// locally and that member is never extracted, while an app that declares nothing leaves
// the reference undefined and pulls it in. Weak was tried and rejected -- GCC carries a
// weak attribute from a declaration onto the definition in the same TU, which made every
// app's override weak too and left link order deciding it.
uint8_t kickos_app_authority(void);

#ifdef __cplusplus
}
#endif

// Defines the override, with the C language linkage that makes it win over the weak
// default. A bare definition in a C++ app TU would mangle and be silently ignored,
// leaving the app running on the default mask.
#ifdef __cplusplus
#define KICKOS_APP_AUTHORITY(mask)                 \
    extern "C" uint8_t kickos_app_authority(void); \
    extern "C" uint8_t kickos_app_authority(void) { return (uint8_t)(mask); }
#else
#define KICKOS_APP_AUTHORITY(mask)          \
    uint8_t kickos_app_authority(void);     \
    uint8_t kickos_app_authority(void) { return (uint8_t)(mask); }
#endif

#endif
