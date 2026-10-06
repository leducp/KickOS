// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The host sim's console driver: the posture the silicon boards ship, where a userspace driver
// owns the console and the kernel chip path is dark.
//
// The driver thread write(2)s to host fd 1, which reaches the console WITHOUT going through
// kconsole_write, so bytes appear if and only if they travelled the endpoint -> driver route.
// Sim-only by construction (host libc).


#include <kickos/config/priorities.h> // KICKOS_PRIO_MIN
#include <kickos/driver/declared/simcon.h>
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/bytes.h> // mem_copy, mem_zero
#include <kickos/sys/console_ring.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <kickos/sys/atomic.h>

#include <stdint.h>

// The host write(2), declared rather than included: this TU is built freestanding
// (kickos_apply_freestanding) and must not pull host headers. fd 1 is "the wire". Its
// long/unsigned long are the libc ABI, and are the only ones in this file.
extern "C" long write(int, void const*, unsigned long);

extern "C" void simconsole_driver(void*);

namespace drv = kickos::driver;
namespace declared = kickos::driver::declared::simcon;

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
        .block_size = declared::k_declared.block_size,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = declared::k_declared.ep_posture,
        .line_count = declared::k_declared.line_count,
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        .lines = {},
        .threads = {{.entry = simconsole_driver,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc),
                  "the simcon descriptor is not a well-formed driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the simcon descriptor departs from its kickos_add_driver declaration");

    // The window thread, invalid where this build has none. Defined unconditionally.
    kos::thread::Handle g_win_thread;

#if (defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD) \
    || (defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE)
#define SIMCON_HAS_WINDOW_THREAD 1
#endif

// Exactly ONE of the three start bodies is compiled. The precedence lives here rather than at
// the dispatch: a build setting both knobs would otherwise define an unused variant, which
// is -Werror=unused-function.
#if defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE
#define SIMCON_START_WEDGE 1
#elif defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
#define SIMCON_START_WINDOWED 1
#endif

#ifdef SIMCON_HAS_WINDOW_THREAD
    // A driver thread owning the console's register window and nothing else.
    //
    // The sim's DEV windows lie in its fake register block, mapped at the first of these
    // candidates the host leaves free. This list and its ORDER must equal
    // arch/sim/sim.cc's SIM_PVREG_BASES, and WIN its SIM_PVREG_WINDOW; a drift shows up as
    // every candidate being refused, never as a pass.
    constexpr uintptr_t SIMCON_WIN_BASES[] = {
        0x40000000u, 0x100000000ull, 0x400000000ull, 0x10000000000ull, 0x100000000000ull,
    };
    constexpr uint32_t SIMCON_WIN = 0x10000u;
    // The block's third window, sim.cc's SIM_PVREG_CONSOLE.
    constexpr uintptr_t SIMCON_WIN_AT = 2u * SIMCON_WIN;

    // Root's wait for the window thread to reach its park, in 1 ms steps.
    constexpr uint32_t WIN_READY_MAX = 500u;
    constexpr uint64_t WIN_READY_NS = 1000000u;

    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_win_ready{0};

    // Under an instance only the init may end the driver's task, so its entry polls this every
    // WIN_READY_NS.
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_win_release{0};

    // Creates the object the thread parks on and spawns `entry` on the first candidate
    // window the host leaves free. The caller's own capability goes before returning: the
    // spawned thread's BIND holds a reference of its own. An invalid handle leaves nothing
    // to close.
    kos::thread::Handle spawn_window_thread(void (*entry)(void*), uint8_t prio,
                                            char const* name, kos_task_t task)
    {
        kos_cap_t note = KOS_CAP_NONE;
        int const note_rc = kos_notify_create(&note);
        if (note_rc != 0)
        {
            return kos::thread::Handle(KOS_THREAD_NONE, note_rc);
        }
        kos_cap_grant const win_caps[1] = {{note, KOS_CAP_WAIT}};
        kos::thread::Handle t;
        for (uintptr_t b : SIMCON_WIN_BASES)
        {
            kos_window const win = {b + SIMCON_WIN_AT, SIMCON_WIN, KOS_WINDOW_DEVICE, 0};
            t = kos::thread::create(
                entry, nullptr, name, prio, KOS_POLICY_FIFO, /*quantum_ns=*/0,
                /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
                /*stack=*/nullptr, /*stack_size=*/0,
                /*windows=*/&win, 1, win_caps, 1,
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
        kos_handle_close(note);
        return t;
    }

    // The two window-thread postures below do NOT go through drv::bring_up, and the loop
    // above is why: the host may refuse any given candidate base, so this window's address is
    // discovered BY SPAWNING, which an instance's single window base cannot express.
    kos::thread::Handle spawn_console_driver(struct kos_driver_instance const& in, kos_task_t task)
    {
        kos::thread::Handle const h =
            drv::spawn_one(k_desc.threads[0], in, /*line=*/nullptr, /*note=*/KOS_CAP_NONE, task,
                           /*core_mask=*/0u);
        if (not h.valid())
        {
            (void)kos_cap_narrow(in.endpoint, drv::KOS_DRV_HANDOVER_KEPT);
        }
        return h;
    }

    int instance_task(struct kos_driver_instance& in, kos_task_t* out)
    {
        int rc = kos_task_create(nullptr, 0, 0, out);
        if (rc != 0)
        {
            return rc;
        }
        in.task = *out;
        rc = kos_task_sched_grant(*out, in.ceiling, in.core_mask);
        if (rc != 0)
        {
            return rc;
        }
        return kos_task_watch(*out, in.watch, in.endpoint);
    }
#endif

#ifdef SIMCON_START_WINDOWED
    // The task's entry, holding nothing: its exit on release ends the task.
    void simconsole_entry_thread(void*)
    {
        while (g_win_release == 0u)
        {
            kos_sleep_ns(WIN_READY_NS);
        }
        wire_puts("[simcon] task entry released, ending the driver's task\n");
        kos_exit(0);
    }

    // Holds the console registers until the task's end slays it. It runs at the lowest
    // priority, so a client above it runs between that end and this thread's exit.
    void simconsole_window_thread(void*)
    {
        wire_puts("[simcon] window thread holding the console registers\n");
        (void)kos_notify_bind(KOS_SPAWN_DELEGATED_CAP0);
        g_win_ready = 1;
        while (kos_notify_wait(KOS_SPAWN_DELEGATED_CAP0, 0xFFFFFFFFu, KOS_TIMEOUT_NONE, nullptr)
               == 0)
        {
        }
        kos_exit(0);
    }
#endif

#if defined(KICKOS_SIMCON_IRQ_WEDGE) && KICKOS_SIMCON_IRQ_WEDGE
    // An IRQ thread that takes the register window and never sets the ready flag root
    // waits on. It parks IN kos_notify_wait, the one wedge shape thread_kill can cancel, so
    // the window is actually released; a thread wedged before its first wait is marked and
    // does not die.
    void simconsole_wedge_thread(void*)
    {
        wire_puts("[simcon] wedge irq thread parked, ready never set\n");
        (void)kos_notify_bind(KOS_SPAWN_DELEGATED_CAP0);
        while (kos_notify_wait(KOS_SPAWN_DELEGATED_CAP0, 0xFFFFFFFFu, KOS_TIMEOUT_NONE,
                               nullptr)
               == 0)
        {
        }
        wire_puts("[simcon] wedge irq thread woke from its wait\n");
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
    int simcon_serve_one(struct kos_uart_stats* stats,
                         kickos::Atomic<uint32_t, kickos::Order::RELAXED>* mode,
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
                kos_counter_increment(&stats->tx_bytes, static_cast<uint32_t>(put));
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
                kickos::console::stats_pack(out + sizeof(rsp), stats, 0u);
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
    // Unprivileged console driver: drain the published endpoint to the "wire". The kernel
    // console diagnostic is DROPPED on a published board; the banner written straight to the
    // wire survives.
    //
    // The `n < 0` break never fires on a lost sender: no kernel path wakes a receiver parked in
    // kos_recv when the last SIGNAL holder goes, so this loop parks forever unless
    // KICKOS_SIMCON_EXIT_AFTER bounds it.
    void simconsole_driver(void* arg)
    {
        (void)arg; // records the posture; the thread takes no arg
        char const diag[] = "[simcon] kernel console diagnostic (dropped post-publish)\n";
        (void)kos_kconsole_write(diag, sizeof(diag) - 1u); // a dropped line is the measurement
        wire_puts("[simcon] driver up (host fd 1)\n");

#if defined(KICKOS_SIMCON_DIE_AT_BRINGUP) && KICKOS_SIMCON_DIE_AT_BRINGUP
        // A driver whose bring-up fails on a real chip: die BEFORE ever receiving. The
        // service's handover probe must then see -KOS_ECONNREFUSED and report it, which is only
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
        // Frame-local and single-threaded; atomic to match simcon_serve_one's signature.
        kickos::Atomic<uint32_t, kickos::Order::RELAXED> mode{0};
#if defined(KICKOS_SIMCON_EXIT_AFTER) && KICKOS_SIMCON_EXIT_AFTER > 0
        unsigned served = 0;
#endif
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, KOS_TIMEOUT_NONE);
        while (true)
        {
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n =
                kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
            if (n < 0)
            {
                break;
            }
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                // A failed reply leaves a caller parked on one, so it is said rather than
                // swallowed; the loop continues, one dead caller not being the console's end.
                if (simcon_serve_one(&stats, &mode, buf, static_cast<size_t>(n),
                                     opts.info.reply_cap) < 0)
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
                kos_counter_increment(&stats.tx_bytes, static_cast<uint32_t>(took));
                kos_counter_increment(&stats.tx_dropped, static_cast<uint32_t>(n - took));
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

    // KOS_THREAD_NONE before the bring-up runs, after it failed, and in every build without
    // the window thread.
    kos_thread_t kickos_simcon_window_thread(void)
    {
        return g_win_thread.id();
    }

    // Ends the driver's task in a build that has a window thread, which its entry notices
    // within WIN_READY_NS.
    void kickos_simcon_window_release(void)
    {
#ifdef SIMCON_HAS_WINDOW_THREAD
        g_win_release = 1;
#endif
    }

#ifdef SIMCON_START_WEDGE
    // The READY TIMEOUT under the init: the wedged thread is the driver task's entry and holds
    // the register window. The init slays the failed start, whose end notes the console dead
    // and whose exit releases the window, and reports the failure on the console that comes
    // back.
    static int simconsole_start_wedge(struct kos_driver_instance* in)
    {
        kos_task_t task = KOS_TASK_NONE;
        int const task_rc = instance_task(*in, &task);
        if (task_rc != 0)
        {
            return task_rc;
        }
        int const pub = kos_console_publish(in->endpoint, task);
        if (pub != 0)
        {
            kos::print("[simcon] ERROR: console_publish failed\n");
            return pub;
        }
        char const wedge[]
            = "[simcon] wedge: post-publish kernel write (must NOT reach the wire)\n";
        (void)kos_kconsole_write(wedge, sizeof(wedge) - 1u); // a dropped line is the measurement
        auto const irqt = spawn_window_thread(simconsole_wedge_thread, in->ceiling, "simconirq", task);
        if (not irqt.valid())
        {
            (void)kos_cap_narrow(in->endpoint, drv::KOS_DRV_HANDOVER_KEPT);
            return -1;
        }
        for (uint32_t waited = 0; g_win_ready == 0u; waited++)
        {
            if (waited >= WIN_READY_MAX)
            {
                (void)kos_cap_narrow(in->endpoint, drv::KOS_DRV_HANDOVER_KEPT);
                return -1;
            }
            kos_sleep_ns(WIN_READY_NS);
        }
        kos::thread::Handle const drvt = spawn_console_driver(*in, task);
        if (not drvt.valid())
        {
            return drvt.error();
        }
        return drv::console_handover_finish(*in, "[simcon] ");
    }
#endif

#ifdef SIMCON_START_WINDOWED
    // The windowed posture under the init: an entry that holds nothing, a window thread that
    // outlives the receiver's death holding the console registers, and the receiver. The
    // entry's release ends the task with the window still held.
    static int simconsole_start_windowed(struct kos_driver_instance* in)
    {
        kos_task_t task = KOS_TASK_NONE;
        int const task_rc = instance_task(*in, &task);
        if (task_rc != 0)
        {
            return task_rc;
        }
        kos::thread::Handle const entry = kos::thread::create(
            simconsole_entry_thread, nullptr, "simconent", in->ceiling, KOS_POLICY_FIFO, 0,
            /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0, nullptr,
            task);
        if (not entry.valid())
        {
            kos::print("[simcon] ERROR: no entry thread\n");
            return entry.error();
        }
        g_win_thread = spawn_window_thread(simconsole_window_thread, KICKOS_PRIO_MIN, "simconwin",
                                           task);
        if (not g_win_thread.valid())
        {
            kos::print("[simcon] ERROR: no notification or DEV window for the window thread\n");
            return -1;
        }
        for (uint32_t waited = 0; g_win_ready == 0u; waited++)
        {
            if (waited >= WIN_READY_MAX)
            {
                kos::print("[simcon] ERROR: window thread never reached its park\n");
                return -1;
            }
            kos_sleep_ns(WIN_READY_NS);
        }
        int const pub = kos_console_publish(in->endpoint, task);
        if (pub != 0)
        {
            return pub;
        }
        kos::thread::Handle const drvt = spawn_console_driver(*in, task);
        if (not drvt.valid())
        {
            return drvt.error();
        }
        return drv::console_handover_finish(*in, "[simcon] ");
    }
#endif

    // kos_console_publish seats the CALLER's cap 0 too, so init and the app print through
    // the driver; the init's cap is narrowed so the driver is the sole receiver.
    int simcon_console_start(struct kos_driver_instance* instance)
    {
#ifdef SIMCON_START_WEDGE
        return simconsole_start_wedge(instance);
#elif defined(SIMCON_START_WINDOWED)
        return simconsole_start_windowed(instance);
#else
        return drv::bring_up(k_desc, instance);
#endif
    }
}
