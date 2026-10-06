// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The restart witness's sensor (docs/design-m10-target.md, section 8), beside its app and
// health checker. Each instance first asks for a thread above its declared priority and, above
// one core, for one on a core it was not declared on, and says how each was answered. It then serves five readings and returns, which ends the instance. Its readings count
// on from the last one /shm/history holds, so every reading of the run is distinct, and an
// instance that finds a reading there is a restart, which sleeps before its first receive.
//
// It prints with puts: the golden composition gives it a one-page stack.

#include <kickos/board_config.h> // KICKOS_KERNEL_CORES
#include <kickos/sys.h>

#include "sample.h"

#include <stdint.h>
#include <stdio.h>

namespace
{
    constexpr uint32_t READINGS = 5u;
    constexpr uint64_t RESTART_SLEEP_NS = 500000000ull;

    void probe_entry(void* arg)
    {
        (void)arg;
        kos_exit(0);
    }

    // Says how a thread asked for at <prio> on <cores> was answered: refused with -KOS_EPERM
    // is the narrowing the init applied, anything else is not.
    void ask(uint8_t prio, uint32_t cores, char const* refused, char const* admitted)
    {
        struct kos_thread_params p = {};
        p.entry = probe_entry;
        p.name = "probe";
        p.prio = prio;
        p.policy = KOS_POLICY_FIFO;
        p.core_mask = cores;
        kos_thread_t thread = KOS_THREAD_NONE;
        int const rc = kos_thread_create(&p, &thread);
        if (rc == -KOS_EPERM)
        {
            puts(refused);
        }
        else
        {
            puts(admitted);
        }
    }
}

extern "C" void restart_sensor_main(kos_self_t const* self)
{
    kos_window_t const history_window = kos_grant_mem(self, "/shm/history");
    auto const hist = static_cast<history*>(kos_window_addr(history_window));
    kos_cap_t const served = kos_grant_endpoint(self, "/svc/sensor");
    if (hist == nullptr or kos_window_size(history_window) < sizeof(history))
    {
        puts("sensor: no /shm/history window");
        return;
    }

    ask(static_cast<uint8_t>(self->priority + 1u), self->core_mask,
        "sensor: a thread above its priority was refused",
        "sensor: a thread above its priority was not refused");
#if KICKOS_KERNEL_CORES > 1
    uint32_t other = 1u;
    if ((self->core_mask & other) != 0u)
    {
        other = 2u;
    }
    ask(self->priority, other, "sensor: a thread on another core was refused",
        "sensor: a thread on another core was not refused");
#endif

    uint32_t value = 0;
    if (history_latest(hist, &value))
    {
        kos_sleep_ns(RESTART_SLEEP_NS);
    }

    struct kos_reply_recv_opts opts;
    kos_cap_t reply = KOS_CAP_NONE;
    struct sample s = {};
    uint32_t count = 0;
    while (count < READINGS)
    {
        kos_reply_recv_opts_init(&opts, served, 0u, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(reply, &s, kos_call_lens_pack(sizeof(s), 0u), &opts);
        reply = KOS_CAP_NONE;
        if (n < 0)
        {
            continue;
        }
        reply = opts.info.reply_cap;
        value++;
        s.value = value;
        history_append(hist, value);
        count++;
    }
    kos_reply(reply, &s, sizeof(s));
}
