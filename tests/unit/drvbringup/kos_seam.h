// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The syscall seam <kickos/sys/driver_service.h> needs, faked and RECORDING.
//
// HOST-ONLY. These are the public kos_* names: a target image linking this TU would satisfy
// them from the executable, and only the fact that every syscall stub in the tree shares one
// archive member (user/src/syscall_stubs.cc) makes that a duplicate-symbol error instead of a
// silent shadow. Nothing gates it: check_class_backend.sh covers the driver classes only.

#ifndef KICKOS_TESTS_UNIT_DRVBRINGUP_KOS_SEAM_H
#define KICKOS_TESTS_UNIT_DRVBRINGUP_KOS_SEAM_H

#include <stdint.h>

enum
{
    // The three ranges must stay disjoint: every expected trace string in the arms names caps,
    // thread ids and task ids by their literal value.
    KOS_SEAM_CAP_BASE = 10,
    KOS_SEAM_THREAD_BASE = 50,
    KOS_SEAM_TASK_BASE = 90,
    KOS_SEAM_TRACE_MAX = 512,
    KOS_SEAM_MSG_MAX = 512
};

// What the next call of each kind does. Zero-initialised means "every call succeeds", which
// is the happy path, so an arm names only the failure it is about.
struct kos_seam_control
{
    // 1-based ordinal of the call that fails; 0 = none ever fails. An ordinal beyond the
    // number of calls made is silently no failure, so a stale knob reads as a happy path
    // and NOT as a spurious red.
    uint32_t irq_claim_fail_at;
    uint32_t spawn_fail_at;

    bool task_create_fails;
    bool ram_alloc_fails;
    bool self_grant_fails;
    bool endpoint_create_fails;
    bool console_publish_fails;

    // What the handover probe returns. 0 = the rendezvous completed.
    int32_t send_timed_rc;

    // The readiness latch a spawned thread would set once it reaches its loop. Null, or
    // latch_on_spawn false, models the thread that never got there.
    volatile uint32_t* latch;
    bool latch_on_spawn;
};

extern struct kos_seam_control g_seam;

// Clears the control block, the trace and the arena. Every arm must call it first: the
// arena is static and a cap counter that kept running would make each arm's oracle depend
// on the arms before it.
void kos_seam_reset();

// The trace, rendered. Consecutive kos_sleep_ns calls collapse to one `sleep*N` token, and
// must keep doing so: the readiness timeout makes KOS_DRV_READY_WAIT_MAX of them and an
// uncollapsed trace overruns the buffer, turning that arm into a truncation.
char const* kos_seam_trace();

// Everything kos_print was handed, concatenated.
char const* kos_seam_msg();

#endif
