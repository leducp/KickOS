// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_USER_APPS_COMMON_SELFTEST_SELFTEST_H
#define KICKOS_USER_APPS_COMMON_SELFTEST_SELFTEST_H

#include <kickos/amp.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/config/cap_width.h>
#include <kickos/sys/bus.h> // compile-checks the wire-ABI struct-size static_asserts
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h>
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
// What a region's TAP_ADD expands to where this image carries the region, and where it does not.
// The asks after the function are the RamAsks the arm takes its arena blocks from.
#define TAP_ADD_LIVE(name, fn, ...)                                                         \
    do                                                                                      \
    {                                                                                       \
        ST_ARM_RECORD(name __VA_OPT__(, ) __VA_ARGS__);                                     \
        ::selftest::st_add<__VA_ARGS__>(name, fn);                                          \
    } while (0)
#define TAP_ADD_ELIDED(name, fn, ...) TAP_ELIDE(fn)

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
    // What an ArmHold gives one slay to finish a teardown.
    constexpr uint32_t ARM_SLAY_US = 500000;

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
#define TAP_ADD_IRQ(name, fn, ...) TAP_ADD(name, main_pinned<fn> __VA_OPT__(, ) __VA_ARGS__)
#define TAP_ADD_PINNED(name, fn, ...) TAP_ADD(name, main_pinned<fn> __VA_OPT__(, ) __VA_ARGS__)
#else
#define irq_spawn kos::thread::create_caps
#define TAP_ADD_IRQ(name, ...) TAP_ADD(name, __VA_ARGS__)
#define TAP_ADD_PINNED(name, ...) TAP_ADD(name, __VA_ARGS__)
#endif

    // Releases what an arm holds however it returns, a failing TAP_CHECK included: unlocks and
    // unbinds, slays tasks, joins threads, slays the ones the joins left, unmaps, and closes
    // capabilities last: a close wakes no waiter, and an unmap under a live thread of main's
    // task faults the whole task. The joins share one bound, each slay waits at most
    // ARM_SLAY_US, and a slay that does not finish is reported and its handle kept.
    // Declare it after every variable it holds, and register through TAP_HOLD.
    // A crowd of one thread per kernel core and one more, beside a capability.
    constexpr int arm_hold_max()
    {
        if (KICKOS_KERNEL_CORES + 2 > 12)
        {
            return KICKOS_KERNEL_CORES + 2;
        }
        return 12;
    }

    class ArmHold
    {
    public:
        static constexpr int MAX = arm_hold_max();

        explicit ArmHold(uint32_t bound_us = STALL_TOLERANT_US)
            : bound_us_(bound_us)
        {
        }
        ArmHold(ArmHold const&) = delete;
        ArmHold& operator=(ArmHold const&) = delete;
        ~ArmHold() { release(); }

        [[nodiscard]] bool cap(kos_cap_t* c) { return add(Kind::CAP, c) != nullptr; }
        // Unlocked first: closing a mutex main holds is refused.
        [[nodiscard]] bool owned(kos_cap_t* m) { return add(Kind::OWNED, m) != nullptr; }
        // Unbound first: closing a notification leaves main bound to it.
        [[nodiscard]] bool bound(kos_cap_t* n) { return add(Kind::BOUND, n) != nullptr; }
        [[nodiscard]] bool thread(kos::thread::Handle const* t)
        {
            Held* const h = add(Kind::THREAD, nullptr);
            if (h == nullptr)
            {
                return false;
            }
            h->thread = t;
            return true;
        }
        [[nodiscard]] bool task(kos_task_t* t)
        {
            Held* const h = add(Kind::TASK, nullptr);
            if (h == nullptr)
            {
                return false;
            }
            h->task = t;
            return true;
        }
#if KICKOS_HAVE_ASPACE
        // Unmapped at *va unless it is 0, then both capabilities closed: hold neither as cap()
        // too, or the unmap finds the run's capability already gone.
        [[nodiscard]] bool mapped(kos_cap_t* frame, kos_cap_t* space, uintptr_t const* va)
        {
            Held* const h = add(Kind::MAPPED, frame);
            if (h == nullptr)
            {
                return false;
            }
            h->space = space;
            h->va = va;
            return true;
        }
#endif

        int close(kos_cap_t* c)
        {
            int const rc = kos_handle_close(*c);
            *c = KOS_CAP_NONE;
            return rc;
        }
        // main's last wait: every thread joined, all within the hold's one bound. A thread that
        // exited and whose slot a later spawn took counts as joined.
        KICKOS_SELFTEST_LOCAL bool joined();

    private:
        enum class Kind : uint8_t
        {
            CAP,
            OWNED,
            BOUND,
            THREAD,
            TASK,
            MAPPED
        };
        struct Held
        {
            union
            {
                kos_cap_t* cap;
                kos::thread::Handle const* thread;
                kos_task_t* task;
            };
            kos_cap_t* space;
            uintptr_t const* va;
            Kind kind;
        };
        KICKOS_SELFTEST_LOCAL Held* add(Kind kind, kos_cap_t* c);
        KICKOS_SELFTEST_LOCAL void release();

        Held held_[MAX];
        int n_ = 0;
        uint32_t bound_us_;
    };

// Returns from the arm when a registration found its ArmHold full, which has failed the arm.
#define TAP_HOLD(registered)  \
    do                        \
    {                         \
        if (not(registered))  \
        {                     \
            return;           \
        }                     \
    } while (0)

#if KICKOS_HAVE_ASPACE
    // The map granule of every translating port; aspace_seam holds the build to it.
    constexpr size_t ASPACE_GRANULE = 4096;

    // kos_frame_map at an address the arm chose.
    inline int map_at(kos_cap_t frame, kos_cap_t space, uintptr_t va, uint32_t flags)
    {
        uintptr_t at = va;
        return kos_frame_map(frame, space, &at, flags);
    }
#endif

    // A bounded POSITIVE edge. A helper waiting on one reports whether the state was REACHED and
    // never that it was not: an absence has no event to wait on, so no arm may read a refusal
    // that outlived the bound as a verdict about ordering.
    constexpr uint64_t IRQ_EDGE_BUDGET_NS = 2000000000ull;
    constexpr uint64_t IRQ_EDGE_POLL_NS = 100000ull;

    // A retired line comes back with its publication record's grace period, which ends when
    // every peer core has left the interrupt dispatch entry, and its binding slot comes back
    // after it. That is not a property of the calling thread's progress, so above one kernel
    // core a first attempt can legally find the line still retiring or the pool still owed its
    // slot; only a refusal that never lifts is a lost line.
    KICKOS_SELFTEST_LOCAL int irq_claim_await(int line, kos_cap_t* out);

    // One message of exactly `len` bytes off `ep`, STALL_TOLERANT_US at most.
    KICKOS_SELFTEST_LOCAL bool report_await(kos_cap_t ep, void* rep, size_t len);
    // Polls `cell`, sleeping between reads, until it reads `want` / anything but `from`, for
    // STALL_TOLERANT_US at most. False when that ran out.
    KICKOS_SELFTEST_LOCAL bool flag_await(Atomic<uint32_t, Order::RELAXED> const& cell,
                                          uint32_t want);
    KICKOS_SELFTEST_LOCAL bool flag_await_change(Atomic<uint32_t, Order::RELAXED> const& cell,
                                                 uint32_t from);
    // Drops main to KICKOS_PRIO_MIN, yields and takes main's priority back, so it returns only
    // once no thread above KICKOS_PRIO_MIN is ready on main's core, including one an interrupt
    // readied meanwhile. An absence check after it holds only for threads above that floor.
    // False when a priority change was refused.
    KICKOS_SELFTEST_LOCAL bool others_parked();
    // Joins *h within `bound_us` and slays it past that; answers the join. The handle is dropped
    // once the thread is gone, and kept for the arm's ArmHold after a refused slay, which fails
    // the arm.
    KICKOS_SELFTEST_LOCAL int thread_end(kos::thread::Handle* h,
                                         uint32_t bound_us = STALL_TOLERANT_US);
    // Slays *t within `bound_us`; on 0 it names no task, and a refusal fails the arm.
    KICKOS_SELFTEST_LOCAL int task_end(kos_task_t* t, uint32_t bound_us = STALL_TOLERANT_US);
    // main's free capability slots, or -1 where an object pool ran out before the table did.
    KICKOS_SELFTEST_LOCAL long cap_census();
    // What an arm holds at once: workers beside main, tasks out of the task pool, objects out of
    // main's task budgets beside the two semaphores main holds for the run, capability slots
    // beyond those objects', the IRQ lines it holds at once, and the one of them the ask claims
    // and hands over. The slots are probed as badged copies of a notification, so an ask stating
    // no notification also holds one for the probe. A line count is weighed against main's
    // task's binding budget, which main spends on no line between arms.
    // tests/static/selftest_demands.py reads each field off TAP_ASK, so every field is a literal
    // or a named constant.
    struct ObjectDemand
    {
        int workers = 0;
        int tasks = 0;
        int sems = 0;
        int mutexes = 0;
        int endpoints = 0;
        int notifies = 0;
        int caps = 0;
        int irqs = 0;
        int irq_line = -1;
    };
    // Everything probed is given back except the line, which is handed over in *line: a line
    // given back is still retiring at its next claim. True once the arm is skipped, naming the
    // supply that ran out, or failed, for a refusal that is not a supply running out.
    KICKOS_SELFTEST_LOCAL bool objects_refused(ObjectDemand want, kos_cap_t* line = nullptr);
// The one ask, before the arm creates anything; returns from the arm when it is refused.
#define TAP_ASK(...)                                       \
    do                                                     \
    {                                                      \
        if (::selftest::objects_refused({__VA_ARGS__}))    \
        {                                                  \
            return;                                        \
        }                                                  \
    } while (0)
// TAP_ASK claiming the demand's irq_line into *line.
#define TAP_ASK_LINE(line, ...)                                    \
    do                                                             \
    {                                                              \
        if (::selftest::objects_refused({__VA_ARGS__}, (line)))    \
        {                                                          \
            return;                                                \
        }                                                          \
    } while (0)
    // --- The arena, reserved once at image start ------------------------------------------
    // An arm states the arena blocks it uses as a RamAsk and names it on its TAP_ADD line, after
    // the function; arms sharing blocks name the same ask. st_ram_seat reserves every ask this
    // image registers before the first arm runs, and an arm takes block I as st_ram<ASK, I>():
    // null when its ask is not registered. A size is in bytes, or ST_GRANULES(n).
    template <int N>
    struct RamAsk
    {
        uint32_t size[N];
    };
    template <class... T>
    RamAsk(T...) -> RamAsk<sizeof...(T)>;
    constexpr uint32_t ST_GRANULE_UNIT = 0x80000000u;
    constexpr uint32_t ST_GRANULES(uint32_t n)
    {
        return ST_GRANULE_UNIT | n;
    }

    // An arm that names no block on a posture where it names one elsewhere names this.
    struct NoRam
    {
    };
    inline constexpr NoRam ST_NO_RAM = {};
    template <int N>
    constexpr int ram_ask_blocks(RamAsk<N> const&)
    {
        return N;
    }
    constexpr int ram_ask_blocks(NoRam const&)
    {
        return 0;
    }

    // KICKOS_SELFTEST_FIT, from this directory's CMakeLists: a region board whose MPU geometry the
    // build scraped, where each image is sized by a first link (arena_fit.py).
#if KICKOS_SELFTEST_FIT
    // arch_ram_region_size and arch_ram_region_align over the two literals the build scraped out
    // of this link's backend.
    constexpr size_t st_region_size(size_t want)
    {
        size_t const min = KICKOS_MPU_MIN_REGION_CFG;
        if (min == 0)
        {
            return (want + 15u) & ~static_cast<size_t>(15u);
        }
        if (want < min)
        {
            want = min;
        }
        if (KICKOS_MPU_REGION_POW2_CFG == 0)
        {
            return (want + (min - 1u)) & ~(min - 1u);
        }
        size_t p = 1;
        while (p < want)
        {
            p = p << 1;
        }
        return p;
    }
    constexpr size_t st_region_align(size_t want)
    {
        size_t geometry = 16u;
        if (KICKOS_MPU_MIN_REGION_CFG != 0)
        {
            geometry = KICKOS_MPU_MIN_REGION_CFG;
            if (KICKOS_MPU_REGION_POW2_CFG != 0)
            {
                geometry = st_region_size(want);
            }
        }
#if defined(KICKOS_TLS) && KICKOS_TLS && KICKOS_TLS_FROM_SP
        if (st_region_size(want) == KICKOS_TLS_STRIDE and KICKOS_TLS_STRIDE > geometry)
        {
            return KICKOS_TLS_STRIDE;
        }
#endif
        return geometry;
    }
    constexpr size_t ST_ARENA_GRANULE = st_region_size(1);
    constexpr size_t st_bytes_at_link(uint32_t size)
    {
        if ((size & ST_GRANULE_UNIT) != 0)
        {
            return (size & ~ST_GRANULE_UNIT) * ST_ARENA_GRANULE;
        }
        return size;
    }
    template <int N>
    constexpr size_t ram_ask_bytes(RamAsk<N> const& a)
    {
        size_t sum = 0;
        for (int i = 0; i < N; i++)
        {
            sum += st_region_size(st_bytes_at_link(a.size[i]));
        }
        return sum;
    }
    constexpr size_t ram_ask_bytes(NoRam const&)
    {
        return 0;
    }
    constexpr size_t ram_ask_align(NoRam const&)
    {
        return 1;
    }
    template <int N>
    constexpr size_t ram_ask_align(RamAsk<N> const& a)
    {
        size_t most = 1;
        for (int i = 0; i < N; i++)
        {
            size_t const align = st_region_align(st_bytes_at_link(a.size[i]));
            if (align > most)
            {
                most = align;
            }
        }
        return most;
    }
#if defined(__riscv)
#define ST_ASM_INT(n) "%" #n
#else
#define ST_ASM_INT(n) "%c" #n
#endif
    // An arm's record, read off the image's sizing link by arena_fit.py: its name, then each
    // ask's name, region bytes and largest alignment. The fit walks them in registration order.
#define ST_ARM_RECORD0(name)                                                                \
    asm volatile(".pushsection .kickos_arena_arm,\"\",%%progbits\n\t.asciz " #name          \
                 "\n\t.byte 0\n\t.popsection" ::)
#define ST_ARM_RECORD1(name, A)                                                             \
    asm volatile(".pushsection .kickos_arena_arm,\"\",%%progbits\n\t.asciz " #name          \
                 "\n\t.byte 1\n\t.asciz \"" #A "\"\n\t.4byte " ST_ASM_INT(0)                 \
                 "\n\t.4byte " ST_ASM_INT(1) "\n\t.popsection"                              \
                 :                                                                          \
                 : "i"(ram_ask_bytes(A)), "i"(ram_ask_align(A)))
#define ST_ARM_RECORD2(name, A, B)                                                          \
    asm volatile(".pushsection .kickos_arena_arm,\"\",%%progbits\n\t.asciz " #name          \
                 "\n\t.byte 2\n\t.asciz \"" #A "\"\n\t.4byte " ST_ASM_INT(0)                 \
                 "\n\t.4byte " ST_ASM_INT(1) "\n\t.asciz \"" #B "\"\n\t.4byte " ST_ASM_INT(2) \
                 "\n\t.4byte " ST_ASM_INT(3) "\n\t.popsection"                              \
                 :                                                                          \
                 : "i"(ram_ask_bytes(A)), "i"(ram_ask_align(A)), "i"(ram_ask_bytes(B)),      \
                   "i"(ram_ask_align(B)))
#define ST_ARM_RECORD_WIDER(...)                                                            \
    static_assert(sizeof(#__VA_ARGS__) == 0, "an arm names at most two asks")
#define ST_ARM_PICK(a, b, c, d, pick, ...) pick
#define ST_ARM_RECORD(...)                                                                  \
    ST_ARM_PICK(__VA_ARGS__, ST_ARM_RECORD_WIDER, ST_ARM_RECORD2, ST_ARM_RECORD1,           \
                ST_ARM_RECORD0, )(__VA_ARGS__)
#else
#define ST_ARM_RECORD(...) ((void)0)
#endif

    // The arena's allocation granule: the stride of two one-byte reservations.
    KICKOS_SELFTEST_LOCAL size_t arena_granule();

    struct RamNode
    {
        uint32_t const* size;
        int count;
        void** block;
        char const* arm;
        RamNode* next;
    };
    // Links `node` into the image's reservation once, whatever number of arms name its ask; `arm`
    // is the first to.
    KICKOS_SELFTEST_LOCAL void st_ram_link(RamNode* node, char const* arm);
    // Reserves every linked ask's blocks; false, the image running no arm, when the arena could
    // not back one.
    KICKOS_SELFTEST_LOCAL bool st_ram_seat();

    template <auto const& A>
    constexpr int ram_ask_count = ram_ask_blocks(A);
    template <auto const& A>
    KICKOS_SELFTEST_LOCAL void* st_ram_blocks[ram_ask_count<A>] = {};
    template <auto const& A>
    KICKOS_SELFTEST_LOCAL RamNode st_ram_node = {A.size, ram_ask_count<A>, st_ram_blocks<A>,
                                                 nullptr, nullptr};
    template <auto const& A, int I>
    void* st_ram()
    {
        static_assert(I >= 0 and I < ram_ask_count<A>, "the ask states no such block");
        return st_ram_blocks<A>[I];
    }

    template <auto const& A>
    KICKOS_SELFTEST_LOCAL void st_ram_note(char const* arm)
    {
        if constexpr (ram_ask_count<A> > 0)
        {
            st_ram_link(&st_ram_node<A>, arm);
        }
    }

    // A bit per registration, in order, set where arena_fit.py left the arm out, and the worker
    // stacks the image seats at start. The bitmap is one size in the sizing link and the image.
    extern KICKOS_SELFTEST_LOCAL unsigned char const st_left_out[];
    extern KICKOS_SELFTEST_LOCAL int const st_seat_workers;
    // Whether the next registration is one arena_fit.py left out.
    KICKOS_SELFTEST_LOCAL bool st_next_left_out();

    template <auto const&... A, size_t N>
    void st_add(char const (&name)[N], tap::TestFn fn)
    {
        static_assert(N - 1 <= tap::NAME_CHARS_MAX,
                      "a TAP name past NAME_CHARS_MAX voids REASON_CHARS_MAX for every arm");
        static_assert(sizeof...(A) <= 2, "an arm names at most two asks");
        if (st_next_left_out())
        {
            return;
        }
        tap::add_named(name, fn);
        (st_ram_note<A>(name), ...);
    }

#if KICKOS_HAVE_ASPACE
    // A reservation in a task of the arm's own, which that task's end gives back. Translating
    // backends only: on a bump arena nothing gives a block back.
    inline void* st_ram_own(size_t size)
    {
        return kos_ram_alloc(size);
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
    // A member of ANOTHER task gets its own copy of this image's static data, so every report
    // from one crosses on an ENDPOINT and every release crosses on a semaphore. A global
    // would be written in the member's copy and read in main's.
    extern KICKOS_SELFTEST_LOCAL kos_cap_t g_pl_ep; // main's report endpoint, delegated at child index 1
#endif

#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    inline constexpr RamAsk SEAM_RAM = {256u};
    inline constexpr RamAsk SP_RAM = {256u};
    inline constexpr RamAsk PW_RAM = {256u, 256u};
    inline constexpr RamAsk SIB_RAM = {256u};
    inline constexpr RamAsk HO_RAM = {256u, 256u};
    inline constexpr RamAsk SL_RAM = {ST_GRANULES(2)};
    inline constexpr RamAsk DX_RAM = {64u, 256u};
#if defined(KICKOS_TLS) && KICKOS_TLS
    // One stride-aligned victim stack of a stride and a verdict slot, whatever the base.
    inline constexpr RamAsk PFH_RAM = {3u * KICKOS_TLS_STRIDE};
#else
    inline constexpr RamAsk PFH_RAM = {3u * 8192u};
#endif
    inline constexpr RamAsk PI_RAM = {256u, 256u};
    inline constexpr RamAsk KW_RAM = {64u};
    inline constexpr RamAsk UA_RAM = {128u, 128u, ST_GRANULES(32)};
    inline constexpr RamAsk FI_RAM = {8u};
    KICKOS_SELFTEST_LOCAL void t_cap_map();
    KICKOS_SELFTEST_LOCAL void t_stack_slot_returns();
    KICKOS_SELFTEST_LOCAL void t_cap_map_over_stack();
    KICKOS_SELFTEST_LOCAL void t_stack_grant_refused();
    KICKOS_SELFTEST_LOCAL void t_stack_handoff_refused();
    KICKOS_SELFTEST_LOCAL void t_cap_map_pins_run();
    KICKOS_SELFTEST_LOCAL void t_cap_share();
    KICKOS_SELFTEST_LOCAL void t_frame_mint_census();
    KICKOS_SELFTEST_LOCAL void t_space_cap_dies_with_task();
    KICKOS_SELFTEST_LOCAL void t_aspace_seam();
    KICKOS_SELFTEST_LOCAL void t_aspace_churn();
    KICKOS_SELFTEST_LOCAL void t_stack_is_frames();
    KICKOS_SELFTEST_LOCAL void t_aspace_two_spaces_same_grant();
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
    KICKOS_SELFTEST_LOCAL void t_process_ipc_same_addr();
    KICKOS_SELFTEST_LOCAL void t_process_call_reply();
    KICKOS_SELFTEST_LOCAL void t_kernel_state_unreachable();
    KICKOS_SELFTEST_LOCAL void t_window_addr();
    KICKOS_SELFTEST_LOCAL void t_port_window();
    KICKOS_SELFTEST_LOCAL void t_vector_survives_block();
    KICKOS_SELFTEST_LOCAL void t_vector_survives_preempt();
    KICKOS_SELFTEST_LOCAL void t_vector_starts_clean();
    KICKOS_SELFTEST_LOCAL void t_vector_fault_contained();
    KICKOS_SELFTEST_LOCAL void t_vector_survives_migrate();
    KICKOS_SELFTEST_LOCAL void t_grant_kernel_word_refused();
    KICKOS_SELFTEST_LOCAL void t_uncached_alias_sync();
    KICKOS_SELFTEST_LOCAL void t_recv_buf_unmapped();
    KICKOS_SELFTEST_LOCAL void t_frame_run_slot_recycle();
    KICKOS_SELFTEST_LOCAL void t_call_reply_undisclosed();
    KICKOS_SELFTEST_LOCAL void t_process_data_from_image();
    KICKOS_SELFTEST_LOCAL void t_app_pointers_relocated();
#endif

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_AMP_NODE
    constexpr uint32_t AG_BLK = 256;
    inline constexpr RamAsk AG_RAM = {AG_BLK};
    inline constexpr RamAsk ASW_RAM = {ST_GRANULES(1)};
    KICKOS_SELFTEST_LOCAL void t_amp_count_reads();
    KICKOS_SELFTEST_LOCAL void t_amp_far_call();
    KICKOS_SELFTEST_LOCAL void t_amp_far_reply_empty();
    KICKOS_SELFTEST_LOCAL void t_amp_port_seating();
    KICKOS_SELFTEST_LOCAL void t_amp_port_unnamed();
    KICKOS_SELFTEST_LOCAL void t_amp_crossing_task_local();
    KICKOS_SELFTEST_LOCAL void t_amp_local_port_slot_held();
#if KICKOS_AMP_OWN_IMAGE
    KICKOS_SELFTEST_LOCAL void t_amp_share_crossing();
#if KICKOS_HAVE_ASPACE
    KICKOS_SELFTEST_LOCAL void t_amp_far_reply_unmapped();
#endif
#if KICKOS_MEMORY_ENFORCED && KICKOS_AMP_USER_SHARE_SIZE != 0
    KICKOS_SELFTEST_LOCAL void t_amp_share_window();
#endif
#endif
#endif

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    KICKOS_SELFTEST_LOCAL void t_pin_places();
    KICKOS_SELFTEST_LOCAL void t_unpin_restores();
    KICKOS_SELFTEST_LOCAL void t_pin_beyond_grant_refused();
    KICKOS_SELFTEST_LOCAL void t_affinity_undriven_refused();
    KICKOS_SELFTEST_LOCAL void t_migrate_running();
    KICKOS_SELFTEST_LOCAL void t_resched_reaches_pinned_caller();
    KICKOS_SELFTEST_LOCAL void t_prio_self_lower_moves_waiter();
    KICKOS_SELFTEST_LOCAL void t_pin_cross_task_refused();
    KICKOS_SELFTEST_LOCAL void t_grant_narrows();
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
    KICKOS_SELFTEST_LOCAL void t_reent_per_thread_cores();
#if defined(__x86_64__)
    KICKOS_SELFTEST_LOCAL void t_fp_enabled_every_core();
#endif
#endif

}

// Every block an arm uses is reserved at image start (st_ram_seat): a reservation made mid-run
// does not build.
#ifndef KICKOS_SELFTEST_RAM_SEAT
#pragma GCC poison kos_ram_alloc ram_alloc
#endif

#endif
