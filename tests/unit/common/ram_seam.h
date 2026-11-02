// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/mem/ramown.cc asks of a task, for a gate whose tasks are fakes of its own: each
// fake's chain of RAM owner records, per instance, and an admission that takes every block, the
// budget being the task module's and not the table's. Included by one translation unit of the
// gate.

#ifndef KICKOS_TESTS_UNIT_COMMON_RAM_SEAM_H
#define KICKOS_TESTS_UNIT_COMMON_RAM_SEAM_H

#include <kickos/budget.h>
#include <kickos/instance_local.h>
#include <kickos/task.h>

#include <stdint.h>

namespace kickos
{
    uint16_t* task_ram_head(Task const* t)
    {
        constexpr int FAKES = 8;
        static Task const* seen[FAKES] = {};
        static uint16_t heads[KICKOS_MAX_INSTANCES][FAKES] = {};
        for (int i = 0; i < FAKES; i++)
        {
            if (seen[i] == nullptr)
            {
                seen[i] = t;
            }
            if (seen[i] == t)
            {
                return &heads[kickos_instance_index()][i];
            }
        }
        return nullptr;
    }

    bool task_admit(Task const*, BudgetKind, uint32_t)
    {
        return true;
    }
}

#endif
