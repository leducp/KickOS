// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A word in the partition's user share, refused on a translating board by every route a space
// maps the share: root's own share range, a window over part of it, and a handoff of it. Only
// root's range carries VR_SHARE; the other two are plain borrowed mappings of the same frames.
// Over the REAL kernel/syscall/word.cc, its write predicate in kernel/syscall/syscall_mem.cc,
// kernel/amp/ampshare.cc's seat and kernel/mem/vrange.cc, and an address-space seam this file
// sets.

#include <kickos/ampshare.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/irqlock.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/vrange.h>
#include <kickos/word.h>

#include <kickos/sys/errno.h>

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
    // Root's own pages right below and right above the share.
    constexpr uintptr_t BELOW = SHARE - G;
    constexpr uintptr_t ABOVE = SHARE + SHARE_SIZE;
    constexpr uintptr_t WINDOW = 0x400000000000ull;
    constexpr uint32_t RW = ARCH_MAP_R | ARCH_MAP_W;
    constexpr uintptr_t AT = 8u;

    enum Who : size_t
    {
        ROOT,
        WINDOWED,
        BORROWER,
        WHO_COUNT
    };

    unsigned char g_spaces[WHO_COUNT];
    kickos::Domain g_domains[WHO_COUNT];
    kickos::Task g_tasks[WHO_COUNT];
    kickos::Thread g_threads[WHO_COUNT];

    struct arch_aspace* space_of(size_t who)
    {
        return reinterpret_cast<struct arch_aspace*>(&g_spaces[who]);
    }

    // Where each space maps its one mapping of the share: a page of it, or the whole of it.
    struct Leaf
    {
        struct arch_aspace const* space;
        uintptr_t va;
        size_t pages;
        arch_phys_addr_t pa;
    };
    Leaf g_leaves[WHO_COUNT + 2];
    size_t g_leaf_count = 0;
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
    bool arch_user_data_writable(uintptr_t, size_t) { return false; }

    arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)
    {
        for (size_t i = 0; i < g_leaf_count; i++)
        {
            Leaf const& l = g_leaves[i];
            if (l.space == space and va >= l.va and va < l.va + l.pages * G)
            {
                return l.pa + ((va - l.va) & ~static_cast<uintptr_t>(G - 1u));
            }
        }
        return 0;
    }

    // Names the frame and reaches nothing: the gate asks only what word_ref answers.
    void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va, bool* uncached,
                              arch_phys_addr_t* frame)
    {
        if (uncached != nullptr)
        {
            *uncached = false;
        }
        if (frame != nullptr)
        {
            *frame = arch_aspace_frame_at(space, va);
        }
        return nullptr;
    }

    void arch_aspace_release(struct arch_aspace*, uintptr_t)
    {
    }
}

namespace kickos
{
    Domain* task_domain(Task const* t)
    {
        for (size_t i = 0; i < WHO_COUNT; i++)
        {
            if (t == &g_tasks[i])
            {
                return &g_domains[i];
            }
        }
        return nullptr;
    }

    struct arch_aspace* domain_space(Domain const* d)
    {
        if (d == nullptr)
        {
            return nullptr;
        }
        return d->space;
    }

    VirtualRanges const* domain_ranges(Domain const* d)
    {
        if (d == nullptr)
        {
            return nullptr;
        }
        return &d->ranges;
    }

    VirtualRanges* domain_ranges_mut(Domain* d)
    {
        if (d == nullptr)
        {
            return nullptr;
        }
        return &d->ranges;
    }
}

namespace
{
    void map(size_t who, uintptr_t va, size_t pages, arch_phys_addr_t pa, uint8_t flags)
    {
        kickos::VirtualRanges& r = g_domains[who].ranges;
        ASSERT_TRUE(r.reserve(va, pages, flags));
        ASSERT_TRUE(r.grant(va, pages, RW, static_cast<uint8_t>(ARCH_MAP_NORMAL)));
        g_leaves[g_leaf_count] = Leaf{space_of(who), va, pages, pa};
        g_leaf_count++;
    }

    class AmpShareWord : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_leaf_count = 0;
            for (size_t i = 0; i < WHO_COUNT; i++)
            {
                g_threads[i].task = &g_tasks[i];
                g_domains[i].space = space_of(i);
                ASSERT_TRUE(g_domains[i].ranges.init(G));
            }
            // Root's share range as the seat reserves it and root's self-grant maps it.
            kickos::amp_share_seat(&g_threads[ROOT]);
            ASSERT_TRUE(g_domains[ROOT].ranges.grant(SHARE, PAGES, RW,
                                                     static_cast<uint8_t>(ARCH_MAP_NORMAL)));
            g_leaves[g_leaf_count] = Leaf{space_of(ROOT), SHARE, PAGES, SHARE};
            g_leaf_count++;
            map(ROOT, BELOW, 1, BELOW, 0);
            map(ROOT, ABOVE, 1, ABOVE, 0);
            // A window over the share's second page, where the kernel placed it.
            map(WINDOWED, WINDOW, 1, SHARE + G, kickos::VR_BORROWED | kickos::VR_WINDOW);
            // The share handed to a task, at its own address.
            map(BORROWER, SHARE, PAGES, SHARE, kickos::VR_BORROWED);
        }

        static int ref(size_t who, uintptr_t va)
        {
            kickos::WordRef w;
            kickos::IrqLock lock;
            return kickos::word_ref(&g_threads[who], va, &w);
        }
    };

    TEST_F(AmpShareWord, root_s_own_share_range_is_refused)
    {
        EXPECT_EQ(ref(ROOT, SHARE), -KOS_EINVAL) << "the share's first word";
        EXPECT_EQ(ref(ROOT, SHARE + AT), -KOS_EINVAL);
        EXPECT_EQ(ref(ROOT, SHARE + SHARE_SIZE - sizeof(uint32_t)), -KOS_EINVAL)
            << "the share's last word";
    }

    TEST_F(AmpShareWord, a_window_over_part_of_the_share_is_refused)
    {
        EXPECT_EQ(ref(WINDOWED, WINDOW + AT), -KOS_EINVAL)
            << "a window carries no share flag, and its frames are the share's";
    }

    TEST_F(AmpShareWord, a_handoff_of_the_share_is_refused)
    {
        EXPECT_EQ(ref(BORROWER, SHARE), -KOS_EINVAL) << "the share's first word";
        EXPECT_EQ(ref(BORROWER, SHARE + G + AT), -KOS_EINVAL);
        EXPECT_EQ(ref(BORROWER, SHARE + SHARE_SIZE - sizeof(uint32_t)), -KOS_EINVAL)
            << "the share's last word";
    }

    TEST_F(AmpShareWord, the_words_on_either_side_of_the_share_are_named)
    {
        EXPECT_EQ(ref(ROOT, SHARE - sizeof(uint32_t)), 0) << "the word below the share";
        EXPECT_EQ(ref(ROOT, ABOVE), 0) << "the word above the share";
    }
}
