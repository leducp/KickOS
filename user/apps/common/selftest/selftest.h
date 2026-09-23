// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the self-test translation units share.

#ifndef KICKOS_USER_APPS_COMMON_SELFTEST_SELFTEST_H
#define KICKOS_USER_APPS_COMMON_SELFTEST_SELFTEST_H

#include <kickos/amp.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/config/cap_width.h>
#include <kickos/sys/bus.h> // compile-checks the wire-ABI struct-size static_asserts
#include <kickos/sys/byte_ring.h>
#include <kickos/sys/uart_service.h>
#include <kickos/sys/spi_service.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/abi_probe.h>
#include <kickos/sys/irq_free.h>
#include <kickos/sys/serve.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/string.h>

#include "tap.h"

// The chip's own constants. A sim build ships none (same guard as config/board.h), so
// anything read from here needs a fallback.
#if defined(__has_include) and __has_include(<kickos/chip_limits.h>)
#include <kickos/chip_limits.h>
#endif

// The registration list in main.cc is cut into KICKOS_SELFTEST_REGIONS contiguous regions,
// and this image carries the run [KICKOS_SELFTEST_FIRST_REGION, KICKOS_SELFTEST_LAST_REGION].
// TAP_ADD is REDEFINED at every boundary, so an arm belongs to the region its line sits in.
// All three come from user/apps/common/selftest/CMakeLists.txt: an image built without them
// would register no arm at all and still plan and pass, so they are required rather than
// defaulted.
#if not defined(KICKOS_SELFTEST_REGIONS) or not defined(KICKOS_SELFTEST_FIRST_REGION)         \
    or not defined(KICKOS_SELFTEST_LAST_REGION)
#error "the selftest region bounds are missing; build this app through its own CMakeLists"
#endif
#define KICKOS_SELFTEST_REGION(n)                                                             \
    ((n) >= KICKOS_SELFTEST_FIRST_REGION and (n) <= KICKOS_SELFTEST_LAST_REGION)

// Unevaluated operand: counts as a use for -Wunused-function without emitting the body.
#define TAP_ELIDE(fn) ((void)sizeof(&(fn)))

#ifndef KICKOS_KERNEL_CORES
#define KICKOS_KERNEL_CORES 1
#endif

// A cross-thread progress order is a single-core claim: priority orders which runnable thread
// gets a core, and above one core a thread with a core of its own proceeds whatever its
// priority. An arm reading the interleaving of two threads, or resting on one of them being
// denied the CPU, therefore asserts what a shared kernel on several cores does not promise.
// Place this first in such an arm, ahead of any pooled object it would have to give back.
#if KICKOS_KERNEL_CORES > 1
#define TAP_SKIP_ONE_CORE_ORDER()                                                          \
    do                                                                                     \
    {                                                                                      \
        tap::skip("a cross-thread progress order is not a property of %u kernel cores",     \
                  static_cast<unsigned>(KICKOS_KERNEL_CORES));                              \
        return;                                                                            \
    } while (0)
#else
#define TAP_SKIP_ONE_CORE_ORDER() \
    do                            \
    {                             \
    } while (0)
#endif

// A name this app resolves across its own translation units. Hidden keeps the reference
// PC-relative, which tools/check-x86_64-no-got.sh refuses a survivor of before every link.
#define KICKOS_SELFTEST_LOCAL __attribute__((visibility("hidden")))

namespace selftest
{
    using kickos::Atomic;
    using kickos::Order;

    extern KICKOS_SELFTEST_LOCAL kos_cap_t g_done; // shared completion counter (MAIN's cap; delegated to workers)
    extern KICKOS_SELFTEST_LOCAL kos_cap_t g_lock; // binary semaphore = mutex over the event log (MAIN's cap)

    // Well-known child cap indices: a fresh child table has cap-gen 0, so delegated cap i
    // lands at index i+1 (index 0 reserved). Every spawn must delegate in exactly this order.
    constexpr int CH_DONE = 1;  // delegated FIRST to every worker
    constexpr int CH_LOCK = 2;  // delegated SECOND (logging workers only)
    constexpr int CH_AUX = 3;
    constexpr int CH_READY = 2; // IRQ-driver tests
    constexpr int CH_IRQ = 3;   // IRQ-driver tests: the LINE, for ack and discard
    constexpr int CH_NOTE = 4;  // IRQ-driver tests: the notification that line signals, which
                                // is what the driver binds and waits on
    constexpr int CH_REL = 4;   // root-to-child release, where the child has no other park
                                // between signaling readiness and waiting for release
    constexpr uint8_t CH_FULL =
        KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    // root's region set is [app code RX, app static data RW, its own stack], and
    // kos_ram_alloc grants the caller nothing: a test that must touch its own allocation
    // asks with kos_mem_self_grant.

    // Pin both test threads to core 0 when priority must determine execution order.
    // A lower-priority thread then runs only after the higher one blocks.
    // Core 0 cannot be isolated and belongs to every default task mask.
    // Spawn both participants: root has no self-handle for affinity changes.
    constexpr uint32_t TAP_PIN_CORE = 0x1u;
    // The two ranks inside that domain: PARKS is the party whose park is the precondition,
    // AFTER the party whose first instruction must not run until it has parked. AFTER is below
    // root's own KICKOS_PRIO_MIN + 1, so it also waits on root reaching its wait.
    constexpr uint8_t TAP_PRIO_PARKS = 10;
    constexpr uint8_t TAP_PRIO_AFTER = 1;

    // A thread handling a line does not migrate: above one kernel core a claim, and a wait, ack
    // or discard on a claimed line, is refused to a thread whose mask is not exactly the claim
    // core. irq_spawn makes such a thread pinned to TAP_PIN_CORE, and TAP_ADD_IRQ runs an arm
    // whose root claims or waits with root pinned there. One kernel core carries no placement.
#if KICKOS_KERNEL_CORES > 1
    inline kos::thread::Handle irq_spawn(void (*entry)(void*), void* arg, char const* name,
                                         uint8_t prio, kos_cap_grant const* caps, uint8_t count,
                                         uint8_t policy = KOS_POLICY_FIFO,
                                         uint32_t quantum_ns = 0, bool privileged = false,
                                         void* mem = nullptr, uint32_t mem_size = 0,
                                         uint8_t authority = 0,
                                         uint16_t const* cap_dest = nullptr)
    {
        return kos::thread::create_caps(entry, arg, name, prio, caps, count, policy, quantum_ns,
                                        privileged, mem, mem_size, authority, cap_dest,
                                        KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
    }

    template <void (*Arm)()>
    void irq_pinned()
    {
        kos_thread_t const self = kos_thread_self();
        int const rc = kos_thread_set_affinity(self, TAP_PIN_CORE);
        if (rc != 0)
        {
            tap::fail("root could not pin itself to the core its line is claimed on: %d", rc);
            return;
        }
        Arm();
        (void)kos_thread_set_affinity(self, 0);
    }
#define TAP_ADD_IRQ(name, fn) TAP_ADD(name, irq_pinned<fn>)
#else
#define irq_spawn kos::thread::create_caps
#define TAP_ADD_IRQ(name, fn) TAP_ADD(name, fn)
#endif

    KICKOS_SELFTEST_LOCAL void wait_n(int n);
    KICKOS_SELFTEST_LOCAL size_t discover_granule();
    KICKOS_SELFTEST_LOCAL void pool_probe_worker(void*);
    KICKOS_SELFTEST_LOCAL bool pool_can_host(int n);

#if defined(KICKOS_ENABLE_SELFTEST)
    // A member of ANOTHER task gets its own copy of this image's static data, so every report
    // from one crosses on an ENDPOINT and every release crosses on a semaphore. A global
    // would be written in the member's copy and read in root's.
    constexpr uint32_t PLACE_JOIN_US = 2000000u;
    extern KICKOS_SELFTEST_LOCAL kos_cap_t g_pl_ep; // root's report endpoint, delegated at child index 1
#endif

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    KICKOS_SELFTEST_LOCAL void t_cap_objects();
    KICKOS_SELFTEST_LOCAL void t_cap_map();
    KICKOS_SELFTEST_LOCAL void t_stack_slot_returns();
    KICKOS_SELFTEST_LOCAL void t_cap_map_over_stack();
    KICKOS_SELFTEST_LOCAL void t_stack_grant_refused();
    KICKOS_SELFTEST_LOCAL void t_stack_guard_intact();
    KICKOS_SELFTEST_LOCAL void t_stack_handoff_refused();
    KICKOS_SELFTEST_LOCAL void t_cap_map_pins_run();
    KICKOS_SELFTEST_LOCAL void t_cap_share();
    KICKOS_SELFTEST_LOCAL void t_aspace_seam();
    KICKOS_SELFTEST_LOCAL void t_aspace_model();
    KICKOS_SELFTEST_LOCAL void t_aspace_map_cycle();
    KICKOS_SELFTEST_LOCAL void t_aspace_translate();
    KICKOS_SELFTEST_LOCAL void t_aspace_refusals();
    KICKOS_SELFTEST_LOCAL void t_aspace_span();
    KICKOS_SELFTEST_LOCAL void t_aspace_acquire_dup();
    KICKOS_SELFTEST_LOCAL void t_aspace_balance();
    KICKOS_SELFTEST_LOCAL void t_aspace_domain_balance();
    KICKOS_SELFTEST_LOCAL void t_aspace_forced_unwind();
    KICKOS_SELFTEST_LOCAL void t_aspace_churn();
    KICKOS_SELFTEST_LOCAL void t_stack_is_frames();
    KICKOS_SELFTEST_LOCAL void t_aspace_two_spaces_same_grant();
    KICKOS_SELFTEST_LOCAL void t_aspace_two_spaces_no_grant();
    KICKOS_SELFTEST_LOCAL void t_process_private_data();
    KICKOS_SELFTEST_LOCAL void t_task_siblings_share();
    KICKOS_SELFTEST_LOCAL void t_task_handoff_readback();
    KICKOS_SELFTEST_LOCAL void t_task_handoff_slice();
    KICKOS_SELFTEST_LOCAL void t_task_handoff_donor_exits();
    KICKOS_SELFTEST_LOCAL void t_reservation_teardown();
    KICKOS_SELFTEST_LOCAL void t_frame_scrub_cross_task();
    KICKOS_SELFTEST_LOCAL void t_spawn_refusal_frees_task();
    KICKOS_SELFTEST_LOCAL void t_spawn_refusal_frees_donor();
    KICKOS_SELFTEST_LOCAL void t_parked_frame_hostile();
    KICKOS_SELFTEST_LOCAL void t_split_access();
    KICKOS_SELFTEST_LOCAL void t_process_ipc_same_addr();
    KICKOS_SELFTEST_LOCAL void t_process_call_reply();
    KICKOS_SELFTEST_LOCAL void t_fault_kills_task();
    KICKOS_SELFTEST_LOCAL void t_grant_kernel_word_refused();
    KICKOS_SELFTEST_LOCAL void t_self_grant_retype();
    KICKOS_SELFTEST_LOCAL void t_recv_buf_unmapped();
    KICKOS_SELFTEST_LOCAL void t_frame_run_slot_recycle();
    KICKOS_SELFTEST_LOCAL void t_call_reply_undisclosed();
    KICKOS_SELFTEST_LOCAL void t_process_data_template();
    KICKOS_SELFTEST_LOCAL void t_reent_seating();
    KICKOS_SELFTEST_LOCAL void t_aspace_acquire_balance();
    KICKOS_SELFTEST_LOCAL void t_map_tlbi_elided();
    KICKOS_SELFTEST_LOCAL void t_aspace_active_cores();
#endif

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_AMP_NODE
    KICKOS_SELFTEST_LOCAL void t_amp_reset_record();
    KICKOS_SELFTEST_LOCAL void t_amp_far_reset_answers();
    KICKOS_SELFTEST_LOCAL void t_amp_far_answer_deferred();
    KICKOS_SELFTEST_LOCAL void t_amp_far_tail_recovery();
    KICKOS_SELFTEST_LOCAL void t_amp_window();
    KICKOS_SELFTEST_LOCAL void t_amp_reply_reserve();
    KICKOS_SELFTEST_LOCAL void t_amp_mint_reply_port();
    KICKOS_SELFTEST_LOCAL void t_amp_far_call();
    KICKOS_SELFTEST_LOCAL void t_amp_far_reply_guard();
    KICKOS_SELFTEST_LOCAL void t_amp_far_reply_empty();
    KICKOS_SELFTEST_LOCAL void t_amp_far_service();
    KICKOS_SELFTEST_LOCAL void t_amp_far_refusal_answered();
    KICKOS_SELFTEST_LOCAL void t_amp_far_deliver_fault();
    KICKOS_SELFTEST_LOCAL void t_amp_far_undisclosed();
    KICKOS_SELFTEST_LOCAL void t_amp_far_infoless();
    KICKOS_SELFTEST_LOCAL void t_amp_deferred_doorbell();
    KICKOS_SELFTEST_LOCAL void t_amp_app_alive();
    KICKOS_SELFTEST_LOCAL void t_amp_inbound_reply();
    KICKOS_SELFTEST_LOCAL void t_amp_port_seating();
    KICKOS_SELFTEST_LOCAL void t_amp_port_unnamed();
    KICKOS_SELFTEST_LOCAL void t_amp_reply_band();
    KICKOS_SELFTEST_LOCAL void t_amp_probe_root_only();
    KICKOS_SELFTEST_LOCAL void t_amp_local_port_slot_held();
    KICKOS_SELFTEST_LOCAL void t_amp_far_slot_reuse();
#endif

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    KICKOS_SELFTEST_LOCAL void t_pin_places();
    KICKOS_SELFTEST_LOCAL void t_pin_wrong_core_never();
    KICKOS_SELFTEST_LOCAL void t_unpin_restores();
    KICKOS_SELFTEST_LOCAL void t_affinity_zero_defaults();
    KICKOS_SELFTEST_LOCAL void t_affinity_undriven_refused();
    KICKOS_SELFTEST_LOCAL void t_migrate_running();
    KICKOS_SELFTEST_LOCAL void t_resched_reaches_pinned_caller();
    KICKOS_SELFTEST_LOCAL void t_pin_same_task_ok();
    KICKOS_SELFTEST_LOCAL void t_pin_cross_task_refused();
    KICKOS_SELFTEST_LOCAL void t_grant_narrows();
    KICKOS_SELFTEST_LOCAL void t_grant_wider_refused();
    KICKOS_SELFTEST_LOCAL void t_grant_second_narrow_only();
    KICKOS_SELFTEST_LOCAL void t_grant_inherited_by_child_task();
    KICKOS_SELFTEST_LOCAL void t_grant_after_member_refused();
    KICKOS_SELFTEST_LOCAL void t_isolated_single_grant_ok();
    KICKOS_SELFTEST_LOCAL void t_affinity_dead_handle_refused();
    KICKOS_SELFTEST_LOCAL void t_isolated_unpinned_never();
    KICKOS_SELFTEST_LOCAL void t_isolated_unpin_excludes();
    KICKOS_SELFTEST_LOCAL void t_isolated_takes_pinned();
    KICKOS_SELFTEST_LOCAL void t_isolated_mixed_mask_ok();
    KICKOS_SELFTEST_LOCAL void t_slice_preempts_every_core();
    KICKOS_SELFTEST_LOCAL void t_threads_reach_every_core();
    KICKOS_SELFTEST_LOCAL void t_irq_cross_core_wake();
    KICKOS_SELFTEST_LOCAL void t_irq_reclaim_stale_raise();
#endif

}

#endif
