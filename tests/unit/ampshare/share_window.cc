// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A translating board's window over the partition's user share, handed by the task holding the
// share's reservation, over the real kernel/amp/ampshare.cc and kernel/mem/vrange.cc and an
// address-space seam this file sets.

#include <kickos/ampshare.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/vrange.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static_assert(KICKOS_AMP_SHARE, "this gate's posture states a share");
static_assert(KICKOS_HAVE_ASPACE, "this gate's posture states a translating board");

namespace
{
    constexpr size_t G = 4096u;
    constexpr uintptr_t SHARE = kickos::AMP_SHARE_BASE;
    constexpr size_t SHARE_SIZE = kickos::AMP_SHARE_SIZE;
    constexpr uint32_t PAGES = static_cast<uint32_t>(SHARE_SIZE / G);
    constexpr uintptr_t OWN = SHARE - 4u * G;

    // The type every live mapping of the share carries.
    uint8_t g_share_type = ARCH_MAP_NORMAL;
    bool g_nocache = true;

    // The holder's space, which the seat reserves the share in.
    kickos::VirtualRanges g_ranges;
    unsigned char g_domain;
}

extern "C"
{
    unsigned char __kickos_amp_shared_start[1];
    unsigned char __kickos_amp_shared_end[1];
    arch_irq_state_t arch_irq_save(void) { return 0; }
    void arch_irq_restore(arch_irq_state_t state) { (void)state; }
    void* kmemset(void* dst, int c, size_t n) { return memset(dst, c, n); }
    size_t arch_aspace_granule(void) { return G; }
    uintptr_t arch_aspace_user_offset(void) { return 0; }
    bool arch_aspace_memtype_support(enum arch_map_memtype type)
    {
        return type != ARCH_MAP_NOCACHE or g_nocache;
    }
}

namespace kickos
{
    Domain* task_domain(Task const* t)
    {
        if (t == nullptr)
        {
            return nullptr;
        }
        return reinterpret_cast<Domain*>(&g_domain);
    }

    VirtualRanges* domain_ranges_mut(Domain* d)
    {
        if (d == nullptr)
        {
            return nullptr;
        }
        return &g_ranges;
    }

    VirtualRanges const* domain_ranges(Domain const* d)
    {
        if (d == nullptr)
        {
            return nullptr;
        }
        return &g_ranges;
    }

    bool aspace_frames_type_ok(arch_phys_addr_t pa, size_t pages, uint8_t memtype,
                               VirtualRange const* self)
    {
        (void)self;
        bool const in_share = pa >= SHARE and pa + pages * G <= SHARE + SHARE_SIZE;
        return not in_share or memtype == g_share_type;
    }
}

namespace
{
    using kickos::amp_share_window;

    // The share's holder, seated by the kernel, and a reservation of its own beside the share.
    kickos::VirtualRanges const& holder()
    {
        static kickos::Task task;
        static kickos::Thread root;
        root.task = &task;
        EXPECT_TRUE(g_ranges.init(G));
        kickos::amp_share_seat(&root);
        EXPECT_TRUE(g_ranges.reserve(OWN, 1u));
        return g_ranges;
    }

    kos_window window(uintptr_t base, size_t size, uint8_t flags = 0)
    {
        return kos_window{base, static_cast<uint32_t>(size), KOS_WINDOW_MEMORY, flags};
    }

    TEST(AmpShareWindow, the_seat_reserves_the_whole_share)
    {
        kickos::VirtualRanges const& v = holder();
        kickos::VirtualRange const* const e = v.at_base(SHARE);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->pages, PAGES);
        EXPECT_NE(e->flags & kickos::VR_SHARE, 0u);
    }

    TEST(AmpShareWindow, a_whole_granule_part_of_the_share_is_admitted)
    {
        kickos::VirtualRanges const& v = holder();
        int rc = 1;
        EXPECT_TRUE(amp_share_window(&v, window(SHARE + G, G), &rc));
        EXPECT_EQ(rc, 0);
        rc = 1;
        EXPECT_TRUE(amp_share_window(&v, window(SHARE, SHARE_SIZE), &rc));
        EXPECT_EQ(rc, 0);
    }

    TEST(AmpShareWindow, one_byte_past_the_share_is_left_to_the_reservation_rules)
    {
        kickos::VirtualRanges const& v = holder();
        int rc = 1;
        EXPECT_FALSE(amp_share_window(&v, window(SHARE + SHARE_SIZE - G, G + 1u), &rc));
        EXPECT_FALSE(amp_share_window(&v, window(SHARE, SHARE_SIZE + 1u), &rc));
        EXPECT_FALSE(amp_share_window(&v, window(SHARE + G, SHARE_SIZE), &rc));
        EXPECT_EQ(rc, 1);
        // Which then refuse it: no reservation of the holder's starts there.
        EXPECT_EQ(v.at_base(SHARE + G), nullptr);
    }

    TEST(AmpShareWindow, a_part_granule_window_is_refused)
    {
        kickos::VirtualRanges const& v = holder();
        int rc = 0;
        EXPECT_TRUE(amp_share_window(&v, window(SHARE + G, G - 1u), &rc));
        EXPECT_EQ(rc, -KOS_EINVAL);
        rc = 0;
        EXPECT_TRUE(amp_share_window(&v, window(SHARE, G + 1u), &rc));
        EXPECT_EQ(rc, -KOS_EINVAL);
    }

    TEST(AmpShareWindow, the_other_memory_type_is_busy)
    {
        kickos::VirtualRanges const& v = holder();
        int rc = 0;
        EXPECT_TRUE(amp_share_window(&v, window(SHARE + G, G, KOS_WINDOW_UNCACHED), &rc));
        EXPECT_EQ(rc, -KOS_EBUSY);
        g_nocache = false;
        rc = 0;
        EXPECT_TRUE(amp_share_window(&v, window(SHARE + G, G, KOS_WINDOW_UNCACHED), &rc));
        EXPECT_EQ(rc, -KOS_ENOTSUP);
        g_nocache = true;
    }

    TEST(AmpShareWindow, a_space_holding_no_share_reservation_is_left_to_the_reservation_rules)
    {
        kickos::VirtualRanges const& v = holder();
        kickos::VirtualRanges stranger;
        ASSERT_TRUE(stranger.init(G));
        int rc = 1;
        EXPECT_FALSE(amp_share_window(&stranger, window(SHARE + G, G), &rc));
        EXPECT_FALSE(amp_share_window(nullptr, window(SHARE + G, G), &rc));
        EXPECT_FALSE(amp_share_window(&v, window(OWN, G), &rc));
        EXPECT_FALSE(amp_share_window(&v, window(SHARE + 4u, G), &rc));
        EXPECT_EQ(rc, 1);
    }
}
