// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K-seam fixture's state and helpers. Contract, and the four traps an arm author must
// know, are in kfixture.h.

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <new> // Thread holds a kickos::Atomic: a reset is a re-construction, not an assignment

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/kernel.h>
#include <kickos/list.h>
#include <kickos/sched.h>
#include <kickos/time.h>

#include "kfixture.h"

namespace kickos
{
    namespace testfix
    {
        bool g_in_isr = false;
        uint64_t g_now_ns = 0;

        Domain g_domains[KICKOS_MAX_TASKS] = {};
        uint16_t g_domain_refs[KICKOS_MAX_TASKS] = {};
        bool g_domain_live[KICKOS_MAX_TASKS] = {};

        int domain_index(Domain const* d)
        {
            for (int i = 0; i < KICKOS_MAX_TASKS; i++)
            {
                if (&g_domains[i] == d)
                {
                    return i;
                }
            }
            return -1;
        }

        uint16_t domain_refs(Domain const* d)
        {
            int const i = domain_index(d);
            if (i < 0)
            {
                return 0;
            }
            return g_domain_refs[i];
        }

        uint32_t g_switches = 0;
        Thread* g_redirect_target = nullptr;
        void (*g_redirect_entry)(void* arg) = nullptr;
        uintptr_t g_redirect_stack_top = 0;
        uint32_t g_redirects = 0;
        uint32_t g_console_noted = 0;
        uint32_t g_console_reclaimed = 0;
        uint32_t g_parked = 0;

        Fixture g_fx;

        namespace
        {
            constexpr int32_t TRACE_CAP = 512;
            char g_trace[TRACE_CAP] = {};
            int32_t g_trace_len = 0;

            jmp_buf g_park_jmp;
            bool g_park_armed = false;

            ParkWaker g_park_waker = nullptr;

            uint32_t g_irq_depth = 0;
            bool g_gap_watch = false;
            bool g_in_gap_action = false;
            uint32_t g_gap_seen = 0;
            uint32_t g_gap_at = 0;
            GapAction g_gap_action = nullptr;
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
        // the length it WOULD have written, so the first truncation walks the cursor past the
        // buffer and the next call writes past the end.
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

        Thread* thread_of_context(struct arch_context* c)
        {
            return KICKOS_CONTAINER_OF(c, Thread, ctx);
        }

        namespace
        {
            void resolve_park(Thread* w)
            {
                w->wait_result = WAIT_RESULT_POISON;
                // switch_to credits the INCOMING thread, which under a returning stub is
                // never the one that parked, so wq_confirm_resume would spin to
                // KICKOS_POLL_SPIN_MAX and panic.
                w->switch_count.store(w->switch_count.load() + 1u);
                if (g_park_waker == nullptr)
                {
                    printf("FIXTURE FAIL: no waker armed for the park of thread %u\n", w->id);
                    exit(1);
                }
                ParkWaker const fn = g_park_waker;
                g_park_waker = nullptr;
                fn(w);
            }
        }

        void wake_next_park(ParkWaker fn)
        {
            g_park_waker = fn;
        }

        void note_switch(Thread* from, Thread* to)
        {
            g_switches++;
            trace_add("switch%u>%u", from->id, to->id);
            // BLOCKED names a thread that parked itself: switch_to demotes a RUNNING
            // outgoing thread to READY and leaves every other state alone.
            if (from->state == ThreadState::BLOCKED)
            {
                resolve_park(from);
            }
        }

        void note_ctx_redirect(Thread* t, void (*entry)(void* arg), void* base, size_t size)
        {
            g_redirects++;
            g_redirect_target = t;
            g_redirect_entry = entry;
            g_redirect_stack_top = reinterpret_cast<uintptr_t>(base) + size;
            trace_add("redirect%u", t->id);
        }

        void note_park()
        {
            g_parked++;
            if (g_park_armed)
            {
                g_park_armed = false;
                longjmp(g_park_jmp, 1);
            }
            printf("FIXTURE FAIL: parked with no arm waiting for it\n");
            exit(1);
        }

        void note_irq_save()
        {
            g_irq_depth++;
        }

        void note_irq_restore()
        {
            if (g_irq_depth > 0)
            {
                g_irq_depth--;
            }
            if (g_irq_depth != 0 or not g_gap_watch or g_in_gap_action)
            {
                return;
            }
            g_gap_seen++;
            trace_add("gap%u", g_gap_seen);
            if (g_gap_action == nullptr or g_gap_seen != g_gap_at)
            {
                return;
            }
            GapAction const fn = g_gap_action;
            g_gap_action = nullptr;
            g_in_gap_action = true;
            fn();
            g_in_gap_action = false;
        }

        void run_in_chunk_gap(GapAction fn, uint32_t ordinal)
        {
            g_gap_watch = true;
            g_gap_seen = 0;
            g_gap_at = ordinal;
            g_gap_action = fn;
        }

        void reset()
        {
            // Before trace_reset below, and before sched::init's own locks: an armed watch
            // would otherwise write this arm's first gap into the next arm's trace.
            g_gap_watch = false;
            g_in_gap_action = false;
            g_gap_action = nullptr;
            g_gap_seen = 0;
            g_gap_at = 0;
            g_park_waker = nullptr;
            if (g_irq_depth != 0)
            {
                printf("FIXTURE FAIL: an IrqLock leaked (g_irq_depth=%u)\n", g_irq_depth);
                exit(1);
            }
            g_irq_depth = 0;
            if (cap_teardown_active())
            {
                printf("FIXTURE FAIL: a capability sweep is still in flight\n");
                exit(1);
            }
            new (&kernel()) Kernel{};
            new (&g_fx) Fixture{};
            g_in_isr = false;
            g_now_ns = 0;
            for (int i = 0; i < KICKOS_MAX_TASKS; i++)
            {
                g_domains[i] = Domain{};
                g_domain_refs[i] = 0;
                g_domain_live[i] = false;
            }
            g_switches = 0;
            g_redirect_target = nullptr;
            g_redirect_entry = nullptr;
            g_redirect_stack_top = 0;
            g_redirects = 0;
            g_console_noted = 0;
            g_console_reclaimed = 0;
            g_parked = 0;
            trace_reset();

            // The chunk free list and the published stdout target live in cap.cc's own
            // constinit state, which the Kernel assignment above does not reach.
            cap_slab_init();
            cap_console_reset();
            // Every dispatch slot back to the null-object default. The Kernel assignment
            // above zeroed the table, and a NULL handler is not what irq_claim reads as a
            // free line: without this every claim answers -KOS_EBUSY.
            irq_init();
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
                printf("FIXTURE FAIL: spawn slot %d out of range\n", slot);
                exit(1);
            }
            Thread* th = new (&g_fx.t[slot]) Thread{};
            th->base_prio = prio;
            th->prio = prio;
            th->id = static_cast<uint16_t>(slot + 1);
            sched::add(th);
            return th;
        }

        // Ids start at 10 so no trace token reads ambiguously against a spawn() thread. Seats
        // the TCB and `next` only, NOT the pool's gen[] or claim state: an arm that wants a
        // thread resolvable by HANDLE (cap_reply_caller reads gen) needs the real alloc.
        Thread* seat_pool(int slot, uint8_t prio)
        {
            if (slot < 0 or slot >= KICKOS_THREAD_SLOTS)
            {
                printf("FIXTURE FAIL: pool slot %d out of range\n", slot);
                exit(1);
            }
            Kernel& k = kernel();
            Thread* w = new (&k.threads.slots[slot]) Thread{};
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
            // POISONED: a fresh TCB already reads 0, so an arm asserting 0 would pass on a
            // waker that never wrote it.
            w->wait_result = WAIT_RESULT_POISON;
        }

        // From the REAL pool: the served-endpoint chain is a pool INDEX biased by one
        // (endpoint.h), which thread_effective_prio resolves through kernel().endpoints, so a
        // stack-local Endpoint cannot be named by that chain at all.
        Endpoint* endpoint()
        {
            int const i = kernel().endpoints.alloc();
            if (i < 0)
            {
                printf("FIXTURE FAIL: endpoint pool exhausted\n");
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
                printf("FIXTURE FAIL: cap slab refused %u slots\n", width);
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
            w->wait_result = WAIT_RESULT_POISON;
            ep->send_waiters.push_back(&w->link);
        }

        // The held_list link is restated here: held_push is TU-local to sync.cc. The sweep's
        // own held_remove is what unlinks it.
        Mutex* own_mutex(Thread* owner, int* out_handle)
        {
            int const i = kernel().mutexes.alloc();
            if (i < 0)
            {
                printf("FIXTURE FAIL: mutex pool exhausted\n");
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

        Task* task(int slot)
        {
            if (slot < 0 or slot >= KICKOS_MAX_TASKS)
            {
                printf("FIXTURE FAIL: task slot %d out of range\n", slot);
                exit(1);
            }
            int err = 0;
            Task* const tk =
                task_create(FIXTURE_TASK_TAG, nullptr, 0, /*caller_authorized=*/false, &err);
            if (tk == nullptr)
            {
                printf("FIXTURE FAIL: task_create refused (%d)\n", err);
                exit(1);
            }
            if (tk != &kernel().tasks[slot])
            {
                printf("FIXTURE FAIL: task_create landed off slot %d\n", slot);
                exit(1);
            }
            return tk;
        }

        void join_task(Thread* t, Task* tk)
        {
            t->task = tk;
            task_ref(tk);
        }

        Semaphore* semaphore(int* out_handle)
        {
            int const i = kernel().sems.alloc();
            if (i < 0)
            {
                printf("FIXTURE FAIL: semaphore pool exhausted\n");
                exit(1);
            }
            Semaphore* s = kernel().sems.at(i);
            sem_init(s, 0);
            kernel().sem_refs[i] = 1;
            if (out_handle != nullptr)
            {
                *out_handle = kernel().sems.handle_for(i);
            }
            return s;
        }

        void park_sem_waiter(Thread* w, Semaphore* s)
        {
            w->state = ThreadState::BLOCKED;
            kernel().policy->on_remove(w);
            w->wait_queue = &s->waiters;
            w->wait_kind = WAIT_SEM;
            w->wait_obj = s;
            w->wait_result = WAIT_RESULT_POISON;
            s->waiters.push_back(&w->link);
        }

        void park_sleeper(Thread* w, uint64_t deadline_ns)
        {
            w->state = ThreadState::BLOCKED;
            kernel().policy->on_remove(w);
            w->wait_queue = nullptr;
            w->wait_kind = WAIT_SLEEP;
            w->wait_obj = nullptr;
            w->wait_result = WAIT_RESULT_POISON;
            // The delta list itself, so ktime_deadline_cancel has something to unlink. A head
            // push, not a sorted insert: one sleeper per arm.
            w->deadline_ns = deadline_ns;
            w->tnext = kernel().sleepq;
            kernel().sleepq = w;
            w->on_timer = true;
        }

        void park_mutex_waiter(Thread* w, Mutex* m)
        {
            w->state = ThreadState::BLOCKED;
            kernel().policy->on_remove(w);
            w->wait_queue = &m->waiters;
            w->wait_kind = WAIT_MUTEX;
            w->wait_obj = m;
            w->wait_result = WAIT_RESULT_POISON;
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

        void fold_stdout_into_stderr()
        {
            fflush(stdout);
            (void) dup2(STDERR_FILENO, STDOUT_FILENO);
        }
    }
}
