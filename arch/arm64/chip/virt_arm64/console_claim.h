// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The own-image AMP console claim: one partition-shared word holds a grant's deadline in its
// high half and its owner in its low half, and a node writes the PL011 only under its own
// grant. Beside it, one partition-shared waiting word per node, which the release clears and
// rings. The unit including this header defines the primitives declared first.

#ifndef KICKOS_ARCH_ARM64_CHIP_VIRT_ARM64_CONSOLE_CLAIM_H
#define KICKOS_ARCH_ARM64_CHIP_VIRT_ARM64_CONSOLE_CLAIM_H

#include <kickos/arch/console_retry.h>
#include <kickos/config/limits.h>
#include <kickos/diag.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::virt_arm64::console
{
    // The partition-common counter, truncated to 32 bits.
    uint32_t ticks(void);
    uint64_t load(void);
    // One attempt, which may fail with the word unchanged; `expected` is left holding what
    // was read.
    bool compare_exchange(uint64_t& expected, uint64_t desired);
    // False once a bounded wait for TX FIFO room has run out.
    bool wait_room(void);
    void store(char c);
    uint32_t waiting(uint32_t node);
    void set_waiting(uint32_t node, uint32_t value);
    // A full store-to-load barrier.
    void fence(void);
    // Raises node's doorbell.
    void ring(uint32_t node);

    using kickos::console_retry::Put;

    class Claim
    {
    public:
        // `owner` is the node index + 1; `nodes` counts the partition's nodes.
        constexpr Claim(uint32_t owner, uint32_t nodes) : owner_(owner), nodes_(nodes) {}

        bool held(void) const { return token_ != 0; }

        void release(void)
        {
            uint64_t const token = token_;
            token_ = 0;
            if (token == 0)
            {
                return;
            }
            // Never release a peer's replacement of an expired grant. A grant left standing
            // makes every node wait out its deadline, so only a changed word ends the attempt.
            for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
            {
                uint64_t expected = token;
                if (compare_exchange(expected, 0))
                {
                    ring_waiting();
                    return;
                }
                if (expected != token)
                {
                    return;
                }
            }
        }

        // One masked attempt: drops a grant this node no longer holds, then takes a free or
        // ended one.
        bool open(uint32_t budget)
        {
            if (held() and not holding(budget))
            {
                release();
            }
            return held() or try_acquire(budget);
        }

        // A user writer the claim refused, masked until it parks: marks this node waiting, then
        // offers once. 0 once this node holds the claim, else the ticks to park for, which the
        // release of the grant that refused it cuts short by ringing this node.
        uint32_t wait(uint32_t budget)
        {
            set_waiting(owner_ - 1u, 1u);
            fence(); // pairs with the release's: either it reads this mark or this reads it free
            if (open(budget))
            {
                set_waiting(owner_ - 1u, 0);
                return 0;
            }
            uint32_t const rest = left(load(), budget);
            if (rest == 0)
            {
                return budget;
            }
            return rest;
        }

        // The holder stops a margin short of its deadline, so a byte checked just before its
        // store cannot land after a peer has taken the expired grant.
        bool holding(uint32_t budget) const
        {
            return token_ != 0 and load() == token_ and left(token_, budget) > budget / 64u;
        }

        bool try_acquire(uint32_t budget)
        {
            uint64_t expected = load();
            if (expected != 0 and left(expected, budget) != 0)
            {
                return false;
            }
            // A store-exclusive can fail with the word unchanged: only a changed word refuses.
            uint64_t const token = grant(budget);
            for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
            {
                uint64_t seen = expected;
                if (compare_exchange(seen, token))
                {
                    token_ = token;
                    kernel_bytes_ = 0;
                    return true;
                }
                if (seen != expected)
                {
                    return false;
                }
            }
            return false;
        }

        // A kernel line: renews an ended grant no peer took.
        size_t kernel_write(char const* buf, size_t n, uint32_t budget)
        {
            return kickos::console_retry::write(buf, n, nullptr, [this, budget](char c) {
                return put(c, budget, Take::TRY, Renew::KERNEL_LINE) == Put::STORED;
            });
        }

        // A syscall writer's chunk: stops at the first byte its grant no longer covers.
        size_t user_write(char const* buf, size_t n, bool* cr_pending, uint32_t budget)
        {
            return kickos::console_retry::write(buf, n, cr_pending, [this, budget](char c) {
                return put(c, budget, Take::TRY, Renew::NEVER) == Put::STORED;
            });
        }

        // Panic, fault and ISR output, already masked: waits for the claim. False once the
        // FIFO wedged.
        bool polled_write(char const* buf, size_t n, uint32_t budget)
        {
            return polled_.write(buf, n, [this, budget](char c) {
                return put(c, budget, Take::WAIT, Renew::KERNEL_LINE);
            });
        }

    private:
        enum class Take
        {
            TRY,
            WAIT, // the masked wait
        };

        enum class Renew
        {
            NEVER,
            KERNEL_LINE,
        };

        uint32_t owner_;
        uint32_t nodes_;
        uint64_t token_ = 0;
        uint32_t kernel_bytes_ = 0; // bytes a renewing writer put under this grant
        kickos::console_retry::PolledLine polled_;

        // One byte under the claim, checked after the FIFO wait: the grant can end while a
        // byte waits for room.
        Put put(char c, uint32_t budget, Take take, Renew renew)
        {
            if (not wait_room())
            {
                release();
                return Put::WEDGED;
            }
            bool const renewing = renew == Renew::KERNEL_LINE;
            if (not holding(budget) and not (renewing and renewed(budget)))
            {
                bool const lost = token_ != 0;
                release();
                if (lost)
                {
                    return Put::LOST;
                }
                bool held = false;
                if (take == Take::WAIT)
                {
                    held = acquire(budget);
                }
                else
                {
                    held = try_acquire(budget);
                }
                if (not held)
                {
                    return Put::LOST;
                }
            }
            store(c);
            if (renewing)
            {
                kernel_bytes_++;
            }
            if (c == '\n')
            {
                release();
            }
            return Put::STORED;
        }

        // A clear landing between a waiter's mark and its park is safe: the ring after it wakes
        // every waiter of that node, and each marks itself again.
        void ring_waiting(void)
        {
            fence();
            for (uint32_t node = 0; node < nodes_; node++)
            {
                if (waiting(node) != 0)
                {
                    set_waiting(node, 0);
                    ring(node);
                }
            }
        }

        bool acquire(uint32_t budget)
        {
            uint32_t const start = ticks();
            for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
            {
                if (try_acquire(budget))
                {
                    return true;
                }
                if (static_cast<uint32_t>(ticks() - start) >= budget)
                {
                    break;
                }
            }
            return false;
        }

        uint64_t grant(uint32_t budget) const
        {
            return (uint64_t(ticks() + budget) << 32) | owner_;
        }

        // Ticks left on a grant, 0 once it has ended. A live deadline lies at most one hold
        // ahead: a dead holder's truncated deadline would read as live again half a wrap
        // later under a signed comparison.
        static uint32_t left(uint64_t token, uint32_t budget)
        {
            uint32_t const remaining = static_cast<uint32_t>(token >> 32) - ticks();
            if (remaining > budget)
            {
                return 0;
            }
            return remaining;
        }

        // A renewal can start inside the margin, where no peer may take the grant yet, so it
        // is bounded by a kernel line's length or the writer could keep the UART for good. A
        // peer's steal is a compare-exchange of the same word, so exactly one of the two wins.
        bool renewed(uint32_t budget)
        {
            if (token_ == 0 or kernel_bytes_ >= KICKOS_DIAG_LINE_MAX)
            {
                return false;
            }
            uint64_t const fresh = grant(budget);
            for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
            {
                uint64_t expected = token_;
                if (compare_exchange(expected, fresh))
                {
                    token_ = fresh;
                    return true;
                }
                if (expected != token_)
                {
                    return false;
                }
            }
            return false;
        }
    };
}

#endif
