// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The AMP arms: the far call across the partition, the ports a partition names this node, and
// the user share.

#include "selftest.h"

#include <kickos/libc/fmt.h>

namespace selftest
{
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_AMP_NODE
    // Under one image the kernel runs on core 0 alone, so this thread is always node 0's.
#if KICKOS_AMP_OWN_IMAGE
    constexpr unsigned AMP_SELF_ROW = KICKOS_AMP_NODE_ID;
#else
    constexpr unsigned AMP_SELF_ROW = 0u;
#endif

    // One of `node`'s window counts, or -1 for a refused read.
    int64_t amp_count(uint32_t node, uint32_t id)
    {
        uint32_t got = 0;
        if (kos_amp_count(node, id, &got) != 0)
        {
            return -1;
        }
        return static_cast<int64_t>(got);
    }

    // A kernel drains its inboxes once at window_init, so a peer that ever ran one has a
    // nonzero serviced count.
    bool amp_peer_kernel_live(uint32_t node)
    {
        if (node == KOS_AMP_SELF_NODE)
        {
            return false;
        }
        return amp_count(node, KOS_AMP_COUNT_SERVICED) > 0;
    }

    void t_amp_count_reads()
    {
        for (uint32_t n = 0; n < static_cast<uint32_t>(KICKOS_AMP_NODES); n++)
        {
            for (uint32_t id = 0; id <= KOS_AMP_COUNT_DELIVER_FAULT; id++)
            {
                uint32_t got = 0xA5A5A5A5u;
                int const rc = kos_amp_count(n, id, &got);
                if (rc != 0 or got == 0xA5A5A5A5u)
                {
                    tap::diag("node %u id %u: rc %d, read 0x%x", static_cast<unsigned>(n),
                              static_cast<unsigned>(id), rc, static_cast<unsigned>(got));
                    TAP_CHECK(false);
                }
            }
        }
        // window_init's own drain: a read confusing the id with another reads a count still 0.
        TAP_CHECK(amp_count(AMP_SELF_ROW, KOS_AMP_COUNT_SERVICED) > 0);
        uint32_t untouched = 0xA5A5A5A5u;
        TAP_CHECK(kos_amp_count(KICKOS_AMP_NODES, KOS_AMP_COUNT_SENT, &untouched) == -KOS_EINVAL);
        TAP_CHECK(kos_amp_count(0, KOS_AMP_COUNT_DELIVER_FAULT + 1u, &untouched) == -KOS_EINVAL);
        TAP_CHECK(untouched == 0xA5A5A5A5u);
        TAP_CHECK(kos_amp_count(0, KOS_AMP_COUNT_SENT, nullptr) == -KOS_EINVAL);
    }

    // --- The partition's port capabilities -------------------------------------------------
    // The kernel seats the partition's ports in root from CONFIG_KICKOS_AMP_PORTS, and the init
    // delegates main the crossings its composition names; nothing below mints or binds.
    //
    // The window layer answers the echo port with no thread; a service port needs a receiver
    // parked in a far kernel, which a peer core under one image does not run.
    constexpr size_t AMP_FAR_LEN = 16;
    constexpr uint64_t AMP_REPARK_POLL_NS = 100000ull;

    // A far call finding no receiver parked on its port is answered empty on the spot, and a
    // receiver parks again only after its last answer: a non-empty request is retried until the
    // peer answers it with bytes, all within one STALL_TOLERANT_US.
    int32_t amp_echo_call(kos_cap_t ep, void* buf, size_t len)
    {
        if (len == 0)
        {
            return -KOS_EINVAL;
        }
        uint64_t const deadline = kos_clock_now() + uint64_t{STALL_TOLERANT_US} * 1000u;
        int32_t got = -KOS_ETIMEDOUT;
        while (true)
        {
            uint64_t const now = kos_clock_now();
            uint32_t left_us = 0;
            if (now < deadline)
            {
                left_us = static_cast<uint32_t>((deadline - now) / 1000u);
            }
            if (left_us == 0)
            {
                return got;
            }
            got = kos_call_timed(ep, buf, len, len, left_us);
            // Empty: the peer is between its answer and its next wait, which node 0 cannot see.
            if (got != 0)
            {
                return got;
            }
            kos_sleep_ns(AMP_REPARK_POLL_NS);
        }
    }

    // The capability main holds for `port` of `node`, or KOS_CAP_NONE where its composition
    // names no such crossing of the partition.
    kos_cap_t amp_crossing(uint32_t node, uint32_t port)
    {
        if (kos_amp_port(node, port) == KOS_CAP_NONE)
        {
            return KOS_CAP_NONE;
        }
        char name[16];
        (void)ksnprintf(name, sizeof(name), "/amp/%u", static_cast<unsigned>(port));
        return kos_grant_endpoint(g_self, name);
    }

    // The port this image serves and its local endpoint's capability. KOS_AMP_NO_ENTRY /
    // KOS_CAP_NONE where the partition names none.
    uint32_t amp_local_port(void)
    {
        for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
        {
            if (kos_amp_entry_node(i) == KOS_AMP_SELF_NODE)
            {
                return kos_amp_entry_port(i);
            }
        }
        return KOS_AMP_NO_ENTRY;
    }

    kos_cap_t amp_local_cap(void)
    {
        uint32_t const port = amp_local_port();
        if (port == KOS_AMP_NO_ENTRY)
        {
            return KOS_CAP_NONE;
        }
        return amp_crossing(KOS_AMP_SELF_NODE, port);
    }

    // The far entry after `skip` matches, echo or service as `want_echo` asks.
    kos_cap_t amp_far_first(uint32_t* out_node, uint32_t* out_port, bool want_echo, int skip)
    {
        for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
        {
            uint32_t const node = kos_amp_entry_node(i);
            uint32_t const port = kos_amp_entry_port(i);
            if (node == KOS_AMP_SELF_NODE)
            {
                continue;
            }
            if (want_echo != (port == KOS_AMP_PORT_ECHO) or amp_crossing(node, port) == KOS_CAP_NONE)
            {
                continue;
            }
            if (skip > 0)
            {
                skip--;
                continue;
            }
            *out_node = node;
            *out_port = port;
            return amp_crossing(node, port);
        }
        return KOS_CAP_NONE;
    }

    // A partition names an echo crossing only when its peers run no kernel; one naming none
    // expects its peers to serve the first service crossing.
    bool amp_partition_has_echo(void)
    {
        uint32_t port = 0;
        uint32_t node = 0;
        return amp_far_first(&node, &port, true, 0) != KOS_CAP_NONE;
    }

    // The first port the partition names `peer`, or KOS_AMP_NO_ENTRY.
    uint32_t amp_served_port(uint32_t peer)
    {
        for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
        {
            if (kos_amp_entry_node(i) == peer)
            {
                return kos_amp_entry_port(i);
            }
        }
        return KOS_AMP_NO_ENTRY;
    }

    // The crossing main calls `node` on: its echo port where main holds one, else the first port
    // the partition names that node, which its echo task serves. KOS_CAP_NONE where main holds
    // neither. *answered is false where the serving peer is not running.
    kos_cap_t amp_peer_crossing(uint32_t node, bool* answered)
    {
        *answered = false;
        if (node == KOS_AMP_SELF_NODE)
        {
            return KOS_CAP_NONE;
        }
        kos_cap_t const echo = amp_crossing(node, KOS_AMP_PORT_ECHO);
        if (echo != KOS_CAP_NONE)
        {
            *answered = true;
            return echo;
        }
        uint32_t const port = amp_served_port(node);
        if (port == KOS_AMP_NO_ENTRY)
        {
            return KOS_CAP_NONE;
        }
        kos_cap_t const served = amp_crossing(node, port);
        if (served != KOS_CAP_NONE)
        {
            *answered = amp_peer_kernel_live(node);
        }
        return served;
    }

    // The first peer whose call is answered, or KOS_CAP_NONE.
    kos_cap_t amp_far_answered(uint32_t* out_node)
    {
        for (uint32_t n = 0; n < static_cast<uint32_t>(KICKOS_AMP_NODES); n++)
        {
            bool answered = false;
            kos_cap_t const ep = amp_peer_crossing(n, &answered);
            if (ep != KOS_CAP_NONE and answered)
            {
                *out_node = n;
                return ep;
            }
        }
        return KOS_CAP_NONE;
    }

    // A far entry no receiver serves. With an echo crossing that is any service port; without
    // one it is the second, the first being the served one.
    kos_cap_t amp_far_unanswered_at(uint32_t* out_node)
    {
        uint32_t port = 0;
        uint32_t node = 0;
        kos_cap_t cap = KOS_CAP_NONE;
        if (amp_partition_has_echo())
        {
            cap = amp_far_first(&node, &port, false, 0);
        }
        else
        {
            cap = amp_far_first(&node, &port, false, 1);
        }
        *out_node = node;
        return cap;
    }

    // Every peer main holds a crossing to takes a call and answers it with the request's own
    // bytes, each over its own pair of rings.
    void t_amp_far_call()
    {
        kos_cap_t eps[KICKOS_AMP_NODES];
        unsigned named = 0;
        unsigned live = 0;
        uint32_t first = KOS_AMP_NO_ENTRY;
        for (uint32_t n = 0; n < static_cast<uint32_t>(KICKOS_AMP_NODES); n++)
        {
            bool answered = false;
            eps[n] = amp_peer_crossing(n, &answered);
            if (eps[n] == KOS_CAP_NONE)
            {
                continue;
            }
            named++;
            if (not answered)
            {
                eps[n] = KOS_CAP_NONE;
                continue;
            }
            live++;
            if (first == KOS_AMP_NO_ENTRY)
            {
                first = n;
            }
        }
        if (live == 0)
        {
            tap::skip("no peer answers a far call on this partition");
            return;
        }
        // One live peer admits the arm and every peer must then answer: a deployment that starts
        // one peer starts all of them.
        TAP_CHECK(live == named);
        // main is unprivileged, so no user thread reaches the far-endpoint mint.
        kos_cap_t refused = KOS_CAP_NONE;
        TAP_CHECK(kos_amp_endpoint_create(first, KOS_AMP_PORT_ECHO, &refused) == -KOS_EPERM);
        TAP_CHECK(refused == KOS_CAP_NONE);
        // Far endpoints have only CAP_SIGNAL, so receive resolution must fail.
        char rbuf[AMP_FAR_LEN];
        struct kos_reply_recv_opts fo;
        kos_reply_recv_opts_init(&fo, eps[first], KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, rbuf, kos_call_lens_pack(0, sizeof(rbuf)), &fo)
                  == -KOS_EACCES);

        // The calls before the send: a far call finding nothing parked is refused on the spot,
        // and a one-thread peer answering the send is not parked.
        unsigned echoed = 0;
        for (uint32_t n = 0; n < static_cast<uint32_t>(KICKOS_AMP_NODES); n++)
        {
            if (eps[n] == KOS_CAP_NONE)
            {
                continue;
            }
            char cbuf[AMP_FAR_LEN];
            for (size_t i = 0; i < sizeof(cbuf); i++)
            {
                cbuf[i] = static_cast<char>(0x50u + n + i);
            }
            int32_t const got = amp_echo_call(eps[n], cbuf, sizeof(cbuf));
            // The echo is the request's own bytes: a wake carrying nothing passes the count.
            bool same = (got == static_cast<int32_t>(sizeof(cbuf)));
            for (size_t i = 0; same and i < sizeof(cbuf); i++)
            {
                if (cbuf[i] != static_cast<char>(0x50u + n + i))
                {
                    same = false;
                }
            }
            tap::diag("far call to node %u returned %ld", static_cast<unsigned>(n),
                      static_cast<long>(got));
            if (same)
            {
                echoed++;
            }
        }
        tap::diag("amp far call: %u of %u peer node(s) answered", echoed, named);
        TAP_CHECK(echoed == named);

        // To a port nobody serves, so no late reply lands in a later arm. The count each outcome
        // moves is this node's own: one publication, or one refusal of a full ring.
        uint32_t node = 0;
        kos_cap_t const quiet = amp_far_unanswered_at(&node);
        if (quiet != KOS_CAP_NONE)
        {
            char sbuf[AMP_FAR_LEN];
            for (size_t i = 0; i < sizeof(sbuf); i++)
            {
                sbuf[i] = static_cast<char>(0x40u + i);
            }
            int64_t const sent0 = amp_count(AMP_SELF_ROW, KOS_AMP_COUNT_SENT);
            int64_t const refused0 = amp_count(AMP_SELF_ROW, KOS_AMP_COUNT_SEND_REFUSED);
            int32_t const rc = kos_send(quiet, sbuf, sizeof(sbuf));
            int64_t const sent1 = amp_count(AMP_SELF_ROW, KOS_AMP_COUNT_SENT);
            int64_t const refused1 = amp_count(AMP_SELF_ROW, KOS_AMP_COUNT_SEND_REFUSED);
            tap::diag("send to node %u: %ld, sent %ld->%ld, refused %ld->%ld",
                      static_cast<unsigned>(node), static_cast<long>(rc),
                      static_cast<long>(sent0), static_cast<long>(sent1),
                      static_cast<long>(refused0), static_cast<long>(refused1));
            TAP_CHECK(sent0 >= 0 and refused0 >= 0);
            // At least, since the kernel's own answers to a peer's calls publish on this row too.
            if (rc == static_cast<int32_t>(sizeof(sbuf)))
            {
                TAP_CHECK(sent1 >= sent0 + 1 and refused1 == refused0);
            }
            else
            {
                TAP_CHECK(rc == -KOS_EAGAIN);
                TAP_CHECK(refused1 >= refused0 + 1);
            }
        }
    }

    // A refused far call still answers its caller: the peer's kernel publishes a zero-length
    // reply for a call to a port bound with no receiver, which a caller under KOS_TIMEOUT_NONE
    // has nothing else to wake it.
    void t_amp_far_reply_empty()
    {
        uint32_t node = 0;
        kos_cap_t const ep = amp_far_unanswered_at(&node);
        if (ep == KOS_CAP_NONE)
        {
            tap::skip("the partition names no far port whose call goes unserved");
            return;
        }
        if (not amp_peer_kernel_live(node))
        {
            tap::skip("the node serving that port runs no kernel to answer it");
            return;
        }
        char cbuf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(cbuf); i++)
        {
            cbuf[i] = static_cast<char>(0x30u + i);
        }
        int32_t const n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), STALL_TOLERANT_US);
        tap::diag("far reply empty: node %u answered a refused call with %ld byte(s)",
                  static_cast<unsigned>(node), static_cast<long>(n));
        // Exactly zero: a caller nobody answered reads -KOS_ETIMEDOUT.
        TAP_CHECK(n == 0);
    }

    // --- What the partition delegated, and where ---------------------------------------------
    // Each crossing main's composition names is a capability the init delegated at its spawn.
    constexpr uint32_t AMP_SEAT_PROBE_US = 2u * 1000u;

    // The crossings main's own row of the system table names.
    unsigned amp_crossings_named(void)
    {
        kos_table_header const* const h = kickos_table;
        kos_table_task const* const tasks = reinterpret_cast<kos_table_task const*>(h + 1);
        kos_table_grant const* const grants =
            reinterpret_cast<kos_table_grant const*>(tasks + h->task_count);
        unsigned n = 0;
        for (uint16_t k = 0; k < g_self->grant_count; k++)
        {
            if (grants[g_self->first_grant + k].kind == KOS_GRANT_PORT)
            {
                n++;
            }
        }
        return n;
    }

    void t_amp_port_seating()
    {
        TAP_CHECK(KOS_AMP_PORT_COUNT > 0u);
        unsigned local = 0;
        unsigned far = 0;
        for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
        {
            uint32_t const node = kos_amp_entry_node(i);
            uint32_t const port = kos_amp_entry_port(i);
            kos_cap_t const cap = amp_crossing(node, port);
            if (cap == KOS_CAP_NONE)
            {
                continue;
            }
            uint32_t const index = cap & 0xFFFFu;
            TAP_CHECK(index >= static_cast<uint32_t>(KOS_SPAWN_DELEGATED_CAP0)
                      and index < KOS_SPAWN_DELEGATED_CAP0 + uint32_t{g_self->cap_grant_count});
            // A local entry carries WAIT, a far one CAP_SIGNAL alone.
            char probe[1] = {};
            if (node == KOS_AMP_SELF_NODE)
            {
                TAP_CHECK(kos_amp_port_is_local(node, port) == 1);
                // Resolves with the wait right, so the receive parks and times out.
                struct kos_reply_recv_opts opts = {};
                opts.timeout_us = AMP_SEAT_PROBE_US;
                opts.ep = cap;
                TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, probe, kos_call_lens_pack(0, sizeof(probe)), &opts) == -KOS_ETIMEDOUT);
                local++;
            }
            else
            {
                TAP_CHECK(kos_amp_port_is_local(node, port) == 0);
                // Far endpoints have only CAP_SIGNAL, so receive resolution must fail.
                struct kos_reply_recv_opts po;
                kos_reply_recv_opts_init(&po, cap, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
                TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, probe,
                                         kos_call_lens_pack(0, sizeof(probe)), &po)
                          == -KOS_EACCES);
                far++;
            }
        }
        unsigned const named = amp_crossings_named();
        tap::diag("partition ports: node %u holds %u local and %u far of the %u its composition "
                  "names, from %u entr(ies)", static_cast<unsigned>(KOS_AMP_SELF_NODE), local, far,
                  named, static_cast<unsigned>(KOS_AMP_PORT_COUNT));
        TAP_CHECK(named > 0u);
        TAP_CHECK(local + far == named);
    }

    // --- A crossing the partition does not name has no capability ---------------------------
    // A node handed no capability for a crossing learns so when it asks.
    void t_amp_port_unnamed()
    {
        // A port inside the mint's width that the list names for nobody.
        uint32_t unnamed = KOS_AMP_NO_ENTRY;
        for (uint32_t p = 2u; p < 32u; p++)
        {
            bool taken = false;
            for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
            {
                if (kos_amp_entry_port(i) == p)
                {
                    taken = true;
                }
            }
            if (not taken)
            {
                unnamed = p;
                break;
            }
        }
        if (unnamed == KOS_AMP_NO_ENTRY)
        {
            tap::skip("the partition names every port of the mint's width");
            return;
        }
        for (uint32_t node = 0; node < KICKOS_AMP_NODES; node++)
        {
            TAP_CHECK(kos_amp_port(node, unnamed) == KOS_CAP_NONE);
            TAP_CHECK(amp_crossing(node, unnamed) == KOS_CAP_NONE);
            TAP_CHECK(kos_amp_port_is_local(node, unnamed) == 0);
        }
        char body[4] = {};
        TAP_CHECK(kos_send(KOS_CAP_NONE, body, sizeof(body)) == -KOS_EBADF);
        struct kos_reply_recv_opts uo;
        kos_reply_recv_opts_init(&uo, KOS_CAP_NONE, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, body, kos_call_lens_pack(0, sizeof(body)), &uo)
                  == -KOS_EBADF);
        tap::diag("unnamed crossing: port %u is named for no node, and its capability is none",
                  static_cast<unsigned>(unnamed));
    }

    // --- Whose table holds the crossings ---------------------------------------------------
    // The crossings are capabilities of main's own, so their indices name nothing in another
    // task's table.
    enum
    {
        AG_RAN = 0,
        AG_SEND = 1,
        AG_CROSSING = 2,
        AG_WORDS = 3
    };
    void amp_crossing_worker(void* arg) // caps: none
    {
        volatile int64_t* const out = static_cast<volatile int64_t*>(arg);
        char body[4] = {};
        out[AG_SEND] = kos_send_timed(static_cast<kos_cap_t>(out[AG_CROSSING]), body,
                                      sizeof(body), 0);
        out[AG_RAN] = 1;
    }

    void t_amp_crossing_task_local()
    {
        kos_cap_t const crossing = amp_local_cap();
        TAP_SKIP_UNLESS(crossing != KOS_CAP_NONE, "the partition names this node no local port");
        TAP_ASK(.workers = 1, .tasks = 1);
        void* const blk = st_ram<AG_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        TAP_CHECK(kos_mem_self_grant(blk, AG_BLK, 0) == 0);
        volatile int64_t* const out = static_cast<volatile int64_t*>(blk);
        for (int i = 0; i < AG_WORDS; i++)
        {
            out[i] = 0;
        }
        out[AG_CROSSING] = crossing;
        // The control: the same index resolves in main's own table, to a local crossing that
        // carries WAIT and no SIGNAL.
        char body[4] = {};
        int32_t const mine = kos_send_timed(crossing, body, sizeof(body), 0);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.thread(&w));
        TAP_CHECK(kos_task_create(blk, AG_BLK, 0, &t) == 0);
        w = kos::thread::create_caps(amp_crossing_worker, blk, "ampgat", 10, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        tap::diag("main's crossing %u: %ld from another task, %ld from main",
                  static_cast<unsigned>(crossing), static_cast<long>(out[AG_SEND]),
                  static_cast<long>(mine));
        TAP_CHECK(out[AG_RAN] == 1);
        TAP_CHECK(out[AG_SEND] == -KOS_EBADF);
        TAP_CHECK(mine == -KOS_EACCES);
    }

    // KICKOS_MAX_ENDPOINTS' Kconfig ceiling: the pool cannot be wider than this, so the fill
    // below either reaches it or the arm skips.
    constexpr int AMP_REUSE_SLOTS = 32;

    // FILLING THE POOL TAKES TWO TASKS: KICKOS_TASK_ENDPOINT_BUDGET sits strictly below the
    // pool's width. The arm below needs the pool full and bump-allocated to its last index, or
    // the create after its close lands on a fresh slot instead of the freed one. main's
    // own AMP port capabilities count against its ceiling.
    constexpr int CH_FILL_REPORT = 1;
    constexpr int CH_FILL_GATE = 2;

    // Fills after main has, so it also takes the slot main gave back.
    void amp_pool_filler(void*) // caps: report@1, gate@2
    {
        kos_sem_wait(CH_FILL_GATE, KOS_TIMEOUT_NONE);
        kos_cap_t mine[AMP_REUSE_SLOTS];
        int32_t n = 0;
        while (n < AMP_REUSE_SLOTS and kos_endpoint_create(&mine[n]) == 0)
        {
            n++;
        }
        (void)kos_send(CH_FILL_REPORT, &n, sizeof(n));
        // Never posted again: returning would end the task and free every slot it holds, so it
        // parks here until main's hold slays the task.
        kos_sem_wait(CH_FILL_GATE, KOS_TIMEOUT_NONE);
    }

    // A local port capability names a slot amp::port_bind also names, by index with no
    // generation, so closing the capability must not free it: freed, the next endpoint_create
    // from any task lands there and answers a peer's callers.
    void t_amp_local_port_slot_held()
    {
        // Closing this SPENDS the partition's local port for the life of the image, so no arm
        // that receives on it may follow.
        kos_cap_t const local = amp_local_cap();
        TAP_SKIP_UNLESS(local != KOS_CAP_NONE, "the partition names this node no local port");
        TAP_ASK(.workers = 1, .tasks = 1, .sems = 1, .endpoints = 1, .caps = 2);
        kos_cap_t report = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos_cap_t probe = KOS_CAP_NONE;
        kos_cap_t reused = KOS_CAP_NONE;
        kos_task_t group = KOS_TASK_NONE;
        kos::thread::Handle filler;
        ArmHold hold;
        TAP_HOLD(hold.cap(&report) and hold.cap(&gate) and hold.cap(&probe)
                 and hold.cap(&reused) and hold.task(&group) and hold.thread(&filler));
        TAP_CHECK(kos_endpoint_create(&report) == 0);
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &group) == 0);
        kos_cap_grant caps[] = {{report, KOS_CAP_SIGNAL}, {gate, KOS_CAP_WAIT}};
        filler = kos::thread::create_caps(amp_pool_filler, nullptr, "ampfill", 10, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY, nullptr, group);
        TAP_CHECK(filler.valid());

        // No check from here until held[] is closed: one failing would leave the pool full
        // under every later arm.
        kos_cap_t held[AMP_REUSE_SLOTS];
        int n = 0;
        while (n < AMP_REUSE_SLOTS and kos_endpoint_create(&held[n]) == 0)
        {
            n++; // stops at this task's own ceiling or the pool's
        }
        // One back, so main keeps a unit of ceiling to PROBE the pool with below.
        if (n > 0)
        {
            n--;
            (void)kos_handle_close(held[n]);
        }
        (void)kos_sem_post(gate);
        int32_t filled = -1;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, report, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const heard =
            kos_reply_recv(KOS_CAP_NONE, &filled, kos_call_lens_pack(0, sizeof(filled)), &opts);
        // THE PROBE IS THE PRECONDITION: -KOS_ENOMEM proves the pool full, forcing the create
        // after the close onto the freed slot.
        int const full = kos_endpoint_create(&probe);
        int closed = 1;
        int created = 1;
        if (heard == static_cast<int32_t>(sizeof(filled)) and full == -KOS_ENOMEM)
        {
            closed = kos_handle_close(local);
            created = kos_endpoint_create(&reused);
        }
        if (closed == 0)
        {
            tap::census_expect(cap_census, 1);
        }
        for (int i = 0; i < n; i++)
        {
            (void)kos_handle_close(held[i]);
        }
        tap::diag("local port slot: main held %d, the filler %ld, close %d, create after it %d",
                  n, static_cast<long>(filled), closed, created);
        TAP_CHECK(heard == static_cast<int32_t>(sizeof(filled)));
        TAP_SKIP_UNLESS(full == -KOS_ENOMEM, "the endpoint pool did not fill");
        TAP_CHECK(closed == 0);
        // The bind holds its own reference, so the close freed no slot.
        TAP_CHECK(created == -KOS_ENOMEM);
    }

#if KICKOS_AMP_OWN_IMAGE
    // --- The partition's user share ---------------------------------------------------------
    constexpr uint32_t AMP_SHARE_WORD = 0x53574F52u;
    // An echo peer's line and word (user/apps/common/ampping/main_echo.c).
    constexpr uintptr_t AMP_SHARE_LINE = 64u;
    constexpr uint32_t amp_share_mark(uint32_t node)
    {
        return 0x53480000u | node;
    }

    // Every node's line, on whole granules: what the crossing maps and the rest keep clear of.
    size_t amp_share_lines(size_t g)
    {
        return (KICKOS_AMP_NODES * AMP_SHARE_LINE + g - 1u) / g * g;
    }

    // The share as main reaches it: the window the partition region its composition maps opens,
    // at the share's base. 0 where its composition maps none.
    uintptr_t amp_share_view(size_t* size)
    {
        kos_window_t const region = kos_grant_mem(g_self, "/shm/share");
        *size = kos_window_size(region);
        return reinterpret_cast<uintptr_t>(kos_window_addr(region));
    }

#if KICKOS_MEMORY_ENFORCED && KICKOS_AMP_USER_SHARE_SIZE != 0
    constexpr uint32_t AMP_SHARE_OTHER_MEM = KOS_AMP_SHARE_MEM_FLAGS ^ KOS_MEM_NOCACHE;

    void amp_share_child(void*) {}

    // The share reaches main as the window its composition's partition region opens, in the
    // share's one memory type. main holds no reservation of the share, which the init does, so
    // main naming any part of it, as its own grant or a child's window, is refused.
    void t_amp_share_window()
    {
        size_t const g = arena_granule();
        size_t size = 0;
        uintptr_t const view = amp_share_view(&size);
        TAP_CHECK(view != 0);
        TAP_SKIP_UNLESS(g != 0 and size >= amp_share_lines(g) + 2u * g,
                        "main's share holds no two granules past the nodes' lines");
        TAP_ASK(.workers = 1);
        auto* const word = reinterpret_cast<Atomic<uint32_t, Order::RELAXED>*>(view + amp_share_lines(g));
        *word = AMP_SHARE_WORD;
        uint32_t const read = *word;
        TAP_CHECK(read == AMP_SHARE_WORD);
#if KICKOS_HAVE_ASPACE
        // A window over a part of the share, which no pool frame backs, where the composition gives
        // main one: the spawn that seated main admitted it.
        kos_window_t const wide = kos_grant_mem(g_self, "/shm/wide");
        uintptr_t const wide_view = reinterpret_cast<uintptr_t>(kos_window_addr(wide));
        size_t const wide_size = kos_window_size(wide);
        tap::diag("share part window: 0x%lx bytes at 0x%lx", static_cast<unsigned long>(wide_size),
                  static_cast<unsigned long>(wide_view));
        if (wide_view != 0)
        {
            TAP_CHECK(wide_size > 16u * g);
        }
#endif
        // The authority control: a reservation of main's own is admitted.
        void* const mine = st_ram<ASW_RAM, 0>();
        TAP_CHECK(mine != nullptr);
        TAP_CHECK(kos_mem_self_grant(mine, g, 0) == 0);
        uintptr_t const part = KOS_AMP_SHARE_BASE + amp_share_lines(g);
        int32_t const grant = kos_mem_self_grant(reinterpret_cast<void*>(part), g,
                                                 KOS_AMP_SHARE_MEM_FLAGS);
        int32_t const other = kos_mem_self_grant(reinterpret_cast<void*>(part), g,
                                                 AMP_SHARE_OTHER_MEM);
        kos_window const w = {part, static_cast<uint32_t>(g), KOS_WINDOW_MEMORY,
                              KOS_AMP_SHARE_WINDOW_FLAGS};
        kos::thread::Handle child;
        ArmHold hold;
        TAP_HOLD(hold.thread(&child));
        child = kos::thread::create(amp_share_child, nullptr, "shx", 10, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0, &w, 1);
        int const window = child.error();
        tap::diag("share named by main: grant %ld, the other type %ld, a child's window %ld",
                  static_cast<long>(grant), static_cast<long>(other), static_cast<long>(window));
        TAP_CHECK(grant == -KOS_EPERM);
        TAP_CHECK(other == -KOS_EPERM);
        TAP_CHECK(window == -KOS_EPERM);
    }
#endif

    // One node's write through its mapping of the share, read back through this node's: the
    // echo peer marks its line before it first receives, so a peer that answered has.
    void t_amp_share_crossing()
    {
        uint32_t far_node = 0;
        kos_cap_t const far = amp_far_answered(&far_node);
        TAP_SKIP_UNLESS(far != KOS_CAP_NONE, "no peer answers a far call on this partition");
        size_t const g = arena_granule();
        size_t size = 0;
        uintptr_t const view = amp_share_view(&size);
        TAP_CHECK(view != 0);
        TAP_SKIP_UNLESS(g != 0 and size >= amp_share_lines(g),
                        "main's share holds no line per node");
        char buf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<char>(0x70u + i);
        }
        TAP_CHECK(amp_echo_call(far, buf, sizeof(buf)) == static_cast<int32_t>(sizeof(buf)));
        auto const* const line =
            reinterpret_cast<Atomic<uint32_t, Order::RELAXED> const*>(view + far_node * AMP_SHARE_LINE);
        uint32_t const word = *line;
        tap::diag("share: node %u's word through this node's window: 0x%lx",
                  static_cast<unsigned>(far_node), static_cast<unsigned long>(word));
        TAP_CHECK(word == amp_share_mark(far_node));
    }

#if KICKOS_HAVE_ASPACE
    // --- A far reply into a page unmapped under its parked caller ---------------------------
    // The echo peer holds a call marked AMP_HOLD_MARK until node 0 stores the call's sequence at
    // AMP_HOLD_WORD of its own line (user/apps/common/ampping/main_echo.c), so the unmap below
    // lands between the request's copy and the reply's.
    constexpr uint8_t AMP_HOLD_MARK = 0xD0u;
    constexpr uintptr_t AMP_HOLD_WORD = 8u;
    constexpr kos_cap_t CH_ARU_FRAME = 1;
    constexpr kos_cap_t CH_ARU_SPACE = 2;
    uintptr_t g_aru_va = 0;
    uintptr_t g_aru_hold = 0;
    Atomic<uint32_t, Order::RELAXED> g_aru_seq{0};
    Atomic<int32_t, Order::RELAXED> g_aru_unmap{1};

    // Below main's priority, so it runs once main's call has parked.
    void amp_ru_unmapper(void*) // caps: frame@1, space@2
    {
        g_aru_unmap = kos_frame_unmap(CH_ARU_FRAME, CH_ARU_SPACE, g_aru_va);
        reinterpret_cast<Atomic<uint32_t, Order::RELEASE>*>(g_aru_hold)->store(g_aru_seq.load());
    }

    void t_amp_far_reply_unmapped()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        uint32_t far_node = 0;
        kos_cap_t const far = amp_far_answered(&far_node);
        TAP_SKIP_UNLESS(far != KOS_CAP_NONE, "no peer answers a far call on this partition");
        // Read by the unmapper's priority, one below main's.
        TAP_CHECK(g_self->priority >= 2u);
        size_t size = 0;
        uintptr_t const view = amp_share_view(&size);
        TAP_CHECK(view != 0 and size >= AMP_SHARE_LINE);
        TAP_ASK(.workers = 1, .caps = 2);
        kos_cap_t frame = KOS_CAP_NONE;
        kos_cap_t space = KOS_CAP_NONE;
        uintptr_t va = 0;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.mapped(&frame, &space, &va) and hold.thread(&w));
        TAP_CHECK(kos_frame_create(ASPACE_GRANULE, &frame) == 0);
        TAP_CHECK(kos_aspace_self(&space) == 0);
        TAP_CHECK(kos_frame_map(frame, space, &va, 0) == 0 and va != 0);
        auto* const release =
            reinterpret_cast<Atomic<uint32_t, Order::RELEASE>*>(view + AMP_HOLD_WORD);
        uint32_t const seq = (g_aru_seq.load() + 1u) & 0xFFu;
        g_aru_seq = seq;
        release->store(seq ^ 0xFFu);
        g_aru_va = va;
        g_aru_hold = view + AMP_HOLD_WORD;
        g_aru_unmap = 1;
        auto* const req = reinterpret_cast<unsigned char*>(va);
        req[0] = AMP_HOLD_MARK;
        req[1] = static_cast<unsigned char>(seq);
        for (size_t i = 2; i < AMP_FAR_LEN; i++)
        {
            req[i] = static_cast<unsigned char>(0x20u + i);
        }
        kos_cap_grant caps[] = {{frame, KOS_CAP_TRANSFER}, {space, KOS_CAP_TRANSFER}};
        w = kos::thread::create_caps(amp_ru_unmapper, nullptr, "ampru",
                                     static_cast<uint8_t>(g_self->priority - 1u), caps, 2,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, KOS_AUTH_MEMORY);
        TAP_CHECK(w.valid());
        int64_t const fault0 = amp_count(AMP_SELF_ROW, KOS_AMP_COUNT_DELIVER_FAULT);
        int32_t const got = kos_call_timed(far, req, AMP_FAR_LEN, AMP_FAR_LEN, STALL_TOLERANT_US);
        // The unmapper stores the sequence only after its unmap, so a word still short of it
        // here means the peer's own hold lapsed and it answered into a mapped page.
        bool const unmapped_first = release->load() == seq;
        int64_t const fault1 = amp_count(AMP_SELF_ROW, KOS_AMP_COUNT_DELIVER_FAULT);
        // Released whatever happened, so a failure here leaves the peer echoing.
        release->store(seq);
        bool const joined = hold.joined();

        // The control: the slot the refused landing held is back, so the next call crosses.
        char cbuf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(cbuf); i++)
        {
            cbuf[i] = static_cast<char>(0x60u + i);
        }
        int32_t const again = amp_echo_call(far, cbuf, sizeof(cbuf));
        tap::diag("far reply unmapped: node %u, unmap %ld, call %ld, deliver_fault %ld->%ld, "
                  "next call %ld", static_cast<unsigned>(far_node),
                  static_cast<long>(g_aru_unmap.load()), static_cast<long>(got),
                  static_cast<long>(fault0), static_cast<long>(fault1), static_cast<long>(again));
        TAP_CHECK(joined and g_aru_unmap.load() == 0);
        // Not retried: the unmapper has run by the time an empty answer lands. An empty answer
        // moves no fault count, so a refused landing answering 0 still fails below; one that
        // also dropped its count could not be told from this skip.
        TAP_SKIP_UNLESS(got != 0 or fault1 != fault0,
                        "the peer had not parked again for the held call");
        TAP_SKIP_UNLESS(unmapped_first or got != static_cast<int32_t>(AMP_FAR_LEN),
                        "the peer's hold lapsed before the unmap");
        // The landing's code, not the deadline's: 0 would be a valid empty answer.
        TAP_CHECK(got == -KOS_EFAULT);
        TAP_CHECK(fault0 >= 0 and fault1 == fault0 + 1);
        TAP_CHECK(again == static_cast<int32_t>(sizeof(cbuf)));
    }
#endif
#endif
#endif
}
