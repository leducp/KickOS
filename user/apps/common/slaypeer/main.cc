// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A SLAY WHOSE VICTIM IS RUNNING ON A PEER CORE AND MAKES NO SYSCALL.
//
// The claim on a slain thread is a switch INTO it: switch_book rebuilds its context into the
// slay stub before the swap. So a victim that is its own core's `current` is claimed only by a
// pass that takes the core off it, and nothing the slayer's core does reaches it.
//
// The shape the hazard needs is exactly this one and no other: the victim PINNED to a peer
// core, alone at the top of that core's ready set, and spinning at the unprivileged level with
// no syscall, no yield and no timed wait. Every other shape hides the bug: a victim merely
// READY is claimed by the switch that starts it, and a victim sharing a core with a runnable
// peer is claimed by the switch that peer takes anyway.
//
// WHAT THIS IMAGE ASSERTS is that kos_thread_slay RETURNS 0 within a bound. On a tree whose
// available_to admits a slain current, the victim's core re-picks it on every pass forever and
// the call answers -KOS_ETIMEDOUT, which is the control: the bound is what turns a hang into a
// verdict.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/emit.h>

using kickos::emit;

namespace
{
    // The victim outranks its slayer, so nothing about the outcome can be read as the slayer
    // simply preempting it.
    constexpr uint8_t PRIO_SLAYER = 8;
    constexpr uint8_t PRIO_VICTIM = 12;

    constexpr uint32_t CORE_SLAYER = 0;
    constexpr uint32_t CORE_VICTIM = 1;

    // Long enough for a doorbell round trip on an emulated four-core machine, short enough
    // that the unfixed kernel's answer arrives as a FAIL line rather than the gate's timeout.
    constexpr uint32_t SLAY_TIMEOUT_US = 2u * 1000u * 1000u;

    // Long enough for the victim to be seated and RUNNING on its own core before the slay: it
    // is spawned READY, and a claim on a READY thread is the case this arm is not about.
    constexpr uint64_t SETTLE_NS = 200u * 1000u * 1000u;

    // Written by the slayer, read by root after it joins, so no publication is owed beyond the
    // join itself.
    int g_slay_rc = -1;

    // volatile, or the body is a loop with no side effect and the compiler may discard it
    // along with the forward progress this arm depends on the thread NOT making.
    volatile uint32_t g_spin = 0;

    void victim(void*)
    {
        while (true)
        {
            g_spin = g_spin + 1u;
        }
    }

    void slayer(void*)
    {
        auto const v = kos::thread::create(victim, nullptr, "victim", PRIO_VICTIM,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                           nullptr, 0, nullptr, 0, nullptr, 0,
                                           nullptr, 0, 0, nullptr, KOS_TASK_NONE,
                                           1u << CORE_VICTIM);
        if (not v.valid())
        {
            emit("[slaypeer] ERROR: victim spawn refused\n");
            kos_exit(1);
        }
        kos_sleep_ns(SETTLE_NS);
        g_slay_rc = kos_thread_slay(v.id(), SLAY_TIMEOUT_US);
        kos_exit(0);
    }
}

int main(int, char**)
{
    // The SLAYER is what spawns the victim: kos_thread_slay admits the spawner alone.
    auto const s = kos::thread::create(slayer, nullptr, "slayer", PRIO_SLAYER,
                                       KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                       nullptr, 0, nullptr, 0, nullptr, 0,
                                       nullptr, 0, 0, nullptr, KOS_TASK_NONE,
                                       1u << CORE_SLAYER);
    if (not s.valid())
    {
        emit("[slaypeer] ERROR: slayer spawn refused\n");
        return 1;
    }
    // Unbounded, the slay carrying the only bound this run needs: a second one here would
    // report the gate's patience instead of the kernel's answer.
    if (s.join(KOS_TIMEOUT_NONE) != 0)
    {
        emit("[slaypeer] ERROR: the slayer never came back\n");
        return 1;
    }

    char msg[96];
    ksnprintf(msg, sizeof(msg), "[slaypeer] slay rc: %d\n", g_slay_rc);
    emit(msg);
    if (g_slay_rc != 0)
    {
        emit("[slaypeer] SLAYPEER FAIL: the victim kept the core it was slain on\n");
        return 1;
    }
    emit("[slaypeer] SLAYPEER PASS\n");
    return 0;
}
