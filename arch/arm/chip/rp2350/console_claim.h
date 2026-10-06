// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The own-image AMP console claim: a hardware spinlock serialises the two nodes, and a
// partition-shared owner and deadline bound how long a node keeps it. The unit including this
// header defines the primitives declared first.
//
// No writer renews an ended grant here: owner and deadline are two words beside the lock, so
// a renewal could not exclude a peer's steal already under way.
//
// The release tests the owner and then frees the lock, which is not one atomic act: a steal
// landing between them frees a claim one line early.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_CONSOLE_CLAIM_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_CONSOLE_CLAIM_H

#include <kickos/config/limits.h>
#include <kickos/sys/atomic.h>

#include <stdint.h>

namespace kickos::rp2350::console
{
    // TIMER0's raw low word, in microseconds.
    uint32_t now(void);
    // One read of the claim spinlock: true when it was free and is now held.
    bool lock_take(void);
    // Any write frees the spinlock, whichever core holds it.
    void lock_free(void);
    void fence(void);

    // 512 bytes at 115200 8N1 take 44.4 ms.
    constexpr uint32_t HOLD_MAX_US = 50000u;

    // Two byte times at 115200. The holder stops this far short of its deadline, so a byte
    // checked just before its store cannot land after a peer has taken the expired grant.
    constexpr uint32_t MARGIN_US = 200u;

    using Word = kickos::Atomic<uint32_t, kickos::Order::RELAXED>;

    class Claim
    {
    public:
        // `owner` is 0 when free, else the holder's node id + 1; `deadline` is in now()'s
        // microseconds. Both are partition-shared.
        constexpr Claim(uint32_t self, Word& owner, Word& deadline)
            : self_(self), owner_(owner), deadline_(deadline)
        {
        }

        bool held(void) const { return held_; }

        bool holding(void) const
        {
            return held_ and owner_ == self_ and left() > MARGIN_US;
        }

        // One byte's claim. A claim not yet held is waited for masked, by a caller already
        // masked: panic, fault and ISR output. A fault landing inside a line this node holds
        // writes into it rather than waiting out its own claim.
        bool claim(void)
        {
            if (held_)
            {
                if (holding())
                {
                    return true;
                }
                drop(true);
                return false;
            }
            // In TIMER0 microseconds like the grant it waits out: counted in iterations, the
            // wait would grow with a degraded core clock. KICKOS_POLL_SPIN_MAX is the backstop:
            // a TIMER0 that has stopped must still leave this node able to emit.
            uint32_t const waited_from = now();
            for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
            {
                if (try_take())
                {
                    return true;
                }
                if ((now() - waited_from) >= HOLD_MAX_US)
                {
                    break;
                }
            }
            return false;
        }

        // One attempt of the unmasked wait; the caller masks around it.
        bool try_open(void)
        {
            if (held_ and not holding())
            {
                drop(true);
            }
            return held_ or try_take();
        }

        void drop(bool ended_line)
        {
            if (not held_)
            {
                return;
            }
            if (not ended_line and left() != 0)
            {
                return;
            }
            held_ = false;
            if (owner_ != self_)
            {
                return;
            }
            owner_ = 0;
            fence(); // no peer may read this node as owner past the free
            lock_free();
        }

    private:
        uint32_t self_;
        Word& owner_;
        Word& deadline_;
        bool held_ = false;

        // Microseconds left on the published grant, 0 once it has ended. A live deadline lies
        // at most one hold ahead: a dead holder's deadline would read as live again half a
        // wrap later under a signed comparison.
        uint32_t left(void) const
        {
            uint32_t const remaining = deadline_ - now();
            if (remaining > HOLD_MAX_US)
            {
                return 0;
            }
            return remaining;
        }

        void taken(void)
        {
            held_ = true;
            deadline_ = now() + HOLD_MAX_US;
            fence(); // a peer reading this owner must see its deadline
            owner_ = self_;
        }

        bool try_take(void)
        {
            if (lock_take())
            {
                taken();
                return true;
            }
            if (owner_ != 0 and left() == 0)
            {
                lock_free();
                if (lock_take())
                {
                    taken();
                    return true;
                }
            }
            return false;
        }
    };
}

#endif
