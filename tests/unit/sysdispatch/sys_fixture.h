// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A K-seam case over the real system-call layer at the translating posture: one task whose main
// thread holds AUTH_MEMORY and AUTH_TASKS and a few user pages, the calls made as a named thread
// through the real dispatch. A task of its own is an instance of the program `app` that main
// creates. Every space a case built goes back at its end.

#ifndef KICKOS_TESTS_UNIT_SYSDISPATCH_SYS_FIXTURE_H
#define KICKOS_TESTS_UNIT_SYSDISPATCH_SYS_FIXTURE_H

#include "../common/seam_pools.h"

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
#include <kickos/sys/image.h>

#include <stddef.h>
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
        inline constexpr size_t TASK_PARAMS_AT = 3200;
        inline constexpr size_t PROGRAM_AT = 3584;
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

        // The tasks a case may create: instances of `app`, started on request at main's priority,
        // holding the memory authority their creator holds.
        inline constexpr int PEERS = 6;
        inline constexpr size_t PEER_NAME_BYTES = 6;
        struct PeerDecls
        {
            kos_task_decl decl[PEERS];
            char names[PEERS * PEER_NAME_BYTES];
        };

        inline PeerDecls make_peer_decls()
        {
            PeerDecls d = {};
            for (int i = 0; i < PEERS; i++)
            {
                size_t const at = static_cast<size_t>(i) * PEER_NAME_BYTES;
                memcpy(&d.names[at], "peer0", PEER_NAME_BYTES);
                d.names[at + 4] = static_cast<char>('0' + i);
                d.decl[i] = {static_cast<uint32_t>(offsetof(PeerDecls, names) + at),
                             0u,
                             0u,
                             KOS_AUTH_MEMORY,
                             1024u,
                             0u,
                             KOS_DECL_ON_REQUEST,
                             MAIN_PRIO,
                             MAIN_PRIO,
                             0u,
                             0u,
                             KICKOS_SEAM_DECL_BUDGETS};
            }
            return d;
        }

        inline PeerDecls const& peer_decls()
        {
            static PeerDecls const d = make_peer_decls();
            return d;
        }

        // A memory window over [base, base + bytes) of its creator's reservation, for a created
        // task's entry thread.
        struct PeerWindow
        {
            uintptr_t base = 0;
            size_t bytes = 0;
            uint8_t flags = 0;
        };

        class SysFixture : public KSeam
        {
          protected:
            static void SetUpTestSuite()
            {
                memset(g_sys_image, 0x5A, SYS_IMAGE_BYTES);
                static VirtualRanges root;
                ASSERT_EQ(aspace_image_seed(nullptr, arch_aspace_create(), &root, 0), 0);
                g_sys_suite_mark = host_pool_mark();
            }

            void SetUp() override
            {
                KSeam::SetUp();
                domain_init();
                declare(peer_decls().decl, PEERS);
                program("app", reinterpret_cast<uintptr_t>(&entry));
                task_ = task(0);
                seat_others();
                main_ = member(1, MAIN_PRIO, task_);
                sched::yield();
                ASSERT_EQ(running(), main_);
                user_ = reserve(main_, USER_PAGES * G, 0);
                ASSERT_NE(user_, 0u);
                g_user_base = user_;
                memcpy(host<char>(uva(MAIN_OUT, PROGRAM_AT)), "app", 4);
                for (ProgramCap& c : progs_)
                {
                    c = {};
                }
                ASSERT_NE(program_for(main_), KCAP_INVALID);
                sys_seam_reset();
            }

            void TearDown() override
            {
                EXPECT_EQ(host_pool_double_frees(), 0u);
                EXPECT_EQ(host_pool_outside_frees(), 0u);
                sys_seam_reset();
                running() = main_;
                // The seam's spaces are few: every one a case built goes back here.
                for (Domain& d : kernel().domains())
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
                return kernel().frame_runs.refs()[idx];
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

            // `t`'s capability on `app`, which it finds on its first create.
            uint32_t program_for(Thread* t)
            {
                int const i = kernel().threads.index_of(t);
                kos_thread_t const h = kernel().threads.handle_for(i);
                if (progs_[i].thread != h)
                {
                    uint32_t* const out = host<uint32_t>(uva(MAIN_OUT, PROGRAM_AT + 8));
                    *out = KCAP_INVALID;
                    if (err(call_as(t, KOS_SYS_PROGRAM_FIND, uva(MAIN_OUT, PROGRAM_AT),
                                    uva(MAIN_OUT, PROGRAM_AT + 8)))
                        != 0)
                    {
                        return KCAP_INVALID;
                    }
                    progs_[i] = {h, *out};
                }
                return progs_[i].cap;
            }

            // The parameters of a create of `app` on `page`: `base`..`bytes` handed over where
            // `bytes` is not 0, and the entry thread holding `w`.
            kos_task_params* task_params(size_t page, uintptr_t base, size_t bytes,
                                         uintptr_t flags, PeerWindow w = {})
            {
                kos_task_params* const p = host<kos_task_params>(uva(page, TASK_PARAMS_AT));
                *p = {};
                p->block = reinterpret_cast<void*>(base);
                p->size = static_cast<uint32_t>(bytes);
                p->flags = static_cast<uint32_t>(flags);
                if (w.bytes != 0)
                {
                    *host<kos_window>(uva(page, WINDOWS_AT)) = {
                        w.base, static_cast<uint32_t>(w.bytes), KOS_WINDOW_MEMORY, w.flags};
                    p->windows = reinterpret_cast<kos_window const*>(uva(page, WINDOWS_AT));
                    p->window_count = 1;
                }
                return p;
            }

            // The create the parameters on `page` name, made by `t`, the answer on `out_page`.
            int32_t create_staged(Thread* t, size_t page, size_t out_page, kos_task_t* task_out)
            {
                uint32_t* const out = host<uint32_t>(uva(out_page));
                *out = KOS_TASK_NONE;
                int32_t const rc =
                    err(call_as(t, KOS_SYS_TASK_CREATE, program_for(t), uva(page, TASK_PARAMS_AT),
                                uva(out_page)));
                *task_out = static_cast<kos_task_t>(*out);
                return rc;
            }

            // A task `t` creates, an instance of `app` with `base`..`bytes` handed over where
            // `bytes` is not 0, its entry thread holding `w`. The parameters are on `page`, the
            // answer on `out_page`.
            int32_t create_task(Thread* t, uintptr_t base, size_t bytes, uintptr_t flags,
                                kos_task_t* task_out, PeerWindow w = {}, size_t page = MAIN_PARAMS,
                                size_t out_page = MAIN_OUT)
            {
                task_params(page, base, bytes, flags, w);
                return create_staged(t, page, out_page, task_out);
            }

            kos_task_t new_task(Thread* t, uintptr_t base, size_t bytes, uintptr_t flags,
                                PeerWindow w = {})
            {
                kos_task_t task_out = KOS_TASK_NONE;
                if (create_task(t, base, bytes, flags, &task_out, w) != 0)
                {
                    return KOS_TASK_NONE;
                }
                return task_out;
            }

            // The entry thread of the task `h` names, the one the create started.
            static Thread* entry_of(kos_task_t h)
            {
                IrqLock lock;
                Task* const tk = task_resolve(h);
                for (Thread& th : kernel().threads.all())
                {
                    if (tk != nullptr and th.task == tk and th.task_role == TASK_ROLE_ENTRY)
                    {
                        return &th;
                    }
                }
                return nullptr;
            }

            // A params block on `page`, with one memory window over [base, base + bytes)
            // when `bytes` is not 0.
            uintptr_t spawn_params(size_t page, uintptr_t base, size_t bytes, uint8_t flags)
            {
                kos_thread_params* const p = host<kos_thread_params>(uva(page));
                *p = {};
                p->entry = entry;
                p->prio = MAIN_PRIO;
                if (bytes != 0)
                {
                    *host<kos_window>(uva(page, WINDOWS_AT)) = {
                        base, static_cast<uint32_t>(bytes), KOS_WINDOW_MEMORY, flags};
                    p->windows = reinterpret_cast<kos_window const*>(uva(page, WINDOWS_AT));
                    p->window_count = 1;
                }
                return uva(page);
            }

            uintptr_t window_spawn(uintptr_t base, size_t bytes)
            {
                return spawn_params(MAIN_PARAMS, base, bytes, KOS_WINDOW_UNCACHED);
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
            struct ProgramCap
            {
                kos_thread_t thread = KOS_THREAD_NONE;
                uint32_t cap = KCAP_INVALID;
            };
            ProgramCap progs_[KICKOS_SEAM_THREAD_SLOTS] = {};
        };
    }
}

#endif
