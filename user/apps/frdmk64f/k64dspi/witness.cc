// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/DSPI0 on the local engine, two tasks: `holder` owns the window and drives the bus,
// `main` watches it. The AIPS0 slot is the only gate on DSPI0, per privilege and not per thread,
// so the image witnesses when it is open:
//   - a windowless thread of the holder's task sets MCR.HALT under a slow transfer, which must
//     end -KOS_EIO, and the next transfer must complete;
//   - a transfer at the slowest divider pair the device open accepts must complete;
//   - K64DSPI_CLOSE_ARM=0: the holder closes, reopens, transfers and returns with the bus open;
//     main then reads MCR from a task of its own, which must fault: the death closed the slot;
//   - K64DSPI_CLOSE_ARM=1: the holder closes and a windowless thread of its task reads MCR,
//     which must fault while the holder still lives: the close closed the slot.
// No fitting is needed: a frame completes whatever SIN reads.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <kickos/chip_mmap.h>
#include <kickos/driver/k64dspi.h>
#include <kickos/driver/spi.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if !KICKOS_HAVE_MPU
#error "the k64dspi witness requires enforcement: build the board's base variant"
#endif

#ifndef K64DSPI_CLOSE_ARM
#error "K64DSPI_CLOSE_ARM selects the witness's ending: 0 the holder's death, 1 the bus close"
#endif

namespace
{
    namespace mmap = kickos::mk64f::mmap;

    constexpr uintptr_t MCR_OFFSET = 0x00u;
    constexpr uint32_t MCR_MSTR = 1u << 31;
    constexpr uint32_t MCR_HALT = 1u << 0;

    // The holder drops below the halter so the halter's wake preempts the transfer it halts.
    constexpr uint8_t HOLDER_RUN_PRIO = 8;
    constexpr uint8_t SIBLING_PRIO = 9;
    constexpr uint8_t PROBE_PRIO = 4;

    constexpr uint32_t FAST_HZ = 1000000u;
    constexpr uint32_t SLOW_HZ = 1000u; // 200 frames take over a second
    constexpr uint64_t HALT_AFTER_NS = 20000000ull;
    constexpr uint32_t SLOW_LEN = 200u;
    constexpr uint32_t SHORT_LEN = 4u;
    // Every transfer the holder makes, stalled to its bound or not, ends within a few seconds.
    constexpr uint32_t HOLDER_END_US = 30000000u;
    // The slowest CTAR pair: PBR /7 and BR /32768.
    constexpr uint32_t SLOWEST_DIV = 7u * 32768u;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // fmt takes one %d.
    void say(char const* fmt, int32_t a)
    {
        char s[112];
        ksnprintf(s, sizeof(s), fmt, static_cast<int>(a));
        kos::print(s);
    }

    [[noreturn]] void give_up(char const* what, int32_t rc)
    {
        char s[96];
        ksnprintf(s, sizeof(s), "[k64dspi] ERROR: %s rc %d\n", what, static_cast<int>(rc));
        kos::print(s);
        exit(1);
    }

    int32_t open_device(struct kos_spi_device* d, struct kos_spi_bus* b, uint8_t slot, uint32_t hz)
    {
        struct kos_spi_device_config cfg = {};
        cfg.hz = hz;
        cfg.slot = slot;
        cfg.word_bits = 8u;
        cfg.cs_policy = KOS_BUS_CS_GPIO;
        cfg.cs_index = 0u;
        return kos_spi_device_open(d, b, &cfg);
    }

    int32_t transfer(struct kos_spi_device* d, uint32_t len)
    {
        static unsigned char buf[KOS_SPI_XFER_MAX];
        for (uint32_t i = 0; i < len; i++)
        {
            buf[i] = static_cast<unsigned char>(i);
        }
        struct kos_bus_seg seg = {static_cast<uint16_t>(len), 0u, 0u};
        return kos_spi_transfer(d, &seg, 1u, buf, len);
    }

    int32_t open_bus(struct kos_spi_bus* b, uintptr_t win)
    {
        struct kos_spi_bus_config cfg = {};
        cfg.base = win;
        cfg.ep = KOS_CAP_NONE;
        cfg.irq = KOS_CAP_NONE;
        cfg.notify = KOS_CAP_NONE;
        return kos_spi_bus_open(b, &cfg);
    }

    // Holds no window: the base arrives as a value, and the open slot is what lets it in.
    void halter(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        kos_sleep_ns(HALT_AFTER_NS);
        r32(win + MCR_OFFSET) = MCR_MSTR | MCR_HALT;
    }

    // Holds no window either; on a closed slot the read faults and the line after it never prints.
    void probe(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        uint32_t const mcr = r32(win + MCR_OFFSET);
        char s[80];
        ksnprintf(s, sizeof(s), "[k64dspi] the probe read MCR=0x%x: the slot stayed open\n",
                  static_cast<unsigned>(mcr));
        kos::print(s);
    }

    void halted_transfer(struct kos_spi_bus* bus, uintptr_t win)
    {
        struct kos_spi_device slow;
        int32_t const hz = open_device(&slow, bus, 1u, SLOW_HZ);
        if (hz <= 0)
        {
            give_up("slow device open", hz);
        }
        int const lowered = kos_thread_set_priority(HOLDER_RUN_PRIO);
        if (lowered != 0)
        {
            give_up("lowering the holder", lowered);
        }
        auto const h = kos::thread::create(halter, reinterpret_cast<void*>(win), "k64halt",
                                           SIBLING_PRIO);
        if (not h.valid())
        {
            give_up("spawn of the halter", h.error());
        }
        say("[k64dspi] halted transfer rc=%d (want -5)\n", transfer(&slow, SLOW_LEN));
        (void)h.join();
    }
}

extern "C" void k64dspi_holder_main(kos_self_t const* self)
{
    kos_window_t const window = kos_grant_mmio(self, "/dev/dspi0");
    uintptr_t const win = reinterpret_cast<uintptr_t>(kos_window_addr(window));
    if (win == 0u or kos_window_size(window) < mmap::DSPI0_SIZE)
    {
        give_up("no /dev/dspi0 window", -1);
    }
    int32_t const muxed = k64dspi_bus_mux(win);
    if (muxed != 0)
    {
        give_up("bus pins", muxed);
    }

    struct kos_spi_bus bus;
    int32_t rc = open_bus(&bus, win);
    if (rc != 0)
    {
        give_up("bus open", rc);
    }
    struct kos_spi_device fast;
    rc = open_device(&fast, &bus, 0u, FAST_HZ);
    if (rc <= 0)
    {
        give_up("device open", rc);
    }
    say("[k64dspi] transfer rc=%d (want 4)\n", transfer(&fast, SHORT_LEN));

    halted_transfer(&bus, win);
    say("[k64dspi] transfer after the halt rc=%d (want 4)\n",
        transfer(&fast, SHORT_LEN));

    struct kos_spi_device slowest;
    uint32_t const f = kos_periph_clock_hz(win);
    rc = open_device(&slowest, &bus, 2u, f / SLOWEST_DIV);
    if (rc <= 0)
    {
        give_up("slowest device open", rc);
    }
    say("[k64dspi] slowest pair at %d Hz\n", rc);
    say("[k64dspi] slowest-pair transfer rc=%d (want 2)\n", transfer(&slowest, 2u));

    say("[k64dspi] bus close rc=%d (want 0)\n", kos_spi_bus_close(&bus));
#if K64DSPI_CLOSE_ARM
    kos::print("[k64dspi] after the close, a thread holding no window reads MCR\n");
    auto const p =
        kos::thread::create(probe, reinterpret_cast<void*>(win), "k64cprb", SIBLING_PRIO);
    if (not p.valid())
    {
        give_up("spawn of the probe", p.error());
    }
    (void)p.join(); // the probe's fault ends this task too
    kos::print("[k64dspi] the holder outlived its probe\n");
#else
    rc = open_bus(&bus, win);
    if (rc != 0)
    {
        give_up("bus reopen", rc);
    }
    rc = open_device(&fast, &bus, 0u, FAST_HZ);
    if (rc <= 0)
    {
        give_up("device reopen", rc);
    }
    say("[k64dspi] transfer after the reopen rc=%d (want 4)\n",
        transfer(&fast, SHORT_LEN));
    kos::print("[k64dspi] the holder returns with the bus open\n");
#endif
}

extern "C" void k64dspi_watch_main(kos_self_t const* self)
{
    kos_cap_t const events = kos_grant_notify(self, "/init/events");
    if (kos_notify_bind(events) != 0)
    {
        give_up("no /init/events to wait the holder's end on", -1);
    }
    uint32_t bits = 0;
    int const waited = kos_notify_wait(events, 1u, HOLDER_END_US, &bits);
    if (waited == -KOS_ETIMEDOUT)
    {
        kos::print("[k64dspi] FAIL: the holder did not end within 30 s\n");
        exit(1);
    }
    if (waited != 0 or (bits & 1u) == 0u)
    {
        give_up("the wait for the holder's end", waited);
    }

    kos_task_t t = KOS_TASK_NONE;
    int rc = kos_task_create(nullptr, 0, 0, &t);
    if (rc != 0)
    {
        give_up("probe task", rc);
    }
    kos::print("[k64dspi] the holder ended; a task holding no window reads MCR\n");
    auto const p = kos::thread::create(probe, reinterpret_cast<void*>(mmap::DSPI0_BASE), "k64dprb",
                                       PROBE_PRIO, KOS_POLICY_FIFO, 0, nullptr, 0, nullptr, 0,
                                       nullptr, 0, nullptr, 0, 0, nullptr, t);
    if (not p.valid())
    {
        give_up("spawn of the probe", p.error());
    }
    (void)p.join();
    int status = 0;
    rc = kos_task_exit_status(t, &status);
    if (rc == 0 and status == KOS_EXIT_FAULT)
    {
        kos::print("[k64dspi] PASS: the probe after the holder's end faulted\n");
    }
    else
    {
        say("[k64dspi] FAIL: the probe after the holder's end was not killed (status %d)\n",
            status);
    }
    exit(0);
}
