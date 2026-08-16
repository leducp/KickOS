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
#include <kickos/sys/bytes.h> // mem_copy, mem_zero
#include <kickos/sys/console_ring.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <stdint.h>

// The host write(2), declared rather than included: this TU is built freestanding
// (kickos_apply_freestanding) and must not pull host headers. fd 1 is "the wire". Its
// long/unsigned long are the libc ABI, and are the only ones in this file.
extern "C" long write(int, void const*, unsigned long);

// Defined at the bottom of this file; the descriptor below names it.
extern "C" void simconsole_driver(void*);

namespace drv = kickos::driver;

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

    // ONE thread, no window (the "device" is host fd 1), no line, no shared block, and so no
    // readiness latch: the one thread IS the endpoint's receiver, and no point exists before
    // it at which a timeout would be reportable.
    constexpr drv::Descriptor k_desc = {
        .tag = "[simcon] ",
        .expected_base = 0,
        .block_size = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_HANDOVER,
        .svc_kind = KOS_SVC_CONSOLE,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = simconsole_driver,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc),
                  "the simcon descriptor is not a well-formed driver shape");

    // The window thread, invalid where this build has none. Defined unconditionally.
    kos::thread::Handle g_win_thread;

#if (defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD) \
    || (defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE)
#define SIMCON_HAS_WINDOW_THREAD 1
#endif

// Exactly ONE of the three start bodies is compiled. The precedence lives here rather than at
// the dispatch: a build setting both knobs would otherwise define a variant nothing calls,
// which is -Werror=unused-function.
#if defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE
#define SIMCON_START_WEDGE 1
#elif defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
#define SIMCON_START_WINDOWED 1
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
    // only holder that needs one. An invalid handle leaves nothing to close.
    kos::thread::Handle spawn_window_thread(void (*entry)(void*), uint8_t prio,
                                            char const* name, kos_task_t task)
    {
        kos_cap_t line = KOS_CAP_NONE;
        int const line_rc = kos_irq_claim(SIMCON_WIN_LINE, KOS_IRQ_EDGE, &line);
        if (line_rc != 0)
        {
            return kos::thread::Handle(KOS_THREAD_NONE, line_rc);
        }
        kos_cap_grant const win_caps[1] = {{line, KOS_CAP_WAIT}};
        kos::thread::Handle t;
        for (uintptr_t b : SIMCON_WIN_BASES)
        {
            t = kos::thread::spawn(
                entry, nullptr, name, prio, KOS_POLICY_FIFO, /*quantum_ns=*/0,
                /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
                /*stack=*/nullptr, /*stack_size=*/0,
                /*mmio=*/reinterpret_cast<void*>(b), SIMCON_WIN, win_caps, 1,
                /*authority=*/0, /*cap_dest=*/nullptr, task);
            if (t.valid())
            {
                break;
            }
            if (t.error() == -KOS_ENOMEM)
            {
                break; // the pool, not the window: no later candidate can succeed
            }
        }
        kos_handle_close(line);
        return t;
    }

    // The two window-thread postures below do NOT go through drv::bring_up, and the loop
    // above is why: the host may refuse any given candidate base, so this window's address is
    // discovered BY SPAWNING, which a descriptor's single cfg->mmio_base cannot express.
    kos::thread::Handle spawn_console_driver(struct kos_service_cfg const* cfg, kos_cap_t ep,
                                             kos_task_t task)
    {
        kos::thread::Handle const h =
            drv::spawn_one(k_desc.threads[0], cfg, /*blk=*/nullptr, ep, /*line=*/nullptr,
                           task);
        if (not h.valid())
        {
            // CLOSE BEFORE PRINTING: the console is USER_OWNED from the publish on, and the
            // close is what reclaims it, so the tag below reaches the wire.
            kos_handle_close(ep);
            kos::print("[simcon] ERROR: driver spawn failed\n");
        }
        return h;
    }
#endif

#ifdef SIMCON_START_WINDOWED
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

namespace
{
    // Returns kos_reply's result: a reply can fail on a dead cap, and a caller that has
    // gone is the one thing this arm cannot see from its own state.
    int simcon_reply_status(kos_cap_t reply_cap, int32_t status, uint16_t len)
    {
        struct kos_uart_rsp rsp;
        rsp.status = status;
        rsp.len = len;
        rsp.rsv = 0;
        return kos_reply(reply_cap, &rsp, sizeof(rsp));
    }

    // The framed arm of the console endpoint, answered out of this thread's own state:
    // there is no ring and no device here. Every op must ANSWER, refusal included: a
    // kos_call left unanswered parks the caller forever.
    int simcon_serve_one(struct kos_uart_stats* stats, volatile uint32_t* mode,
                         uint8_t const* msg, size_t n, kos_cap_t reply_cap)
    {
        if (n < sizeof(struct kos_uart_req))
        {
            return simcon_reply_status(reply_cap, -KOS_EINVAL, 0);
        }
        struct kos_uart_req req;
        mem_copy(&req, msg, sizeof(req));
        uint8_t const* payload = msg + sizeof(req);
        size_t const payload_len = n - sizeof(req);

        switch (req.op)
        {
            case KOS_UART_WRITE:
            {
                if (req.len > payload_len)
                {
                    return simcon_reply_status(reply_cap, -KOS_EINVAL, 0);
                }
                // The ACTUAL count: fd 1 is a pipe under a harness, where a short write is
                // constructible, and req.len would report bytes that never reached the wire.
                long const put = write(1, payload, req.len);
                if (put < 0)
                {
                    return simcon_reply_status(reply_cap, -KOS_EPIPE, 0);
                }
                stats->tx_bytes += static_cast<uint32_t>(put);
                return simcon_reply_status(reply_cap, 0, static_cast<uint16_t>(put));
            }
            case KOS_UART_READ:
            {
                // No RX arm at all: a 0-byte reply would read as "nothing yet".
                return simcon_reply_status(reply_cap, -KOS_ENOSYS, 0);
            }
            case KOS_UART_STATS:
            {
                uint8_t out[sizeof(struct kos_uart_rsp) + sizeof(struct kos_uart_stats)];
                struct kos_uart_rsp rsp;
                rsp.status = 0;
                rsp.len = static_cast<uint16_t>(sizeof(struct kos_uart_stats));
                rsp.rsv = 0;
                mem_copy(out, &rsp, sizeof(rsp));
                mem_copy(out + sizeof(rsp), stats, sizeof(*stats));
                return kos_reply(reply_cap, out, sizeof(out));
            }
            case KOS_UART_SET_MODE:
            {
                // Nothing required: a host fd write always completes.
                return simcon_reply_status(
                    reply_cap, kickos::console::mode_apply(mode, req.flags, 0u), 0);
            }
            case KOS_UART_CONFIGURE:
            {
                return simcon_reply_status(reply_cap, -KOS_ENOSYS, 0);
            }
            default:
            {
                return simcon_reply_status(reply_cap, -KOS_EINVAL, 0);
            }
        }
    }
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
        uint8_t buf[KOS_EP_MSG_MAX];
        // In this thread's frame: no shared block, no second thread. They DIE WITH THE
        // THREAD, so the survives-a-restart property <kickos/sys/uart.h> states does not hold.
        struct kos_uart_stats stats;
        mem_zero(&stats, sizeof(stats));
        volatile uint32_t mode = 0;
#if defined(KICKOS_SIMCON_EXIT_AFTER) && KICKOS_SIMCON_EXIT_AFTER > 0
        unsigned served = 0;
#endif
        while (true)
        {
            struct kos_recv_info info;
            int32_t const n = kos_recv(ep, buf, sizeof(buf), &info);
            if (n < 0)
            {
                break;
            }
            if (info.reply_cap != KOS_CAP_NONE)
            {
                // A failed reply leaves a caller parked on one, so it is said rather than
                // swallowed; the loop continues, one dead caller not being the console's end.
                if (simcon_serve_one(&stats, &mode, buf, static_cast<size_t>(n),
                                     info.reply_cap) < 0)
                {
                    wire_puts("[simcon] reply failed\n");
                }
            }
            else
            {
                // Zero length means FLUSH, and nothing is buffered to drain. A plain send
                // has no reply, so a short write can only be counted, not reported.
                long const put = write(1, buf, static_cast<unsigned long>(n)); // libc ABI
                long took = put;
                if (took < 0)
                {
                    took = 0;
                }
                stats.tx_bytes += static_cast<uint32_t>(took);
                stats.tx_dropped += static_cast<uint32_t>(n - took);
            }
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

    // The window thread's handle, for the app that cancels it. KOS_THREAD_NONE before the
    // bring-up runs, after it failed, and in every build without the window thread.
    kos_thread_t kickos_simcon_window_thread(void)
    {
        return g_win_thread.id();
    }

#ifdef SIMCON_START_WEDGE
    // The READY TIMEOUT every silicon console driver carries, executable with no board.
    // The ORDER is the thing under test: root waits for the IRQ thread while it is still
    // the endpoint's ONLY receiver holder, so closing E takes recv_holders to 0 and notes
    // the console dead. Once the service thread exists it holds a WAIT cap on E and that
    // close no longer reclaims, which leaves the tag below with nowhere to go.
    //
    // A HAND COPY of bring_up's order, not a call into it, because the wedge thread's window
    // base has to be probed (spawn_window_thread). Leg L8 defends the real one.
    static int simconsole_start_wedge(struct kos_service_cfg const* cfg)
    {
        // Both threads are ONE driver, so one task: the group kill below names no thread, and
        // either thread's death ends the other. No shared region, because the sim's "device" is
        // fd 1 and there is no ring block.
        kos_task_t task = KOS_TASK_NONE;
        int const task_rc = kos_task_create(nullptr, 0, &task);
        if (task_rc != 0)
        {
            return task_rc;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        int const ep_rc = kos_endpoint_create(&ep);
        if (ep_rc != 0)
        {
            (void)kos_task_kill(task);
            return ep_rc;
        }
        if (kos_console_publish(ep) != 0)
        {
            kos::print("[simcon] ERROR: console_publish failed\n");
            kos_handle_close(ep);
            (void)kos_task_kill(task);
            return -1;
        }
        // Dropped while the console is USER_OWNED. On the wire it means the publish did not
        // take, and then the timeout tag below would prove nothing.
        kos::print("[simcon] wedge: post-publish kernel write (must NOT reach the wire)\n");

        auto const irqt = spawn_window_thread(simconsole_wedge_thread,
                                              static_cast<uint8_t>(cfg->prio + 1), "simconirq",
                                              task);
        if (not irqt.valid())
        {
            kos_handle_close(ep);
            (void)kos_task_kill(task);
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
                (void)kos_task_kill(task);
                kos::print("[simcon] ERROR: IRQ thread never reached its loop\n");
                return -1;
            }
            waited++;
            kos_sleep_ns(WIN_READY_NS);
        }

        // Unreachable while the wedge thread holds ready at 0.
        kos::thread::Handle const drvt = spawn_console_driver(cfg, ep, task);
        if (not drvt.valid())
        {
            (void)kos_task_kill(task);
            return drvt.error();
        }
        return drv::console_handover_finish(ep, "[simcon] ", task);
    }
#endif

#ifdef SIMCON_START_WINDOWED
    static int simconsole_start_windowed(struct kos_service_cfg const* cfg)
    {
        // The window thread is deliberately NOT a member of the driver's task, and that is
        // this posture's whole subject: it models a FOREIGN holder of the console registers,
        // which is the only shape in which the deferred reclaim is observable. Coupling it
        // would end it with the driver, the window would already be free when
        // console_on_driver_death ran, and the defer arm would never be taken. A real
        // multi-thread driver does put its window holder in the group (drv::bring_up).
        //
        // BEFORE the publish, so a failure here still reports on a kernel-owned console.
        // That ordering is why this posture cannot be drv::bring_up, which publishes first.
        g_win_thread = spawn_window_thread(simconsole_window_thread,
                                          static_cast<uint8_t>(cfg->prio + 1), "simconwin",
                                          KOS_TASK_NONE);
        if (not g_win_thread.valid())
        {
            kos::print("[simcon] ERROR: no line or DEV window for the window thread\n");
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
                (void)g_win_thread.kill();
                kos::print("[simcon] ERROR: window thread never reached its park\n");
                return -1;
            }
            kos_sleep_ns(WIN_READY_NS);
        }

        // The driver thread's own group, holding only it. An explicit task even for one
        // member, because the handover tail ends a GROUP and nothing else.
        kos_task_t task = KOS_TASK_NONE;
        int const task_rc = kos_task_create(nullptr, 0, &task);
        if (task_rc != 0)
        {
            (void)g_win_thread.kill();
            return task_rc;
        }
        kos_cap_t ep = KOS_CAP_NONE;
        int const ep_rc = kos_endpoint_create(&ep);
        if (ep_rc != 0)
        {
            (void)kos_task_kill(task);
            (void)g_win_thread.kill();
            return ep_rc;
        }
        int const pub = kos_console_publish(ep);
        if (pub != 0)
        {
            kos_handle_close(ep);
            (void)kos_task_kill(task);
            (void)g_win_thread.kill();
            return pub;
        }
        // The driver thread gets NO window: the sim's "device" is fd 1, and the registers
        // belong to the thread above.
        kos::thread::Handle const drvt = spawn_console_driver(cfg, ep, task);
        if (not drvt.valid())
        {
            (void)kos_task_kill(task);
            (void)g_win_thread.kill();
            return drvt.error(); // the helper already closed ep, which reclaimed the console
        }
        // Close root's cap, then prove the driver is serving before any client runs. The
        // window thread is cancelled SEPARATELY on the failure path, because it is not in the
        // driver's group: the console comes back only once the WINDOW is free, and the window
        // holder is not the thread whose death EPIPEs the probe.
        int const rc = drv::console_handover_finish(ep, "[simcon] ", task);
        if (rc != 0)
        {
            (void)g_win_thread.kill();
        }
        return rc;
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
#ifdef SIMCON_START_WEDGE
        return simconsole_start_wedge(cfg);
#elif defined(SIMCON_START_WINDOWED)
        return simconsole_start_windowed(cfg);
#else
        return drv::bring_up(k_desc, cfg, nullptr);
#endif
    }

    // prio 12 matches the silicon console services: it must sit at or above every
    // stdout client's priority (there is no PI on the console rendezvous).
    static struct kos_service_cfg const simcon_cfg = {
        .name = "simcon",
        .mmio_base = 0,
        .mmio_window = 0,
        .hz = 0,
        .addr = 0,
        .prio = 12,
        .kind = KOS_SVC_CONSOLE,
        .rsv = { 0, 0, 0, 0 }
    };

    static struct kos_service_bringup const sim_services[] = {
        { simconsole_start, &simcon_cfg },
    };
    struct kos_service_list const kickos_board_services = { sim_services, 1 };
}
