// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The syscall seam <kickos/sys/driver_service.h> needs, faked and RECORDING.
//
// HOST-ONLY. These are the public kos_* names: a target image linking this TU would satisfy
// them from the executable, and only the fact that every syscall stub in the tree shares one
// archive member (user/src/syscall_stubs.cc) makes that a duplicate-symbol error instead of a
// silent shadow.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_KOS_SEAM_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_KOS_SEAM_H

#include <stdint.h>

enum
{
    // The thread ids the seam hands out, which every expected trace string names by value.
    KOS_SEAM_THREAD_BASE = 50,
    KOS_SEAM_TRACE_MAX = 512,
    KOS_SEAM_MSG_MAX = 512
};

// What the next call of each kind does. Zero-initialised is the happy path, so an arm names
// only the failure it is about.
struct kos_seam_control
{
    // 1-based ordinal of the spawn that fails; 0 = none ever fails.
    uint32_t spawn_fail_at;
    // What kos_thread_set_priority answers.
    int set_priority_rc;
    // A spawned thread runs at its spawn, before the spawner goes on, as one above the spawner's
    // priority does.
    bool run_on_spawn;
    // A park on a word: the producer publishes it and wakes the parked thread, or nothing comes and
    // the park lasts its whole timeout.
    bool publish_on_park;
};

extern struct kos_seam_control g_seam;

// kos_exit's code, thrown: the calling thread's end.
struct kos_seam_exit
{
    int code;
};

// Clears the control block, the trace and the clock. Every arm must call it first.
void kos_seam_reset();

// The trace, rendered.
char const* kos_seam_trace();

// Everything kos_print and the kernel console took, concatenated.
char const* kos_seam_msg();

// The arg the `i`-th spawn since the reset handed its thread, or null past the spawns recorded.
void* kos_seam_spawn_arg(uint32_t i);

// Records `token` in the trace, as a thread body of an arm marks that it ran.
void kos_seam_note(char const* token);

#endif
