// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kickos_thread_fault_exit prints for the record its fault left: a PC line when the frame
// was trusted, `PC unknown` when it was not.

#include <kickos/arch/arch.h>
#include <kickos/diag.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <string.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class FaultRecordPc : public KSeam
            {
            };

            constexpr int SLOT_VICTIM = 0;
            constexpr int SLOT_PEER = 1;
            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_HIGH = 6;

            void seat_victim()
            {
                Thread* const peer = seat_pool(SLOT_PEER, PRIO_LOW);
                Thread* const victim = seat_pool(SLOT_VICTIM, PRIO_HIGH);
                {
                    IrqLock lock;
                    sched::reschedule(nullptr, lock);
                }
                EXPECT_NE(peer, nullptr);
                EXPECT_EQ(kernel().current(kickos_kernel_core()), victim)
                    << "fixture: the victim is current";
                g_fault_fmts = 0;
            }

            void fault_exit()
            {
                kickos_thread_fault_exit();
            }

            bool printed(char const* fmt)
            {
                for (uint32_t i = 0; i < g_fault_fmts; i++)
                {
                    if (strcmp(g_fault_fmt[i], fmt) == 0)
                    {
                        return true;
                    }
                }
                return false;
            }

            TEST_F(FaultRecordPc, a_fault_that_stacked_nothing_prints_no_pc)
            {
                seat_victim();
                kickos_fault_record_no_pc("CFSR", 0x100000u, 0, 0);
                run_noreturn(fault_exit);
                EXPECT_TRUE(printed(KDIAG_F_FAULT_NO_PC));
                EXPECT_FALSE(printed(KDIAG_F_FAULT_PC_STAT));
            }

            // An asynchronous error's return address is where the thread was when it arrived,
            // not the access that raised it.
            TEST_F(FaultRecordPc, an_asynchronous_error_prints_its_pc_as_unverified)
            {
                seat_victim();
                kickos_fault_record_unverified_pc("ESR_EL1", 0xBE000000u, 0x1234u);
                run_noreturn(fault_exit);
                EXPECT_TRUE(printed(KDIAG_F_FAULT_PC_UNVERIFIED));
                EXPECT_FALSE(printed(KDIAG_F_FAULT_PC_STAT));
            }

            TEST_F(FaultRecordPc, a_fault_with_no_status_and_no_frame_prints_a_bare_no_pc)
            {
                seat_victim();
                kickos_fault_record_no_pc(nullptr, 0, 0, 0);
                run_noreturn(fault_exit);
                EXPECT_TRUE(printed(KDIAG_F_FAULT_NO_PC_BARE));
                EXPECT_FALSE(printed(KDIAG_F_FAULT_NO_PC));
                EXPECT_FALSE(printed(KDIAG_F_FAULT_PC));
            }

            TEST_F(FaultRecordPc, a_dropped_svcall_is_named_in_its_record)
            {
                seat_victim();
                kickos_fault_record("CFSR", 0x400u, 0x1234u, 0, 0);
                kickos_fault_record_svc_dropped();
                run_noreturn(fault_exit);
                EXPECT_TRUE(printed(KDIAG_F_FAULT_SVC_DROPPED));
            }

            TEST_F(FaultRecordPc, a_fresh_record_drops_no_svcall)
            {
                seat_victim();
                kickos_fault_record_svc_dropped();
                kickos_fault_record("CFSR", 0x400u, 0x1234u, 0, 0);
                run_noreturn(fault_exit);
                EXPECT_FALSE(printed(KDIAG_F_FAULT_SVC_DROPPED));
            }

            TEST_F(FaultRecordPc, an_address_a_bus_unit_latched_names_that_unit)
            {
                seat_victim();
                kickos_fault_record_no_pc("CFSR", 0x1400u, 0x2002ff70u, 1);
                kickos_fault_record_addr_unit("SYSMPU");
                run_noreturn(fault_exit);
                EXPECT_TRUE(printed(KDIAG_F_FAULT_ADDR));
                EXPECT_TRUE(printed(KDIAG_F_FAULT_ADDR_UNIT));
            }

            TEST_F(FaultRecordPc, a_fresh_record_names_no_unit)
            {
                seat_victim();
                kickos_fault_record_addr_unit("SYSMPU");
                kickos_fault_record("CFSR", 0x8200u, 0x1234u, 0x4002c000u, 1);
                run_noreturn(fault_exit);
                EXPECT_TRUE(printed(KDIAG_F_FAULT_ADDR));
                EXPECT_FALSE(printed(KDIAG_F_FAULT_ADDR_UNIT));
            }

            TEST_F(FaultRecordPc, a_fault_with_a_frame_prints_its_pc)
            {
                seat_victim();
                kickos_fault_record("CFSR", 0x10000u, 0x1234u, 0, 0);
                run_noreturn(fault_exit);
                EXPECT_TRUE(printed(KDIAG_F_FAULT_PC_STAT));
                EXPECT_FALSE(printed(KDIAG_F_FAULT_NO_PC));
            }
        }
    }
}
