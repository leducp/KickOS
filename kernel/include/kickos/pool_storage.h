// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The storage of the kernel's pools at one system's sizes, which the system's generated pools TU
// instantiates once per instance and describes to the kernel (<kickos/pools.h>). Every check
// a pool's size owes the encodings it is read through is here, so a size the kernel cannot
// carry fails the system's own build.

#ifndef KICKOS_POOL_STORAGE_H
#define KICKOS_POOL_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/pools.h>
#include <kickos/ramown.h>
#include <kickos/vrange.h>

#if KICKOS_HAVE_ASPACE
#include <kickos/arch/aspace_residency.h>
#endif

namespace kickos
{
    // A fingerprint of everything the kernel archives and a pools TU must agree on: the ceilings,
    // the instance count, the size and alignment of every pooled type, the offset and size of
    // every member of the descriptor the TU fills, and each budget kind's name and value. A
    // pooled type's own members are left out: the TU stores each zeroed, whatever their order
    // (tests/unit/taskpool/pool_layout.cc).
    constexpr uint32_t pool_layout_mix(uint32_t h, uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            h = (h ^ ((v >> (8 * i)) & 0xFFu)) * 16777619u;
        }
        return h;
    }

    template <class T>
    constexpr uint32_t pool_layout_type(uint32_t h)
    {
        return pool_layout_mix(pool_layout_mix(h, sizeof(T)), alignof(T));
    }

    constexpr uint32_t pool_layout_name(uint32_t h, char const* name)
    {
        while (*name != '\0')
        {
            h = pool_layout_mix(h, static_cast<unsigned char>(*name));
            name++;
        }
        return h;
    }

#define KICKOS_POOL_LAYOUT_FIELD(h, T, m) \
    h = pool_layout_mix(pool_layout_mix(h, offsetof(T, m)), sizeof(T::m))

    constexpr uint32_t pool_layout_descriptor(uint32_t h)
    {
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, stride);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, threads);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, thread_gen);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, thread_gen_hi);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, thread_links);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, word_records);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, cap_dirs);
#if KICKOS_HAVE_ASPACE
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, presync);
#endif
#if KICKOS_KERNEL_STACKS
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, kstacks);
#endif
#if KICKOS_KERNEL_CORES > 1
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, sched_rings);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, sched_ring_depth);
#endif
#if KICKOS_BENCH
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, bench_moves);
#endif
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, thread_slots);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, tasks);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, task_links);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, task_count);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, domains);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, domain_count);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, endpoints);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, notifies);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, irq_bindings);
#if KICKOS_HAVE_ASPACE
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, frame_runs);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, residency_rows);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, residency_taken);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, image_headers);
#endif
#if not KICKOS_HAVE_ASPACE
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, ram_owners);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, ram_owner_count);
#endif
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, cap_chunks);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, cap_chunk_count);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, cap_root_width);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, cap_dir_length);
        KICKOS_POOL_LAYOUT_FIELD(h, KernelPools, init_budgets);
        KICKOS_POOL_LAYOUT_FIELD(h, SlotArrays, gen);
        KICKOS_POOL_LAYOUT_FIELD(h, SlotArrays, refs);
        KICKOS_POOL_LAYOUT_FIELD(h, SlotArrays, nodes);
        KICKOS_POOL_LAYOUT_FIELD(h, SlotArrays, count);
        KICKOS_POOL_LAYOUT_FIELD(h, SlotStore<Endpoint>, arrays);
        KICKOS_POOL_LAYOUT_FIELD(h, SlotStore<Endpoint>, slots);
#define KICKOS_POOL_LAYOUT_KIND(name, key, pool) \
    h = pool_layout_mix(pool_layout_name(h, #key), KOS_BUDGET_##name);
        KOS_BUDGET_KINDS(KICKOS_POOL_LAYOUT_KIND)
#undef KICKOS_POOL_LAYOUT_KIND
        h = pool_layout_mix(h, KOS_BUDGET_KIND_COUNT);
        return h;
    }

    constexpr uint32_t pool_layout()
    {
        uint32_t h = 2166136261u;
        h = pool_layout_mix(h, KICKOS_THREAD_SLOT_CEILING);
        h = pool_layout_mix(h, KICKOS_HANDLE_INDEX_BITS);
        h = pool_layout_mix(h, KCAP_WIDTH_CEILING);
        h = pool_layout_mix(h, KCAP_CHUNK_SLOTS);
        h = pool_layout_mix(h, KICKOS_MAX_INSTANCES);
        h = pool_layout_mix(h, KICKOS_KERNEL_CORES);
        h = pool_layout_type<KernelPools>(h);
        h = pool_layout_type<Thread>(h);
        h = pool_layout_type<Task>(h);
        h = pool_layout_type<Domain>(h);
        h = pool_layout_type<Endpoint>(h);
        h = pool_layout_type<Notification>(h);
        h = pool_layout_type<IrqBinding>(h);
        h = pool_layout_type<WordRecord>(h);
        h = pool_layout_type<CapEntry>(h);
        h = pool_layout_type<ThreadLinks>(h);
        h = pool_layout_type<TaskLinks>(h);
#if KICKOS_HAVE_ASPACE
        h = pool_layout_type<FrameRun>(h);
        h = pool_layout_type<aspace::ResidencyRow>(h);
#if KICKOS_PRESYNC
        h = pool_layout_type<PresyncRecord>(h);
#endif
#endif
#if not KICKOS_HAVE_ASPACE
        h = pool_layout_type<RamBlock>(h);
#endif
#if KICKOS_KERNEL_STACKS
        h = pool_layout_mix(h, KICKOS_KERNEL_STACK_SIZE);
        h = pool_layout_mix(h, KICKOS_STACK_ALIGN);
#endif
#if KICKOS_BENCH_SCHED_ON
        h = pool_layout_type<BenchMoveStamp>(h);
#endif
        return pool_layout_descriptor(h);
    }

    constexpr uint32_t POOL_LAYOUT = pool_layout();

    // One generational pool's slots and their side arrays.
    template <class T, int N>
    struct SlotStorage
    {
        static_assert(N < static_cast<int>(HANDLE_INDEX_MASK),
                      "a pool this wide would seat the reserved all-ones index");

        T slots[N];
        uint32_t gen[N] = {};
        ListNode nodes[N] = {};
        uint8_t refs[N] = {};

        constexpr SlotStore<T> store()
        {
            return SlotStore<T>{{gen, refs, nodes, N}, slots};
        }
    };

    template <int Threads, int Tasks, int Domains, int Endpoints, int Notifies, int IrqBindings,
              int FrameRuns, int RamOwners, uint32_t CapSlots, uint32_t DirLength,
              uint32_t RootWidth>
    struct PoolStorage
    {
        static constexpr uint32_t DIR_LENGTH = DirLength;
        static constexpr uint32_t CAP_CHUNKS = CapSlots / KCAP_CHUNK_SLOTS;
        static constexpr int IMAGES = Tasks + 1;
#if KICKOS_KERNEL_CORES > 1
        static constexpr uint32_t RING_DEPTH = sched_ring_depth_for(2u * Threads);
#endif

        static_assert(Threads >= 1 and Threads <= KICKOS_THREAD_SLOT_CEILING,
                      "the thread pool holds root's slot and stays within the thread-slot "
                      "ceiling that Task::refcount and the lock id are sized for");
        static_assert(Threads < ThreadPool::FAR_REPLY_BASE,
                      "the thread pool reaches into the reply-record band, so a far reply "
                      "capability could resolve to a local thread");
        static_assert(Tasks >= 2,
                      "the task pool must seat idle's task and root's, which boot cannot refuse");
        static_assert(Tasks <= KICKOS_TASK_SLOT_CEILING and KICKOS_TASK_SLOT_CEILING < UINT8_MAX,
                      "Task::image holds an image index in a byte, and a creator's list an "
                      "index plus one");
        static_assert(Domains > 2,
                      "the domain pool must seat both immortal singletons and still leave a "
                      "slot to hand out");
        static_assert(Domains <= KICKOS_SLOT_POOL_CEILING
                          and KICKOS_SLOT_POOL_CEILING < static_cast<int>(HANDLE_INDEX_MASK),
                      "a domain index must never be all ones");
        static_assert(2ull * Tasks + Domains <= UINT16_MAX,
                      "Domain::refcount is uint16_t and counts live tasks, creator holds and "
                      "borrowers: twice the task pool plus the domain pool must fit it");
        static_assert(Endpoints <= KICKOS_SLOT_POOL_CEILING and Notifies <= KICKOS_SLOT_POOL_CEILING
                          and KICKOS_SLOT_POOL_CEILING < UINT16_MAX,
                      "an endpoint's served-chain ref would collide with EP_SERVED_NONE");
        static_assert(IrqBindings <= KICKOS_IRQ_BINDING_CEILING
                          and KICKOS_IRQ_BINDING_CEILING < 255,
                      "a binding pool index biased by one must fit the uint8_t chain link, with "
                      "zero left over for the sentinel");
        static_assert(RootWidth > KICKOS_CAP_FIRST_DYNAMIC and RootWidth <= KCAP_WIDTH_MAX
                          and RootWidth == kcap_width_for(RootWidth),
                      "the init's table holds a dynamic slot, is whole chunks and stays within "
                      "the width ceiling");
        static_assert(DirLength >= kcap_chunks_for(RootWidth)
                          and DirLength * KCAP_CHUNK_SLOTS <= KCAP_WIDTH_MAX,
                      "a directory row seats the init's table and no table past the ceiling");
        static_assert(CapSlots % KCAP_CHUNK_SLOTS == 0 and CapSlots >= RootWidth,
                      "the slab is whole chunks and backs at least the init's table");
        static_assert(CAP_CHUNKS * KCAP_CHUNK_SLOTS <= UINT32_MAX / sizeof(CapEntry),
                      "the slab's bytes overflow");
#if KICKOS_HAVE_ASPACE
        static_assert(FrameRuns <= KICKOS_SLOT_POOL_CEILING,
                      "a frame-run slot index plus one must fit VirtualRange::run");
        // Root's space: its image, its one thread's stack, the share and VR_APP_HEADROOM, which
        // may not fall to zero, as the self-grant arm reads the free-slot count live and takes
        // one more. A task's own space holds a stack per thread of its THREADS budget, which the
        // composer checks per declaration (supply.ranges).
        static_assert(KICKOS_ASPACE_RANGES
                          >= VR_IMAGE_SLOTS + 1 + VR_APP_HEADROOM + VR_SHARE_SLOTS,
                      "KICKOS_ASPACE_RANGES is below 2 + 1 + 4, plus 1 where root holds an AMP "
                      "partition's user share: the image spends two slots, root's thread's stack "
                      "one, the share one in root's space, and the app needs the rest. Raise "
                      "KICKOS_ASPACE_RANGES");
#endif
#if not KICKOS_HAVE_ASPACE
        static_assert(RamOwners <= KICKOS_SLOT_POOL_CEILING,
                      "a RAM owner record's chain link is a uint16_t index plus one");
#endif

#if KICKOS_KERNEL_STACKS
        static_assert(KICKOS_KERNEL_STACK_SIZE % KICKOS_STACK_ALIGN == 0,
                      "slot i's kernel stack top is misaligned for every odd i");
        alignas(KICKOS_STACK_ALIGN) unsigned char kstacks[Threads][KICKOS_KERNEL_STACK_SIZE];
#endif
        CapEntry cap_chunks[CAP_CHUNKS * KCAP_CHUNK_SLOTS];
        Thread threads[Threads];
#if KICKOS_PRESYNC
        PresyncRecord presync[Threads] = {};
#endif
        WordRecord word_records[Threads];
        Domain domains[Domains];
        Task tasks[Tasks];
#if KICKOS_HAVE_ASPACE
        SlotStorage<FrameRun, FrameRuns> frame_runs;
        kos_image_header const* image_headers[IMAGES] = {};
#endif
        SlotStorage<Endpoint, Endpoints> endpoints;
        SlotStorage<Notification, Notifies> notifies;
        SlotStorage<IrqBinding, IrqBindings> irq_bindings;
#if not KICKOS_HAVE_ASPACE
        RamBlock ram_owners[RamOwners];
#endif
        CapEntry* cap_dirs[Threads * DIR_LENGTH] = {};
#if KICKOS_KERNEL_CORES > 1
        uint16_t sched_rings[KICKOS_KERNEL_CORES * KICKOS_KERNEL_CORES * RING_DEPTH] = {};
#endif
        uint16_t thread_gen[Threads] = {};
        uint16_t thread_gen_hi[Threads] = {};
        ThreadLinks thread_links[Threads] = {};
        TaskLinks task_links[Tasks] = {};

        // What one machine keeps once, however many kernel instances it hosts: the arch's
        // residency rows, a row per domain slot, and the scheduler bench's move stamps.
        struct Shared
        {
#if KICKOS_HAVE_ASPACE
            aspace::ResidencyRow residency_rows[Domains] = {};
            uint32_t residency_taken[(Domains + 31) / 32] = {};
#endif
#if KICKOS_BENCH_SCHED_ON
            BenchMoveStamp bench_moves[Threads] = {};
#endif
        };

        // The descriptor of `first`, instance 0 of an array of KICKOS_MAX_INSTANCES, and of the
        // machine's `shared`.
        static constexpr KernelPools descriptor(PoolStorage* first, Shared* shared,
                                                uint32_t const* init_budgets)
        {
            (void)shared;
            KernelPools d = {};
            for (int k = 0; k < KOS_BUDGET_KIND_COUNT; k++)
            {
                d.init_budgets[k] = init_budgets[k];
            }
            d.stride = sizeof(PoolStorage);
            d.threads = first->threads;
            d.thread_gen = first->thread_gen;
            d.thread_gen_hi = first->thread_gen_hi;
            d.thread_links = first->thread_links;
            d.word_records = first->word_records;
            d.cap_dirs = first->cap_dirs;
#if KICKOS_HAVE_ASPACE
            d.presync = nullptr;
#if KICKOS_PRESYNC
            d.presync = first->presync;
#endif
#endif
#if KICKOS_KERNEL_STACKS
            d.kstacks = &first->kstacks[0][0];
#endif
#if KICKOS_KERNEL_CORES > 1
            d.sched_rings = first->sched_rings;
            d.sched_ring_depth = RING_DEPTH;
#endif
#if KICKOS_BENCH
            d.bench_moves = nullptr;
#if KICKOS_BENCH_SCHED_ON
            d.bench_moves = shared->bench_moves;
#endif
#endif
            d.thread_slots = Threads;
            d.tasks = first->tasks;
            d.task_links = first->task_links;
            d.task_count = Tasks;
            d.domains = first->domains;
            d.domain_count = Domains;
            d.endpoints = first->endpoints.store();
            d.notifies = first->notifies.store();
            d.irq_bindings = first->irq_bindings.store();
#if KICKOS_HAVE_ASPACE
            d.frame_runs = first->frame_runs.store();
            d.residency_rows = shared->residency_rows;
            d.residency_taken = shared->residency_taken;
            d.image_headers = first->image_headers;
#endif
#if not KICKOS_HAVE_ASPACE
            d.ram_owners = first->ram_owners;
            d.ram_owner_count = RamOwners;
#endif
            d.cap_chunks = first->cap_chunks;
            d.cap_chunk_count = CAP_CHUNKS;
            d.cap_root_width = RootWidth;
            d.cap_dir_length = DIR_LENGTH;
            return d;
        }
    };
}

// The machine's one copy of PoolStorage::Shared, where it holds anything.
#if KICKOS_HAVE_ASPACE or KICKOS_BENCH_SCHED_ON
#define KICKOS_POOLS_SHARED_DEFINE(pools) constinit pools::Shared system_shared;
#define KICKOS_POOLS_SHARED (&system_shared)
#else
#define KICKOS_POOLS_SHARED_DEFINE(pools)
#define KICKOS_POOLS_SHARED nullptr
#endif

#define KICKOS_POOLS_STR_(x) #x
#define KICKOS_POOLS_STR(x) KICKOS_POOLS_STR_(x)

// The pools of one system, each count a figure the composer emitted: the object pools, the
// capability slab's slots, a directory row's chunks and the init's table width, then the init's
// budgets in enum kos_budget_kind order. In one TU of the system's kernel archive: the storage,
// the one descriptor the kernel reads, the layout witness, and the absolute symbol
// kickos_pool_stacks, the default stacks the chip script's arena assert prices
// (<boot_arena.ld.h>): every slot's but root's, whose stack is the boot replay's.
#define KICKOS_SYSTEM_POOLS(threads, tasks, domains, endpoints, notifies, irq_bindings,      \
                            frame_runs, ram_owners, cap_slots, cap_dir, root_width, ...)      \
    namespace kickos                                                                          \
    {                                                                                         \
        namespace                                                                             \
        {                                                                                     \
            using SystemPools = PoolStorage<threads, tasks, domains, endpoints, notifies,      \
                                            irq_bindings, frame_runs, ram_owners, cap_slots,   \
                                            cap_dir, root_width>;                              \
            constinit SystemPools system_pools[KICKOS_MAX_INSTANCES];                         \
            KICKOS_POOLS_SHARED_DEFINE(SystemPools)                                           \
            constexpr uint32_t system_init_budgets[] = {__VA_ARGS__};                         \
            static_assert(sizeof(system_init_budgets) / sizeof(system_init_budgets[0])        \
                              == KOS_BUDGET_KIND_COUNT,                                       \
                          "the init's budgets name one figure per budget kind");              \
        }                                                                                     \
        constinit KernelPools const kickos_kernel_pools =                                     \
            SystemPools::descriptor(system_pools, KICKOS_POOLS_SHARED, system_init_budgets);  \
        template <>                                                                           \
        char const PoolLayout<POOL_LAYOUT>::tag = 0;                                          \
    }                                                                                         \
    __asm__(".globl " KICKOS_POOLS_STR(__USER_LABEL_PREFIX__) "kickos_pool_stacks\n"         \
            ".set " KICKOS_POOLS_STR(__USER_LABEL_PREFIX__) "kickos_pool_stacks, "            \
            #threads " - 1\n");

#endif
