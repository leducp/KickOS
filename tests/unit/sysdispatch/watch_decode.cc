// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_WATCH over the REAL kernel/syscall/syscall.cc and kernel/notify/watch.cc at the region
// posture: the spec's decode, each kind's refusal order as the table entry lists it, both kinds'
// disarm, and the TASK watch raising only the events it asks for.

#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kseam_test.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3, uintptr_t a4);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint8_t PRIO = 10;
            constexpr int LINE = 5;
            constexpr uint32_t ENDS = KOS_TASK_ENDED | KOS_TASK_DEAD;
            constexpr uint32_t ALL = KOS_TASK_READY | KOS_TASK_ENDED | KOS_TASK_DEAD;
            constexpr uint32_t BAD_CAP = 0x7FFFu;

            uint32_t spec(uint32_t kind, uint32_t events)
            {
                return (kind << KOS_WATCH_KIND_SHIFT) | events;
            }

            class SysWatch : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    creator_ = seat_pool(0, PRIO);
                    stranger_ = seat_pool(1, PRIO);
                    attach_caps(creator_, KICKOS_SEAM_CAP_WIDTH);
                    attach_caps(stranger_, KICKOS_SEAM_CAP_WIDTH);
                    kernel().current(kickos_kernel_core()) = creator_;
                    int err = 0;
                    {
                        IrqLock lock;
                        task_ = task_held(kernel().threads.kill_tag_of(creator_), &err, lock);
                        foreign_ = task_held(kernel().threads.kill_tag_of(stranger_), &err, lock);
                    }
                    ASSERT_NE(task_, nullptr) << "fixture: no task (" << err << ")";
                    ASSERT_NE(foreign_, nullptr) << "fixture: no foreign task (" << err << ")";
                    ASSERT_EQ(notify_create(creator_, &note_), 0);
                    {
                        IrqLock lock;
                        obj_ = cap_lookup(creator_, note_)->obj;
                    }
                    wait_only_ = alias(obj_, CapType::CAP_NOTIFY, CAP_WAIT);
                }

                uint32_t alias(int obj, CapType type, uint8_t rights)
                {
                    IrqLock lock;
                    uint32_t cap = KCAP_INVALID;
                    EXPECT_TRUE(obj_ref_inc(type, obj, rights));
                    EXPECT_EQ(cap_install(creator_, obj, type, rights, &cap), 0);
                    return cap;
                }

                // The task names a ready endpoint, as a create of a declaration naming one seats.
                void seat_ready()
                {
                    Endpoint* const ep = endpoint();
                    IrqLock lock;
                    ready_ = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
                    task_ready_seat(task_, ready_);
                }

                uint32_t claim()
                {
                    uint32_t cap = KCAP_INVALID;
                    EXPECT_EQ(irq_claim(creator_, LINE, 0, &cap), 0);
                    return cap;
                }

                int32_t watch(uint32_t s, uintptr_t source, uint32_t notify, uintptr_t arg = 0,
                              uintptr_t out = 0)
                {
                    kernel().current(kickos_kernel_core()) = creator_;
                    return static_cast<int32_t>(
                        syscall_dispatch(KOS_SYS_WATCH, s, source, notify, arg, out));
                }

                uintptr_t handle() const
                {
                    return task_handle(task_);
                }

                uint32_t pending() const
                {
                    return kernel().notifies.resolve(obj_)->pending;
                }

                void clear()
                {
                    kernel().notifies.resolve(obj_)->pending = 0;
                }

                void end_task()
                {
                    IrqLock lock;
                    task_stop(task_, lock);
                }

                void check_bit(uint32_t bit)
                {
                    uint32_t badged = KCAP_INVALID;
                    ASSERT_EQ(notify_badge(creator_, note_, bit, &badged), 0);
                    ASSERT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_DEAD), handle(), badged), 0);
                    clear();
                    end_task();
                    EXPECT_EQ(pending(), 0u) << "an unasked ENDED raised";
                    uint16_t const gen = task_gen(task_);
                    {
                        IrqLock lock;
                        task_report_death(task_, gen);
                    }
                    EXPECT_EQ(pending(), 1u << bit);
                }

                void kill_task()
                {
                    uint16_t const gen = task_gen(task_);
                    {
                        IrqLock lock;
                        task_stop(task_, lock);
                    }
                    IrqLock lock;
                    task_report_death(task_, gen);
                }

                Thread* creator_ = nullptr;
                Thread* stranger_ = nullptr;
                Task* task_ = nullptr;
                Task* foreign_ = nullptr;
                uint32_t note_ = KCAP_INVALID;
                uint32_t wait_only_ = KCAP_INVALID;
                int obj_ = -1;
                int ready_ = -1;
            };
        }

        TEST_F(SysWatch, a_spec_naming_no_kind_or_more_than_its_kind_defines_is_invalid)
        {
            EXPECT_EQ(watch(spec(0, KOS_TASK_ENDED), handle(), note_), -KOS_EINVAL) << "kind 0";
            EXPECT_EQ(watch(spec(4, 1u), handle(), note_), -KOS_EINVAL) << "an unknown kind";
            EXPECT_EQ(watch(spec(0xFFu, 1u), handle(), note_), -KOS_EINVAL) << "the top kind";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_LIVE), handle(), note_), -KOS_EINVAL)
                << "LIVE is a state, not an event";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, 1u << 4), handle(), note_), -KOS_EINVAL);
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, 1u << 15), handle(), note_), -KOS_EINVAL);
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, 2u), 0, note_), -KOS_EINVAL);
            for (uint32_t flag = 0; flag < 8u; flag++)
            {
                uint32_t const flagged =
                    spec(KOS_WATCH_TASK, KOS_TASK_ENDED) | (1u << (KOS_WATCH_FLAGS_SHIFT + flag));
                EXPECT_EQ(watch(flagged, handle(), note_), -KOS_EINVAL) << "flag bit " << flag;
            }
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_ENDED), handle(), note_, 1u), -KOS_EINVAL)
                << "a TASK arg";
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), claim(), note_, 1u), -KOS_EINVAL)
                << "an IRQ arg";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, 0), handle(), note_), -KOS_EINVAL)
                << "a TASK disarm naming a notification";
            clear();
            end_task();
            EXPECT_EQ(pending(), 0u) << "a refused spec armed something";
        }

        TEST_F(SysWatch, a_task_watch_refuses_spec_then_source_then_authority_then_notification)
        {
            // Each line holds every later fault too, so only the order can answer it.
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_LIVE), KOS_TASK_NONE, BAD_CAP), -KOS_EINVAL);
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ALL), KOS_TASK_NONE, BAD_CAP), -KOS_EBADF)
                << "no such task";
            if (sizeof(uintptr_t) > sizeof(uint32_t))
            {
                uintptr_t const top = static_cast<uintptr_t>(1u) << (8 * sizeof(uintptr_t) - 1);
                uintptr_t const wide = handle() | top;
                EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), wide, note_), -KOS_EBADF)
                    << "a source wider than a handle";
            }
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ALL), task_handle(foreign_), BAD_CAP), -KOS_EPERM)
                << "another creator's task, READY of no ready endpoint and a bad notification";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, 0), task_handle(foreign_), KOS_CAP_NONE), -KOS_EPERM)
                << "the disarm of another creator's task";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ALL), handle(), BAD_CAP), -KOS_EINVAL)
                << "READY of a task naming no ready endpoint, before the notification";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), handle(), KOS_CAP_NONE), -KOS_EBADF)
                << "an arm naming no notification";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), handle(), BAD_CAP), -KOS_EBADF);
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), handle(), wait_only_), -KOS_EACCES)
                << "a notification capability without SIGNAL";
            int32_t const armed = watch(spec(KOS_WATCH_TASK, ENDS), handle(), note_);
            EXPECT_GE(armed, 0);
            EXPECT_EQ(armed, 0);
        }

        TEST_F(SysWatch, ready_is_watched_only_of_a_task_whose_declaration_names_a_ready_endpoint)
        {
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_READY), handle(), note_), -KOS_EINVAL);
            seat_ready();
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ALL), handle(), note_), 0);
            clear();
            {
                IrqLock lock;
                EXPECT_EQ(task_note_receive(task_, ready_), nullptr) << "nothing is bound to wake";
            }
            EXPECT_EQ(pending(), 1u) << "the first receive did not raise READY";
        }

        TEST_F(SysWatch, a_watch_not_asking_ready_stays_quiet_at_the_first_receive)
        {
            seat_ready();
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), handle(), note_), 0);
            clear();
            {
                IrqLock lock;
                EXPECT_EQ(task_note_receive(task_, ready_), nullptr);
            }
            EXPECT_EQ(pending(), 0u) << "the first receive raised an unasked READY";
        }

        TEST_F(SysWatch, an_ended_and_dead_watch_raises_once_across_the_end_and_the_death)
        {
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), handle(), note_), 0);
            clear();
            end_task();
            EXPECT_EQ(pending(), 1u) << "the end was missed";
            clear();
            uint16_t const gen = task_gen(task_);
            {
                IrqLock lock;
                task_report_death(task_, gen);
            }
            EXPECT_EQ(pending(), 1u) << "the death was missed";
        }

        TEST_F(SysWatch, an_ended_watch_does_not_raise_again_at_the_death_after_its_end)
        {
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_ENDED), handle(), note_), 0);
            clear();
            end_task();
            EXPECT_EQ(pending(), 1u) << "the end was missed";
            clear();
            uint16_t const gen = task_gen(task_);
            {
                IrqLock lock;
                task_report_death(task_, gen);
            }
            EXPECT_EQ(pending(), 0u) << "the death raised the end a second time";
        }

        TEST_F(SysWatch, a_watch_badged_at_bit_16_raises_that_bit_and_keeps_its_event_selection)
        {
            check_bit(16u);
        }

        TEST_F(SysWatch, a_watch_badged_at_bit_31_raises_that_bit_and_keeps_its_event_selection)
        {
            check_bit(31u);
        }

        TEST_F(SysWatch, a_task_watch_raises_only_the_events_it_asks_for)
        {
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_DEAD), handle(), note_), 0);
            clear();
            end_task();
            EXPECT_EQ(pending(), 0u) << "a DEAD watch raised at the end";
            uint16_t const gen = task_gen(task_);
            {
                IrqLock lock;
                task_report_death(task_, gen);
            }
            EXPECT_EQ(pending(), 1u) << "a DEAD watch missed the death";
        }

        TEST_F(SysWatch, an_ended_watch_raises_at_a_death_that_never_reported_the_end)
        {
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_ENDED), handle(), note_), 0);
            clear();
            uint16_t const gen = task_gen(task_);
            {
                IrqLock lock;
                task_report_death(task_, gen);
            }
            EXPECT_EQ(pending(), 1u);
        }

        TEST_F(SysWatch, a_task_watch_armed_after_its_event_raises_at_once_and_a_re_arm_replaces_it)
        {
            kill_task();
            clear();
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_DEAD), handle(), note_), 0);
            EXPECT_EQ(pending(), 1u) << "a death before the arming is not raised at it";

            uint32_t badged = KCAP_INVALID;
            ASSERT_EQ(notify_badge(creator_, note_, 3u, &badged), 0);
            clear();
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, KOS_TASK_ENDED), handle(), badged), 0);
            EXPECT_EQ(pending(), 1u << 3) << "the re-arm raised the replaced bit";
        }

        TEST_F(SysWatch, an_instance_watch_refuses_its_out_pointer_first_and_a_task_watch_never_reads_it)
        {
            uint32_t const instance = spec(KOS_WATCH_INSTANCE, KOS_INSTANCE_STARTABLE);
            EXPECT_EQ(watch(instance, BAD_CAP, BAD_CAP, 1u), -KOS_EINVAL) << "no out-pointer";
            EXPECT_EQ(watch(instance, BAD_CAP, BAD_CAP, 1u, 1u), -KOS_EINVAL) << "a misaligned one";
            EXPECT_EQ(watch(instance, BAD_CAP, BAD_CAP, 1u, 4u), -KOS_EFAULT) << "an unwritable one";
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), handle(), note_, 0, 1u), 0);
        }

        TEST_F(SysWatch, a_task_disarm_ends_the_watch)
        {
            ASSERT_EQ(watch(spec(KOS_WATCH_TASK, ENDS), handle(), note_), 0);
            EXPECT_EQ(watch(spec(KOS_WATCH_TASK, 0), handle(), KOS_CAP_NONE), 0);
            clear();
            kill_task();
            EXPECT_EQ(pending(), 0u) << "a disarmed watch raised";
        }

        TEST_F(SysWatch, an_irq_watch_refuses_spec_then_line_then_disarm_then_second_then_notification)
        {
            uint32_t const line = claim();
            int line_obj = -1;
            {
                IrqLock lock;
                line_obj = cap_lookup(creator_, line)->obj;
            }
            uint32_t const signal_only = alias(line_obj, CapType::CAP_IRQ, CAP_SIGNAL);

            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, 2u), BAD_CAP, BAD_CAP), -KOS_EINVAL);
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, 0), BAD_CAP, BAD_CAP), -KOS_EBADF)
                << "a disarm of no line";
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), note_, note_), -KOS_EBADF)
                << "a capability naming no line";
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, 0), signal_only, BAD_CAP), -KOS_EACCES)
                << "a disarm through a line capability without WAIT";
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), signal_only, BAD_CAP), -KOS_EACCES);
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, 0), line, BAD_CAP), -KOS_ENOTSUP)
                << "a disarm of an unwatched line";
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), line, KOS_CAP_NONE), -KOS_EBADF);
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), line, wait_only_), -KOS_EACCES);
            int32_t const armed = watch(spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), line, note_);
            EXPECT_GE(armed, 0);
            EXPECT_EQ(armed, 0);
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, 0), line, KOS_CAP_NONE), -KOS_ENOTSUP)
                << "the watch is one-way";
            EXPECT_EQ(watch(spec(KOS_WATCH_IRQ, KOS_IRQ_FIRED), line, BAD_CAP), -KOS_EBUSY)
                << "a second watch, before its notification";
        }
    }
}
