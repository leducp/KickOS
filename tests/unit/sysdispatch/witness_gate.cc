// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_WITNESS_UACCESS over the REAL kernel/syscall/syscall.cc at the region posture, built
// twice: with KICKOS_WITNESS_UACCESS, where a caller without the whole authority word is refused
// before the kernel touches anything, and without it, where the number answers as an unknown
// one. On the host the access the gate lets through lands, which is what shows the arm is live.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/witness.h>

#include "kseam_test.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint8_t SEEN = 0x5Au;

            class SysWitness : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    caller_ = seat_pool(1, 10);
                    join_task(caller_, task(0));
                    kernel().current(kickos_kernel_core()) = caller_;
                }

                int64_t write_word(uint8_t volatile* word)
                {
                    return static_cast<int64_t>(syscall_dispatch(
                        KOS_SYS_WITNESS_UACCESS, KOS_WITNESS_UWRITE_NOW,
                        reinterpret_cast<uintptr_t>(word), 0, 0));
                }

                Thread* caller_ = nullptr;
            };

#if KICKOS_WITNESS_UACCESS
            TEST_F(SysWitness, a_caller_short_of_the_whole_authority_word_is_refused_untouched)
            {
                uint8_t volatile word = SEEN;
                cap_seat_authority(caller_, CAP_AUTH_ALL & ~AUTH_BOOTLOADER);
                EXPECT_EQ(write_word(&word), -KOS_EPERM);
                EXPECT_EQ(word, SEEN) << "a refusal touches nothing";

                cap_seat_authority(caller_, AUTH_MEMORY | AUTH_SYSTEM | AUTH_TASKS);
                EXPECT_EQ(write_word(&word), -KOS_EPERM) << "the default composition's main";
                EXPECT_EQ(word, SEEN);
            }

            TEST_F(SysWitness, the_whole_word_passes_the_gate_and_an_unknown_op_is_einval)
            {
                uint8_t volatile word = SEEN;
                cap_seat_authority(caller_, CAP_AUTH_ALL);
                EXPECT_EQ(static_cast<int64_t>(syscall_dispatch(KOS_SYS_WITNESS_UACCESS, 99,
                                                                reinterpret_cast<uintptr_t>(&word),
                                                                0, 0)),
                          -KOS_EINVAL);
                EXPECT_EQ(write_word(&word), 0);
                EXPECT_EQ(word, 0xA5u) << "the host lets the plant land";
            }
#else
            TEST_F(SysWitness, a_kernel_built_without_the_witness_answers_an_unknown_number)
            {
                uint8_t volatile word = SEEN;
                cap_seat_authority(caller_, CAP_AUTH_ALL);
                EXPECT_EQ(write_word(&word), -KOS_EINVAL);
                EXPECT_EQ(word, SEEN);
            }
#endif
        }
    }
}
