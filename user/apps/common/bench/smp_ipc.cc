// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Independent call/reply pairs, all active at once. The optional remote clients
// mix same-core and cross-core IPC in one workload.

#include <kickos/kos.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/emit.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>
#include <string.h>

namespace
{
    using kickos::Atomic;
    using kickos::Order;

#ifndef KICKOS_M95_BENCH_ROUNDS
#define KICKOS_M95_BENCH_ROUNDS 50000
#endif
    constexpr uint32_t ROUNDS = KICKOS_M95_BENCH_ROUNDS;
    static_assert(ROUNDS > 0, "IPC benchmark needs at least one call per pair");
    constexpr uint64_t READY_LIMIT_NS = 5000000000ull;
    constexpr uint32_t ACTIVE_PAIRS = KICKOS_M95_BENCH_PAIRS;
    constexpr uint32_t REMOTE_PAIRS = KICKOS_M95_BENCH_REMOTE_PAIRS;
    static_assert(ACTIVE_PAIRS > 0 and ACTIVE_PAIRS <= KICKOS_KERNEL_CORES,
                  "active IPC pairs must fit the guest kernel cores");
    static_assert(REMOTE_PAIRS <= ACTIVE_PAIRS,
                  "remote IPC pairs must be a subset of active pairs");
    static_assert(KICKOS_MAX_THREADS >= 2 * ACTIVE_PAIRS + 1,
                  "one root and two IPC peers per core must fit the thread pool");
    static_assert(KICKOS_MAX_ENDPOINTS >= ACTIVE_PAIRS,
                  "one independent endpoint per core must fit the endpoint pool");

    struct alignas(64) Pair
    {
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> server_ready{0};
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> client_ready{0};
        uint32_t core = 0;
        uint32_t client_core = 0;
        uint32_t completed = 0;
        int32_t client_error = 0;
        int32_t server_error = 0;
        uint64_t first_ns = 0;
        uint64_t last_ns = 0;
    };

    Pair g_pair[ACTIVE_PAIRS] = {};
    Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> g_start{0};
    uint32_t g_len = 8;

    void server(void* arg)
    {
        Pair& p = *static_cast<Pair*>(arg);
        unsigned char buf[KOS_EP_MSG_MAX];
        kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 1, 0, KOS_TIMEOUT_NONE);
        kos_cap_t reply = KOS_CAP_NONE;
        size_t reply_len = 0;
        p.server_ready = 1;
        for (uint32_t i = 0; i < ROUNDS; ++i)
        {
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n = kos_reply_recv(reply, buf,
                                              kos_call_lens_pack(reply_len, sizeof(buf)), &opts);
            reply = KOS_CAP_NONE;
            if (n != static_cast<int32_t>(g_len) or opts.info.reply_cap == KOS_CAP_NONE
                or buf[0] != static_cast<unsigned char>(p.core))
            {
                p.server_error = -1;
                if (n < 0)
                {
                    p.server_error = n;
                }
                return;
            }
            reply = opts.info.reply_cap;
            reply_len = static_cast<size_t>(n);
        }
        p.server_error = kos_reply(reply, buf, reply_len);
    }

    void client(void* arg)
    {
        Pair& p = *static_cast<Pair*>(arg);
        unsigned char buf[KOS_EP_MSG_MAX];
        memset(buf, 0x5a, sizeof(buf));
        buf[0] = static_cast<unsigned char>(p.core);
        p.client_ready = 1;
        while (g_start == 0)
        {
            kos_sleep_ns(1000000ull);
        }
        p.first_ns = kos_clock_now();
        for (uint32_t i = 0; i < ROUNDS; ++i)
        {
            int32_t const n = kos_call_generic(1, buf, g_len, g_len);
            if (n != static_cast<int32_t>(g_len)
                or buf[0] != static_cast<unsigned char>(p.core))
            {
                p.client_error = -1;
                if (n < 0)
                {
                    p.client_error = n;
                }
                break;
            }
            p.completed++;
        }
        p.last_ns = kos_clock_now();
    }

    void line(char const* fmt, unsigned a, unsigned b, unsigned c, unsigned d)
    {
        char out[160];
        ksnprintf(out, sizeof(out), fmt, a, b, c, d);
        kickos::emit(out);
    }

    bool measure(uint32_t len)
    {
        g_len = len;
        g_start = 0;
        line("ipc-pairs: active=%u guest=%u remote=%u\n",
             ACTIVE_PAIRS, KICKOS_KERNEL_CORES, REMOTE_PAIRS, 0);
        kos_cap_t ep[ACTIVE_PAIRS] = {};
        kos_thread_t sid[ACTIVE_PAIRS] = {};
        kos_thread_t cid[ACTIVE_PAIRS] = {};
        for (uint32_t core = 0; core < ACTIVE_PAIRS; ++core)
        {
            Pair& pair = g_pair[core];
            pair.core = core;
            pair.client_core = core;
            if (core < REMOTE_PAIRS)
            {
                pair.client_core = (core + 1u) % KICKOS_KERNEL_CORES;
            }
            pair.server_ready = 0;
            pair.client_ready = 0;
            pair.completed = 0;
            pair.client_error = 0;
            pair.server_error = 0;
            pair.first_ns = 0;
            pair.last_ns = 0;
            int const erc = kos_endpoint_create(&ep[core]);
            if (erc != 0)
            {
                line("ipc-pairs: endpoint failed len=%u core=%u rc=%u\n",
                     len, core, static_cast<unsigned>(-erc), 0);
                return false;
            }
            kos_cap_grant grant = {ep[core], KOS_CAP_WAIT};
            kos_thread_params args{};
            args.entry = server;
            args.arg = &pair;
            args.name = "ipc_server";
            args.prio = 4;
            args.policy = KOS_POLICY_FIFO;
            args.core_mask = 1u << core;
            args.caps = &grant;
            args.cap_count = 1;
            int const src = kos_thread_create(&args, &sid[core]);
            if (src != 0)
            {
                line("ipc-pairs: server spawn failed len=%u core=%u rc=%u\n",
                     len, core, static_cast<unsigned>(-src), 0);
                return false;
            }
            grant.rights_mask = KOS_CAP_SIGNAL;
            args.entry = client;
            args.name = "ipc_client";
            args.core_mask = 1u << pair.client_core;
            int const crc = kos_thread_create(&args, &cid[core]);
            if (crc != 0)
            {
                line("ipc-pairs: client spawn failed len=%u core=%u rc=%u\n",
                     len, core, static_cast<unsigned>(-crc), 0);
                return false;
            }
        }

        uint64_t const deadline = kos_clock_now() + READY_LIMIT_NS;
        while (true)
        {
            uint32_t ready = 0;
            for (uint32_t core = 0; core < ACTIVE_PAIRS; ++core)
            {
                ready += g_pair[core].server_ready.load();
                ready += g_pair[core].client_ready.load();
            }
            if (ready == 2 * ACTIVE_PAIRS)
            {
                break;
            }
            if (kos_clock_now() > deadline)
            {
                line("ipc-pairs: ready timeout len=%u ready=%u need=%u\n",
                     len, ready, 2 * ACTIVE_PAIRS, 0);
                return false;
            }
            kos_sleep_ns(1000000ull);
        }

        // The reset runs after every peer is ready, so no spawn or endpoint mint
        // contributes to this pass's IPC and lock distributions.
        (void)kos_bench(KOS_BENCH_OP_RESET, 0, 0);
        uint64_t const start_ns = kos_clock_now();
        g_start = 1;
        for (uint32_t core = 0; core < ACTIVE_PAIRS; ++core)
        {
            int const crc = kos_thread_join(cid[core], KOS_TIMEOUT_NONE);
            int const src = kos_thread_join(sid[core], KOS_TIMEOUT_NONE);
            if (crc != 0 or src != 0)
            {
                line("ipc-pairs: join failed len=%u core=%u client=%u server=%u\n",
                     len, core, static_cast<unsigned>(-crc), static_cast<unsigned>(-src));
                return false;
            }
        }
        uint64_t const elapsed = kos_clock_now() - start_ns;
        uint32_t completed = 0;
        uint64_t earliest = UINT64_MAX;
        uint64_t latest = 0;
        uint64_t latest_start = 0;
        uint64_t earliest_end = UINT64_MAX;
        uint32_t class_calls[2] = {};
        uint64_t class_first[2] = {UINT64_MAX, UINT64_MAX};
        uint64_t class_last[2] = {};
        for (uint32_t core = 0; core < ACTIVE_PAIRS; ++core)
        {
            Pair const& p = g_pair[core];
            completed += p.completed;
            uint32_t group = 0u;
            if (core < REMOTE_PAIRS)
            {
                group = 1u;
            }
            class_calls[group] += p.completed;
            if (p.first_ns < class_first[group]) { class_first[group] = p.first_ns; }
            if (p.last_ns > class_last[group]) { class_last[group] = p.last_ns; }
            if (p.first_ns < earliest) { earliest = p.first_ns; }
            if (p.last_ns > latest) { latest = p.last_ns; }
            if (p.first_ns > latest_start) { latest_start = p.first_ns; }
            if (p.last_ns < earliest_end) { earliest_end = p.last_ns; }
            line("ipc-pairs: len=%u core=%u calls=%u active_ms=%u\n",
                 len, core, p.completed,
                 static_cast<unsigned>((p.last_ns - p.first_ns) / 1000000ull));
            if (p.client_error != 0 or p.server_error != 0)
            {
                line("ipc-pairs: error len=%u core=%u client=%u server=%u\n",
                     len, core, static_cast<unsigned>(-p.client_error),
                     static_cast<unsigned>(-p.server_error));
                return false;
            }
        }
        uint32_t rate = 0;
        if (elapsed != 0)
        {
            rate = static_cast<uint32_t>(
                static_cast<uint64_t>(completed) * 1000000000ull / elapsed);
        }
        uint32_t overlap = 0;
        if (earliest_end > latest_start)
        {
            overlap = static_cast<uint32_t>((earliest_end - latest_start) / 1000000ull);
        }
        line("ipc-pairs: len=%u cores=%u calls=%u rate=%u/s\n",
             len, KICKOS_KERNEL_CORES, completed, rate);
        line("ipc-pairs: len=%u wall_ms=%u span_ms=%u overlap_ms=%u\n",
             len, static_cast<unsigned>(elapsed / 1000000ull),
             static_cast<unsigned>((latest - earliest) / 1000000ull), overlap);
        for (uint32_t group = 0; group < 2; ++group)
        {
            uint32_t group_rate = 0u;
            if (class_calls[group] != 0u)
            {
                uint64_t const span = class_last[group] - class_first[group];
                if (span != 0u)
                {
                    group_rate = static_cast<uint32_t>(
                        static_cast<uint64_t>(class_calls[group]) * 1000000000ull / span);
                }
            }
            line("ipc-pairs: len=%u remote=%u calls=%u rate=%u/s\n",
                 len, group, class_calls[group], group_rate);
        }
        line("ipc-pairs: report len=%u cores=%u\n", len, KICKOS_KERNEL_CORES, 0, 0);
        (void)kos_bench(KOS_BENCH_OP_PHASE_PRINT, 0, 0);
        (void)kos_bench(KOS_BENCH_OP_DIST_PRINT, 0, 0);
        if (completed != ROUNDS * ACTIVE_PAIRS or overlap == 0)
        {
            return false;
        }
        for (uint32_t core = 0; core < ACTIVE_PAIRS; ++core)
        {
            if (kos_handle_close(ep[core]) != 0)
            {
                return false;
            }
        }
        return true;
    }
}

KICKOS_APP_AUTHORITY(KOS_AUTH_SYSTEM);

int main(int, char**)
{
    if (not measure(8) or not measure(256))
    {
        return 1;
    }
    kickos::emit("ipc-pairs: done\n");
    return 0;
}
