// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K-seam fixture's state and helpers. Contract, and the four traps an arm author must
// know, are in kfixture.h.

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/list.h>
#include <kickos/sched.h>
#include <kickos/time.h>

#include "kfixture.h"

namespace kickos
{
    namespace testfix
    {
        bool g_resume_on_switch = false;
        bool g_in_isr = false;
        uint64_t g_now_ns = 0;

        unsigned g_switches = 0;
        unsigned g_console_noted = 0;
        unsigned g_console_reclaimed = 0;
        unsigned g_parked = 0;
        int g_failures = 0;

        Fixture g_fx;

        namespace
        {
            constexpr int TRACE_CAP = 512;
            char g_trace[TRACE_CAP] = {};
            int g_trace_len = 0;

            jmp_buf g_park_jmp;
            bool g_park_armed = false;
        }

        char const* trace()
        {
            return g_trace;
        }

        void trace_reset()
        {
            g_trace[0] = '\0';
            g_trace_len = 0;
        }

        // Advances the cursor by what was WRITTEN, never by snprintf's return: that return is
        // the length it WOULD have written, so on the first truncation the cursor walks past
        // the buffer and the next call gets a past-the-end destination with a huge size.
        // Memory corruption, not truncation, and dormant until one arm gets long.
        void trace_add(char const* fmt, ...)
        {
            if (TRACE_CAP - g_trace_len <= 1)
            {
                return;
            }
            if (g_trace_len > 0)
            {
                g_trace[g_trace_len] = ' ';
                g_trace_len++;
            }
            int const room = TRACE_CAP - g_trace_len;
            va_list ap;
            va_start(ap, fmt);
            int const n = vsnprintf(&g_trace[g_trace_len], static_cast<size_t>(room), fmt, ap);
            va_end(ap);
            if (n < 0)
            {
                g_trace[g_trace_len] = '\0';
                return;
            }
            int written = n;
            if (written > room - 1)
            {
                written = room - 1;
            }
            g_trace_len += written;
        }

        void check(bool ok, char const* what)
        {
            if (ok)
            {
                return;
            }
            printf("not ok - %s\n", what);
            fflush(stdout);
            g_failures++;
        }

        void check_eq(unsigned got, unsigned want, char const* what)
        {
            if (got == want)
            {
                return;
            }
            printf("not ok - %s (got %u, want %u)\n", what, got, want);
            fflush(stdout);
            g_failures++;
        }

        void check_trace(char const* want, char const* what)
        {
            if (strcmp(trace(), want) == 0)
            {
                return;
            }
            printf("not ok - %s\n#   want: %s\n#    got: %s\n", what, want, trace());
            fflush(stdout);
            g_failures++;
        }

        // ctx is the TCB's first member, but go through the offset rather than assume that.
        Thread* thread_of_context(struct arch_context* c)
        {
            return KICKOS_CONTAINER_OF(c, Thread, ctx);
        }

        void note_switch(Thread* from, Thread* to)
        {
            g_switches++;
            trace_add("switch%u>%u", from->id, to->id);
            if (g_resume_on_switch)
            {
                from->switch_count++;
            }
        }

        void note_park()
        {
            g_parked++;
            if (g_park_armed)
            {
                g_park_armed = false;
                longjmp(g_park_jmp, 1);
            }
            printf("not ok - fixture: parked with no arm waiting for it\n");
            exit(1);
        }

        void reset()
        {
            if (cap_teardown_active())
            {
                printf("not ok - fixture: a capability sweep is still in flight\n");
                exit(1);
            }
            kernel() = Kernel{};
            g_fx = Fixture{};
            g_resume_on_switch = false;
            g_in_isr = false;
            g_now_ns = 0;
            g_switches = 0;
            g_console_noted = 0;
            g_console_reclaimed = 0;
            g_parked = 0;
            trace_reset();

            // The chunk free list lives in cap.cc's own CapState, which the Kernel
            // assignment above does not reach.
            cap_slab_init();
            sched::init();
            g_fx.idle.base_prio = KICKOS_PRIO_IDLE;
            g_fx.idle.prio = KICKOS_PRIO_IDLE;
            sched::add(&g_fx.idle);
            sched::start();
        }

        // base_prio is the anchor a priority recompute falls back to, so it is set here and
        // never again.
        Thread* spawn(int slot, uint8_t prio)
        {
            if (slot < 0 or slot >= MAX_TEST_THREADS)
            {
                printf("not ok - fixture: spawn slot %d out of range\n", slot);
                exit(1);
            }
            Thread* th = &g_fx.t[slot];
            *th = Thread{};
            th->base_prio = prio;
            th->prio = prio;
            th->id = static_cast<uint16_t>(slot + 1);
            sched::add(th);
            return th;
        }

        // Ids start at 10 so no trace token reads ambiguously against a spawn() thread. Seats
        // the TCB and `next` only, and NOT the pool's gen[] or claim state: the exit sweep
        // finds a waiter by SCANNING, so that is all it needs. An arm that wants a thread
        // resolvable by HANDLE (cap_reply_caller reads gen) has to go through the real alloc.
        Thread* seat_pool(int slot, uint8_t prio)
        {
            if (slot < 0 or slot >= KICKOS_THREAD_SLOTS)
            {
                printf("not ok - fixture: pool slot %d out of range\n", slot);
                exit(1);
            }
            Kernel& k = kernel();
            Thread* w = &k.threads.slots[slot];
            *w = Thread{};
            w->base_prio = prio;
            w->prio = prio;
            w->id = static_cast<uint16_t>(10 + slot);
            if (k.threads.next <= slot)
            {
                k.threads.next = slot + 1;
            }
            sched::add(w);
            return w;
        }

        void park_join(Thread* w, Thread* target)
        {
            kernel().policy->on_remove(w);
            w->state = ThreadState::BLOCKED;
            w->wait_kind = WAIT_JOIN;
            w->wait_obj = target;
            // POISONED, because a fresh TCB already reads 0 and an arm asserting 0 would pass
            // on a waker that never wrote it. A real parked waiter carries whatever its last
            // wake left; writing the result belongs to the waker.
            w->wait_result = WAIT_RESULT_POISON;
        }

        // From the REAL pool, because the served-endpoint chain is a pool INDEX biased by one
        // (endpoint.h) and thread_effective_prio resolves it through kernel().endpoints. A
        // stack-local Endpoint cannot be named by that chain at all.
        Endpoint* endpoint()
        {
            int const i = kernel().endpoints.alloc();
            if (i < 0)
            {
                printf("not ok - fixture: endpoint pool exhausted\n");
                exit(1);
            }
            Endpoint* ep = kernel().endpoints.at(i);
            *ep = Endpoint{};
            return ep;
        }

        void attach_caps(Thread* t, uint32_t width)
        {
            if (not cap_slab_attach(&t->caps, width, &t->cap_free_head, &t->cap_width))
            {
                printf("not ok - fixture: cap slab refused %u slots\n", width);
                exit(1);
            }
        }

        // Mirrors wq_block's ORDER, which is load-bearing: BLOCKED before the detach because
        // on_remove reads `state` to tell a park from a set_prio re-seat, and the ready-list
        // removal reads `link` before the queue push re-uses that same node.
        void park_plain_sender(Thread* w, Endpoint* ep)
        {
            w->state = ThreadState::BLOCKED;
            kernel().policy->on_remove(w);
            w->wait_queue = &ep->send_waiters;
            w->wait_kind = WAIT_EP_SEND;
            w->wait_obj = ep;
            w->call_state = CALL_NONE;
            ep->send_waiters.push_back(&w->link);
        }

        // The held_list link is restated here because held_push is TU-local to sync.cc. The
        // sweep's own held_remove is what unlinks it, so getting this wrong fails the sweep's
        // totality asserts rather than passing quietly.
        Mutex* own_mutex(Thread* owner, int* out_handle)
        {
            int const i = kernel().mutexes.alloc();
            if (i < 0)
            {
                printf("not ok - fixture: mutex pool exhausted\n");
                exit(1);
            }
            Mutex* m = kernel().mutexes.at(i);
            *m = Mutex{};
            m->owner = owner;
            m->next_held = owner->held_list;
            owner->held_list = m;
            kernel().mutex_refs[i] = 2;
            *out_handle = kernel().mutexes.handle_for(i);
            return m;
        }

        void park_mutex_waiter(Thread* w, Mutex* m)
        {
            w->state = ThreadState::BLOCKED;
            kernel().policy->on_remove(w);
            w->wait_queue = &m->waiters;
            w->wait_kind = WAIT_MUTEX;
            w->wait_obj = m;
            m->waiters.push_back(&w->link);
        }

        void run_exit(int code)
        {
            if (setjmp(g_park_jmp) == 0)
            {
                g_park_armed = true;
                sched::exit_current(code);
            }
            // Cleared on BOTH paths. exit_current cannot return without parking, but a stale
            // arm would let a later stray arch_idle_wait longjmp into this dead frame.
            g_park_armed = false;
        }
    }
}
