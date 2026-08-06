// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The userspace init-service seam. The kernel's root thread calls kickos_init_entry once
// kernel init is complete; whatever CMake target the build selected as
// KICKOS_INIT_PROVIDER supplies that symbol (default: kickos_default_init).
//
// Lifecycle, chosen by whether the entry RETURNS:
//   * Single-shot (the default init). It walks the board's service list
//     (kickos_service_list_run), then runs the app's kickos_app_main. A nonzero bring-up
//     result short-circuits: the entry returns it WITHOUT running the app, so the app
//     never runs against a dark console. RETURNING from kickos_init_entry tears the
//     system down (root_entry flushes the console, then arch_shutdown(status)).
//   * An init that brings services up must PERSIST: it parks (a sleep loop or a wait on a
//     semaphore nobody posts) and NEVER returns. Returning would exit, taking down every
//     service it spawned.
//
// The app main / init body runs in an UNPRIVILEGED root holding a full authority word. An
// app must not assume ambient privilege: the privileged acts are gated on authority bits
// (kos_console_publish on AUTH_CONSOLE, and so on), and what root holds when main is
// entered is what kickos_app_authority() below declares.
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

// The kernel -> init argument handoff. kmain fills it; the root thread reads it
// immediately before calling kickos_init_entry above.
//
// MUST stay defined app-side (libkickos_user.a), so the enforcement linker scripts route
// it into the .appdata/.appbss grant that every unprivileged thread holds
// (arch_domain_static_regions). Kernel-side storage, or a kmain stack local sitting
// outside the arena, faults an unprivileged root before its first statement on every
// enforcing board.
//
// argv is null (argc 0) on MCU. On the hosted sim it points into the host process's own
// argv, which no grant covers and which the sim does not enforce.
struct kos_init_args
{
    int argc;
    char** argv;
};

extern struct kos_init_args kickos_init_args;

// The default init body: narrow root's authority to kickos_app_authority() below, then
// run the app's kickos_app_main. Exposed so a custom init provider can delegate to it.
// It does NOT bring the service list up (that is kickos_service_list_run below). CALL IT
// LAST: any bring-up sequenced after it runs with the app's narrowed set and earns
// -KOS_EPERM.
int kickos_default_init_run(int argc, char** argv);

// Run the selected board's service list (see <kickos/sys/service.h>): walk each entry's
// start() in array order. The console, where a board has a userspace one, is the first
// KOS_SVC_CONSOLE entry. Returns 0 on success (or empty list), or the first failing
// entry's negative code. The default kickos_init_entry runs this AFTER the pin map and
// BEFORE the app main, aborting the app on a nonzero result. MUST NOT use libc stdio
// (bring-up self-deadlock rule).
int kickos_service_list_run(void);

// Apply the selected board's pin map (see <kickos/sys/pinmap.h>) BEFORE the service list.
// A board with an empty map (count = 0) is a no-op. Returns 0 on success, or the first
// failing entry's negative rc. The default kickos_init_entry runs this FIRST and aborts
// the app on a nonzero result. MUST NOT use libc stdio (same bring-up rule as above).
int kickos_pinmux_run(void);

// The authority the app's main needs: a mask of kos_cap_authority KOS_AUTH_* bits
// (<kickos/sys/abi.h>). kickos_default_init_run narrows root's authority cap to this
// before kickos_app_main. It can only CLEAR bits.
//
// The fallback is KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM. An app needing more declares it in
// its OWN TU, at file scope next to main:
//
//     KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_PINMUX);
//
// An app whose main RETURNS must keep KOS_AUTH_SYSTEM: root_entry ends the system with
// kos_shutdown, and a refusal panics "root: shutdown refused" (kernel/init/kmain.cc). The
// same holds for a main that calls exit() or abort(), root's kos_exit being a shutdown too
// (<kickos/sys.h>). A never-returning app may declare 0.
//
// Not weak, and must not become weak: the attribute would propagate to the app's own
// definition (GCC carries it from declaration to definition in one TU) and leave link
// order deciding the winner. An app's definition wins instead by keeping
// system/init/common/app_authority_default.cc from being extracted at all.
uint8_t kickos_app_authority(void);

#ifdef __cplusplus
}
#endif

// A bare definition in a C++ app TU would mangle and be silently ignored, leaving the
// app on the fallback mask; the macro supplies the C linkage.
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
