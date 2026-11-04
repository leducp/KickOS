// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/slotpool.h>

namespace kickos
{
    // A never-used slot first, then the one freed longest ago. O(1) past the admission.
    int slot_claim(SlotPoolState& st, SlotArrays const* a, BudgetKind kind, Task const* charged)
    {
        if (not task_admit(charged, kind, 1))
        {
            return SLOT_OVER_BUDGET;
        }
        if (st.fresh < a->count)
        {
            int const index = st.fresh;
            st.fresh++;
            return index;
        }
        ListNode* const n = st.free.pop_front();
        if (n == nullptr)
        {
            return -1;
        }
        return static_cast<int>(n - pool_local(a->nodes));
    }

    namespace
    {
        // Whether slot `index` is claimed and not released since. `fresh` never passes the
        // slot count, so this is also the bound. O(1).
        bool slot_live(SlotPoolState const& st, SlotArrays const* a, uint32_t index)
        {
            return index < st.fresh and not st.free.holds(&pool_local(a->nodes)[index]);
        }
    }

    // O(1).
    void slot_release(SlotPoolState& st, SlotArrays const* a, int handle)
    {
        uint32_t const index = handle_index(static_cast<uint32_t>(handle));
        if (not slot_live(st, a, index))
        {
            return;
        }
        pool_local(a->gen)[index]++;
        st.free.push_back(&pool_local(a->nodes)[index]);
    }

    // O(1).
    int slot_live_index(SlotPoolState const& st, SlotArrays const* a, int handle)
    {
        uint32_t const u = static_cast<uint32_t>(handle);
        uint32_t const index = handle_index(u);
        if (not slot_live(st, a, index)
            or static_cast<uint16_t>(pool_local(a->gen)[index]) != handle_gen(u))
        {
            return -1;
        }
        return static_cast<int>(index);
    }
}
