// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// M10 GOLDEN EXAMPLE: does not build yet. `kos_self_t`, the `kos_grant_*` lookups and the
// `kos_window_*` accessors are what M10 adds; everything else is today's API.
//
// The sensor task for QEMU `virt` on A53 cores: a driver that owns its device, unlike the
// XMC's, which is a client of a bus service. Its measurement is the PL031 clock's seconds
// count. The composition grants the PL031's page; this task holds no authority.
//
// `kos_window_addr` answers the address the window is mapped at IN THIS TASK: on a translating
// board that is wherever the task's address space put the page, not the physical address the
// grant named, and on an MPU board it is the physical address itself. So this source never
// writes a physical address down, and the same call is right on both classes.

#include <kickos/sys.h>

#include "sample.h"

#include <stdint.h>

namespace
{
    constexpr uintptr_t RTCDR = 0x000; // PL031 data register: seconds since it started
}

extern "C" void sensor_main(kos_self_t const* self)
{
    // A window lookup answers an opaque handle into this task's entry of the emitted table;
    // its address and size come from there, the size from the description and never from
    // this source.
    kos_window_t const rtc_window = kos_grant_mmio(self, "/dev/rtc");
    kos_window_t const history_window = kos_grant_mem(self, "/shm/history");
    auto const rtc = static_cast<unsigned char volatile*>(kos_window_addr(rtc_window));
    auto const hist = static_cast<history*>(kos_window_addr(history_window));
    kos_cap_t const served = kos_grant_endpoint(self, "/svc/sensor");
    if (rtc == nullptr or kos_window_size(rtc_window) <= RTCDR or hist == nullptr
        or kos_window_size(history_window) < sizeof(history))
    {
        return;
    }

    struct kos_reply_recv_opts opts;
    kos_cap_t reply = KOS_CAP_NONE;
    struct sample s = {};
    while (true)
    {
        kos_reply_recv_opts_init(&opts, served, 0u, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(reply, &s, kos_call_lens_pack(sizeof(s), 0u), &opts);
        reply = KOS_CAP_NONE;
        if (n < 0)
        {
            continue;
        }
        reply = opts.info.reply_cap;
        s.value = *reinterpret_cast<uint32_t volatile const*>(rtc + RTCDR);
        history_append(hist, s.value);
    }
}
