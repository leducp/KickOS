// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The redirect hook in switch_to (docs/design-kill-and-slay.md sections 3.2 and 3.5). Four
// claims, and the second is the one no other gate in the tree can make:
//   * a CANCEL_SLAY thread's context is rebuilt into kickos_thread_slay_exit at the top of
//     its own stack, and a CANCEL_KILL one's is not, so the two verbs must stay distinct at
//     the point they diverge, or a kill silently loses its cleanup window.
//   * the rebuild names the INCOMING thread and happens BEFORE arch_switch. Every backend
//     that pends saves the outgoing thread's live registers over prev->ctx and restores
//     next->ctx, so a rebuild of prev there is overwritten and lost with no symptom. The sim
//     swaps inline and would survive that mutant, which is why the ordered trace is the
//     oracle here rather than a counter.
//   * `dying` declines it, so a victim preempted mid-cap_teardown is not restarted from the
//     top of a sweep over a table that is already partly empty.
//   * the rebuild is idempotent: applied again before the stub runs, it changes nothing.
//
// WHAT THIS SEAM CANNOT SEE, stated so an arm is not written against it: the fixture never
// resumes a context, so arch_ctx_redirect here RECORDS instead of rebuilding. That the
// rebuilt frame actually lands on the stub, privileged, is qemu's and qemu-riscv's to
// witness, and per-backend beyond that.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class SlayHook : public KSeam
            {
            };

            constexpr int SLOT_RUNNER = 0;
            constexpr int SLOT_VICTIM = 1;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_HIGH = 6;

            // Arbitrary but distinct from any real address the fixture holds, so an assertion
            // on the stack TOP cannot be satisfied by a coincidence.
            void* const STACK_BASE = reinterpret_cast<void*>(static_cast<uintptr_t>(0x20040000u));
            constexpr size_t STACK_SIZE = 0x1000u;

            // A runnable thread with a stack of its own, at `prio`.
            Thread* victim_at(uint8_t prio)
            {
                Thread* const v = spawn(SLOT_VICTIM, prio);
                v->stack_base = STACK_BASE;
                v->stack_size = STACK_SIZE;
                return v;
            }

            // The low-priority thread the scheduler is running when an arm starts, so that a
            // reschedule has somewhere to switch FROM and the trace carries both tokens.
            Thread* running_thread()
            {
                Thread* const r = spawn(SLOT_RUNNER, PRIO_LOW);
                sched::reschedule();
                EXPECT_EQ(kernel().current, r) << "fixture: the runner is current";
                g_switches = 0;
                g_redirects = 0;
                g_redirect_target = nullptr;
                trace_reset();
                return r;
            }
        }

        // --- the rebuild, and who it names ---------------------------------------------

        TEST_F(SlayHook, a_slain_thread_is_rebuilt_when_it_is_switched_in)
        {
            running_thread();
            Thread* const v = victim_at(PRIO_HIGH);
            v->cancel_kind = CANCEL_SLAY;

            sched::reschedule();

            EXPECT_EQ(kernel().current, v) << "fixture: the higher-priority victim was picked";
            EXPECT_EQ(g_redirects, 1u) << "its resume was claimed";
            EXPECT_EQ(g_redirect_target, v) << "and the context named is the victim's own";
        }

        // The placement rule, as an ORDER rather than a count. A counter oracle cannot fail
        // on a reordering, and a rebuild after arch_switch is exactly a reordering.
        TEST_F(SlayHook, the_rebuild_precedes_the_switch)
        {
            Thread* const r = running_thread();
            Thread* const v = victim_at(PRIO_HIGH);
            v->cancel_kind = CANCEL_SLAY;

            sched::reschedule();

            char expect[64];
            snprintf(expect, sizeof(expect), "redirect%u switch%u>%u", v->id, r->id, v->id);
            EXPECT_STREQ(trace(), expect)
                << "on every backend that pends, a rebuild placed after arch_switch is "
                   "overwritten by the switcher's own save and restore";
        }

        // The converse of the arm above, and the mutant it kills is "the hook fires on prev":
        // the OUTGOING thread is the slain one here, and nothing may be rebuilt.
        TEST_F(SlayHook, a_slain_outgoing_thread_is_not_rebuilt)
        {
            Thread* const r = running_thread();
            r->stack_base = STACK_BASE;
            r->stack_size = STACK_SIZE;
            r->cancel_kind = CANCEL_SLAY;
            Thread* const v = victim_at(PRIO_HIGH);

            sched::reschedule();

            EXPECT_EQ(kernel().current, v) << "fixture: the switch happened";
            EXPECT_EQ(g_redirects, 0u)
                << "the hook reads the INCOMING thread only; a rebuild of the outgoing "
                   "context is what the switcher's save destroys";
        }

        // --- what the rebuild is given -------------------------------------------------

        TEST_F(SlayHook, the_rebuild_targets_the_slay_stub_at_the_top_of_the_victims_stack)
        {
            running_thread();
            Thread* const v = victim_at(PRIO_HIGH);
            v->cancel_kind = CANCEL_SLAY;

            sched::reschedule();

            EXPECT_EQ(reinterpret_cast<void*>(g_redirect_entry),
                      reinterpret_cast<void*>(&kickos_thread_slay_exit))
                << "the victim runs its OWN teardown through the same exit_current every "
                   "other death uses; no stranger runs its cap_teardown";
            EXPECT_EQ(g_redirect_stack_top,
                      reinterpret_cast<uintptr_t>(STACK_BASE) + STACK_SIZE)
                << "at the TOP of its own stack, not at the depth it had reached: the stub "
                   "needs headroom for exit_current and the whole capability sweep";
        }

        // --- which kinds it claims -----------------------------------------------------

        TEST_F(SlayHook, a_killed_thread_is_not_rebuilt)
        {
            running_thread();
            Thread* const v = victim_at(PRIO_HIGH);
            v->cancel_kind = CANCEL_KILL;

            sched::reschedule();

            EXPECT_EQ(kernel().current, v) << "fixture: it was switched in";
            EXPECT_EQ(g_redirects, 0u)
                << "a kill keeps its cleanup window and dies at its next syscall ENTRY; "
                   "claiming its resume here would silently make kill mean slay";
        }

        TEST_F(SlayHook, an_uncancelled_thread_is_not_rebuilt)
        {
            running_thread();
            victim_at(PRIO_HIGH);

            sched::reschedule();

            EXPECT_EQ(g_redirects, 0u) << "the hot path costs two tests and nothing else";
        }

        // --- the restart guard ----------------------------------------------------------

        // cap_teardown releases IrqLock between chunks, so a half-swept victim is preemptible
        // and can be switched back in. Re-entering the stub from the top would restart the
        // sweep over a table it has already partly emptied.
        TEST_F(SlayHook, a_dying_victim_is_not_rebuilt)
        {
            running_thread();
            Thread* const v = victim_at(PRIO_HIGH);
            v->cancel_kind = CANCEL_SLAY;
            v->dying = true;

            sched::reschedule();

            EXPECT_EQ(kernel().current, v) << "fixture: it was switched in";
            EXPECT_EQ(g_redirects, 0u)
                << "`dying` is the window's existing marker and no fourth flag is needed";
        }

        // The other half of the guard: it must be `dying` that declines, not the FIRST
        // rebuild having happened. Before the stub makes progress the rebuild is idempotent
        // in its values, so repeating it is correct and must not be suppressed.
        TEST_F(SlayHook, a_rebuild_repeats_while_the_stub_has_not_started)
        {
            Thread* const r = running_thread();
            Thread* const v = victim_at(PRIO_HIGH);
            v->cancel_kind = CANCEL_SLAY;

            sched::reschedule();       // switch to the victim, rebuild #1
            kernel().current = r;      // the fixture never switches, so put the runner back
            r->state = ThreadState::RUNNING;
            v->state = ThreadState::READY;
            sched::reschedule();       // pick the victim again, rebuild #2

            EXPECT_EQ(g_redirects, 2u)
                << "an absolute entry and an absolute stack top make a second rebuild a "
                   "no-op in its values, so nothing has to remember the first";
        }
    }
}
