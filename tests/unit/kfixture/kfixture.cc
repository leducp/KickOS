// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The K-seam fixture's state and helpers; the contract is in kfixture.h.

#include "../common/seam_pools.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <new> // Thread holds a kickos::Atomic, so a reset is a re-construction
#include <vector>

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/image.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/list.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/time.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kfixture.h"

#if KICKOS_HAVE_ASPACE
// One image, empty, for a gate whose link states none (tests/unit/common/host_image.cmake states
// one over these): what a declaration's create prices its data copy from.
extern "C"
{
    __attribute__((weak)) uintptr_t __kickos_image_frames[8] = {};
}
__asm__(".weak __kickos_image_frames_end\n"
        ".set __kickos_image_frames_end, __kickos_image_frames + 64\n"
        ".weak __kickos_image_frames_anchor\n"
        ".set __kickos_image_frames_anchor, 0\n");
#endif

namespace kickos
{
    namespace testfix
    {
        bool g_in_isr = false;
        uint64_t g_now_ns = 0;
        uint64_t g_seat_cost_ns = 0;
        uint32_t g_timer_writes = 0;
        uint32_t g_irq_saves = 0;

        Domain g_domains[FIXTURE_DOMAIN_SLOTS] = {};
        uint16_t g_domain_refs[FIXTURE_DOMAIN_SLOTS] = {};
        bool g_domain_live[FIXTURE_DOMAIN_SLOTS] = {};

        int domain_index(Domain const* d)
        {
            for (int i = 0; i < FIXTURE_DOMAIN_SLOTS; i++)
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
        char g_held[128] = {};
        uint32_t g_held_len = 0;
        uint32_t g_held_off = 0;
        uint32_t g_console_noted = 0;
        uint32_t g_console_reclaimed = 0;
        char const* g_fault_fmt[FAULT_FMT_MAX] = {};
        uint32_t g_fault_fmts = 0;
        bool g_console_dark = false;
        bool g_console_window_held = false;
        uint32_t g_parked = 0;
        uintptr_t g_periph_disabled[PERIPH_RECORD_MAX] = {};
        uint32_t g_periph_disables = 0;
        uint32_t g_periph_disables_held = 0;
        int g_periph_disable_rc = 0;
        bool g_line_armed[KICKOS_MAX_IRQ] = {};
        int g_routed_armed = -1;
        uint32_t g_line_clears[KICKOS_MAX_IRQ] = {};

#if KICKOS_KERNEL_CORES > 1
        uint32_t g_core = 0;
        uint32_t g_parks_committed = 0;
        uint32_t g_parks_without_lock = 0;
        uint32_t g_exit_window_opened = 0;
        Thread* g_park_from = nullptr;
        ThreadState g_park_from_state = ThreadState::INACTIVE;
        uint32_t g_ipi_sends = 0;
        uint32_t g_ipi_send_mask = 0;
        uint32_t g_ipi_self_raises = 0;
#endif

        Fixture g_fx;

        namespace
        {
            constexpr int32_t TRACE_CAP = 512;
            char g_trace[TRACE_CAP] = {};
            int32_t g_trace_len = 0;

            jmp_buf g_park_jmp;
            bool g_park_armed = false;

            ParkWaker g_park_waker = nullptr;
            SwitchAction g_switch_action = nullptr;

            uint32_t g_irq_depth = 0;
            bool g_gap_watch = false;
            bool g_in_gap_action = false;
            uint32_t g_gap_seen = 0;
            uint32_t g_gap_at = 0;
            GapAction g_gap_action = nullptr;

#if KICKOS_KERNEL_CORES > 1
            bool g_klock_held = false;
            SwapMode g_swap_mode = SwapMode::IMMEDIATE;
            // A deferred swap's ends: the first `from` an exception booked, which is the frame
            // the core stands on, and the last `to`.
            Thread* g_pending_from = nullptr;
            Thread* g_pending_to = nullptr;
#endif
        }

#if KICKOS_KERNEL_CORES > 1
        namespace
        {
            void commit_park(Thread* from)
            {
                g_parks_committed++;
                g_park_from = from;
                g_park_from_state = from->state;
                if (not g_klock_held)
                {
                    g_parks_without_lock++;
                }
                g_pending_from = nullptr;
                g_pending_to = nullptr;
                kickos_switch_unlock();
            }

            // A booking back onto the frame the core stands on parks nothing and still ends the
            // lock span.
            void commit_pending()
            {
                if (g_pending_from == nullptr)
                {
                    return;
                }
                if (g_pending_to == g_pending_from)
                {
                    g_pending_from = nullptr;
                    g_pending_to = nullptr;
                    kickos_switch_unlock();
                    return;
                }
                commit_park(g_pending_from);
            }
        }

        void set_swap_mode(SwapMode m)
        {
            g_swap_mode = m;
        }

        void exception_exit()
        {
            commit_pending();
        }

        bool klock_held()
        {
            return g_klock_held;
        }

        void note_klock_acquire()
        {
            if (g_klock_held)
            {
                printf("FIXTURE FAIL: the kernel lock was acquired while already held\n");
                exit(1);
            }
            g_klock_held = true;
        }

        void note_klock_release()
        {
            if (not g_klock_held)
            {
                printf("FIXTURE FAIL: the kernel lock was released while free\n");
                exit(1);
            }
            g_klock_held = false;
            Thread* const c = kernel().current(kickos_kernel_core());
            if (c != nullptr and c->state == ThreadState::EXITED and c != g_park_from)
            {
                g_exit_window_opened++;
            }
        }
#endif

        char const* trace()
        {
            return g_trace;
        }

        void trace_reset()
        {
            g_trace[0] = '\0';
            g_trace_len = 0;
        }

        // Advance by bytes written, not snprintf's potentially larger return value.
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
                // Credit the parked thread explicitly; the returning switch stub cannot resume it.
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
#if KICKOS_KERNEL_CORES > 1
            // Commit the outgoing park before resolving the incoming wake.
            if (g_swap_mode == SwapMode::IMMEDIATE)
            {
                commit_park(from);
            }
            else
            {
                if (g_pending_from == nullptr)
                {
                    g_pending_from = from;
                }
                g_pending_to = to;
            }
#endif
            // Only RUNNING threads become READY on switch-out; blocked threads stay blocked.
            if (from->state == ThreadState::BLOCKED)
            {
                resolve_park(from);
            }
            if (g_switch_action != nullptr)
            {
                SwitchAction const fn = g_switch_action;
                g_switch_action = nullptr;
                fn();
            }
        }

        void run_at_next_switch(SwitchAction fn)
        {
            g_switch_action = fn;
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
#if KICKOS_KERNEL_CORES > 1
            commit_pending();
#endif
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
            g_irq_saves++;
            g_irq_depth++;
        }

        uint32_t irq_depth()
        {
            return g_irq_depth;
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

        uint32_t gaps_seen()
        {
            return g_gap_seen;
        }

        namespace
        {
            kos_task_decl const* g_decls = nullptr;
            size_t g_decl_count = 0;
            char const* g_program = nullptr;
            uintptr_t g_decl_start = 0;
            kos_image_header g_program_header = {};
        }

        void declare(kos_task_decl const* decls, size_t count)
        {
            g_decls = decls;
            g_decl_count = count;
        }

        void program(char const* name, uintptr_t decl_start)
        {
            g_program = name;
            g_decl_start = decl_start;
        }

        void reset()
        {
            g_decls = nullptr;
            g_decl_count = 0;
            g_program = nullptr;
            g_decl_start = 0;
            // Disable gap injection before resetting traces or taking scheduler locks.
            g_gap_watch = false;
            g_in_gap_action = false;
            g_gap_action = nullptr;
            g_gap_seen = 0;
            g_gap_at = 0;
            g_park_waker = nullptr;
            g_switch_action = nullptr;
            if (g_irq_depth != 0)
            {
                printf("FIXTURE FAIL: an IrqLock leaked (g_irq_depth=%u)\n", g_irq_depth);
                exit(1);
            }
            g_irq_depth = 0;
#if KICKOS_KERNEL_CORES > 1
            // Clear pending switch state so later lock acquisition cannot be skipped.
            if (g_klock_held)
            {
                printf("FIXTURE FAIL: the kernel lock survived the arm\n");
                exit(1);
            }
            g_core = 0;
            g_swap_mode = SwapMode::IMMEDIATE;
            g_pending_from = nullptr;
            g_pending_to = nullptr;
            g_parks_committed = 0;
            g_parks_without_lock = 0;
            g_exit_window_opened = 0;
            g_park_from = nullptr;
            g_park_from_state = ThreadState::INACTIVE;
            g_ipi_sends = 0;
            g_ipi_send_mask = 0;
            g_ipi_self_raises = 0;
#endif
            if (cap_teardown_active())
            {
                printf("FIXTURE FAIL: a capability sweep is still in flight\n");
                exit(1);
            }
            new (&kernel()) Kernel{};
            pools_reset();
            new (&g_fx) Fixture{};
            g_in_isr = false;
            g_word_fault = false;
            g_now_ns = 0;
#ifdef KFIXTURE_REAL_TIME
            for (uint64_t& c : g_comparator)
            {
                c = 0;
            }
#endif
            g_seat_cost_ns = 0;
            g_timer_writes = 0;
            g_irq_saves = 0;
            for (int i = 0; i < FIXTURE_DOMAIN_SLOTS; i++)
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
            g_held_len = 0;
            g_held_off = 0;
            g_console_noted = 0;
            g_console_reclaimed = 0;
            g_fault_fmts = 0;
            g_console_dark = false;
            g_console_window_held = false;
            g_parked = 0;
            for (uintptr_t& b : g_periph_disabled)
            {
                b = 0;
            }
            g_periph_disables = 0;
            g_periph_disables_held = 0;
            g_periph_disable_rc = 0;
            for (int line = 0; line < KICKOS_MAX_IRQ; line++)
            {
                g_line_armed[line] = false;
                g_line_clears[line] = 0;
            }
            g_routed_armed = -1;
            trace_reset();

            // Reset capability state stored outside Kernel.
            cap_slab_init();
            cap_console_reset();
            // Install the default handler: irq_claim does not treat a null handler as free.
            irq_init();
            sched::init();
            g_fx.idle.base_prio = KICKOS_PRIO_IDLE;
            g_fx.idle.prio = KICKOS_PRIO_IDLE;
            {
                IrqLock lock;
                sched::add(&g_fx.idle, lock);
            }
            sched::start();
        }

#if KICKOS_KERNEL_CORES > 1
        bool unstage(Thread const* t)
        {
            Kernel& k = kernel();
            int const slot = k.threads.index_of(t);
            bool found = false;
            if (slot < 0)
            {
                return false;
            }
            for (uint32_t p = 0; p < KICKOS_KERNEL_CORES; p++)
            {
                for (uint32_t c = 0; c < KICKOS_KERNEL_CORES; c++)
                {
                    uint16_t* const ring = k.sched_ring(p, c);
                    uint32_t const mask = k.sched_ring_mask();
                    uint32_t const tail = k.sched_in[c].tail[p].load();
                    uint32_t const head = k.sched_out[p].head[c].load();
                    uint32_t const staged = k.sched_out[p].staged[c];
                    uint32_t kept = tail;
                    uint32_t kept_head = tail;
                    for (uint32_t i = tail; i != staged; i++)
                    {
                        uint16_t const e = ring[i & mask];
                        if (static_cast<int>(e) == slot)
                        {
                            found = true;
                            continue;
                        }
                        ring[kept & mask] = e;
                        kept++;
                        if (i - tail < head - tail)
                        {
                            kept_head = kept;
                        }
                    }
                    k.sched_out[p].staged[c] = kept;
                    k.sched_out[p].head[c].store(kept_head);
                }
            }
            return found;
        }
#endif

        // Keyed on the rings and not on `state`, which a caller may already have rewritten.
        void detach_ready(Thread* t)
        {
#if KICKOS_KERNEL_CORES > 1
            if (unstage(t))
            {
                return;
            }
#endif
            policy_on_remove(t);
        }

        // base_prio is the anchor a priority recompute falls back to.
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
            // Outside the pool, so a word naming it resolves to no thread: generation 1, where
            // seat_pool's slots stay at 0.
            th->lock_id = lock_id_pack(1u, static_cast<uint32_t>(slot));
#if KICKOS_KERNEL_CORES > 1
            // Supply the placement mask normally set by thread_create; zero is unschedulable.
            th->affinity = KICKOS_CORE_SET_ALL;
#endif
            {
                IrqLock lock;
                sched::add(th, lock);
            }
            return th;
        }

        // Fixture IDs start at 10 to distinguish them from spawned threads.
        // Use the real pool allocator when a test needs a resolvable thread handle.
        Thread* seat_pool(int slot, uint8_t prio)
        {
            if (slot < 0 or slot >= KICKOS_SEAM_THREAD_SLOTS)
            {
                printf("FIXTURE FAIL: pool slot %d out of range\n", slot);
                exit(1);
            }
            Kernel& k = kernel();
#if KICKOS_KERNEL_CORES > 1
            // The slot's own state survives its reuse, as it does across thread_create.
            uint8_t const reseat_owed = k.threads.slots()[slot].reseat_owed;
#endif
            Thread* w = new (&k.threads.slots()[slot]) Thread{};
#if KICKOS_KERNEL_CORES > 1
            w->reseat_owed = reseat_owed;
#endif
            w->base_prio = prio;
            w->prio = prio;
            w->id = static_cast<uint16_t>(10 + slot);
            w->lock_id = k.threads.lock_id_for(slot);
            // A slot's directory row, as thread_create seats it.
            w->caps = cap_directory(slot);
#if KICKOS_KERNEL_CORES > 1
            // Supply the placement mask normally set by thread_create; zero is unschedulable.
            w->affinity = KICKOS_CORE_SET_ALL;
#endif
            if (k.threads.next <= slot)
            {
                k.threads.next = slot + 1;
            }
            {
                IrqLock lock;
                sched::add(w, lock);
            }
            return w;
        }

        void park_join(Thread* w, Thread* target)
        {
            detach_ready(w);
            testfix::seat_blocked(w);
            w->wait_kind = WAIT_JOIN;
            w->wait_obj = target;
            // Poison wait_result so tests cannot pass without a waker writing it.
            w->wait_result = WAIT_RESULT_POISON;
        }

        // Use the real pool because served-endpoint links are pool indices.
        // Keep initialization consistent with endpoint_slot_claim.
        Endpoint* endpoint()
        {
            Endpoint* ep = nullptr;
            if (endpoint_slot_claim(nullptr, &ep) < 0)
            {
                printf("FIXTURE FAIL: endpoint pool exhausted\n");
                exit(1);
            }
            return ep;
        }

        void attach_caps(Thread* t, uint32_t width)
        {
            // A pool slot's directory is its row; a fixture TCB outside the pool gets one of its
            // own, as long as a row.
            int const slot = kernel().threads.index_of(t);
            if (slot >= 0)
            {
                t->caps = cap_directory(slot);
            }
            else
            {
                static std::map<Thread*, std::vector<CapEntry*>> outside;
                std::vector<CapEntry*>& dir = outside[t];
                dir.assign(cap_dir_length(), nullptr);
                t->caps = dir.data();
            }
            if (not cap_slab_attach(t->caps, width, &t->cap_free_head, &t->cap_width))
            {
                printf("FIXTURE FAIL: cap slab refused %u slots\n", width);
                exit(1);
            }
            // Charged to its task as a spawn charges it, whichever of the two the case seats
            // first.
            IrqLock lock;
            task_run_join(t->task, slot);
        }

        // Match wq_block: set BLOCKED before on_remove, then unlink the ready node
        // before reusing it in the wait queue.
        void park_plain_sender(Thread* w, Endpoint* ep)
        {
            testfix::seat_blocked(w);
            detach_ready(w);
            w->wait_queue = &ep->send_waiters;
            w->wait_kind = WAIT_EP_SEND;
            w->wait_obj = ep;
            w->call_state = CALL_NONE;
            w->wait_result = WAIT_RESULT_POISON;
            ep->send_waiters.push_back(&w->link);
        }

        namespace
        {
            // Linked by hand: the record pool is private to sync.cc.
            WordRecord* record_listed(uint32_t* word, WordKind kind)
            {
                Kernel& k = kernel();
                WordRecord* m = nullptr;
                for (WordRecord& r : k.word_records())
                {
                    if (r.kind == WordKind::FREE)
                    {
                        m = &r;
                        break;
                    }
                }
                if (m == nullptr)
                {
                    printf("FIXTURE FAIL: every word record is in use\n");
                    exit(1);
                }
                *m = WordRecord{};
                m->key = static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(word));
                m->kind = kind;
                m->next_active = k.word_active;
                if (k.word_active != nullptr)
                {
                    k.word_active->prev_active = m;
                }
                k.word_active = m;
                return m;
            }
        }

        // held_push is private to sync.cc.
        WordRecord* own_mutex(Thread* owner, uint32_t* word)
        {
            WordRecord* const m = record_listed(word, WordKind::PI);
            m->owner = owner;
            m->next_held = owner->held_list;
            owner->held_list = m;
            *word = owner->lock_id | KOS_WORD_WAITERS;
            return m;
        }

        WordRecord* plain_word(uint32_t* word)
        {
            *word = *word | KOS_WORD_WAITERS;
            return record_listed(word, WordKind::PLAIN);
        }

        namespace
        {
            bool record_free(Task const& r)
            {
                return r.refcount == 0 and r.creator_tag == ThreadPool::KILL_TAG_NONE
                       and r.sweeping == 0;
            }
        }

        Task* task_held(uint16_t tag, int* err, Held held)
        {
            *err = 0;
            for (int decl = 0; decl < KICKOS_SEAM_TASKS; decl++)
            {
                if (record_free(kernel().tasks()[KICKOS_SEAM_TASKS - 1 - decl]))
                {
                    return task_create_declared(static_cast<size_t>(decl), 0, tag, nullptr, 0,
                                                0u, nullptr, err, held);
                }
            }
            *err = KOS_ENOMEM;
            return nullptr;
        }

        Task* task(int slot)
        {
            if (slot < 0 or slot >= KICKOS_SEAM_TASKS)
            {
                printf("FIXTURE FAIL: task slot %d out of range\n", slot);
                exit(1);
            }
            if (not record_free(kernel().tasks()[slot]))
            {
                printf("FIXTURE FAIL: task slot %d is taken\n", slot);
                exit(1);
            }
            int err = 0;
            IrqLock lock;
            Task* const tk = task_create_declared(static_cast<size_t>(KICKOS_SEAM_TASKS - 1 - slot),
                                                  0, FIXTURE_TASK_TAG, nullptr, 0, 0u, nullptr,
                                                  &err, lock);
            if (tk == nullptr)
            {
                printf("FIXTURE FAIL: the create refused (%d)\n", err);
                exit(1);
            }
            return tk;
        }

        void join_task(Thread* t, Task* tk)
        {
            IrqLock lock;
            int const slot = kernel().threads.index_of(t);
            task_run_leave(t->task, slot);
            task_member_leave(t, slot);
            task_member_join(tk, t, slot);
            task_ref(tk);
            if (t->cap_width != 0)
            {
                task_run_join(tk, slot);
            }
        }

        void park_sleeper(Thread* w, uint64_t deadline_ns)
        {
            testfix::seat_blocked(w);
            detach_ready(w);
            w->wait_queue = nullptr;
            w->wait_kind = WAIT_SLEEP;
            w->wait_obj = nullptr;
            w->wait_result = WAIT_RESULT_POISON;
            // Insert into the timer delta list so cancellation has an entry to remove.
            w->deadline_ns = deadline_ns;
            w->tnext = kernel().sleepq(queue_core_of(w));
            kernel().sleepq(queue_core_of(w)) = w;
            w->on_timer = true;
        }

        void park_mutex_waiter(Thread* w, WordRecord* m)
        {
            testfix::seat_blocked(w);
            detach_ready(w);
            w->wait_queue = &m->waiters;
            w->wait_kind = WAIT_MUTEX;
            w->wait_obj = m;
            w->wait_va = static_cast<uintptr_t>(m->key);
            w->wait_result = WAIT_RESULT_POISON;
            m->waiters.push_back(&w->link);
        }

        void park_word_waiter(Thread* w, WordRecord* m)
        {
            testfix::seat_blocked(w);
            detach_ready(w);
            w->wait_queue = &m->waiters;
            w->wait_kind = WAIT_WORD;
            w->wait_obj = m;
            w->wait_va = static_cast<uintptr_t>(m->key);
            w->wait_result = WAIT_RESULT_POISON;
            m->waiters.push_back(&w->link);
        }

        void park_death_waiter(Thread* w, Task* t)
        {
            testfix::seat_blocked(w);
            detach_ready(w);
            w->wait_queue = nullptr;
            w->wait_kind = WAIT_TASK_EMPTY;
            w->wait_obj = t;
            w->wait_result = WAIT_RESULT_POISON;
            kernel().death_waiters.push(&w->link);
        }

        void run_exit(int code)
        {
            run_exit_as(code, sched::EXIT_RETURN);
        }

        void run_exit_faulted(int code)
        {
            run_exit_as(code, sched::EXIT_FAULTED);
        }

        void run_exit_as(int code, sched::ExitCause cause)
        {
            if (setjmp(g_park_jmp) == 0)
            {
                g_park_armed = true;
                sched::exit_current(code, cause);
            }
            // Clear on both paths to prevent longjmp into an expired frame.
            g_park_armed = false;
        }

        void run_exit_in_gap(Thread* t, int code)
        {
            if (not g_in_gap_action)
            {
                printf("FIXTURE FAIL: run_exit_in_gap outside a chunk-gap action\n");
                exit(1);
            }
            jmp_buf outer;
            memcpy(&outer, &g_park_jmp, sizeof(jmp_buf));
            bool const outer_armed = g_park_armed;
            Thread* const was = kernel().current(kickos_kernel_core());
            kernel().current(kickos_kernel_core()) = t;
            run_exit(code);
            kernel().current(kickos_kernel_core()) = was;
            memcpy(&g_park_jmp, &outer, sizeof(jmp_buf));
            g_park_armed = outer_armed;
        }

        void run_noreturn(void (*fn)())
        {
            if (setjmp(g_park_jmp) == 0)
            {
                g_park_armed = true;
                fn();
                printf("FIXTURE FAIL: run_noreturn's body returned\n");
                exit(1);
            }
            g_park_armed = false;
        }

        void fold_stdout_into_stderr()
        {
            fflush(stdout);
            (void) dup2(STDERR_FILENO, STDOUT_FILENO);
        }
    }

    kos_task_decl const* image_decl(size_t i)
    {
        if (i >= testfix::g_decl_count)
        {
            return nullptr;
        }
        return &testfix::g_decls[i];
    }

    size_t image_decl_count(void)
    {
        return testfix::g_decl_count;
    }

    size_t image_count(void)
    {
        return 1;
    }

    size_t image_of_decl(size_t i)
    {
        if (i >= testfix::g_decl_count)
        {
            return 1;
        }
        return 0;
    }

    bool image_same_program(size_t a, size_t b)
    {
        return a == b;
    }

    void image_thread_return_select(size_t)
    {
    }

    void image_decls_of(int image, size_t* first, size_t* count)
    {
        *first = 0;
        *count = 0;
        if (image == 0)
        {
            *count = testfix::g_decl_count;
        }
    }

    uintptr_t image_decl_start(size_t)
    {
        return testfix::g_decl_start;
    }

    char const* image_decl_name(kos_task_decl const* d)
    {
        return reinterpret_cast<char const*>(testfix::g_decls) + d->name;
    }

    kos_decl_grant const* image_decl_grants(kos_task_decl const* d)
    {
        return reinterpret_cast<kos_decl_grant const*>(reinterpret_cast<char const*>(testfix::g_decls)
                                                       + d->grants);
    }

    char const* image_program_name(int)
    {
        return testfix::g_program;
    }

    kos_image_header const* image_program(int i)
    {
        if (i != 0 or testfix::g_program == nullptr)
        {
            return nullptr;
        }
        return &testfix::g_program_header;
    }
}
