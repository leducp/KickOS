// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host-sim service-list provider: the posture the silicon boards ship, where a userspace
// driver owns the console and the kernel chip path is dark.
//
// The driver thread write(2)s to host fd 1, which reaches the console WITHOUT going
// through kconsole_write, so bytes appear if and only if they travelled the endpoint ->
// driver route. Sim-only by construction (host libc), gated to KICKOS_ARCH == sim in
// system/CMakeLists.txt.
//
// Selected with -DKICKOS_SERVICE_LIST=kickos_services_sim; the sim default stays
// kickos_services_none so the ordinary sim gate keeps testing the pre-publish route.

#include <kickos/sys/service.h>

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/driver_bringup.h>
#include <kickos/sys/errno.h>

#include <stdint.h>

// The host write(2), declared rather than included: this TU is built freestanding
// (kickos_apply_freestanding) and must not pull host headers. fd 1 is "the wire".
extern "C" long write(int, void const*, unsigned long);

namespace
{
    void wire_puts(char const* s)
    {
        unsigned long n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        (void)write(1, s, n);
    }

    // The window thread, or -1 where this build has none. Defined unconditionally.
    int g_win_thread = -1;

#if (defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD) \
    || (defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE)
#define SIMCON_HAS_WINDOW_THREAD 1
#endif

#ifdef SIMCON_HAS_WINDOW_THREAD
    // A SECOND driver thread owning the console's register window and nothing else, as
    // every silicon two-thread console driver does (uart_service.h). It is NOT a receiver
    // on the console endpoint, so the endpoint losing its last WAIT cap says nothing
    // about it.
    //
    // The sim admits exactly ONE DEV window: its fake register block, mapped at the first
    // of these candidates the host leaves free. This list and its ORDER must equal
    // arch/sim/sim.cc's SIM_PVREG_BASES, and WIN its SIM_PVREG_WINDOW; a drift shows up as
    // every candidate being refused, never as a pass.
    constexpr uintptr_t SIMCON_WIN_BASES[] = {
        0x40000000u, 0x100000000ull, 0x400000000ull, 0x10000000000ull, 0x100000000000ull,
    };
    constexpr uint32_t SIMCON_WIN = 0x10000u;
    // Lines taken elsewhere: 30 (sim console ring), 29 (simuart loopback), 5..14 and 20
    // (selftest arms). Nothing raises this one; the thread only has to hold a window and
    // be cancellable.
    constexpr int SIMCON_WIN_LINE = 28;

    // Root's wait for the window thread to reach its park, in 1 ms steps.
    constexpr uint32_t WIN_READY_MAX = 500u;
    constexpr uint64_t WIN_READY_NS = 1000000u;

    volatile uint32_t g_win_ready = 0;

    // Claims the line and spawns `entry` on the first candidate window the host leaves
    // free. The caller's own line cap goes before returning: the spawned thread is the
    // only holder that needs one. A negative return leaves nothing to close.
    int spawn_window_thread(void (*entry)(void*), uint8_t prio, char const* name)
    {
        int const line = kos_irq_claim(SIMCON_WIN_LINE, KOS_IRQ_EDGE);
        if (line < 0)
        {
            return line;
        }
        kos_cap_grant const win_caps[1] = {{line, KOS_CAP_WAIT}};
        int t = -1;
        for (uintptr_t b : SIMCON_WIN_BASES)
        {
            t = kos::thread::spawn(
                entry, nullptr, name, prio, KOS_POLICY_FIFO, /*quantum_ns=*/0,
                /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
                /*stack=*/nullptr, /*stack_size=*/0,
                /*mmio=*/reinterpret_cast<void*>(b), SIMCON_WIN, win_caps, 1);
            if (t >= 0)
            {
                break;
            }
            if (t == -KOS_ENOMEM)
            {
                break; // the pool, not the window: no later candidate can succeed
            }
        }
        kos_handle_close(line);
        return t;
    }
#endif

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    void simconsole_window_thread(void*)
    {
        wire_puts("[simcon] window thread holding the console registers\n");
        g_win_ready = 1;
        // kos_irq_wait returns non-zero once thread_kill cancels it, exactly as
        // uart_service.h's irq_loop expects. Exiting releases the DEV window, which is
        // what lets the console come back.
        while (kos_irq_wait(KOS_SPAWN_DELEGATED_CAP0) == 0)
        {
        }
        wire_puts("[simcon] window thread cancelled, releasing the registers\n");
        kos_exit(0);
    }
#endif

#if defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE
    // An IRQ thread that takes the register window and never sets the ready flag root
    // waits on. It parks IN kos_irq_wait, the one wedge shape thread_kill can cancel, so
    // the window is actually released; a thread wedged before its first kos_irq_wait is
    // marked and does not die.
    void simconsole_wedge_thread(void*)
    {
        wire_puts("[simcon] wedge irq thread parked, ready never set\n");
        while (kos_irq_wait(KOS_SPAWN_DELEGATED_CAP0) == 0)
        {
        }
        wire_puts("[simcon] wedge irq thread cancelled, releasing the registers\n");
        kos_exit(0);
    }
#endif
}

extern "C"
{
    // Unprivileged console driver: drain the published endpoint to the "wire". The
    // kos::print diagnostic is DROPPED on a published board; the banner written straight
    // to the wire survives.
    //
    // The `n < 0` break never fires on a lost sender: no kernel path wakes a receiver
    // parked in kos_recv when the last SIGNAL holder goes (only the mirror case exists,
    // recv_holders -> 0 EPIPEing parked SENDERS), so this loop parks forever unless
    // KICKOS_SIMCON_EXIT_AFTER bounds it. Filed as the kos_cap_narrow endpoint-rights
    // residual in TODO.md.
    void simconsole_driver(void*)
    {
        kos::print("[simcon] kos::print diagnostic (kernel console path -- dropped post-publish)\n");
        wire_puts("[simcon] driver up (host fd 1)\n");

#if defined(KICKOS_SIMCON_DIE_AT_BRINGUP) && KICKOS_SIMCON_DIE_AT_BRINGUP
        // A driver whose bring-up fails on a real chip: die BEFORE ever receiving. The
        // service's handover probe must then see -KOS_EPIPE and report it, which is only
        // possible because the death reclaimed the console.
        wire_puts("[simcon] driver dying during bring-up\n");
        kos_exit(1);
#endif
        int const ep = KOS_SPAWN_DELEGATED_CAP0; // delegated {E | WAIT} recv cap
        char buf[KOS_EP_MSG_MAX];
#if defined(KICKOS_SIMCON_EXIT_AFTER) && KICKOS_SIMCON_EXIT_AFTER > 0
        unsigned served = 0;
#endif
        while (true)
        {
            long const n = kos_recv(ep, buf, sizeof(buf), nullptr);
            if (n < 0)
            {
                break;
            }
            (void)write(1, buf, static_cast<unsigned long>(n));
#if defined(KICKOS_SIMCON_EXIT_AFTER) && KICKOS_SIMCON_EXIT_AFTER > 0
            served++;
            if (served >= KICKOS_SIMCON_EXIT_AFTER)
            {
                wire_puts("[simcon] driver exiting (bounded serve)\n");
                break;
            }
#endif
        }
        kos_exit(0);
    }

    // The window thread's handle, for the app that cancels it. -1 before the bring-up
    // runs, after it failed, and in every build without the window thread.
    int kickos_simcon_window_thread(void)
    {
        return g_win_thread;
    }

#if defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE
    // The READY TIMEOUT every silicon console driver carries, executable with no board.
    // The ORDER mirrors xmcuartirq.cc steps 3 to 8 and is the thing under test: root waits
    // for the IRQ thread while it is still the endpoint's ONLY receiver holder, so closing
    // E takes recv_holders to 0 and notes the console dead. Once the service thread exists
    // it holds a WAIT cap on E and that close no longer reclaims, which leaves the tag
    // below with nowhere to go.
    static int simconsole_start_wedge(struct kos_service_cfg const* cfg, int ep)
    {
        if (kos_console_publish(ep) != 0)
        {
            kos::print("[simcon] ERROR: console_publish failed\n");
            kos_handle_close(ep);
            return -1;
        }
        // Dropped while the console is USER_OWNED. On the wire it means the publish did not
        // take, and then the timeout tag below would prove nothing.
        kos::print("[simcon] wedge: post-publish kernel write (must NOT reach the wire)\n");

        int const irqt = spawn_window_thread(simconsole_wedge_thread,
                                             static_cast<uint8_t>(cfg->prio + 1), "simconirq");
        if (irqt < 0)
        {
            kos_handle_close(ep);
            kos::print("[simcon] ERROR: no line or DEV window for the wedge irq thread\n");
            return -1;
        }

        // Close BEFORE cancelling, so the death note is already set when the cancelled
        // thread's exit runs the reclaim. The note alone does not reclaim: the wedged
        // thread still holds the register window, and the cancel is what releases it.
        uint32_t waited = 0;
        while (g_win_ready == 0u)
        {
            if (waited >= WIN_READY_MAX)
            {
                kos_handle_close(ep);
                (void)kos_thread_kill(irqt);
                kos::print("[simcon] ERROR: IRQ thread never reached its loop\n");
                return -1;
            }
            waited++;
            kos_sleep_ns(WIN_READY_NS);
        }

        // Unreachable while the wedge thread holds ready at 0.
        int const drv = kickos::driver::spawn_unprivileged(
            simconsole_driver, /*win_base=*/0, /*win_size=*/0, cfg->name, cfg->prio, ep,
            "[simcon] ERROR: driver spawn failed\n");
        if (drv < 0)
        {
            (void)kos_thread_kill(irqt);
            return drv;
        }
        return kickos::driver::console_handover_finish(
            ep, "[simcon] ERROR: driver died during bring-up\n", irqt);
    }
#endif

    // kos_console_publish seats the CALLER's cap 0 too, so init and the app print through
    // the driver; the parent's cap is dropped so the driver is the sole receiver.
    static int simconsole_start(struct kos_service_cfg const* cfg)
    {
        if (cfg == nullptr or cfg->kind != KOS_SVC_CONSOLE)
        {
            return -1; // cfg authored for another service class
        }
        int const ep = kos_endpoint_create();
        if (ep < 0)
        {
            return ep;
        }
#if defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE
        return simconsole_start_wedge(cfg, ep);
#endif
#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
        // BEFORE the publish, so a failure here still reports on a kernel-owned console.
        g_win_thread = spawn_window_thread(simconsole_window_thread,
                                           static_cast<uint8_t>(cfg->prio + 1), "simconwin");
        if (g_win_thread < 0)
        {
            kos::print("[simcon] ERROR: no line or DEV window for the window thread\n");
            kos_handle_close(ep);
            return -1;
        }
        // A spawn does NOT reschedule, so the new thread has not run yet whatever its
        // priority. Wait for its park BEFORE the publish: a window thread that never
        // parked cannot be cancelled, and that reads as a broken reclaim rather than a
        // broken bring-up.
        for (uint32_t waited = 0; g_win_ready == 0u; waited++)
        {
            if (waited >= WIN_READY_MAX)
            {
                kos::print("[simcon] ERROR: window thread never reached its park\n");
                kos_handle_close(ep);
                return -1;
            }
            kos_sleep_ns(WIN_READY_NS);
        }
#endif
        int const pub = kos_console_publish(ep);
        if (pub != 0)
        {
            kos_handle_close(ep);
            return pub;
        }
        // The driver thread gets NO window: the sim's "device" is fd 1, and under
        // KICKOS_SIMCON_WINDOW_THREAD the registers belong to the thread above.
        int const drv = kickos::driver::spawn_unprivileged(
            simconsole_driver, /*win_base=*/0, /*win_size=*/0, cfg->name, cfg->prio, ep,
            "[simcon] ERROR: driver spawn failed\n");
        if (drv < 0)
        {
            return drv; // the helper already closed ep, which reclaimed the console
        }
        // Close root's cap, then prove the driver is serving before any client runs.
        return kickos::driver::console_handover_finish(
            ep, "[simcon] ERROR: driver died during bring-up\n", g_win_thread);
    }

    // prio 12 matches the silicon console services: it must sit at or above every
    // stdout client's priority (there is no PI on the console rendezvous).
    static struct kos_service_cfg const simcon_cfg = {
        /*name=*/"simcon", /*mmio_base=*/0, /*mmio_window=*/0,
        /*hz=*/0, /*addr=*/0, /*prio=*/12, /*kind=*/KOS_SVC_CONSOLE,
        /*cs_policy=*/KOS_SVC_CS_NONE, /*cs_index=*/0, /*rsv=*/{ 0, 0 }
    };

    static struct kos_service_bringup const sim_services[] = {
        { simconsole_start, &simcon_cfg },
    };
    struct kos_service_list const kickos_board_services = { sim_services, 1 };
}
