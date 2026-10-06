// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

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
#include <kickos/sys/table.h>
#include <kickos/sys/errno.h>
#include <kickos/libc/string.h>

#include "tap.h"

#include <kickos/chip_limits.h>

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

// Unevaluated operand: counts as a use for -Wunused-function without emitting the body. The
// cast gives a template-id such as main_pinned<f> the type that names its one specialization.
#define TAP_ELIDE(fn) ((void)sizeof(static_cast<void (*)()>(&(fn))))

// A registration this image compiles. The absolute symbol `kickos_tap_arm.<name>` it leaves in
// the symbol table is how tests/integration/check_selftest_manifest.sh reads which arms the
// linked image carries, so it must stay beside the tap::add it names.
#define TAP_REGISTER(name, fn)                                                                \
    do                                                                                        \
    {                                                                                         \
        __asm__(".set \"kickos_tap_arm." name "\", 1");                                     \
        tap::add(name, fn);                                                                   \
    } while (false)

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
    // main's own row of the system table: its declared priority, ceiling and authority.
    extern KICKOS_SELFTEST_LOCAL kos_self_t const* g_self;
    extern KICKOS_SELFTEST_LOCAL kos_thread_t g_main; // main's own thread

    // Well-known child cap indices: a fresh child table has cap-gen 0, so delegated cap i
    // lands at index i+1 (index 0 reserved). Every spawn must delegate in exactly this order.
    constexpr int CH_DONE = 1;  // delegated FIRST to every worker
    constexpr int CH_LOCK = 2;  // delegated SECOND (logging workers only)
    constexpr int CH_AUX = 3;
    constexpr int CH_READY = 2; // IRQ-driver tests
    constexpr int CH_IRQ = 3;   // IRQ-driver tests: the LINE, for ack and discard
    constexpr int CH_NOTE = 4;  // IRQ-driver tests: the notification that line signals, which
                                // is what the driver binds and waits on
    constexpr int CH_REL = 4;   // main-to-child release, where the child has no other park
                                // between signaling readiness and waiting for release
    constexpr uint8_t CH_FULL =
        KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER;

    // main's `authority` in system.yaml and consoles/*.yaml. Never KOS_AUTH_PSTATE: a retune
    // would retime every deadline the timing arms assert.
    constexpr uint32_t SELFTEST_AUTHORITY = KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_PINMUX
                                            | KOS_AUTH_IRQ | KOS_AUTH_CONSOLE | KOS_AUTH_TASKS
                                            | KOS_AUTH_BUS_MASTER;

    // main's region set is [app code RX, app static data RW, its own stack], and
    // kos_ram_alloc grants the caller nothing: a test that must touch its own allocation
    // asks with kos_mem_self_grant.

    // Pin both test threads to core 0 when priority must determine execution order.
    // A lower-priority thread then runs only after the higher one blocks.
    // Core 0 cannot be isolated and belongs to every default task mask.
    // Spawn both participants.
    constexpr uint32_t TAP_PIN_CORE = 0x1u;
    // The two ranks inside that domain: PARKS is the party whose park is the precondition,
    // AFTER the party whose first instruction must not run until it has parked. AFTER is below
    // main's own KICKOS_PRIO_MIN + 1, so it also waits on main reaching its wait.
    constexpr uint8_t TAP_PRIO_PARKS = 10;
    constexpr uint8_t TAP_PRIO_AFTER = 1;

    // Bounds an event the kernel will deliver, not how soon: an emulated core the host
    // deschedules holds it back by hundreds of milliseconds, a dying thread's teardown most.
    constexpr uint32_t STALL_TOLERANT_US = 5000000;

    // A thread handling a line does not migrate: above one kernel core a claim, and a wait, ack
    // or discard on a claimed line, is refused to a thread whose mask is not exactly the claim
    // core. irq_spawn makes such a thread pinned to TAP_PIN_CORE, and TAP_ADD_IRQ runs an arm
    // whose main claims or waits with main pinned there. TAP_ADD_PINNED runs an arm with main
    // pinned there too, so a thread it spawns on TAP_PIN_CORE is ordered against main by
    // priority alone. One kernel core carries no placement.
#if KICKOS_KERNEL_CORES > 1
    inline kos::thread::Handle irq_spawn(void (*entry)(void*), void* arg, char const* name,
                                         uint8_t prio, kos_cap_grant const* caps, uint8_t count,
                                         uint8_t policy = KOS_POLICY_FIFO,
                                         uint32_t quantum_ns = 0, bool privileged = false,
                                         void* mem = nullptr, uint32_t mem_size = 0,
                                         uint32_t authority = 0,
                                         uint16_t const* cap_dest = nullptr)
    {
        return kos::thread::create_caps(entry, arg, name, prio, caps, count, policy, quantum_ns,
                                        privileged, mem, mem_size, authority, cap_dest,
                                        KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
    }

    template <void (*Arm)()>
    void main_pinned()
    {
        kos_thread_t const self = kos_thread_self();
        int const rc = kos_thread_set_affinity(self, TAP_PIN_CORE);
        if (rc != 0)
        {
            tap::fail("main could not pin itself to TAP_PIN_CORE: %d", rc);
            return;
        }
        Arm();
        (void)kos_thread_set_affinity(self, 0);
    }
#define TAP_ADD_IRQ(name, fn) TAP_ADD(name, main_pinned<fn>)
#define TAP_ADD_PINNED(name, fn) TAP_ADD(name, main_pinned<fn>)
#else
#define irq_spawn kos::thread::create_caps
#define TAP_ADD_IRQ(name, fn) TAP_ADD(name, fn)
#define TAP_ADD_PINNED(name, fn) TAP_ADD(name, fn)
#endif

    // Releases what an arm still holds however it returns, a failing TAP_CHECK included: every
    // capability first, so a worker parked on one is refused rather than stranded, then every
    // thread, joined, and slain if it outlives STALL_TOLERANT_US.
    class ArmHold
    {
    public:
        static constexpr int MAX = 4;

        ArmHold() = default;
        ArmHold(ArmHold const&) = delete;
        ArmHold& operator=(ArmHold const&) = delete;
        ~ArmHold()
        {
            for (int i = 0; i < ncaps_; i++)
            {
                if (*caps_[i] != KOS_CAP_NONE)
                {
                    (void)close(caps_[i]);
                }
            }
            for (int i = 0; i < nthreads_; i++)
            {
                if (threads_[i]->valid() and threads_[i]->join(STALL_TOLERANT_US) != 0)
                {
                    (void)threads_[i]->slay(STALL_TOLERANT_US);
                }
            }
        }

        void cap(kos_cap_t* c)
        {
            if (ncaps_ == MAX)
            {
                tap::fail("an ArmHold holds at most %d capabilities", MAX);
                return;
            }
            caps_[ncaps_] = c;
            ncaps_ = ncaps_ + 1;
        }
        void thread(kos::thread::Handle* t)
        {
            if (nthreads_ == MAX)
            {
                tap::fail("an ArmHold holds at most %d threads", MAX);
                return;
            }
            threads_[nthreads_] = t;
            nthreads_ = nthreads_ + 1;
        }
        int close(kos_cap_t* c)
        {
            int const rc = kos_handle_close(*c);
            *c = KOS_CAP_NONE;
            return rc;
        }

    private:
        kos_cap_t* caps_[MAX] = {};
        kos::thread::Handle* threads_[MAX] = {};
        int ncaps_ = 0;
        int nthreads_ = 0;
    };

    KICKOS_SELFTEST_LOCAL void wait_n(int n);
    KICKOS_SELFTEST_LOCAL size_t discover_granule();
    KICKOS_SELFTEST_LOCAL void pool_probe_worker(void*);
    KICKOS_SELFTEST_LOCAL bool pool_can_host(int n);
    // The objects an arm holds at once out of main's task budgets, beside the two semaphores
    // main holds for the run. tests/static/selftest_demands.py reads each field off the call,
    // so every field is a literal or a named constant.
    struct ObjectDemand
    {
        int sems = 0;
        int mutexes = 0;
        int endpoints = 0;
        int notifies = 0;
    };
    KICKOS_SELFTEST_LOCAL int objects_can_host(ObjectDemand want);
    // kos_ram_alloc, refused while g_ram_starved is set: the refusal paths of the arms that
    // reserve through it are otherwise reached only once a board's arena has run out. A
    // member of another task reads its own copy of the flag, so it is told through
    // st_ram_alloc_as.
    extern KICKOS_SELFTEST_LOCAL bool g_ram_starved;
    KICKOS_SELFTEST_LOCAL void* st_ram_alloc(size_t size);
    KICKOS_SELFTEST_LOCAL void* st_ram_alloc_as(bool starved, size_t size);
    // The tap::skip_tagged tag of an arm declining because st_ram_alloc refused it.
    constexpr unsigned ST_SKIP_RAM_REFUSED = 1;

#if defined(KICKOS_ENABLE_SELFTEST)
    // A member of ANOTHER task gets its own copy of this image's static data, so every report
    // from one crosses on an ENDPOINT and every release crosses on a semaphore. A global
    // would be written in the member's copy and read in main's.
    extern KICKOS_SELFTEST_LOCAL kos_cap_t g_pl_ep; // main's report endpoint, delegated at child index 1
#endif

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    // The live thread count while main runs alone, read before the first arm spawns.
    extern KICKOS_SELFTEST_LOCAL uintptr_t g_live_rest;
    KICKOS_SELFTEST_LOCAL void settle_exits();
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
    KICKOS_SELFTEST_LOCAL void t_kernel_state_unreachable();
    KICKOS_SELFTEST_LOCAL void t_window_addr();
    KICKOS_SELFTEST_LOCAL void t_port_window();
    KICKOS_SELFTEST_LOCAL void t_vector_survives_block();
    KICKOS_SELFTEST_LOCAL void t_vector_survives_preempt();
    KICKOS_SELFTEST_LOCAL void t_vector_starts_clean();
    KICKOS_SELFTEST_LOCAL void t_vector_fault_contained();
    KICKOS_SELFTEST_LOCAL void t_vector_survives_migrate();
    KICKOS_SELFTEST_LOCAL void t_grant_kernel_word_refused();
    KICKOS_SELFTEST_LOCAL void t_self_grant_retype();
    KICKOS_SELFTEST_LOCAL void t_uncached_alias_sync();
    KICKOS_SELFTEST_LOCAL void t_uncached_teardown();
    KICKOS_SELFTEST_LOCAL void t_presync_retried();
    KICKOS_SELFTEST_LOCAL void t_presync_race();
    KICKOS_SELFTEST_LOCAL void t_presync_flip();
    KICKOS_SELFTEST_LOCAL void t_presync_slain();
    KICKOS_SELFTEST_LOCAL void t_presync_staged();
    KICKOS_SELFTEST_LOCAL void t_presync_cancel();
    KICKOS_SELFTEST_LOCAL void t_presync_late_params();
    KICKOS_SELFTEST_LOCAL void t_out_of_lock_windows();
    KICKOS_SELFTEST_LOCAL void t_recv_buf_unmapped();
    KICKOS_SELFTEST_LOCAL void t_frame_run_slot_recycle();
    KICKOS_SELFTEST_LOCAL void t_call_reply_undisclosed();
    KICKOS_SELFTEST_LOCAL void t_process_data_from_image();
    KICKOS_SELFTEST_LOCAL void t_process_data_template();
    KICKOS_SELFTEST_LOCAL void t_reent_seating();
    KICKOS_SELFTEST_LOCAL void t_aspace_acquire_balance();
    KICKOS_SELFTEST_LOCAL void t_map_tlbi_elided();
#if KICKOS_KERNEL_CORES > 1 && defined(__x86_64__)
    KICKOS_SELFTEST_LOCAL void t_shootdown_per_change();
#endif
    KICKOS_SELFTEST_LOCAL void t_aspace_active_cores();
    KICKOS_SELFTEST_LOCAL void t_app_pointers_relocated();
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
    KICKOS_SELFTEST_LOCAL void t_amp_inbound_reply();
    KICKOS_SELFTEST_LOCAL void t_amp_port_seating();
    KICKOS_SELFTEST_LOCAL void t_amp_port_unnamed();
    KICKOS_SELFTEST_LOCAL void t_amp_reply_band();
    KICKOS_SELFTEST_LOCAL void t_amp_probe_crossing_holder();
    KICKOS_SELFTEST_LOCAL void t_amp_local_port_slot_held();
    KICKOS_SELFTEST_LOCAL void t_amp_far_slot_reuse();
#if KICKOS_AMP_OWN_IMAGE
    KICKOS_SELFTEST_LOCAL void t_amp_share_crossing();
#if KICKOS_MEMORY_ENFORCED && KICKOS_AMP_USER_SHARE_SIZE != 0
    KICKOS_SELFTEST_LOCAL void t_amp_share_seated();
    KICKOS_SELFTEST_LOCAL void t_amp_share_window();
#endif
#endif
#endif

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    KICKOS_SELFTEST_LOCAL void t_pin_places();
    KICKOS_SELFTEST_LOCAL void t_pin_wrong_core_never();
    KICKOS_SELFTEST_LOCAL void t_unpin_restores();
    KICKOS_SELFTEST_LOCAL void t_affinity_zero_defaults();
    KICKOS_SELFTEST_LOCAL void t_affinity_undriven_refused();
    KICKOS_SELFTEST_LOCAL void t_migrate_running();
    KICKOS_SELFTEST_LOCAL void t_resched_reaches_pinned_caller();
    KICKOS_SELFTEST_LOCAL void t_prio_self_lower_moves_waiter();
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
#if KICKOS_HAVE_ASPACE
    KICKOS_SELFTEST_LOCAL void t_crowd_room_in_root_space();
#endif
    KICKOS_SELFTEST_LOCAL void t_slice_preempts_every_core();
    KICKOS_SELFTEST_LOCAL void t_threads_reach_every_core();
    KICKOS_SELFTEST_LOCAL void t_irq_cross_core_wake();
    KICKOS_SELFTEST_LOCAL void t_irq_reclaim_stale_raise();
    KICKOS_SELFTEST_LOCAL void t_reent_per_thread_cores();
#if defined(__x86_64__)
    KICKOS_SELFTEST_LOCAL void t_fp_enabled_every_core();
#endif
#endif

}

#endif
