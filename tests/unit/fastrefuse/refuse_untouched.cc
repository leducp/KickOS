// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kickos_ipc_fastpath decides before it mutates: every test that can fail runs before the
// first write, so a refusal leaves the trap able to continue into the generic dispatch with
// nothing committed. This drives every refusal this library's posture can reach and reads
// the WHOLE of the state each one could have reached.
//
// ONE REFUSAL IS OUTSIDE THAT POSTURE and no arm below stands for it: the far-endpoint
// fall-through. This library compiles with KICKOS_AMP_NODE off, where endpoint_is_far is
// `return false` for every argument, so an arm there would refuse on the staging it shares
// rather than on the clause it names, and would pass whichever way that clause went. The
// order the clause's own comment justifies, far ahead of the dead-endpoint test, is
// unobservable here for the same reason: both are the same nullptr fall-through, so no
// oracle separates them. Reaching it means an AMP build of the same entry point.
//
// THE ORACLE IS A BYTE IMAGE AND NOT A FIELD LIST, which is what makes it total: the Kernel
// object (the thread pool, the endpoint pool, the ready lists, this core's current), the
// fixture's own thread storage, both capability tables out of the slab, and the caller's
// register frame beside the receiver's two landing buffers. A field this file forgot to name
// cannot slip through, and a mutation that is undone AFTER the fact still fails, because an
// undo restores values and not the generation counters and queue order it spent reaching
// them.
//
// A REFUSAL PROVES NOTHING ON ITS OWN: the same staging with the defect removed must COMPLETE,
// or an arm refusing for a reason it did not build would pass. take_completes below is that
// control and every refusing arm shares its staging.

#include <string.h>

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "syscall_internal.h"

#include "kseam_test.h"

// The trap's entry point. Returns the incoming thread's context, or nullptr for a refusal.
extern "C" struct arch_context* kickos_ipc_fastpath(uint32_t* args);
extern "C" uint32_t g_fast_result;
extern "C" uint32_t g_fast_result_stores;

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class FastRefuse : public KSeam
            {
            };

            constexpr int SLOT_CALLER = 0;
            constexpr uint8_t PRIO = 5;
            constexpr int FX_RECEIVER = 1;
            constexpr int FX_PEER = 2;

            constexpr uint32_t REPLY_MAX = KICKOS_CAP_REPLY_MAX;
            // Wide enough that cap_can_take_reply is satisfied by slack rather than by luck.
            constexpr uint32_t TABLE_WIDTH = KICKOS_CAP_FIRST_DYNAMIC + REPLY_MAX + 2;
            // Exactly one dynamic slot, so one mint empties the free list.
            constexpr uint32_t TABLE_NARROW = KICKOS_CAP_FIRST_DYNAMIC + 1;
            static_assert(TABLE_WIDTH <= KICKOS_MAX_HANDLES,
                          "the arms below ask the slab for a table wider than the codec's "
                          "ceiling, which cap_slab_attach asserts on");

            constexpr size_t ARG_WORDS = KOS_CALL_REG_WORDS + 3;
            constexpr size_t SEND_WORDS = 2;
            constexpr size_t SEND_BYTES = SEND_WORDS * sizeof(uint32_t);

            // Every byte the fastpath could write OUTSIDE the kernel, the fixture storage and
            // the capability slab, in one block so one comparison covers all of it: the
            // caller's saved register frame, and the two buffers the receiver's parked
            // descriptor names.
            struct Scratch
            {
                uint32_t args[ARG_WORDS];
                uint32_t recv_buf[KOS_CALL_REG_WORDS];
                kos_recv_info info;
                uint32_t tail;
            };
            Scratch g_scratch;

            // The state a refusal must leave standing, whole.
            struct Snap
            {
                unsigned char kern[sizeof(Kernel)];
                unsigned char fix[sizeof(Fixture)];
                CapEntry caller_caps[TABLE_WIDTH];
                CapEntry recv_caps[TABLE_WIDTH];
                uint32_t caller_width;
                uint32_t recv_width;
                Scratch scratch;
                uint32_t taken;
                uint32_t switches;
                uint32_t result_stores;
            };
            Snap g_before;
            Snap g_after;

            void snap_table(Thread const* t, CapEntry* out, uint32_t* out_width)
            {
                uint32_t const w = thread_cap_capacity(t);
                if (w > TABLE_WIDTH)
                {
                    printf("FIXTURE FAIL: a table of %u slots outgrew the snapshot\n", w);
                    exit(1);
                }
                *out_width = w;
                for (uint32_t i = 0; i < w; i++)
                {
                    out[i] = *cap_slot(t->caps, i);
                }
            }

            void snap_take(Snap* s, Thread const* caller, Thread const* recv)
            {
                memcpy(s->kern, &kernel(), sizeof(s->kern));
                memcpy(s->fix, &g_fx, sizeof(s->fix));
                snap_table(caller, s->caller_caps, &s->caller_width);
                snap_table(recv, s->recv_caps, &s->recv_width);
                s->scratch = g_scratch;
                s->taken = ipc_fast_taken_count();
                s->switches = g_switches;
                s->result_stores = g_fast_result_stores;
            }

            void expect_same(char const* what, void const* a, void const* b, size_t n)
            {
                unsigned char const* x = static_cast<unsigned char const*>(a);
                unsigned char const* y = static_cast<unsigned char const*>(b);
                for (size_t i = 0; i < n; i++)
                {
                    if (x[i] != y[i])
                    {
                        ADD_FAILURE()
                            << what << " changed at byte " << i << " (0x" << std::hex
                            << static_cast<unsigned>(x[i]) << " -> 0x"
                            << static_cast<unsigned>(y[i]) << std::dec
                            << "): the refusal committed something, or undid it after the "
                               "fact and spent a counter doing so";
                        return;
                    }
                }
            }

            // THE CLAIM. Nothing the fastpath can reach differs.
            void expect_untouched(Snap const& before, Snap const& after)
            {
                expect_same("the Kernel object", before.kern, after.kern, sizeof(before.kern));
                expect_same("the fixture's thread storage", before.fix, after.fix,
                            sizeof(before.fix));
                EXPECT_EQ(before.caller_width, after.caller_width)
                    << "the caller's table width";
                EXPECT_EQ(before.recv_width, after.recv_width) << "the receiver's table width";
                expect_same("the caller's capability table", before.caller_caps,
                            after.caller_caps,
                            before.caller_width * sizeof(CapEntry));
                expect_same("the receiver's capability table", before.recv_caps,
                            after.recv_caps, before.recv_width * sizeof(CapEntry));
                expect_same("the caller's register frame and the receiver's buffers",
                            &before.scratch, &after.scratch, sizeof(before.scratch));
                EXPECT_EQ(before.taken, after.taken)
                    << "a refusal does not count as a completed call";
                EXPECT_EQ(before.switches, after.switches) << "a refusal switches to nobody";
                EXPECT_EQ(before.result_stores, after.result_stores)
                    << "a refusal writes no result into a saved frame";
            }

            // A receiver parked on ep->recv_waiters, which no fixture helper builds: mirrors
            // wq_block's ORDER (BLOCKED before the detach, the ready-list removal before the
            // queue push re-uses the same link node) for a thread that is not current.
            void park_receiver(Thread* w, Endpoint* ep, uintptr_t buf, size_t cap_len,
                               uintptr_t badge_out)
            {
                w->state = ThreadState::BLOCKED;
                kernel().policy->on_remove(w);
                w->wait_queue = &ep->recv_waiters;
                w->wait_kind = WAIT_EP_RECV;
                w->wait_obj = ep;
                w->call_state = CALL_NONE;
                w->ipc.buf = buf;
                w->ipc.len = cap_len;
                w->ipc.badge_out = badge_out;
                w->wait_result = WAIT_RESULT_POISON;
                ep->recv_waiters.push_back(&w->link);
            }

            struct Stage
            {
                Thread* c;
                Thread* w;
                Thread* peer;
                Endpoint* ep;
                uint32_t signal_cap; // CAP_ENDPOINT with CAP_SIGNAL: what a call resolves
                uint32_t wait_cap;   // the same endpoint with CAP_WAIT only
            };

            // A staging the fastpath COMPLETES on, so an arm builds exactly one defect into
            // it. `recv_width` is the receiver's table, which one arm narrows.
            //
            // TWO receivers at ONE priority, the second behind the first: wq_peek_highest is
            // FIFO within a priority band, so a pop that is undone by a re-push puts the
            // receiver behind its peer and the byte image says so. Without the peer a pop and
            // a re-push are indistinguishable.
            Stage stage(uint32_t recv_width = TABLE_WIDTH)
            {
                Stage s = {};
                memset(&g_scratch, 0, sizeof(g_scratch));

                s.c = seat_pool(SLOT_CALLER, PRIO);
                attach_caps(s.c, TABLE_WIDTH);

                s.ep = endpoint();
                s.ep->recv_holders = 1;
                int const obj = kernel().endpoints.handle_for(kernel().endpoints.index_of(s.ep));
                if (cap_install(s.c, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL, &s.signal_cap) != 0
                    or cap_install(s.c, obj, CapType::CAP_ENDPOINT, CAP_WAIT, &s.wait_cap) != 0)
                {
                    printf("FIXTURE FAIL: the caller's table refused an endpoint cap\n");
                    exit(1);
                }

                s.w = spawn(FX_RECEIVER, PRIO);
                attach_caps(s.w, recv_width);
                park_receiver(s.w, s.ep, reinterpret_cast<uintptr_t>(g_scratch.recv_buf),
                              sizeof(g_scratch.recv_buf),
                              reinterpret_cast<uintptr_t>(&g_scratch.info));
                s.peer = spawn(FX_PEER, PRIO);
                attach_caps(s.peer, TABLE_WIDTH);
                park_receiver(s.peer, s.ep, reinterpret_cast<uintptr_t>(g_scratch.recv_buf),
                              sizeof(g_scratch.recv_buf),
                              reinterpret_cast<uintptr_t>(&g_scratch.info));

                // Last, so the caller holds the CPU with both receivers already parked.
                sched::reschedule();
                if (kernel().current[kickos_kernel_core()] != s.c)
                {
                    printf("FIXTURE FAIL: the caller does not hold the CPU\n");
                    exit(1);
                }

                g_scratch.args[0] = KOS_SYS_CALL_REG;
                g_scratch.args[1] = s.signal_cap;
                g_scratch.args[2] = static_cast<uint32_t>(
                    kos_call_lens_pack(SEND_BYTES, KOS_CALL_REG_BYTES));
                for (size_t i = 3; i < ARG_WORDS; i++)
                {
                    g_scratch.args[i] = static_cast<uint32_t>(0xA0A00000u + i);
                }
                g_switches = 0;
                trace_reset();
                return s;
            }

            // Every refusing arm ends here.
            void expect_refused(Stage const& s)
            {
                snap_take(&g_before, s.c, s.w);
                struct arch_context* const ctx = kickos_ipc_fastpath(g_scratch.args);
                snap_take(&g_after, s.c, s.w);
                EXPECT_EQ(ctx, nullptr) << "a refusal returns no context to restore";
                expect_untouched(g_before, g_after);
            }
        }

        // --- the positive control ------------------------------------------------------
        // WITHOUT THIS EVERY ARM BELOW IS VACUOUS: a staging the fastpath refuses for some
        // reason nobody built refuses every arm and passes every one of them.

        TEST_F(FastRefuse, take_completes)
        {
            Stage s = stage();
            uint32_t const taken = ipc_fast_taken_count();

            struct arch_context* const ctx = kickos_ipc_fastpath(g_scratch.args);

            ASSERT_NE(ctx, nullptr) << "the staging every arm below shares must COMPLETE";
            EXPECT_EQ(ipc_fast_taken_count(), taken + 1u) << "and count as one taken call";
            EXPECT_EQ(ctx, &s.w->ctx) << "the context handed back is the receiver's";
            EXPECT_EQ(s.c->call_state, CALL_REPLY_WAIT) << "the caller parks on the reply";
            EXPECT_EQ(s.c->call_frame_parked, 1u)
                << "with no kernel continuation: the switch owes the result to its frame";
            EXPECT_EQ(s.c->state, ThreadState::BLOCKED);
            EXPECT_EQ(s.w->state, ThreadState::RUNNING) << "the receiver takes the CPU";
            EXPECT_EQ(s.w->wait_result, static_cast<intptr_t>(SEND_BYTES))
                << "the receiver is told how many bytes arrived";
            EXPECT_EQ(g_scratch.recv_buf[0], 0xA0A00003u) << "the request landed";
            EXPECT_NE(g_scratch.info.reply_cap, KCAP_INVALID)
                << "and the receiver holds a reply capability";
            EXPECT_EQ(wq_peek_highest(s.ep->recv_waiters), s.peer)
                << "the peer is still parked, and is now the highest";
        }

        // --- no current thread ---------------------------------------------------------

        TEST_F(FastRefuse, no_current_thread)
        {
            Stage s = stage();
            kernel().current[kickos_kernel_core()] = nullptr;

            expect_refused(s);
        }

        // --- the caller is already asked to die ----------------------------------------
        // The generic dispatch exits a cancelled thread on entry, so bypassing dispatch must
        // not bypass that. Both kinds and the past-the-point flag, because the clause is a
        // disjunction and one operand lost would leave the other passing.

        TEST_F(FastRefuse, a_cancelled_caller)
        {
            Stage s = stage();
            s.c->cancel_kind = CANCEL_KILL;

            expect_refused(s);
        }

        TEST_F(FastRefuse, a_slain_caller)
        {
            Stage s = stage();
            s.c->cancel_kind = CANCEL_SLAY;

            expect_refused(s);
        }

        TEST_F(FastRefuse, a_dying_caller)
        {
            Stage s = stage();
            s.c->dying = 1;

            expect_refused(s);
        }

        // --- the payload does not fit the registers it arrived in ----------------------

        TEST_F(FastRefuse, an_oversize_request)
        {
            Stage s = stage();
            g_scratch.args[2] = static_cast<uint32_t>(
                kos_call_lens_pack(KOS_CALL_REG_BYTES + 1, KOS_CALL_REG_BYTES));

            expect_refused(s);
        }

        TEST_F(FastRefuse, an_oversize_reply_capacity)
        {
            Stage s = stage();
            g_scratch.args[2] = static_cast<uint32_t>(
                kos_call_lens_pack(SEND_BYTES, KOS_CALL_REG_BYTES + 1));

            expect_refused(s);
        }

        // --- the capability ------------------------------------------------------------

        TEST_F(FastRefuse, a_handle_naming_nothing)
        {
            Stage s = stage();
            g_scratch.args[1] = 0xDEADBEEFu;

            expect_refused(s);
        }

        TEST_F(FastRefuse, a_capability_without_the_signal_right)
        {
            Stage s = stage();
            g_scratch.args[1] = s.wait_cap;

            expect_refused(s);
        }

        // --- the endpoint --------------------------------------------------------------

        TEST_F(FastRefuse, a_dead_endpoint)
        {
            Stage s = stage();
            s.ep->recv_holders = 0;

            expect_refused(s);
        }

        TEST_F(FastRefuse, no_parked_receiver)
        {
            Stage s = stage();
            // Both receivers off the queue: the peek answers nothing. Through the real
            // unlink, so the queue is left as an empty queue rather than a broken one.
            s.ep->recv_waiters.unlink(&s.w->link);
            s.ep->recv_waiters.unlink(&s.peer->link);
            ASSERT_TRUE(s.ep->recv_waiters.empty());

            expect_refused(s);
        }

        // --- the receiver --------------------------------------------------------------

        TEST_F(FastRefuse, an_info_less_receiver)
        {
            Stage s = stage();
            s.w->ipc.badge_out = 0;

            expect_refused(s);
        }

        TEST_F(FastRefuse, a_dying_receiver)
        {
            Stage s = stage();
            s.w->dying = 1;

            expect_refused(s);
        }

        TEST_F(FastRefuse, a_caller_outranking_the_receiver)
        {
            Stage s = stage();
            // Through the funnel: prio is not a field a caller may write, and a bare store
            // would leave the ready lists indexed by the old value.
            sched::set_prio(s.c, PRIO + 1);
            g_switches = 0;
            trace_reset();

            expect_refused(s);
        }

        // The RECEIVER's table, and its reply bound, are what refuse: no side effect has
        // happened yet when the probe answers no.
        TEST_F(FastRefuse, a_receiver_whose_table_is_full)
        {
            Stage s = stage(TABLE_NARROW);
            uint32_t spent = KCAP_INVALID;
            ASSERT_EQ(cap_install_reply(s.w, s.c, &spent), 0)
                << "fixture: the one dynamic slot is taken by hand";
            ASSERT_FALSE(cap_can_take_reply(s.w)) << "fixture: the table is full";

            expect_refused(s);
        }

        TEST_F(FastRefuse, a_receiver_at_its_reply_bound)
        {
            Stage s = stage();
            for (uint32_t i = 0; i < REPLY_MAX; i++)
            {
                uint32_t spent = KCAP_INVALID;
                ASSERT_EQ(cap_install_reply(s.w, s.c, &spent), 0)
                    << "fixture: filling to the bound is not itself a refusal";
            }
            ASSERT_NE(s.w->cap_free_head, KCAP_FREE_NONE)
                << "fixture: slots are still free, so the BOUND is the only clause left";
            ASSERT_FALSE(cap_can_take_reply(s.w));

            expect_refused(s);
        }

        // --- the copy ------------------------------------------------------------------
        // The one refusal that sits after a copy has started in every other IPC path, and
        // ahead of the commit here. The receiver names a range inside the caller's own saved
        // frame, which is the same-owner overlap ep_copy refuses, and it must not be a panic.

        TEST_F(FastRefuse, a_receiver_naming_the_callers_own_frame)
        {
            Stage s = stage();
            s.w->ipc.buf = reinterpret_cast<uintptr_t>(&g_scratch.args[3]);
            ASSERT_FALSE(ep_copy(nullptr, s.w->ipc.buf, nullptr,
                                 reinterpret_cast<uintptr_t>(&g_scratch.args[3]), SEND_BYTES))
                << "fixture: the overlap is what ep_copy refuses";

            expect_refused(s);
        }
    }
}
