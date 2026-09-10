// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two whole-table operations a page-table backend spells the same way whatever its entry
// encoding: every format here spells an absent entry as a zero word, and neither of these
// reads anything else. The entry count is the backend's own, so no granule, level width or
// descriptor bit crosses this header.

#ifndef KICKOS_ARCH_ASPACE_TABLE_H
#define KICKOS_ARCH_ASPACE_TABLE_H

#include <stddef.h>
#include <stdint.h>

namespace kickos::aspace
{
    inline void zero_table(uint64_t* table, size_t entries)
    {
        for (size_t i = 0; i < entries; i++)
        {
            table[i] = 0;
        }
    }

    inline bool table_empty(uint64_t const* table, size_t entries)
    {
        for (size_t i = 0; i < entries; i++)
        {
            if (table[i] != 0)
            {
                return false;
            }
        }
        return true;
    }
}

#endif
