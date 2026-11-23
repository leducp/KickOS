// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// See serror_armv8a.h.

#include "serror_armv8a.h"

#include "vector_slots.h"

namespace kickos
{
    namespace armv8a
    {
        bool serror_containable(uint64_t esr, uint64_t pfr0)
        {
            if (((esr >> 26) & 0x3Fu) != KICKOS_ARMV8A_EC_SERROR)
            {
                return false;
            }
            if (((pfr0 >> 28) & 0xFu) == 0)
            {
                return true;
            }
            bool const ids = (esr & (1ull << 24)) != 0;
            bool const async = (esr & 0x3Fu) == 0x11u;
            if (ids or not async)
            {
                return false;
            }
            // UEU, UEO, UER and CE; UC and the reserved encodings are refused.
            uint64_t const aet = (esr >> 10) & 0x7u;
            return aet == 1u or aet == 2u or aet == 3u or aet == 6u;
        }
    }
}
