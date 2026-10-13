// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A terminal refusal on a core holding the kernel lock outside an IrqLock, as the doorbell's boot
// check does, over the REAL kernel/init/console.cc and console_tx.cc at two kernel cores: the
// fixture refuses a second acquisition, which on the machine spins on this core's own ticket.

#include <string>

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include "dark_seam.h"
#include "isolated_arm.h"
#include "kseam_test.h"

using namespace kickos::testfix;

namespace
{
    constexpr uint32_t kRing = 64u;
    char g_storage[kRing];
    bool g_slot_free = false;
    uint32_t g_pushes = 0;
    bool g_published_during_flush = false;
    bool g_refuse_on_core1 = false;

    int slot_free(void)
    {
        return static_cast<int>(g_slot_free);
    }

    void push(uint8_t b)
    {
        if (console_handover_refused() != 0)
        {
            g_published_during_flush = true;
        }
        g_pushes = g_pushes + 1;
        darkseam::g_wire.push_back(static_cast<char>(b));
        if (g_refuse_on_core1)
        {
            g_refuse_on_core1 = false;
            g_core = 1u;
            kpanic_enter_unlocked();
            g_core = 0u;
        }
    }

    void gate(void)
    {
    }

    console_tx_backend const kBackend = {slot_free, push, gate, gate, nullptr};

    // A line queued and not yet sent, the device then free to take bytes.
    void stage(void)
    {
        darkseam::reset();
        g_slot_free = false;
        g_pushes = 0;
        g_published_during_flush = false;
        g_refuse_on_core1 = false;
        console_tx_init(&kBackend, g_storage, kRing, 3);
        ASSERT_EQ(console_tx_insert_line("queued\n", 7, 0), 7);
        g_slot_free = true;
    }

    class ConsolePanicUnlocked : public KSeam
    {
    };
}

// The refusal's own line reaches the device, the ring is left unread, and no lock is taken.
TEST_F(ConsolePanicUnlocked, ARefusalUnderTheBareLockWritesItsLineAndTakesNoLock)
{
    run_isolated([]() {
        stage();
        arch_kernel_lock();
        kpanic_enter_unlocked();
        kpanic_write_unlocked("refused\n", 8);
        EXPECT_EQ(darkseam::g_wire, "refused\n");
        EXPECT_EQ(g_pushes, 0u) << "the refusal read the ring";
        EXPECT_EQ(darkseam::g_reclaims, 1u);
        EXPECT_EQ(console_tx_armed(), 0);
    });
}

// A real panic after it, on the core still holding the lock bare: its entry and its banner take
// no lock.
TEST_F(ConsolePanicUnlocked, ALaterPanicsBannerIsWrittenWithoutTheLock)
{
    run_isolated([]() {
        stage();
        arch_kernel_lock();
        kpanic_enter_unlocked();
        kpanic_write_unlocked("refused\n", 8);
        kpanic_enter();
        kickos::kputs("KERNEL PANIC: later\n");
        EXPECT_EQ(darkseam::g_wire, "refused\nKERNEL PANIC: later\n");
        EXPECT_EQ(darkseam::g_reclaims, 1u);
    });
}

// The ordinary panic: its flush runs before PANICKED, so another core's line waits on the lock
// rather than taking the polled path under the flush.
TEST_F(ConsolePanicUnlocked, AnOrdinaryPanicStoresPanickedOnlyAfterItsFlush)
{
    run_isolated([]() {
        stage();
        kpanic_enter();
        EXPECT_EQ(darkseam::g_wire, "queued\n");
        EXPECT_FALSE(g_published_during_flush) << "PANICKED was stored before the ring was out";
        EXPECT_NE(console_handover_refused(), 0);
    });
}

// Core 1 refuses while core 0's panic is flushing under the lock: core 0 claimed first, so core 1
// does not reclaim again.
TEST_F(ConsolePanicUnlocked, ARefusalDuringAPanicsFlushDoesNotReclaimAgain)
{
    run_isolated([]() {
        stage();
        g_refuse_on_core1 = true;
        kpanic_enter();
        EXPECT_FALSE(g_refuse_on_core1) << "the flush pushed nothing; the arm staged no race";
        EXPECT_EQ(darkseam::g_reclaims, 1u) << "a refusal racing the panic reclaimed again";
    });
}
