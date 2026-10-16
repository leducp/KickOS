// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The park unwind MUST stay total over WaitKind: a cancel that reached only the kinds a
// thread happens to park on is not a kill.

#include <kickos/console_tx.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/klock.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/time.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    void thread_abort_park(Thread* t, intptr_t result, Held held)
    {
        switch (t->wait_kind)
        {
            case WAIT_EP_SEND:
            case WAIT_EP_RECV:
            {
                // Only a CALL_SEND_WAIT thread donates: a caller no receiver has taken yet,
                // which D2-boosted the endpoint's conventional server.
                bool const donor =
                    (t->wait_kind == WAIT_EP_SEND and t->call_state == CALL_SEND_WAIT);
                Endpoint* const e = t->wait_endpoint();
                KICKOS_ASSERT(e != nullptr); // wq_block never parks here without one
                t->wait_queue->unlink(&t->link);
                t->call_state = CALL_NONE;
                if (donor and e->server != nullptr)
                {
                    // Both the unlink and the CALL_NONE above MUST precede this: the funnel
                    // counts a SEND_WAIT donor still linked on send_waiters, so recomputing
                    // first would re-derive the very boost we are reverting.
                    sched::set_prio(e->server, thread_effective_prio(e->server), held);
                }
                break;
            }
            case WAIT_EP_REPLY:
            {
                // Queue-less: the server is the only edge back, and reply_donor_unpark
                // clears it as it unlinks.
                Thread* const server = t->wait_server();
                KICKOS_ASSERT(server != nullptr); // reply_donor_park never seats a null
                bool const unlinked = reply_donor_unpark(server, t);
                // The edge names the list this thread is on, so a miss is a kernel bug.
                KICKOS_ASSERT(unlinked);
                t->call_state = CALL_NONE;
                // The abandoned reply cap must die with the call it named: cap_reply_caller
                // matches only the low 8 bits of call_seq, so a cap left on this call's seq
                // resolves to this thread again after exactly 256 further calls.
                t->call_seq++;
                // D3, after the unlink and the CALL_NONE for the same reason as above.
                sched::set_prio(server, thread_effective_prio(server), held);
                // The server keeps the reply capability: reclaiming it would reach into
                // another thread's table. Its eventual reply consumes it and answers
                // -KOS_ESRCH.
                break;
            }
            case WAIT_EP_FAR_REPLY:
            {
                // Queue-less on no list at all, and no donation to revert: priority does not
                // cross to a peer's scheduler (docs/design-multicore.md N6e).
                t->call_state = CALL_NONE;
                // For the same reason the local arm above bumps it: a reply still in flight
                // names this call by call_seq, so a seq left standing resolves to this thread
                // again after the sequence wraps. The far arm validates all 16 bits, so that is
                // 65536 calls here where the local arm's masked 8 make it 256.
                t->call_seq++;
                break;
            }
            case WAIT_MUTEX:
            {
                // Unlinked first, so the funnel no longer counts this waiter.
                Mutex* const m = t->wait_mutex();
                KICKOS_ASSERT(m != nullptr); // WAIT_MUTEX never parks without one
                t->wait_queue->unlink(&t->link);
                Thread* owner = m->owner;
                for (int depth = 0; owner != nullptr and depth <= KICKOS_THREAD_SLOTS; depth++)
                {
                    uint8_t const p = thread_effective_prio(owner);
                    if (p == owner->prio)
                    {
                        break;
                    }
                    sched::set_prio(owner, p, held);
                    Mutex const* const next = owner->wait_mutex();
                    if (next == nullptr)
                    {
                        break;
                    }
                    owner = next->owner;
                }
                break;
            }
            case WAIT_SEM:
            {
                // The count is untouched, so a later poster still hands its token to a
                // genuine waiter.
                KICKOS_ASSERT_EXCLUSION_HELD();
                t->wait_queue->unlink(&t->link);
                break;
            }
            case WAIT_NOTIFY:
            case WAIT_SLEEP:
            case WAIT_JOIN:
            case WAIT_TASK_EMPTY:
            case WAIT_CONSOLE:
            {
                // On no list at all, and the wake drops any deadline, so clearing the edge is
                // the whole unwind; it is also what makes exit_current's sweep miss a waiter
                // that has given up. A raised bit remains pending in the OBJECT for the next
                // wait if cancellation races delivery.
                break;
            }
            default:
            {
                KICKOS_UNREACHABLE(::kickos::diag::kParkNoKind);
            }
        }
        t->clear_wait_edge();
        t->wait_result = result;
        sched::wake(t, held);
    }

    bool thread_cancel_escalate(Thread* t, uint8_t kind)
    {
        if (t == nullptr or t->state == ThreadState::EXITED
            or t->state == ThreadState::INACTIVE or t->dying)
        {
            return false;
        }
        // Escalation only, ordered NONE < KILL < SLAY: a kill arriving after a slay must not
        // hand back the cleanup window the slay took away.
        if (kind <= t->cancel_kind)
        {
            return false;
        }
        // Set before anything else: a thread that is READY right now must find it at its own
        // death point, and switch_to reads it when the scheduler next lists this thread.
        t->cancel_kind = kind;
        return true;
    }

    void thread_cancel_kind(Thread* t, uint8_t kind, Held held)
    {
        if (not thread_cancel_escalate(t, kind))
        {
            return;
        }
        // An ISR-context slay would need on_cpu here: there current may be booked, not entered.
        if (t == sched::current())
        {
            return; // it is inside the kernel already and will see this on its way out
        }
#if KICKOS_KERNEL_CORES > 1
        // A RUNNING target is on a PEER core, `t == current` having returned above, and the
        // claim is a switch INTO the victim (sched.cc, switch_book), so only that core's own
        // pass can take it off.
        if (kind == CANCEL_SLAY and t->state == ThreadState::RUNNING)
        {
            sched::reseat(t, held);
            return;
        }
#endif
        if (t->state != ThreadState::BLOCKED)
        {
            return;
        }
        // Reaching the death point needs it runnable, so the park ends here. A slain thread
        // never reads the result, but the unwind is the other side of each park's bookkeeping
        // and must still run.
        thread_abort_park(t, -KOS_ECANCELED, held);
    }

    void thread_cancel(Thread* t, Held held)
    {
        thread_cancel_kind(t, CANCEL_KILL, held);
    }

    namespace
    {
#if KICKOS_AMP_CONSOLE_CLAIM
        // Whether a writer parked on a claim since the last ring.
        bool g_claim_waiting = false;
#endif

        // What holds a console writer parked, asked under the same lock as the park.
        enum ConsoleHold
        {
#if KICKOS_AMP_CONSOLE_CLAIM
            HOLD_CLAIM,
#endif
            HOLD_DARK,
            HOLD_ROOM,
            HOLD_WRITERS,
        };

        int console_park(ConsoleHold hold, char const* buf, size_t n, int crlf)
        {
            Thread* const c = sched::current();
            uint32_t epoch = 0;
            {
                IrqLock lock;
                ParkToken const ask = park_cancel_pending(c);
                if (ask.cancelled() and hold != HOLD_WRITERS)
                {
                    return -KOS_ECANCELED;
                }
                if ((hold == HOLD_DARK and console_dark() == 0)
                    or (hold == HOLD_ROOM and console_tx_room_want(buf, n, crlf) == 0)
                    or (hold == HOLD_WRITERS and console_chip_writers() == 0))
                {
                    return 0;
                }
                uint32_t timeout_us = KOS_TIMEOUT_NONE;
#if KICKOS_AMP_CONSOLE_CLAIM
                // Masked from the mark to the park: a release in between rings a doorbell that
                // is taken once this thread is parked.
                if (hold == HOLD_CLAIM)
                {
                    timeout_us = arch_console_claim_wait();
                    if (timeout_us == 0)
                    {
                        // The take cleared this node's mark, which a release already reading the
                        // marks may now miss: a writer still parked here offers again instead.
                        console_claim_rung(lock);
                        return 0;
                    }
                    g_claim_waiting = true;
                }
#endif
                epoch = park_queueless(ask)(c, WAIT_CONSOLE, nullptr, lock);
                ktime_deadline_arm(c, timeout_us, lock);
                sched::reschedule(nullptr, lock);
            }
            wq_confirm_resume(c, epoch);
            return static_cast<int>(c->wait_result);
        }
    }

    int console_dark_wait(void)
    {
        return console_park(HOLD_DARK, nullptr, 0, 0);
    }

    int console_room_wait(char const* buf, size_t n, int crlf)
    {
        if (console_tx_make_room(buf, n, crlf) == 0)
        {
            return 0;
        }
        return console_park(HOLD_ROOM, buf, n, crlf);
    }

#if KICKOS_AMP_CONSOLE_CLAIM
    int console_claim_wait(void)
    {
        int const rc = console_park(HOLD_CLAIM, nullptr, 0, 0);
        if (rc != -KOS_ETIMEDOUT)
        {
            return rc;
        }
        // At the deadline one masked offer more, never a second park: it takes the ended grant and
        // clears this node's mark.
        IrqLock lock;
        if (arch_console_claim_wait() == 0)
        {
            console_claim_rung(lock);
        }
        return 0;
    }

    void console_claim_rung(Held held)
    {
        if (not g_claim_waiting)
        {
            return;
        }
        g_claim_waiting = false;
        console_dark_wake(held);
    }
#endif

    void console_publish_drain(void)
    {
        while (console_chip_writers() != 0)
        {
            (void)console_park(HOLD_WRITERS, nullptr, 0, 0);
        }
    }

    void console_dark_wake(Held held)
    {
        Kernel& k = kernel();
        for (int s = 0; s < k.threads.next; s++)
        {
            Thread* const t = &k.threads.slots[s];
            if (t->wait_kind != WAIT_CONSOLE)
            {
                continue;
            }
            t->clear_wait_edge();
            t->wait_result = 0;
            sched::wake(t, held);
        }
    }

    void task_cancel_group(Task* t, Held held)
    {
        if (t == nullptr)
        {
            return;
        }
        Kernel& k = kernel();
        // thread_cancel_kind refuses a dying thread, so a task holding one thread matches
        // only itself.
        for (int s = 0; s < k.threads.next; s++)
        {
            Thread* const p = &k.threads.slots[s];
            if (p->task != t)
            {
                continue;
            }
            thread_cancel_kind(p, CANCEL_SLAY, held);
        }
    }
}
