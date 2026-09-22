// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// THE BADGE, over the real capability layer. Three claims, each with the arm that would pass
// without it:
//   1. cap_install_at SEATS the badge and never inherits it. A released slot keeps its last
//      occupant's spare bits, so an install that only wrote obj/type/rights would hand a
//      fresh CAP_NOTIFY the badge of whatever died in that slot.
//   2. A mint is refused from a BADGED source. A holder that could mint from its own badged
//      copy reaches every bit of the object, and the confinement is vacuous.
//   3. A badge travels with a DELEGATED copy. Left behind, the child's copy would be
//      unbadged and raise bit 0, which is a different bit of the same object.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO = 10;

    class CapBadge : public KSeam
    {
    };

    Thread* seat_holder(int slot)
    {
        Thread* const t = seat_pool(slot, PRIO);
        attach_caps(t, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule();
        }
        return t;
    }

    uint8_t badge_of(Thread* t, uint32_t cap)
    {
        IrqLock lock;
        CapEntry const* const e = cap_lookup(t, cap);
        if (e == nullptr)
        {
            return 0xFF;
        }
        return cap_badge(*e);
    }

    uint32_t pending_of(Thread* t, uint32_t cap)
    {
        IrqLock lock;
        CapEntry const* const e = cap_lookup(t, cap);
        Notification const* const n = kernel().notifies.resolve(e->obj);
        return n->pending;
    }
}

// A fresh capability is UNBADGED whatever the slot held before, and an unbadged one raises
// bit 0. Seat a badge in a slot, release it, and mint an ordinary object into the same slot:
// the free list releases to its TAIL, so the arm takes the whole table round rather than
// assuming which slot comes back.
TEST_F(CapBadge, a_released_slot_hands_no_badge_to_the_next_entry)
{
    Thread* const t = seat_holder(0);
    uint32_t src = KCAP_INVALID;
    ASSERT_EQ(notify_create(t, &src), 0);

    uint32_t badged = KCAP_INVALID;
    ASSERT_EQ(notify_badge(t, src, 7u, &badged), 0);
    ASSERT_EQ(badge_of(t, badged), 8u) << "fixture: the badge is stored as bit + 1";
    uint32_t const index = badged & KCAP_INDEX_MASK;

    {
        IrqLock lock;
        ASSERT_EQ(handle_close(t, badged), 0);
    }

    // Walk the free list round until that index comes back, minting an unbadged copy each
    // time. Bounded by the table width: cap_install refuses past it.
    uint32_t reused = KCAP_INVALID;
    bool seen = false;
    for (uint32_t i = 0; i < thread_cap_capacity(t) and not seen; i++)
    {
        uint32_t cap = KCAP_INVALID;
        if (notify_badge(t, src, 0u, &cap) != 0)
        {
            break;
        }
        if ((cap & KCAP_INDEX_MASK) == index)
        {
            reused = cap;
            seen = true;
        }
    }
    ASSERT_TRUE(seen) << "the released slot never came back, so nothing was reused here";
    EXPECT_EQ(badge_of(t, reused), 1u)
        << "the new entry inherited the slot's last badge instead of the one it was installed "
           "with";
}

// The mint is the ONLY place a badge is set, and it refuses a source already carrying one.
TEST_F(CapBadge, a_mint_from_a_badged_source_is_refused)
{
    Thread* const t = seat_holder(0);
    uint32_t src = KCAP_INVALID;
    ASSERT_EQ(notify_create(t, &src), 0);
    EXPECT_EQ(badge_of(t, src), KCAP_BADGE_NONE) << "a created capability is unconfined";

    uint32_t first = KCAP_INVALID;
    ASSERT_EQ(notify_badge(t, src, 3u, &first), 0);

    uint32_t second = KCAP_INVALID;
    EXPECT_EQ(notify_badge(t, first, 4u, &second), -KOS_EALREADY)
        << "a holder able to mint from its own badged copy reaches every bit of the object";
    EXPECT_EQ(second, KCAP_INVALID) << "a refused mint discloses no capability";

    // The converse, so the refusal above is not one that refuses everybody: the UNBADGED
    // source still mints, and on a second bit.
    uint32_t another = KCAP_INVALID;
    EXPECT_EQ(notify_badge(t, src, 4u, &another), 0);
    EXPECT_EQ(badge_of(t, another), 5u);
}

// A badge outside the word is refused rather than truncated into a bit the holder did not ask
// for.
TEST_F(CapBadge, a_bit_past_the_word_is_refused)
{
    Thread* const t = seat_holder(0);
    uint32_t src = KCAP_INVALID;
    ASSERT_EQ(notify_create(t, &src), 0);
    uint32_t out = KCAP_INVALID;
    EXPECT_EQ(notify_badge(t, src, KCAP_BADGE_BITS, &out), -KOS_EINVAL);
    EXPECT_EQ(notify_badge(t, src, 0xFFFFFFFFu, &out), -KOS_EINVAL);
    EXPECT_EQ(out, KCAP_INVALID);
    // And the last legal bit is legal.
    EXPECT_EQ(notify_badge(t, src, KCAP_BADGE_BITS - 1u, &out), 0);
}

// The badge is what a signal through that copy raises, and two copies on two bits raise two
// bits. Without the badge both would land on bit 0 and the waiter could not tell them apart.
TEST_F(CapBadge, two_badged_copies_raise_two_different_bits)
{
    Thread* const t = seat_holder(0);
    uint32_t src = KCAP_INVALID;
    ASSERT_EQ(notify_create(t, &src), 0);
    uint32_t a = KCAP_INVALID;
    uint32_t b = KCAP_INVALID;
    ASSERT_EQ(notify_badge(t, src, 5u, &a), 0);
    ASSERT_EQ(notify_badge(t, src, 9u, &b), 0);

    EXPECT_EQ(notify_signal(t, a), 0);
    EXPECT_EQ(pending_of(t, src), 1u << 5);
    EXPECT_EQ(notify_signal(t, b), 0);
    EXPECT_EQ(pending_of(t, src), (1u << 5) | (1u << 9));
    // A second raise of a bit already set had no effect, and says so.
    EXPECT_EQ(notify_signal(t, a), -KOS_EALREADY);
    EXPECT_EQ(pending_of(t, src), (1u << 5) | (1u << 9));
    // The UNBADGED source is the unconfined name, and it raises bit 0.
    EXPECT_EQ(notify_signal(t, src), 0);
    EXPECT_EQ(pending_of(t, src), (1u << 5) | (1u << 9) | 1u);
}

// The badge travels with a copy installed into another table, which is what a spawn's
// delegation does.
TEST_F(CapBadge, a_delegated_copy_carries_the_badge)
{
    Thread* const t = seat_holder(0);
    Thread* const child = seat_holder(1);
    uint32_t src = KCAP_INVALID;
    ASSERT_EQ(notify_create(t, &src), 0);
    uint32_t badged = KCAP_INVALID;
    ASSERT_EQ(notify_badge(t, src, 11u, &badged), 0);

    int obj = -1;
    uint8_t rights = 0;
    uint8_t carried = 0;
    {
        IrqLock lock;
        CapEntry const* const e = cap_lookup(t, badged);
        ASSERT_NE(e, nullptr);
        obj = e->obj;
        rights = e->rights;
        carried = cap_badge(*e);
        ASSERT_TRUE(obj_ref_inc(CapType::CAP_NOTIFY, obj, rights));
        ASSERT_NE(cap_install_at(child, KICKOS_CAP_FIRST_DYNAMIC, obj, CapType::CAP_NOTIFY,
                                 rights, carried),
                  nullptr);
    }
    uint32_t const childs = (static_cast<uint32_t>(
                                 cap_slot(child->caps, KICKOS_CAP_FIRST_DYNAMIC)->gen)
                             << KCAP_INDEX_BITS)
                            | KICKOS_CAP_FIRST_DYNAMIC;
    EXPECT_EQ(badge_of(child, childs), 12u) << "the copy landed unbadged and would raise bit 0";
    EXPECT_EQ(notify_signal(child, childs), 0);
    EXPECT_EQ(pending_of(t, src), 1u << 11);
}
