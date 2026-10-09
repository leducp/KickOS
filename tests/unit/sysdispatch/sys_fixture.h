// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A K-seam case over the real system-call layer at the translating posture: one explicit task
// whose main thread holds AUTH_MEMORY and AUTH_TASKS and a few user pages, the calls made as a
// named thread through the real dispatch. Every space a case built goes back at its end.

#ifndef KICKOS_TESTS_UNIT_SYSDISPATCH_SYS_FIXTURE_H
#define KICKOS_TESTS_UNIT_SYSDISPATCH_SYS_FIXTURE_H

#include <kickos/aspace.h>
#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/frame_pool.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/vrange.h>

#include <kickos/sys/abi.h>

#include <string.h>

#include "kseam_test.h"
#include "sys_seam.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

// The image the root space is seeded from, defined by each gate at SYS_IMAGE_BYTES.
extern "C" unsigned char g_sys_image[];

namespace kickos
{
    namespace testfix
    {
        inline constexpr size_t G = SYS_GRANULE;
        inline constexpr uint32_t CAPS = 16;
        inline constexpr uint8_t MAIN_PRIO = 10;
        inline constexpr uint32_t GRANT_BYTES = 64;

        // The caller's user pages, and where on a page each call's arguments sit.
        inline constexpr size_t USER_PAGES = 5;
        inline constexpr size_t MAIN_PARAMS = 0;
        inline constexpr size_t MAIN_WINDOWS = 1;
        inline constexpr size_t EDIT_PARAMS = 2;
        inline constexpr size_t MAIN_OUT = 3;
        inline constexpr size_t EDIT_OUT = 4;
        inline constexpr size_t WINDOWS_AT = 1024;
        inline constexpr size_t CAPS_AT = 2048;
        inline constexpr size_t DEST_AT = 2560;
        inline constexpr size_t VA_WORD_AT = 3072;
        inline constexpr unsigned char DIRTY = 0xEE;

        inline Thread*& running()
        {
            return kernel().current(kickos_kernel_core());
        }

        inline uint64_t call_as(Thread* t, uintptr_t nr, uintptr_t a0 = 0, uintptr_t a1 = 0,
                         uintptr_t a2 = 0, uintptr_t a3 = 0)
        {
            Thread* const was = running();
            running() = t;
            uint64_t const rc = syscall_dispatch(nr, a0, a1, a2, a3);
            running() = was;
            return rc;
        }

        inline int32_t err(uint64_t rc)
        {
            return static_cast<int32_t>(rc);
        }

        template <typename T>
        T* host(uintptr_t va)
        {
            return reinterpret_cast<T*>(user_bytes(va));
        }

        // Main's user pages, as the running case reserved them.
        inline uintptr_t g_user_base = 0;

        // A KOS_SYS_FRAME_MAP address word on `page` of main's pages, holding `va`.
        inline uintptr_t va_word(uintptr_t va, size_t page = MAIN_OUT)
        {
            uintptr_t const w = g_user_base + page * G + VA_WORD_AT;
            *host<uintptr_t>(w) = va;
            return w;
        }

        struct RunCaps
        {
            int obj = FRAME_RUN_NONE;
            arch_phys_addr_t base = 0;
            uint32_t fcap = KCAP_INVALID;
            uint32_t acap = KCAP_INVALID;
        };

        inline HostPoolMark g_sys_suite_mark;

        class SysFixture : public KSeam
        {
          protected:
            static void SetUpTestSuite()
            {
                memset(g_sys_image, 0x5A, SYS_IMAGE_BYTES);
                static VirtualRanges root;
                ASSERT_TRUE(aspace_image_seed(arch_aspace_create(), &root, false, nullptr));
                g_sys_suite_mark = host_pool_mark();
            }

            void SetUp() override
            {
                KSeam::SetUp();
                domain_init();
                task_ = task(0);
                seat_others();
                main_ = member(1, MAIN_PRIO, task_);
                sched::yield();
                ASSERT_EQ(running(), main_);
                user_ = reserve(main_, USER_PAGES * G, 0);
                ASSERT_NE(user_, 0u);
                g_user_base = user_;
                sys_seam_reset();
            }

            void TearDown() override
            {
                EXPECT_EQ(host_pool_double_frees(), 0u);
                EXPECT_EQ(host_pool_outside_frees(), 0u);
                sys_seam_reset();
                running() = main_;
                // The seam's spaces are few: every one a case built goes back here.
                for (Domain& d : kernel().domains)
                {
                    if (d.space != nullptr)
                    {
                        aspace_release(d.space, &d.ranges);
                        d.space = nullptr;
                    }
                }
                host_pool_free_since(g_sys_suite_mark);
            }

            // Threads a case seats before main, which is seated at slot 1.
            virtual void seat_others() {}

            // A thread of `tk`, with main's authority.
            Thread* member(int slot, uint8_t prio, Task* tk)
            {
                Thread* const t = seat_pool(slot, prio);
                attach_caps(t, CAPS);
                join_task(t, tk);
                cap_seat_authority(t, AUTH_MEMORY | AUTH_TASKS);
                return t;
            }

            uintptr_t uva(size_t page, size_t offset = 0) const
            {
                return user_ + page * G + offset;
            }

            static uintptr_t reserve(Thread* t, size_t bytes, uintptr_t flags)
            {
                uintptr_t const va = call_as(t, KOS_SYS_RAM_ALLOC, bytes);
                if (va == 0 or call_as(t, KOS_SYS_MEM_SELF_GRANT, va, bytes, flags) != 0)
                {
                    return 0;
                }
                return va;
            }

            uintptr_t reserve_only(size_t bytes)
            {
                return call_as(main_, KOS_SYS_RAM_ALLOC, bytes);
            }

            // A mint as `t`, whose out word is at `out`.
            static int32_t create_as(Thread* t, uintptr_t out, size_t bytes, uint32_t* cap)
            {
                *host<uint32_t>(out) = KCAP_INVALID;
                int32_t const rc = err(call_as(t, KOS_SYS_FRAME_CREATE, bytes, out));
                *cap = *host<uint32_t>(out);
                return rc;
            }

            int32_t create(size_t bytes, uint32_t* cap)
            {
                return create_as(main_, uva(MAIN_OUT), bytes, cap);
            }

            int32_t space_self(uint32_t* cap)
            {
                *host<uint32_t>(uva(MAIN_OUT)) = KCAP_INVALID;
                int32_t const rc = err(call_as(main_, KOS_SYS_ASPACE_SELF, uva(MAIN_OUT)));
                *cap = *host<uint32_t>(uva(MAIN_OUT));
                return rc;
            }

            static FrameRun* run_of(Thread* t, uint32_t cap, int* err)
            {
                IrqLock lock;
                return static_cast<FrameRun*>(cap_resolve_e(t, cap, CapType::CAP_FRAME, 0, err));
            }

            FrameRun* run_of(uint32_t cap)
            {
                int err = 0;
                return run_of(main_, cap, &err);
            }

            static int obj_of(Thread* t, uint32_t cap)
            {
                IrqLock lock;
                CapEntry const* const e = cap_lookup(t, cap);
                if (e == nullptr)
                {
                    return FRAME_RUN_NONE;
                }
                return e->obj;
            }

            // Holders, capabilities and mappings alike; 0 when the handle does not resolve.
            static uint8_t run_refs(int obj)
            {
                IrqLock lock;
                int const idx = kernel().frame_runs.live_index(obj);
                if (idx < 0)
                {
                    return 0;
                }
                return kernel().frame_run_refs[idx];
            }

            static bool frame_taken(arch_phys_addr_t pa)
            {
                return host_pool_mark().used[(pa - HOST_POOL_LO) / G];
            }

            static int close(Thread* t, uint32_t cap)
            {
                IrqLock lock;
                return handle_close(t, cap, lock);
            }

            // The same run in `t`'s table, as a delegation takes it.
            static uint32_t delegate(Thread* t, int obj)
            {
                IrqLock lock;
                uint32_t cap = KCAP_INVALID;
                if (not obj_ref_inc(CapType::CAP_FRAME, obj, CAP_TRANSFER)
                    or cap_install(t, obj, CapType::CAP_FRAME, CAP_TRANSFER, &cap) != 0)
                {
                    return KCAP_INVALID;
                }
                return cap;
            }

            // A capability naming `d`'s space in `holder`'s table.
            static uint32_t space_cap(Thread* holder, Domain* d)
            {
                IrqLock lock;
                uint32_t cap = KCAP_INVALID;
                if (cap_install(holder, domain_handle(d), CapType::CAP_ASPACE, CAP_TRANSFER, &cap)
                    != 0)
                {
                    return KCAP_INVALID;
                }
                return cap;
            }

            // A run of main's minting, with main's own space capability.
            RunCaps mint(uint32_t pages)
            {
                RunCaps r;
                if (create(pages * G, &r.fcap) != 0 or space_self(&r.acap) != 0)
                {
                    return r;
                }
                FrameRun const* const run = run_of(r.fcap);
                if (run != nullptr)
                {
                    r.obj = obj_of(main_, r.fcap);
                    r.base = run->base;
                }
                // The mint's clear opened windows.
                sys_seam_reset();
                return r;
            }

            RunCaps share(Thread* t, RunCaps const& run)
            {
                RunCaps r = run;
                r.fcap = delegate(t, run.obj);
                r.acap = space_cap(t, thread_domain(t));
                if (r.fcap == KCAP_INVALID or r.acap == KCAP_INVALID)
                {
                    r.obj = FRAME_RUN_NONE;
                }
                return r;
            }

            // `t`, made the running thread, leaves through the real exit path.
            void exit_as(Thread* t)
            {
                sched::yield();
                ASSERT_EQ(running(), t);
                run_exit(0);
                running() = main_;
            }

            static void dirty_free_frames()
            {
                HostPoolMark const m = host_pool_mark();
                for (size_t i = 0; i < HOST_POOL_FRAMES; i++)
                {
                    if (not m.used[i])
                    {
                        arch_phys_addr_t const pa =
                            HOST_POOL_LO + static_cast<arch_phys_addr_t>(i * G);
                        memset(host_pool_bytes(pa, 1), DIRTY, G);
                    }
                }
            }

            int32_t create_task(Thread* t, uintptr_t base, size_t bytes, uintptr_t flags,
                                kos_task_t* task_out)
            {
                uint32_t* const out = host<uint32_t>(uva(MAIN_OUT));
                *out = KOS_TASK_NONE;
                int32_t const rc =
                    err(call_as(t, KOS_SYS_TASK_CREATE, base, bytes, uva(MAIN_OUT), flags));
                *task_out = static_cast<kos_task_t>(*out);
                return rc;
            }

            kos_task_t new_task(Thread* t, uintptr_t base, size_t bytes, uintptr_t flags)
            {
                kos_task_t task_out = KOS_TASK_NONE;
                if (create_task(t, base, bytes, flags, &task_out) != 0)
                {
                    return KOS_TASK_NONE;
                }
                return task_out;
            }

            // A params block on `page`, with one memory window over [base, base + bytes)
            // when `bytes` is not 0, the child joining `into`.
            uintptr_t spawn_params(size_t page, uintptr_t base, size_t bytes, uint8_t flags,
                                   kos_task_t into)
            {
                kos_thread_params* const p = host<kos_thread_params>(uva(page));
                *p = {};
                p->entry = entry;
                p->prio = MAIN_PRIO;
                p->task = into;
                if (bytes != 0)
                {
                    *host<kos_window>(uva(page, WINDOWS_AT)) = {
                        base, static_cast<uint32_t>(bytes), KOS_WINDOW_MEMORY, flags};
                    p->windows = reinterpret_cast<kos_window const*>(uva(page, WINDOWS_AT));
                    p->window_count = 1;
                }
                return uva(page);
            }

            uintptr_t window_spawn(uintptr_t base, size_t bytes, kos_task_t into)
            {
                return spawn_params(MAIN_PARAMS, base, bytes, KOS_WINDOW_UNCACHED, into);
            }

            // A spawn of a child with a data grant, so the spawn builds a task.
            uintptr_t building_spawn(uintptr_t grant)
            {
                uintptr_t const va = spawn_params(MAIN_PARAMS, 0, 0, 0, KOS_TASK_NONE);
                params(va)->mem_base = reinterpret_cast<void*>(grant);
                params(va)->mem_size = GRANT_BYTES;
                return va;
            }

            static kos_thread_params* params(uintptr_t va)
            {
                return host<kos_thread_params>(va);
            }

            int32_t spawn(Thread* t, uintptr_t p, size_t out_page, kos_thread_t* child)
            {
                uint32_t* const out = host<uint32_t>(uva(out_page));
                *out = KOS_THREAD_NONE;
                int32_t const rc = err(call_as(t, KOS_SYS_THREAD_CREATE, p, uva(out_page)));
                *child = static_cast<kos_thread_t>(*out);
                return rc;
            }

            static void entry(void*) {}

            Task* task_ = nullptr;
            Thread* main_ = nullptr;
            uintptr_t user_ = 0;
        };
    }
}

#endif
