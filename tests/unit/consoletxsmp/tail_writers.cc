// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console ring's tail against a drain on ANOTHER CORE. A producer moves tail itself: the
// insert primes an idle channel under IrqLock, and console_tx_flush_sync drains under it. At
// one kernel core that mask is what keeps the drain ISR out of the window between a producer's
// read of tail and its write; above one core it reaches only the core that took it, and the
// drain runs on a core it never masked.
//
// EACH ARM IS A RENDEZVOUS, NOT A STRESS LOOP. Core zero releases the drain from inside its
// own push to the TX edge, which is the instant it has published head and read tail but not
// yet written it, and then waits for whichever of the two postures it is in: a drain that
// takes the lock is WAITING on the one this core holds, and a drain that does not has RUN TO
// COMPLETION. Both readings are reachable, so no arm here depends on host scheduling.

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>

#include <kickos/console_tx.h>

#include "tx_smp_seam.h"

namespace
{
    constexpr uint32_t kRing = 64u;
    char const kLine[] = "abcdefgh";
    constexpr size_t kLineLen = 8u;

    std::atomic<bool> g_drain_go{false};
    std::atomic<bool> g_drain_done{false};
    std::atomic<bool> g_budget_blew{false};

    // Core one, on its own thread: the TX-empty drain and nothing else.
    void remote_drain(void)
    {
        consoletxsmp::g_core = 1;
        bool const armed = consoletxsmp::wait_for([] { return g_drain_go.load(); });
        if (not armed)
        {
            g_budget_blew.store(true);
            return;
        }
        console_tx_isr();
        g_drain_done.store(true);
    }

    // Seated in core zero's own push, with the ring's indices half written.
    void release_the_drain(void)
    {
        g_drain_go.store(true);
        bool const met = consoletxsmp::wait_for(
            [] { return consoletxsmp::lock_contended() or g_drain_done.load(); });
        if (not met)
        {
            g_budget_blew.store(true);
        }
    }

    class ConsoleTxTailWriters : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            consoletxsmp::g_core = 0;
            consoletxsmp::reset(kRing);
            g_drain_go.store(false);
            g_drain_done.store(false);
            g_budget_blew.store(false);
        }

        // Every arm's own controls: the hand-off happened, the peer really ran, and it really
        // had to wait. An arm that skips these reads green on a drain that never started.
        void expect_the_rendezvous_happened()
        {
            EXPECT_FALSE(g_budget_blew.load()) << "a hand-off blew its budget, so what the "
                                                  "wire shows below is not this interleaving";
            EXPECT_TRUE(consoletxsmp::hold_fired()) << "core zero never reached the push the "
                                                       "drain is released from";
            EXPECT_TRUE(consoletxsmp::lock_contended())
                << "the drain never waited for the lock core zero held, so nothing here "
                   "witnesses exclusion";
        }
    };
}

// The insert primes the channel: it reads tail, pushes that byte at the device and writes
// tail back. A drain entered on another core inside that window samples the same tail, drains
// the whole ring behind it and publishes head, and the insert's write then walks tail back to
// one. Everything from the second byte on is transmitted a second time.
TEST_F(ConsoleTxTailWriters, RemoteDrainDoesNotResendWhatTheInsertPrimed)
{
    consoletxsmp::hold_at_push(0u, release_the_drain);
    std::thread peer(remote_drain);

    EXPECT_EQ(console_tx_insert_line(kLine, kLineLen, 0), 1);
    peer.join();
    console_tx_flush_sync(); // whatever the ring still holds, so nothing is merely stranded

    expect_the_rendezvous_happened();
    EXPECT_GT(consoletxsmp::pushes_by(1u), 0u) << "the drain pushed nothing, so this arm read "
                                                  "a wire only core zero wrote";
    EXPECT_EQ(consoletxsmp::wire(), std::string(kLine, kLineLen));
}

// The same interleaving, read at the mechanism rather than at the wire: a push made by a core
// that is not the one holding the kernel lock is two writers of the ring's indices overlapping.
TEST_F(ConsoleTxTailWriters, RemoteDrainTouchesTheDeviceOnlyUnderTheLock)
{
    consoletxsmp::hold_at_push(0u, release_the_drain);
    std::thread peer(remote_drain);

    EXPECT_EQ(console_tx_insert_line(kLine, kLineLen, 0), 1);
    peer.join();

    expect_the_rendezvous_happened();
    EXPECT_FALSE(consoletxsmp::push_outside_the_lock())
        << "a core pushed while another held the kernel lock";
}

// console_tx_flush_sync is the other producer-side writer of tail, and the panic path's. It
// disables the TX interrupt AT THE PERIPHERAL, which says nothing about a drain already
// entered on another core.
TEST_F(ConsoleTxTailWriters, RemoteDrainDoesNotResendWhatASyncFlushIsDraining)
{
    ASSERT_EQ(console_tx_insert_line(kLine, kLineLen, 0), 1);
    ASSERT_EQ(consoletxsmp::wire().size(), 1u); // the prime alone; the rest sits in the ring

    consoletxsmp::hold_at_push(0u, release_the_drain);
    std::thread peer(remote_drain);

    console_tx_flush_sync();
    peer.join();

    expect_the_rendezvous_happened();
    EXPECT_EQ(consoletxsmp::wire(), std::string(kLine, kLineLen));
}
