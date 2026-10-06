// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A spawn refused because its MPU would decide an overlap otherwise than the kernel hands back
// everything it claimed: the reclaimed slot EXITED at its generation and its region set
// untouched, the kernel stack on the free list, and the task it built held by nobody.

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <string.h>

#include "kseam_test.h"

#if not KICKOS_MEMORY_ENFORCED or not KICKOS_HAVE_MPU or KICKOS_HAVE_ASPACE
#error "this gate refuses a spawn over a region set"
#endif

namespace kickos
{
    int thread_create_call(kos_thread_params const* p, kos_thread_t* out_thread);
}

namespace
{
    alignas(4096) unsigned char g_arena[2u * KICKOS_USER_STACK_SIZE];
    size_t g_arena_used = 0;

    unsigned char g_code[64];
    unsigned char g_appdata[64];
}

extern "C"
{
    unsigned char* const __kickos_code_start = g_code;
    unsigned char* const __kickos_code_end = g_code + sizeof(g_code);
    unsigned char* const __kickos_appdata_start = g_appdata;
    unsigned char* const __kickos_appdata_end = g_appdata + sizeof(g_appdata);

    void* arch_ram_alloc(size_t size)
    {
        if (g_arena_used + size > sizeof(g_arena))
        {
            return nullptr;
        }
        void* const p = &g_arena[g_arena_used];
        g_arena_used += size;
        return p;
    }

    uintptr_t arch_ram_base(void)
    {
        return reinterpret_cast<uintptr_t>(g_arena);
    }

    size_t arch_ram_size(void)
    {
        return sizeof(g_arena);
    }

    size_t arch_mpu_min_region(void)
    {
        return 32u;
    }

    int arch_mpu_region_pow2(void)
    {
        return 0;
    }

    bool arch_mpu_region_encodable(uintptr_t, size_t size)
    {
        return size != 0;
    }

    int arch_mpu_nocache_support(void)
    {
        return ARCH_MPU_NOCACHE_ALREADY;
    }

    int arch_bitband_present(void)
    {
        return 0;
    }

    struct arch_reserved_span arch_reserved_blocks(void)
    {
        return {};
    }

    struct arch_reserved_span arch_bus_master_apertures(void)
    {
        return {};
    }

    size_t arch_domain_static_regions(struct arch_mpu_region*, size_t)
    {
        return 0;
    }

    void arch_context_init(struct arch_context*, void (*)(void*), void*, void*, size_t, int)
    {
    }
}

namespace kickos
{
    bool user_readable_ok(uintptr_t, size_t)
    {
        return true;
    }

    bool kaccess_from_user(void* dst, Thread const*, uintptr_t src, size_t n)
    {
        memcpy(dst, reinterpret_cast<void const*>(src), n);
        return true;
    }

    bool console_window_withheld(uintptr_t, size_t, Task const*)
    {
        return false;
    }

    Domain* domain_kernel(void)
    {
        return nullptr;
    }

    size_t domain_region_count(Domain const* d)
    {
        if (d == nullptr)
        {
            return 0;
        }
        return d->region_count;
    }

    arch_mpu_region const* domain_region_at(Domain const* d, size_t i)
    {
        return &d->regions[i];
    }

    namespace testfix
    {
        namespace
        {
            class SpawnUnwind : public KSeam
            {
            };

            void entry(void*) {}

            // The domain_for of the K-seam hands out its first free slot.
            Domain* next_domain()
            {
                for (int i = 0; i < FIXTURE_DOMAIN_SLOTS; i++)
                {
                    if (g_domain_refs[i] == 0 and not g_domain_live[i])
                    {
                        return &g_domains[i];
                    }
                }
                return nullptr;
            }

            TEST_F(SpawnUnwind, an_overlap_refusal_hands_back_everything_it_claimed)
            {
                Kernel& k = kernel();
                ASSERT_EQ(ARCH_MPU_OVERLAP, ARCH_MPU_OVERLAP_HIGHER);

                // A privileged spawner, so the unprivileged child builds a task of its own.
                int derr = 0;
                Task* const own = task_for(DOM_CALLER_PRIVILEGED, nullptr, 0, nullptr, &derr);
                ASSERT_NE(own, nullptr);
                Thread* const spawner = seat_pool(2, 5);
                spawner->privileged = true;
                spawner->task = own;
                task_ref(own);
                k.current[kickos_kernel_core()] = spawner;

                // Slot 1 is reclaimable, with a region set of its own left from its occupant.
                Thread& was = k.threads.slots[1];
                was.state = ThreadState::EXITED;
                was.mpu.clear();
                uintptr_t const mark = reinterpret_cast<uintptr_t>(g_code);
                ASSERT_TRUE(was.mpu.add(mark, sizeof(g_code), ARCH_MPU_R));
                uint16_t const gen = k.threads.gen[1];

                // The child's task data reads the block its kernel stack will be: read-only
                // under a writable stack is decided writable by the higher region.
                void* const stack = &g_arena[g_arena_used];
                Domain* const dom = next_domain();
                ASSERT_NE(dom, nullptr);
                dom->regions[0] = {reinterpret_cast<uintptr_t>(stack),
                                   arch_ram_region_size(KICKOS_USER_STACK_SIZE), ARCH_MPU_R};
                dom->region_count = 1;

                kos_thread_params p = {};
                p.entry = entry;
                p.name = "refused";
                p.prio = 1;
                kos_thread_t out = 0;
                EXPECT_EQ(thread_create_call(&p, &out), -KOS_EINVAL);
                EXPECT_EQ(out, KOS_THREAD_NONE);

                EXPECT_EQ(was.state, ThreadState::EXITED);
                EXPECT_EQ(k.threads.gen[1], gen) << "the slot kept the claim's generation";
                ASSERT_EQ(was.mpu.end() - was.mpu.begin(), 1) << "the slot's set was written";
                EXPECT_EQ(was.mpu.begin()->base, mark);

                EXPECT_EQ(k.threads.stack_pop(), stack) << "the kernel stack was not handed back";
                EXPECT_EQ(k.threads.stack_pop(), nullptr);

                // Liveness is intrinsic on a region backend: a task and a domain nobody holds
                // are free slots.
                EXPECT_EQ(dom->refcount, 0u) << "the built task's domain is held";
                for (int i = 0; i < KICKOS_MAX_TASKS; i++)
                {
                    Task const& t = k.tasks[i];
                    if (&t != own)
                    {
                        EXPECT_EQ(t.refcount, 0u) << "task " << i << " is held";
                        EXPECT_EQ(t.creator_tag, ThreadPool::KILL_TAG_NONE) << "task " << i;
                    }
                }
            }
        }
    }
}
