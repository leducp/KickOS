// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The AMP arms: the shared window, the far call and its reply ring, and the ports a
// partition names this node.

#include "selftest.h"

namespace selftest
{
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_AMP_NODE
    // --- The shared window, and the validation the reading node owes it -------------------
    // Far over the crossing's cost: what they wait on is the host scheduling another vCPU thread.
    constexpr uint64_t AMP_REPLY_NS = 2ull * 1000ull * 1000ull * 1000ull;
    constexpr uint64_t AMP_REPLY_TICK_NS = 1ull * 1000ull * 1000ull;

    // Under one image the kernel runs on core 0 alone, so this thread is always node 0's.
#if KICKOS_AMP_OWN_IMAGE
    constexpr unsigned AMP_SELF_ROW = KICKOS_AMP_NODE_ID;
#else
    constexpr unsigned AMP_SELF_ROW = 0u;
#endif

    // -1 for any refusal: a negative errno arrives through the uintptr_t with its top bit set,
    // and every `> 0` on the raw word would hold.
    int64_t amp_count(uint32_t op, uint32_t a1)
    {
        uintptr_t const raw = kos_amp_probe(op, a1);
        if (static_cast<intptr_t>(raw) < 0)
        {
            return -1;
        }
        return static_cast<int64_t>(raw);
    }

    // A kernel drains its inboxes once at window_init, so a peer that ever ran one has a
    // nonzero serviced count.
    bool amp_peer_kernel_live(uint32_t node)
    {
        if (node == KOS_AMP_SELF_NODE)
        {
            return false;
        }
        return amp_count(KOS_AMP_OP_SERVICED, node) > 0;
    }

    // The reset advances indices past a slot a record still names, so exactly one of the two
    // owns that record's death. The probe drives the whole interleaving in one masked call.
    //
    // STOMPS the peer's call ring: a real publication in flight from that peer is lost.
    void t_amp_reset_record()
    {
        uintptr_t const bits = kos_amp_probe(KOS_AMP_OP_RESET_RECORD, 0u);
        // Bit 16 first: a forge that could not run answers zero.
        TAP_CHECK((bits & 16u) != 0u);
        // The reset freed the record whose slot it abandoned; left live, it refuses a seat to
        // every later call on that masked slot.
        TAP_CHECK((bits & 1u) != 0u);
        // The next call at that slot is granted a record.
        TAP_CHECK((bits & 2u) != 0u);
        // A different token: same table slot, a generation on.
        TAP_CHECK((bits & 4u) != 0u);
        // Spending the abandoned token released nothing; accepted, it would free the slot the
        // new call is still served on.
        TAP_CHECK((bits & 8u) != 0u);
    }

    // The first two arms stomp a peer's rings and skip where that peer runs a kernel; the third
    // forges on this node's unused self reply ring and runs either way.

    void t_amp_far_reset_answers()
    {
        int64_t const unsent0 = amp_count(KOS_AMP_OP_REPLY_UNSENT, AMP_SELF_ROW);
        // Before anything keys on it: the op exists, and a row outside the built range answers
        // zero.
        TAP_CHECK(unsent0 >= 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_REPLY_UNSENT, KICKOS_AMP_NODES) == 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_REPLY_UNSENT, KICKOS_AMP_NODES + 7u) == 0);

        uint32_t const bits =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_RESET_ANSWERS, 0u));
        if ((bits & 32u) != 0u)
        {
            tap::skip("the peer runs a kernel of its own and moves both of these indices");
            return;
        }
        int64_t const unsent1 = amp_count(KOS_AMP_OP_REPLY_UNSENT, AMP_SELF_ROW);
        tap::diag("reset answers: bits 0x%lx, reply_unsent %ld->%ld",
                  static_cast<unsigned long>(bits), static_cast<long>(unsent0),
                  static_cast<long>(unsent1));
        // Bit 16 first: a forge that could not run answers zero.
        TAP_CHECK((bits & 16u) != 0u);
        TAP_CHECK((bits & 1u) != 0u);
        TAP_CHECK((bits & 2u) != 0u);
        // The caller the dead record named got an empty reply carrying its tag: a far caller
        // under KOS_TIMEOUT_NONE dropped in silence stays parked for the life of the image.
        TAP_CHECK((bits & 4u) != 0u);
        TAP_CHECK((bits & 8u) != 0u);
        TAP_CHECK(unsent1 == unsent0 + 1);
    }

    void t_amp_far_answer_deferred()
    {
        int64_t const unsent0 = amp_count(KOS_AMP_OP_REPLY_UNSENT, AMP_SELF_ROW);
        TAP_CHECK(unsent0 >= 0);
        uint32_t const defer =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_ANSWER_DEFER, 0u));
        if ((defer & 16u) != 0u)
        {
            tap::skip("the peer drains the ring this holds full, so no answer of ours is lost");
            return;
        }
        int64_t const unsent1 = amp_count(KOS_AMP_OP_REPLY_UNSENT, AMP_SELF_ROW);
        uint32_t const done =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_ANSWER_DISCHARGE, 0u));
        int64_t const unsent2 = amp_count(KOS_AMP_OP_REPLY_UNSENT, AMP_SELF_ROW);
        tap::diag("answer deferred: defer 0x%lx, discharge 0x%lx, unsent %ld->%ld->%ld",
                  static_cast<unsigned long>(defer), static_cast<unsigned long>(done),
                  static_cast<long>(unsent0), static_cast<long>(unsent1),
                  static_cast<long>(unsent2));
        TAP_CHECK((defer & 8u) != 0u);
        TAP_CHECK((defer & 1u) != 0u);
        // The call slot is retained, so its caller is still owed an answer.
        TAP_CHECK((defer & 2u) != 0u);
        TAP_CHECK((defer & 4u) != 0u);
        TAP_CHECK(unsent1 == unsent0 + 1);
        TAP_CHECK((done & 8u) != 0u);
        TAP_CHECK((done & 1u) != 0u);
        TAP_CHECK((done & 2u) != 0u);
        TAP_CHECK((done & 4u) != 0u);
        // The discharge counts no second loss.
        TAP_CHECK(unsent2 == unsent1);
    }

    void t_amp_far_tail_recovery()
    {
        int64_t const reset0 = amp_count(KOS_AMP_OP_TAIL_RESET, AMP_SELF_ROW);
        TAP_CHECK(reset0 >= 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_TAIL_RESET, KICKOS_AMP_NODES) == 0);
        uint32_t const bits =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_TAIL_RECOVERY, 0u));
        int64_t const reset1 = amp_count(KOS_AMP_OP_TAIL_RESET, AMP_SELF_ROW);
        tap::diag("tail recovery: bits 0x%lx, tail_reset %ld->%ld",
                  static_cast<unsigned long>(bits), static_cast<long>(reset0),
                  static_cast<long>(reset1));
        TAP_CHECK((bits & 16u) != 0u);
        // The first invalid-tail publication is refused.
        TAP_CHECK((bits & 1u) != 0u);
        // The answer that reached the limit is the one the recovery delivers.
        TAP_CHECK((bits & 2u) != 0u);
        // At the adopted far tail: a recovery keeping its own head publishes into a slot the
        // consumer reads as already past.
        TAP_CHECK((bits & 4u) != 0u);
        TAP_CHECK((bits & 8u) != 0u);
        TAP_CHECK(reset1 == reset0 + 1);
    }

    void t_amp_window()
    {
        // Keyed on the sentinel, never a named op: KOS_AMP_OP_MAX is the one op no dispatch will
        // ever carry.
        TAP_CHECK(amp_count(KOS_AMP_OP_MAX, 0) == -1);
        // The last inbox forge must leave the ring well formed: a refused depth does not advance
        // the tail.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_HEAD_DEPTH)
                  == KOS_AMP_V_DEPTH);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_LENGTH)
                  == KOS_AMP_V_LENGTH);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PORT)
                  == KOS_AMP_V_PORT);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PORT_WIDE)
                  == KOS_AMP_V_PORT);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_ZERO_LEN)
                  == KOS_AMP_V_TOOK);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_WELL_FORMED)
                  == KOS_AMP_V_TOOK);
        // A REPLY-class port in the CALL ring, both fields well formed. No counter reaches
        // wrong_class, so the verdict must be its own code: folded into KOS_AMP_V_EMPTY it
        // reads as an empty ring.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_CLASS)
                  == KOS_AMP_V_CLASS);
        // The send side's untrusted index, forged on the self-ring so a live peer's consumer
        // index is never written.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_TAIL_DEPTH)
                  == KOS_AMP_V_SEND_DEPTH);
        // node_service skips the self-ring, so accepted self-sends would fill it permanently.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_SELF_SEND)
                  == KOS_AMP_V_SEND_NODE);
        // After DEPTH_STRIKES refusals the ring is resynchronised and the next well-formed
        // publication is taken.
        int64_t const resets = amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW);
        // First, so a refused read cannot enter the arithmetic below.
        TAP_CHECK(resets >= 0);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_DEPTH_RESET)
                  == KOS_AMP_V_TOOK);
        // The reset counter moved: a build that stopped refusing the depth also answers TOOK.
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW) == resets + 1);

        // The reply ring's own bound. The real service body runs between strikes and takes from
        // the same pair's call ring, so a strike count keyed by the ordered pair alone is cleared
        // each time and the reply ring stays dead for the life of the image.
        int64_t const reply_resets = amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_DEPTH_SERVICE)
                  == KOS_AMP_V_TOOK);
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW) == reply_resets + 1);

        // A row outside the partition answers zero. Keyed on the reset counter, whose own row is
        // now nonzero: `sent` is still zero here, the forges bypassing amp::send.
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW) > 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, KICKOS_AMP_NODES) == 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, KICKOS_AMP_NODES + 7u) == 0);

        // Decided at runtime: a node booted alone under one image per node has no running peer.
        // Keyed on any live peer, never node 1, which is this node's own row where this image is
        // node 1.
        unsigned live = 0;
        for (unsigned n = 0; n < static_cast<unsigned>(KICKOS_AMP_NODES); n++)
        {
            if (amp_peer_kernel_live(n))
            {
                live++;
            }
        }
        if (live > 0)
        {
        unsigned answered = 0;
        for (unsigned n = 0; n < static_cast<unsigned>(KICKOS_AMP_NODES); n++)
        {
            // Skip self, not index 0: a sweep from 1 is node 0's peer set only.
            if (n == AMP_SELF_ROW)
            {
                continue;
            }
            int64_t const mine_took0 = amp_count(KOS_AMP_OP_TOOK, AMP_SELF_ROW);
            int64_t const peer_took0 = amp_count(KOS_AMP_OP_TOOK, n);
            int64_t const peer_sent0 = amp_count(KOS_AMP_OP_SENT, n);
            int64_t const peer_svc0 = amp_count(KOS_AMP_OP_SERVICED, n);
            TAP_CHECK(mine_took0 >= 0);
            TAP_CHECK(kos_amp_probe(KOS_AMP_OP_ROUND, n) == 0u);
            // This node's own doorbell service takes the reply, so this thread must leave the
            // CPU with interrupts open: a syscall runs with them masked.
            //
            // No bound: the peer is live, and its vCPU is a thread the HOST schedules, which can
            // leave it unscheduled for longer than any bound while this core's clock runs on.
            while (amp_count(KOS_AMP_OP_TOOK, AMP_SELF_ROW) == mine_took0)
            {
                kos_sleep_ns(AMP_REPLY_TICK_NS);
            }
            int64_t const mine_took1 = amp_count(KOS_AMP_OP_TOOK, AMP_SELF_ROW);
            int64_t const peer_took1 = amp_count(KOS_AMP_OP_TOOK, n);
            int64_t const peer_sent1 = amp_count(KOS_AMP_OP_SENT, n);
            int64_t const peer_svc1 = amp_count(KOS_AMP_OP_SERVICED, n);
            // SERVICED tells a peer that never entered its handler from one that entered and
            // took nothing.
            tap::diag("node %u: took %ld->%ld sent %ld->%ld serviced %ld->%ld; node 0 took "
                      "%ld->%ld",
                      n, static_cast<long>(peer_took0), static_cast<long>(peer_took1),
                      static_cast<long>(peer_sent0), static_cast<long>(peer_sent1),
                      static_cast<long>(peer_svc0), static_cast<long>(peer_svc1),
                      static_cast<long>(mine_took0), static_cast<long>(mine_took1));
            if (mine_took1 == mine_took0)
            {
                continue;
            }
            answered++;
            // The peer took the payload and published an answer.
            TAP_CHECK(peer_took1 > peer_took0);
            TAP_CHECK(peer_sent1 > peer_sent0);
        }
        // One live peer admits this half and every peer must then answer: a deployment that
        // starts one peer starts all of them.
        tap::diag("amp window: %u of %u peer node(s) answered over the doorbell, %u live "
                  "before the sweep", answered,
                  static_cast<unsigned>(KICKOS_AMP_NODES) - 1u, live);
        TAP_CHECK(answered == static_cast<unsigned>(KICKOS_AMP_NODES) - 1u);
        // Not vacuous: a node that never serviced a doorbell answered nothing.
        TAP_CHECK(amp_count(KOS_AMP_OP_SERVICED, AMP_SELF_ROW) > 0);
        }
    }

    // --- The partition's port capabilities -------------------------------------------------
    // The kernel seats every capability used below into root from CONFIG_KICKOS_AMP_PORTS;
    // nothing below mints or binds.
    //
    // The window layer answers the echo port with no thread; a service port needs a receiver
    // parked in a far kernel, which a peer core under one image does not run.
    constexpr uint32_t AMP_FAR_US = 2u * 1000u * 1000u; // as AMP_REPLY_NS: a host-scheduled vCPU
    constexpr size_t AMP_FAR_LEN = 16;

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
        return kos_amp_port(KOS_AMP_SELF_NODE, port);
    }

    // Any far entry this image may call. KOS_CAP_NONE where the partition names this node none.
    kos_cap_t amp_far_any(void)
    {
        for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
        {
            uint32_t const node = kos_amp_entry_node(i);
            if (node == KOS_AMP_SELF_NODE)
            {
                continue;
            }
            return kos_amp_port(node, kos_amp_entry_port(i));
        }
        return KOS_CAP_NONE;
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
            if (want_echo != (port == KOS_AMP_PORT_ECHO))
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
            return kos_amp_port(node, port);
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

    // A far entry whose call is answered. KOS_CAP_NONE where the serving peer is not running:
    // the far call waits with no deadline.
    kos_cap_t amp_far_answered(uint32_t* out_node)
    {
        uint32_t port = 0;
        uint32_t node = 0;
        kos_cap_t const echo = amp_far_first(&node, &port, true, 0);
        if (echo != KOS_CAP_NONE)
        {
            *out_node = node;
            return echo;
        }
        kos_cap_t const served = amp_far_first(&node, &port, false, 0);
        if (served != KOS_CAP_NONE and amp_peer_kernel_live(node))
        {
            *out_node = node;
            return served;
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

    kos_cap_t amp_far_unanswered(void)
    {
        uint32_t node = 0;
        return amp_far_unanswered_at(&node);
    }

    // True once `node`'s serviced count and this node's reply-drop count hold still across one
    // tick. FALSE AT THE DEADLINE, and the caller must then exit: a partition still moving
    // cannot tell its own delta from a neighbour's.
    bool amp_wait_quiet(uint32_t node)
    {
        uint64_t const deadline = kos_clock_now() + AMP_REPLY_NS;
        int64_t serviced = -2;
        int64_t drops = -2;
        while (kos_clock_now() < deadline)
        {
            int64_t const s = amp_count(KOS_AMP_OP_SERVICED, node);
            int64_t const d = amp_count(KOS_AMP_OP_REPLY_DROP, AMP_SELF_ROW);
            if (s == serviced and d == drops)
            {
                return true;
            }
            serviced = s;
            drops = d;
            kos_sleep_ns(AMP_REPLY_TICK_NS);
        }
        return false;
    }

    // The peer KOS_AMP_OP_ROUND is driven at. Build constants alone, so a member of another
    // task derives the same node.
    uint32_t amp_round_peer(void)
    {
        if (KOS_AMP_SELF_NODE == 0u)
        {
            return 1u;
        }
        return 0u;
    }


    // Unlike amp_count, keeps which errno refused.
    int64_t amp_rc(uint32_t op, uint32_t a1)
    {
        return static_cast<int64_t>(static_cast<intptr_t>(kos_amp_probe(op, a1)));
    }

    // A take with no reply slot to reserve leaves the call unread and counts the deferral; the
    // call is taken once the ring has room.
    void t_amp_reply_reserve()
    {
        uint32_t const peer = amp_round_peer();
        if (peer >= static_cast<uint32_t>(KICKOS_AMP_NODES))
        {
            tap::skip("the partition holds no peer at the kernel's own choice");
            return;
        }
        int64_t const reserve0 = amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW);
        int64_t const took0 = amp_count(KOS_AMP_OP_TOOK, AMP_SELF_ROW);
        uint32_t const answer = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_RESERVE));
        if ((answer & KOS_AMP_RESERVE_RAN) == 0u)
        {
            tap::skip("the peer drains its own reply ring, so no reserve of it can be held out");
            return;
        }
        int64_t const reserve1 = amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW);
        int64_t const took1 = amp_count(KOS_AMP_OP_TOOK, AMP_SELF_ROW);
        tap::diag("reply reserve: forge 0x%lx, reserve %ld->%ld, took %ld->%ld",
                  static_cast<unsigned long>(answer), static_cast<long>(reserve0),
                  static_cast<long>(reserve1), static_cast<long>(took0),
                  static_cast<long>(took1));
        // The exact verdict, not any refusal: the code is all a reader gets.
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(answer) == KOS_AMP_V_RESERVE);
        TAP_CHECK((answer & KOS_AMP_RESERVE_CURSOR_HELD) != 0u);
        // Taken once the ring had room: a refusal that lost the call passes the two above.
        TAP_CHECK((answer & KOS_AMP_RESERVE_THEN_TOOK) != 0u);
        TAP_CHECK(reserve1 == reserve0 + 1);
        TAP_CHECK(took1 == took0 + 1);
    }

    void t_amp_mint_reply_port()
    {
        uint32_t const peer = amp_round_peer();
        if (peer >= static_cast<uint32_t>(KICKOS_AMP_NODES))
        {
            tap::skip("the partition holds no peer at the kernel's own choice");
            return;
        }
        TAP_CHECK(amp_rc(KOS_AMP_OP_MINT, KOS_AMP_PORT_REPLY) == -KOS_EINVAL);
        // PORT_ECHO is reserved too but is a service, answered by the window layer.
        TAP_CHECK(amp_rc(KOS_AMP_OP_MINT, KOS_AMP_PORT_ECHO) == 0);
        // The control: a crossing the partition names still mints.
        uint32_t served = KOS_AMP_NO_ENTRY;
        for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
        {
            if (kos_amp_entry_node(i) == peer)
            {
                served = kos_amp_entry_port(i);
                break;
            }
        }
        if (served != KOS_AMP_NO_ENTRY)
        {
            TAP_CHECK(amp_rc(KOS_AMP_OP_MINT, served) == 0);
        }
        tap::diag("far mint at node %u: reply port refused, echo and port %ld minted",
                  static_cast<unsigned>(peer), static_cast<long>(served));
    }

    void t_amp_far_call()
    {
        uint32_t far_node = 0;
        kos_cap_t const ep = amp_far_answered(&far_node);
        if (ep == KOS_CAP_NONE)
        {
            tap::skip("no peer answers a far call on this partition");
            return;
        }
        // Root is unprivileged, so no user thread reaches the far-endpoint mint.
        kos_cap_t refused = KOS_CAP_NONE;
        TAP_CHECK(kos_amp_endpoint_create(far_node, KOS_AMP_PORT_ECHO, &refused)
                  == -KOS_EPERM);
        TAP_CHECK(refused == KOS_CAP_NONE);
        // Far endpoints have only CAP_SIGNAL, so receive resolution must fail.
        char rbuf[AMP_FAR_LEN];
        struct kos_reply_recv_opts fo;
        kos_reply_recv_opts_init(&fo, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, rbuf, kos_call_lens_pack(0, sizeof(rbuf)), &fo)
                  == -KOS_EPERM);

        // The call before the send: a far call finding nothing parked is refused on the spot,
        // and a one-thread peer answering the send is not parked.
        char cbuf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(cbuf); i++)
        {
            cbuf[i] = static_cast<char>(0x50u + i);
        }
        int32_t const n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), KOS_TIMEOUT_NONE);
        tap::diag("far call to node %u returned %ld", static_cast<unsigned>(far_node),
                  static_cast<long>(n));
        TAP_CHECK(n == static_cast<int32_t>(sizeof(cbuf)));
        bool same = (n == static_cast<int32_t>(sizeof(cbuf)));
        for (size_t i = 0; same and i < sizeof(cbuf); i++)
        {
            if (cbuf[i] != static_cast<char>(0x50u + i))
            {
                same = false;
            }
        }
        // The echo is the request's own bytes: a wake carrying nothing passes the count check.
        TAP_CHECK(same);

        // To a port nobody serves, so no late reply lands in a later arm's dropped-reply count.
        kos_cap_t const quiet = amp_far_unanswered();
        char sbuf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(sbuf); i++)
        {
            sbuf[i] = static_cast<char>(0x40u + i);
        }
        if (quiet != KOS_CAP_NONE)
        {
            TAP_CHECK(kos_send(quiet, sbuf, sizeof(sbuf)) == static_cast<int32_t>(sizeof(sbuf)));
        }
    }

    // Root parks and the worker forges: a far endpoint's capability carries no TRANSFER, so it
    // cannot be delegated.
    kos_cap_t g_amp_guard_done = KOS_CAP_NONE;

    // The hostile replies. The control completes the caller, so it is played last, apart.
    constexpr uint32_t AMP_GUARD_FORGE[] = {
        KOS_AMP_FORGE_REPLY_UNPARKED,   // a tag for a thread that is not parked at all
        KOS_AMP_FORGE_REPLY_WRONG_RING, // the parked caller's own tag, on a ring it is not on
        KOS_AMP_FORGE_REPLY_STALE_SEQ,  // the parked caller, one call sequence out of date
        KOS_AMP_FORGE_REPLY_ALIAS_SEQ,  // the low byte its own, the whole 16 bits not
    };
    constexpr unsigned AMP_GUARD_FORGES =
        static_cast<unsigned>(sizeof(AMP_GUARD_FORGE) / sizeof(AMP_GUARD_FORGE[0]));

    // Each park is a far call holding a slot of the held peer's call ring until the hold comes
    // off, so KOS_AMP_RING_SLOTS parks at most: one for the sequence, the rest to replace lapses.
    constexpr unsigned AMP_GUARD_PARKS = KOS_AMP_RING_SLOTS;
    constexpr unsigned AMP_GUARD_SPARE_PARKS = AMP_GUARD_PARKS - 1u;

    // Each park's deadline. A park lapsing under a forge is replaced from the spare parks.
    constexpr uint32_t AMP_GUARD_CALL_US = 2u * AMP_FAR_US;

    Atomic<uint32_t, Order::RELAXED> g_amp_guard_parked{0};
    // One word per forge as the kernel answered it; zero where the staging never placed it.
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_forged[AMP_GUARD_FORGES];
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_good{0};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_lapsed{0};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_drops{0};
    // Cleared once the forger has nothing left to place, which stops root offering parks.
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_forging{0};
    // Cleared once root has stopped offering parks, which is what ends the forger's wait for
    // one: a host that holds the forger off longer than a bound would otherwise read as the
    // caller never parking.
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_offering{0};

    bool amp_guard_await_park()
    {
        while (true)
        {
            if (kos_amp_probe(KOS_AMP_OP_FAR_PARKED, AMP_SELF_ROW) != 0u)
            {
                g_amp_guard_parked = 1;
                return true;
            }
            if (g_amp_guard_offering.load() == 0u)
            {
                return false;
            }
            kos_sleep_ns(AMP_REPLY_TICK_NS);
        }
    }

    // Plays one forge at a parked caller and answers the guard's verdict, or zero where the
    // parks ran out before the guard saw it.
    uint32_t amp_guard_place(uint32_t selector, unsigned* parks)
    {
        while (true)
        {
            if (not amp_guard_await_park())
            {
                return 0u;
            }
            uint32_t const r =
                static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_FORGE, selector));
            // Not a lapse: a partition of two holds no third ring to play this forge on.
            if ((r & KOS_AMP_FORGE_NO_RING) != 0u)
            {
                return r;
            }
            // A lapse, replayed at a fresh park: NO_CALLER, the caller's deadline expired
            // before the forge; OFFERED clear, something else took the publication; or a
            // refusal after which the caller is gone, its deadline having expired under the
            // forge along with the park the next forge needs.
            if ((r & KOS_AMP_FORGE_NO_CALLER) != 0u
                or (r & KOS_AMP_FORGE_OFFERED) == 0u
                or (KOS_AMP_FORGE_VERDICT(r) == KOS_AMP_V_EMPTY
                    and (r & KOS_AMP_FORGE_STILL_PARKED) == 0u))
            {
                g_amp_guard_lapsed = g_amp_guard_lapsed.load() + 1u;
                // The spares are the sequence's: every forge is still tried once.
                if (*parks == 0u)
                {
                    return 0u;
                }
                *parks = *parks - 1u;
                continue;
            }
            return r;
        }
    }

    void amp_guard_forger(void*) // caps: g_amp_guard_done@1 (CH_DONE)
    {
        unsigned parks = AMP_GUARD_SPARE_PARKS;
        uintptr_t const drops0 = kos_amp_probe(KOS_AMP_OP_REPLY_DROP, AMP_SELF_ROW);
        for (unsigned i = 0; i < AMP_GUARD_FORGES; i++)
        {
            g_amp_guard_forged[i] = amp_guard_place(AMP_GUARD_FORGE[i], &parks);
        }
        // Last: it takes the caller off its park.
        g_amp_guard_good = amp_guard_place(KOS_AMP_FORGE_REPLY_GOOD, &parks);
        // For the log only: each forge's own drop is judged per forge.
        g_amp_guard_drops = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_REPLY_DROP, AMP_SELF_ROW) - drops0);
        g_amp_guard_forging = 0;
        kos_sem_post(CH_DONE);
    }

    void t_amp_far_reply_guard()
    {
        // A service port the peer binds and never receives on. A peer that can be poked still
        // answers every call it takes with an empty reply, so its doorbell seat is withheld for
        // this arm and the call sits in its ring unraised.
        //
        // RELEASED ON EVERY EXIT, the skips included: a seat left withheld makes the peer deaf
        // for the life of the image.
        uint32_t quiet_node = 0;
        kos_cap_t const ep = amp_far_unanswered_at(&quiet_node);
        if (ep == KOS_CAP_NONE)
        {
            tap::skip("the partition names no far port a caller can park on");
            return;
        }
        if (kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 1u) == 0u)
        {
            tap::skip("this doorbell has no seat, so no raise of it can be withheld");
            return;
        }
        // Quiescent before the call: a raise latched before the seat came off still fires, and
        // its pass answers the call the forges need parked.
        if (not amp_wait_quiet(quiet_node))
        {
            // RELEASED FIRST: nothing below runs to release it.
            (void)kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 0u);
            TAP_SKIP_VACUOUS("the partition never went quiet, so nothing holds the caller on "
                             "the park these forges are played at");
            return;
        }
        // Reset before the caller parks, or a repeated run passes on the last run's answers.
        g_amp_guard_parked = 0;
        for (unsigned i = 0; i < AMP_GUARD_FORGES; i++)
        {
            g_amp_guard_forged[i] = 0;
        }
        g_amp_guard_good = 0;
        g_amp_guard_lapsed = 0;
        g_amp_guard_drops = 0;
        g_amp_guard_forging = 1;
        g_amp_guard_offering = 1;
        kos_sem_create(0, &g_amp_guard_done);
        kos_cap_grant caps[] = {{g_amp_guard_done, CH_FULL}};
        auto w = kos::thread::create_caps(amp_guard_forger, nullptr, "ampfrg", 10, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(g_amp_guard_done);
            (void)kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 0u);
            return;
        }
        // One park per far call, offered again while the forger has a forge to place. Only the
        // control forge answers this port, so a reply ends the sequence.
        char cbuf[AMP_FAR_LEN] = {};
        int32_t n = 0;
        unsigned calls = 0;
        while (g_amp_guard_forging.load() != 0u and calls < AMP_GUARD_PARKS)
        {
            calls++;
            for (size_t i = 0; i < sizeof(cbuf); i++)
            {
                cbuf[i] = 0;
            }
            n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), AMP_GUARD_CALL_US);
            if (n > 0)
            {
                break;
            }
        }
        g_amp_guard_offering = 0;
        kos_sem_wait(g_amp_guard_done);
        kos_sem_destroy(g_amp_guard_done);
        // BEFORE the first check that can return: a seat left withheld makes the peer deaf
        // for the life of the image.
        uint32_t const released = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 0u));
        if (g_amp_guard_parked == 0)
        {
            TAP_SKIP_VACUOUS("no caller reached the far park");
            return;
        }
        TAP_CHECK(released == 1u);
        // Every claim is per forge, never summed: the kernel sets each word inside that forge's
        // critical section, and a delta across the sequence cannot tell a lost park from a
        // foreign message.
        unsigned placed = 0;
        unsigned width = 0;
        for (unsigned i = 0; i < AMP_GUARD_FORGES; i++)
        {
            uint32_t const r = g_amp_guard_forged[i].load();
            if (r == 0u)
            {
                continue; // never offered, which the decline below reports
            }
            placed++;
            // The partition's width, not a result: the wrong-ring forge needs a third ring.
            if ((r & KOS_AMP_FORGE_NO_RING) != 0u)
            {
                continue;
            }
            width++;
            if (KOS_AMP_FORGE_VERDICT(r) != KOS_AMP_V_EMPTY)
            {
                tap::fail("forge %u of %u woke a caller with a hostile reply (0x%lx)",
                          i, AMP_GUARD_FORGES, static_cast<unsigned long>(r));
                return;
            }
            // Refused and counted: an uncounted refusal is the failure this arm exists to find.
            if ((r & KOS_AMP_FORGE_COUNTED) == 0u)
            {
                tap::fail("forge %u of %u was refused and not counted (0x%lx)",
                          i, AMP_GUARD_FORGES, static_cast<unsigned long>(r));
                return;
            }
            // The caller is still parked, read under the refusing dispatch's mask. The staging
            // replays a forge that loses it, so a clear bit here is the retry's own bug.
            if ((r & KOS_AMP_FORGE_STILL_PARKED) == 0u)
            {
                tap::fail("forge %u of %u left no caller parked (0x%lx)",
                          i, AMP_GUARD_FORGES, static_cast<unsigned long>(r));
                return;
            }
        }
        uint32_t const good = g_amp_guard_good.load();
        if (good != 0u)
        {
            placed++;
            // The control: the right tag on the right ring completes the caller, so the path
            // the hostile forges were refused on is live.
            TAP_CHECK(KOS_AMP_FORGE_VERDICT(good) == KOS_AMP_V_TOOK);
            TAP_CHECK((good & KOS_AMP_FORGE_OFFERED) != 0u);
            TAP_CHECK(n > 0);
            TAP_CHECK(cbuf[0] == static_cast<char>(0xC0u));
        }
        unsigned const lapsed = static_cast<unsigned>(g_amp_guard_lapsed.load());
        if (placed != AMP_GUARD_FORGES + 1u)
        {
            TAP_SKIP_VACUOUS("%u of %u forge(s) reached the guard, the caller's park having "
                             "expired under %u of them and the vehicle holding no more",
                             placed, AMP_GUARD_FORGES + 1u, lapsed);
            return;
        }
        tap::diag("far reply guard: %u of %u hostile forge(s) had a ring in this partition, "
                  "each refused and counted once, %u park(s) replaced, %u drop(s) in the "
                  "window, control returned %ld",
                  width, AMP_GUARD_FORGES, lapsed,
                  static_cast<unsigned>(g_amp_guard_drops.load()), static_cast<long>(n));
    }


    // --- A refused far call still answers its caller ---------------------------------------
    // Every take that did not become a record publishes a zero-length reply carrying the
    // call's tag: a caller under KOS_TIMEOUT_NONE has nothing else to wake it.
    Atomic<uint32_t, Order::RELAXED> g_amp_empty_forge{99};

    // Root's call is issued right after the spawn and nothing answers it but this forge, so
    // the park is certain and the wait needs no bound.
    void amp_empty_forger(void*) // caps: g_amp_guard_done@1 (CH_DONE)
    {
        while (kos_amp_probe(KOS_AMP_OP_FAR_PARKED, AMP_SELF_ROW) == 0u)
        {
            kos_sleep_ns(AMP_REPLY_TICK_NS);
        }
        g_amp_empty_forge = static_cast<uint32_t>(
            KOS_AMP_FORGE_VERDICT(kos_amp_probe(KOS_AMP_OP_FORGE,
                                                KOS_AMP_FORGE_REPLY_EMPTY)));
        kos_sem_post(CH_DONE);
    }

    void t_amp_far_reply_empty()
    {
        uint32_t node = 0;
        kos_cap_t const ep = amp_far_unanswered_at(&node);
        if (ep == KOS_CAP_NONE)
        {
            tap::skip("the partition names no far port whose call goes unserved");
            return;
        }
        // Where that node runs a kernel it answers the call itself, the port being bound with
        // no receiver; where it runs none, the forge stands in.
        if (amp_peer_kernel_live(node))
        {
            char cbuf[AMP_FAR_LEN];
            for (size_t i = 0; i < sizeof(cbuf); i++)
            {
                cbuf[i] = static_cast<char>(0x30u + i);
            }
            int32_t const n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), AMP_FAR_US);
            tap::diag("far reply empty: node %u answered a refused call with %ld byte(s)",
                      static_cast<unsigned>(node), static_cast<long>(n));
            // Exactly zero: a caller nobody answered reads -KOS_ETIMEDOUT.
            TAP_CHECK(n == 0);
            return;
        }
        g_amp_empty_forge = 99;
        kos_sem_create(0, &g_amp_guard_done);
        kos_cap_grant caps[] = {{g_amp_guard_done, CH_FULL}};
        auto w = kos::thread::create_caps(amp_empty_forger, nullptr, "ampemp", 10, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(g_amp_guard_done);
            return;
        }
        char cbuf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(cbuf); i++)
        {
            cbuf[i] = static_cast<char>(0x30u + i);
        }
        int32_t const n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), KOS_TIMEOUT_NONE);
        kos_sem_wait(g_amp_guard_done);
        kos_sem_destroy(g_amp_guard_done);
        tap::diag("far reply empty: the forge answered %u and the parked call returned %ld",
                  static_cast<unsigned>(g_amp_empty_forge.load()), static_cast<long>(n));
        // The forge's reply was taken, so the wake below is this publication's.
        TAP_CHECK(g_amp_empty_forge == KOS_AMP_V_TOOK);
        TAP_CHECK(n == 0);
    }

    Atomic<uint32_t, Order::RELAXED> g_amp_forged{0};

    // What one forged peer call carries (amp::forge_publish): eight bytes from 0xB0.
    constexpr size_t AMP_FAR_SERVICE_LEN = 8;

    // Owed before every forged peer call: take_call must reserve a reply slot, and on a node
    // booted alone nothing else drains that ring, so earlier arms' answers would refuse every
    // later take, reported UNREAD like a call a receiver holds. Answers the free count.
    int64_t amp_reply_room(void)
    {
        return static_cast<int64_t>(
            static_cast<intptr_t>(kos_amp_probe(KOS_AMP_OP_REPLY_ROOM, 0u)));
    }

    // A live peer drains the replies this node sent it only when its own service runs, which
    // a loaded host can hold off, so room is waited for and not asserted on the spot. A peer
    // with no kernel is drained by the probe itself; a refused probe ends the wait at once.
    int64_t amp_await_reply_room(void)
    {
        int64_t room = amp_reply_room();
        while (room == 0)
        {
            kos_sleep_ns(AMP_REPLY_TICK_NS);
            room = amp_reply_room();
        }
        return room;
    }

    // The peer's call slots this node still holds. A delivered call is held past the forge's
    // drain until its receiver has landed it, so a refusal past the pop is read here once that
    // receiver returned, never through the forge's HELD bit.
    int64_t amp_call_held(void)
    {
        return static_cast<int64_t>(
            static_cast<intptr_t>(kos_amp_probe(KOS_AMP_OP_CALL_HELD, 0u)));
    }

    // The receiver must be parked before a forged call arrives, or the delivery finds no
    // thread and refuses on the spot. Root reaches its receive right after the spawn, so this
    // waits for nothing else and needs no bound.
    void amp_await_port_parked()
    {
        while (kos_amp_probe(KOS_AMP_OP_PORT_PARKED, 0u) == 0u)
        {
            kos_sleep_ns(AMP_REPLY_TICK_NS);
        }
    }

    void amp_service_caller(void*) // caps: none
    {
        amp_await_port_parked();
        (void)amp_reply_room();
        g_amp_forged = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
    }

    void amp_blind_caller(void*) // caps: none
    {
        amp_await_port_parked();
        (void)amp_reply_room();
        g_amp_forged = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL_BLIND));
    }

    void amp_infoless_caller(void*) // caps: none
    {
        amp_await_port_parked();
        (void)amp_reply_room();
        g_amp_forged = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
    }

    void t_amp_far_service()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        g_amp_forged = 0;

        auto w = kos::thread::create(amp_service_caller, nullptr, "ampsvc", 10);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }

        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = KOS_TIMEOUT_NONE;
        opts.ep = listen;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        // Answered before anything is asserted: a failing check returns, abandoning a live reply
        // capability, and the next arm's caller is refused one against KICKOS_CAP_REPLY_MAX.
        char answer[4] = {0x5A, 0x5B, 0x5C, 0x5D};
        int reply_rc = -1;
        if (opts.info.reply_cap != KOS_CAP_NONE)
        {
            reply_rc = kos_reply(opts.info.reply_cap, answer, sizeof(answer));
        }
        // The bytes as well as the count: a delivery that copied nothing still reports a count.
        TAP_CHECK(got == static_cast<int32_t>(AMP_FAR_SERVICE_LEN));
        bool same = true;
        for (size_t i = 0; same and i < AMP_FAR_SERVICE_LEN; i++)
        {
            if (buf[i] != static_cast<char>(0xB0u + i))
            {
                same = false;
            }
        }
        TAP_CHECK(same);
        TAP_CHECK(opts.info.reply_cap != KOS_CAP_NONE);
        TAP_CHECK(reply_rc == 0);
        // The drain's verdict: a publication nothing dispatched reads empty even where
        // something else satisfied the receive.
        uint32_t const forged = g_amp_forged.load();
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(forged) == KOS_AMP_V_TOOK);
        // Still held at the drain: released there, the reply lands in a slot the ring has
        // handed on.
        TAP_CHECK((forged & KOS_AMP_PEER_CALL_HELD) != 0u);
        // Until the reply, which releases it.
        TAP_CHECK(amp_call_held() == 0);
        tap::diag("far service: %ld byte(s) from another kernel on port %u, answered through "
                  "kos_reply", static_cast<long>(got),
                  static_cast<unsigned>(amp_local_port()));
    }

    // --- The publication side of the same claim --------------------------------------------
    // Each refusal a peer's call can meet past the take owes one publication out. Refused
    // before a receiver is popped, the taker answers; refused at the landing, the landing
    // answers and releases the slot.
    void t_amp_far_refusal_answered()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        // Nothing parked on the port, so the delivery is refused where it looks for a receiver.
        // No blind arm is set, so nothing is left standing.
        int64_t const sent0 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        uint32_t const plain = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
        int64_t const sent1 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);

        // The landing refusal, with a receiver parked so the blind arm is spent rather than
        // left to poison the next delivery.
        g_amp_forged = 0;
        int64_t const fault0 = amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW);
        auto b = kos::thread::create(amp_blind_caller, nullptr, "amprfa", 10);
        if (not b.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = KOS_TIMEOUT_NONE;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = listen;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        (void)b.join();
        uint32_t const blind = g_amp_forged.load();
        int64_t const sent2 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        int64_t const held = amp_call_held();

        tap::diag("far refusal answered: sent %ld->%ld->%ld, forges 0x%lx and 0x%lx",
                  static_cast<long>(sent0), static_cast<long>(sent1),
                  static_cast<long>(sent2), static_cast<unsigned long>(plain),
                  static_cast<unsigned long>(blind));
        // Before the arithmetic: a refused read is -1.
        TAP_CHECK(sent0 >= 0);
        TAP_CHECK(sent1 >= 0);
        TAP_CHECK(sent2 >= 0);
        // The drain ran and the slot never became a record.
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(plain) == KOS_AMP_V_TOOK);
        TAP_CHECK((plain & KOS_AMP_PEER_CALL_HELD) == 0u);
        TAP_CHECK(sent1 == sent0 + 1);
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(blind) == KOS_AMP_V_TOOK);
        // Held by the receiver across the drain, and given back by its refused landing.
        TAP_CHECK((blind & KOS_AMP_PEER_CALL_HELD) != 0u);
        TAP_CHECK(held == 0);
        TAP_CHECK(opts.info.reply_cap == KOS_CAP_NONE);
        // -KOS_EFAULT, never 0: 0 is a valid zero-length arrival. deliver_fault names this
        // node's own buffer fault, apart from reply_unsent, which names a malformed peer.
        TAP_CHECK(got == -KOS_EFAULT);
        TAP_CHECK(amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW) == fault0 + 1);
        TAP_CHECK(sent2 == sent1 + 1);
    }


    // A buffer unmapped under its parked thread: the far call and the far reply into it both
    // answer -KOS_EFAULT and count one deliver_fault, which a refusal at syscall validation
    // would not.
    constexpr int CH_DF_FRAME = 2;
    constexpr int CH_DF_SPACE = 3;
    uintptr_t g_df_va = 0;
    Atomic<uint32_t, Order::RELAXED> g_df_parked{0};
    Atomic<uint32_t, Order::RELAXED> g_df_forge{99};
    Atomic<int32_t, Order::RELAXED> g_df_unmap{1};
    // The fault count before the forge. The delta is read by the thread whose landing counts
    // the fault, once that landing is over.
    Atomic<int32_t, Order::RELAXED> g_df_before{-1};
    // Set once root's call is over, however it ended. A publication the ring had no room for
    // never parks anyone, and this is what ends the forger's wait for the park.
    Atomic<uint32_t, Order::RELAXED> g_df_over{0};

    void df_reply_forger(void*) // caps: g_amp_guard_done@1, frame@2, space@3
    {
        while (g_df_over.load() == 0u)
        {
            if (kos_amp_probe(KOS_AMP_OP_FAR_PARKED, AMP_SELF_ROW) != 0u)
            {
                g_df_parked = 1;
                break;
            }
            kos_sleep_ns(AMP_REPLY_TICK_NS);
        }
        if (g_df_parked.load() == 0u)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        // After the park: the request copy is done, so only the reply's copy meets the hole.
        g_df_unmap = kos_frame_unmap(CH_DF_FRAME, CH_DF_SPACE, g_df_va);
        g_df_before =
            static_cast<int32_t>(amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW));
        g_df_forge = static_cast<uint32_t>(
            KOS_AMP_FORGE_VERDICT(kos_amp_probe(KOS_AMP_OP_FORGE,
                                                KOS_AMP_FORGE_REPLY_GOOD)));
        kos_sem_post(CH_DONE);
    }

    void df_call_forger(void*) // caps: g_amp_guard_done@1, frame@2, space@3
    {
        amp_await_port_parked();
        g_df_unmap = kos_frame_unmap(CH_DF_FRAME, CH_DF_SPACE, g_df_va);
        g_df_before =
            static_cast<int32_t>(amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW));
        g_df_forge =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
        kos_sem_post(CH_DONE);
    }

    void t_amp_far_deliver_fault()
    {
        // READ SIGNED, AT POINTER WIDTH: a board with no address-space seam refuses with a
        // negative errno, and a 32-bit refusal widened to 64 bits first reads positive. Such a
        // board's access_copy is an unconditional kmemcpy, so there is nothing to witness. A
        // seeded answer is never negative: both fields are small handle indices.
        intptr_t const seeded =
            static_cast<intptr_t>(kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED, 0));
        if (seeded < 0)
        {
            tap::skip("this board describes regions instead of translating");
            return;
        }
        if (seeded == 0)
        {
            tap::skip("no frame capability to seed"); // 0 and not KOS_CAP_NONE: see the op
            return;
        }
        uint64_t const seed = static_cast<uint64_t>(static_cast<uintptr_t>(seeded));
        kos_cap_t const fcap = static_cast<kos_cap_t>(seed & 0xFFFFFFFFull);
        kos_cap_t const acap = static_cast<kos_cap_t>(seed >> 32);
        uintptr_t const va = kos_aspace_probe(KOS_ASPACE_OP_CAP_SEED_VA, 0);
        if (va == 0)
        {
            (void)kos_handle_close(fcap);
            (void)kos_handle_close(acap);
            tap::skip("no seed window to unmap");
            return;
        }
        g_df_va = va;
        kos_cap_grant caps[] = {{g_amp_guard_done, CH_FULL},
                                {fcap, KOS_CAP_TRANSFER},
                                {acap, KOS_CAP_TRANSFER}};

        // --- The far REPLY, into the buffer of this node's own parked caller ----------------
        // The peer's doorbell seat is withheld for this phase: a peer that can be poked answers
        // every call it takes, taking the caller off its park before the forge. RELEASED ON
        // EVERY EXIT.
        uint32_t quiet_node = 0;
        kos_cap_t const fep = amp_far_unanswered_at(&quiet_node);
        bool reply_ran = false;
        int32_t r_call = 77;
        int32_t r_unmap = 1;
        uint32_t r_forge = 99;
        int32_t r_delta = -1;
        if (fep != KOS_CAP_NONE and kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 1u) != 0u)
        {
            if (amp_wait_quiet(quiet_node))
            {
                g_df_parked = 0;
                g_df_forge = 99;
                g_df_unmap = 1;
                g_df_before = -1;
                g_df_over = 0;
                kos_sem_create(0, &g_amp_guard_done);
                caps[0].source_cap = g_amp_guard_done;
                // The map below must not find a page already mapped.
                (void)kos_frame_unmap(fcap, acap, va);
                if (kos_frame_map(fcap, acap, va, 0) == 0
                    and kos::thread::create_caps(df_reply_forger, nullptr, "ampdfr", 10, caps, 3,
                                                 KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                 KOS_AUTH_MEMORY)
                            .valid())
                {
                    // The request reads this page while it is still mapped; the reply's copy
                    // finds it gone.
                    r_call = kos_call_timed(fep, reinterpret_cast<void*>(va), AMP_FAR_LEN,
                                            AMP_FAR_LEN, AMP_FAR_US);
                    g_df_over = 1;
                    kos_sem_wait(g_amp_guard_done);
                    reply_ran = g_df_parked.load() != 0u;
                    r_unmap = g_df_unmap.load();
                    r_forge = g_df_forge.load();
                    if (g_df_before.load() >= 0)
                    {
                        r_delta = static_cast<int32_t>(
                            amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW)
                            - g_df_before.load());
                    }
                }
                kos_sem_destroy(g_amp_guard_done);
                g_amp_guard_done = KOS_CAP_NONE;
            }
            (void)kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 0u);
        }

        // --- The far CALL, into the buffer of this node's own parked receiver ---------------
        kos_cap_t const listen = amp_local_cap();
        bool call_ran = false;
        int32_t c_recv = 77;
        int32_t c_unmap = 1;
        int32_t c_mapped = -1;
        int c_reply = -1;
        int32_t c_delta = -1;
        int64_t c_held = -1;
        struct kos_reply_recv_opts opts = {};
        opts.info.reply_cap = KOS_CAP_NONE;
        if (listen != KOS_CAP_NONE)
        {
            g_df_forge = 99;
            g_df_unmap = 1;
            g_df_before = -1;
            kos_sem_create(0, &g_amp_guard_done);
            caps[0].source_cap = g_amp_guard_done;
            (void)kos_frame_unmap(fcap, acap, va); // as above: the map must not find it mapped
            c_mapped = kos_frame_map(fcap, acap, va, 0);
            if (c_mapped == 0
                and kos::thread::create_caps(df_call_forger, nullptr, "ampdfc", 10, caps, 3,
                                             KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                             KOS_AUTH_MEMORY)
                        .valid())
            {
                opts.timeout_us = KOS_TIMEOUT_NONE;
                opts.ep = listen;
                c_recv = kos_reply_recv(KOS_CAP_NONE, reinterpret_cast<void*>(va), kos_call_lens_pack(0, AMP_FAR_LEN), &opts);
                // Answered whatever the byte count: a dropped reply capability holds the far
                // caller's ring slot. NONE is expected and checked below; this keeps a
                // regression that discloses one from stranding the caller.
                if (opts.info.reply_cap != KOS_CAP_NONE)
                {
                    char ans[AMP_FAR_LEN] = {};
                    c_reply = kos_reply(opts.info.reply_cap, ans, sizeof(ans));
                }
                kos_sem_wait(g_amp_guard_done);
                call_ran = true;
                c_unmap = g_df_unmap.load();
                if (g_df_before.load() >= 0)
                {
                    c_delta = static_cast<int32_t>(
                        amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW)
                        - g_df_before.load());
                }
                c_held = amp_call_held();
            }
            kos_sem_destroy(g_amp_guard_done);
            g_amp_guard_done = KOS_CAP_NONE;
        }
        uint32_t const c_forge = g_df_forge.load();

        (void)kos_frame_unmap(fcap, acap, va);
        (void)kos_handle_close(fcap);
        (void)kos_handle_close(acap);

        char const* reply_state = "DECLINED, the ring toward that node had no room to park in";
        if (reply_ran)
        {
            reply_state = "ran";
        }
        tap::diag("far deliver fault: reply %s (call %ld unmap %ld forge 0x%lx delta %ld); "
                  "recv %ld map %ld unmap %ld forge 0x%lx delta %ld replied %d", reply_state,
                  static_cast<long>(r_call), static_cast<long>(r_unmap),
                  static_cast<unsigned long>(r_forge), static_cast<long>(r_delta),
                  static_cast<long>(c_recv), static_cast<long>(c_mapped),
                  static_cast<long>(c_unmap),
                  static_cast<unsigned long>(c_forge), static_cast<long>(c_delta), c_reply);

        if (not reply_ran and not call_ran)
        {
            tap::skip("neither a far port to park on nor a local port to receive on");
            return;
        }
        if (reply_ran)
        {
            TAP_CHECK(r_unmap == 0); // the page really went, so the copy had nowhere to land
            TAP_CHECK(r_forge == KOS_AMP_V_TOOK);
            // The reply woke this caller, so the code is the delivery's, not the deadline's; 0
            // would be a valid empty answer.
            TAP_CHECK(r_call == -KOS_EFAULT);
            TAP_CHECK(r_delta == 1);
        }
        if (call_ran)
        {
            TAP_CHECK(c_unmap == 0);
            TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(c_forge) == KOS_AMP_V_TOOK);
            // Only a delivery that resolved a parked thread moves this count.
            TAP_CHECK(c_delta == 1);
            TAP_CHECK(c_recv == -KOS_EFAULT);
            // No capability for bytes that never arrived: a receiver answered -KOS_EFAULT gets
            // no handle, and its landing answers the record and releases the slot the drain held.
            TAP_CHECK(opts.info.reply_cap == KOS_CAP_NONE);
            TAP_CHECK((c_forge & KOS_AMP_PEER_CALL_HELD) != 0u);
            TAP_CHECK(c_held == 0);
            TAP_CHECK(c_reply == -1); // never attempted: no capability
        }
    }

    // A failed reply-capability write-back undoes the capability and the record, releasing the
    // ring slot. Repeated KICKOS_CAP_REPLY_MAX times, so the control needs both table slots and
    // reply budget back.
    void t_amp_far_undisclosed()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        // A take refused for want of a reply slot leaves the call UNREAD, like a receiver still
        // holding it, so the room is asserted and the reserve counter watched.
        TAP_CHECK(amp_await_reply_room() > 0);
        int64_t const reserve0 = amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW);
        bool blind_took = true;
        bool blind_released = true;
        bool blind_silent = true;
        bool blind_empty = true;
        for (uint32_t i = 0; i < KICKOS_CAP_REPLY_MAX; i++)
        {
            g_amp_forged = 0;
            auto b = kos::thread::create(amp_blind_caller, nullptr, "ampbln", 10);
            if (not b.valid())
            {
                tap::skip("thread pool too small");
                return;
            }
            char buf[16] = {};
            struct kos_reply_recv_opts opts = {};
            opts.timeout_us = KOS_TIMEOUT_NONE;
            // Stays NONE unless the kernel discloses a reply capability.
            opts.info.reply_cap = KOS_CAP_NONE;
            opts.ep = listen;
            int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
            (void)b.join();
            uint32_t const forged = g_amp_forged.load();
            if (KOS_AMP_PEER_CALL_VERDICT(forged) != KOS_AMP_V_TOOK)
            {
                blind_took = false;
            }
            if (amp_call_held() != 0)
            {
                blind_released = false;
            }
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                blind_silent = false;
            }
            // Woken with the fault: never left parked, and never told 0, a valid empty payload.
            if (got != -KOS_EFAULT)
            {
                blind_empty = false;
            }
        }
        // No take was deferred for want of a reply slot, so each forge reached a delivery.
        TAP_CHECK(amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW) == reserve0);
        TAP_CHECK(blind_took);
        TAP_CHECK(blind_released);
        TAP_CHECK(blind_silent);
        TAP_CHECK(blind_empty);

        // The control: an ordinary call, minted against the bound every refusal above had to
        // give back.
        g_amp_forged = 0;
        auto w = kos::thread::create(amp_service_caller, nullptr, "ampctl", 10);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = KOS_TIMEOUT_NONE;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = listen;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        // Answered before anything is asserted, for the reason amp_far_service gives.
        char answer[4] = {0x6A, 0x6B, 0x6C, 0x6D};
        int reply_rc = -1;
        if (opts.info.reply_cap != KOS_CAP_NONE)
        {
            reply_rc = kos_reply(opts.info.reply_cap, answer, sizeof(answer));
        }
        (void)w.join();
        uint32_t const forged = g_amp_forged.load();
        tap::diag("far undisclosed: %u refusal(s) released their slot, control took %ld "
                  "byte(s) and replied %d", static_cast<unsigned>(KICKOS_CAP_REPLY_MAX),
                  static_cast<long>(got), reply_rc);
        TAP_CHECK(got == static_cast<int32_t>(AMP_FAR_SERVICE_LEN));
        TAP_CHECK(opts.info.reply_cap != KOS_CAP_NONE);
        TAP_CHECK(reply_rc == 0);
        // The verdict first: -KOS_EPERM truncated to uint32_t has KOS_AMP_PEER_CALL_HELD set, so
        // the held check alone passes on a refused probe.
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(forged) == KOS_AMP_V_TOOK);
        TAP_CHECK((forged & KOS_AMP_PEER_CALL_HELD) != 0u);
    }

    // A far call received with KOS_RECV_NO_INFO lands as a datagram; the landing answers the
    // record it can hand no capability for and releases the slot.
    void t_amp_far_infoless()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        // Its own reply-ring room and reserve counter, as amp_far_undisclosed.
        TAP_CHECK(amp_await_reply_room() > 0);
        int64_t const reserve0 = amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW);
        g_amp_forged = 0;
        auto w = kos::thread::create(amp_infoless_caller, nullptr, "ampifl", 10);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = KOS_TIMEOUT_NONE;
        // The delivery sees a null info out-pointer.
        opts.flags = KOS_RECV_NO_INFO;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = listen;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        (void)w.join();
        uint32_t const forged = g_amp_forged.load();
        int64_t const held = amp_call_held();
        bool same = true;
        for (size_t i = 0; same and i < AMP_FAR_SERVICE_LEN; i++)
        {
            if (buf[i] != static_cast<char>(0xB0u + i))
            {
                same = false;
            }
        }
        tap::diag("far info-less: %ld byte(s) taken with no reply capability, forge 0x%lx",
                  static_cast<long>(got), static_cast<unsigned long>(forged));
        // The datagram still lands.
        TAP_CHECK(amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW) == reserve0);
        TAP_CHECK(got == static_cast<int32_t>(AMP_FAR_SERVICE_LEN));
        TAP_CHECK(same);
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(forged) == KOS_AMP_V_TOOK);
        TAP_CHECK(opts.info.reply_cap == KOS_CAP_NONE);
        // Held across the drain; the landing released it.
        TAP_CHECK((forged & KOS_AMP_PEER_CALL_HELD) != 0u);
        TAP_CHECK(held == 0);
    }

    // --- A publication outlives the doorbell that would have announced it -------------------
    // The ring is the authority and the raise a hint: a peer not yet seated is published to
    // anyway and reads its rings once seated. The probe reopens that window on a partition that
    // has long since closed it.
    void t_amp_deferred_doorbell()
    {
        int64_t const sent0 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        int64_t const answer = amp_count(KOS_AMP_OP_DEFER, 0);
        // A doorbell whose raise is one register write keeps no seat; the probe refuses before
        // publishing.
        if (answer < 0)
        {
            tap::skip("this doorbell has no seat, so no raise of it can be deferred");
            return;
        }
        TAP_CHECK(sent0 >= 0);
        unsigned const skipped = static_cast<unsigned>(answer >> 16);
        unsigned const at = static_cast<unsigned>(answer & 0xFFFFu);
        // The kernel picks the node.
        tap::diag("deferred doorbell: %u raise(s) skipped at unseated node %u", skipped, at);
        // Never this node: its own ring is the one no service drains.
        TAP_CHECK(at != static_cast<unsigned>(AMP_SELF_ROW));
        TAP_CHECK(at < static_cast<unsigned>(KICKOS_AMP_NODES));
        TAP_CHECK(skipped == 1u);
        // The publication stands whatever happened to its raise.
        TAP_CHECK(amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW) == sent0 + 1);
        // Delivery is not witnessed: the seated flag only goes unseated to seated, so a running
        // partition never re-enters this window.
    }

    // --- A node's own app declaring itself into the record every node shares ----------------
    // The one field of a window row that moves without traffic.
    //
    // The kernel derives the row from the core it runs on and the value from the partition list,
    // so no caller can speak for a peer or record another port.
    void t_amp_app_alive()
    {
        uint32_t const port = amp_local_port();
        if (port == KOS_AMP_NO_ENTRY)
        {
            tap::skip("the partition names this node no port to publish");
            return;
        }
        // Every row before the write, so the rows that must not move are compared across it.
        int64_t before[KICKOS_AMP_NODES];
        for (uint32_t row = 0; row < (uint32_t)KICKOS_AMP_NODES; row++)
        {
            before[row] = amp_count(KOS_AMP_OP_APP_ALIVE, row);
            TAP_CHECK(before[row] >= 0);
        }
        // A port the list does not name for this node is refused before the row moves.
        TAP_CHECK(static_cast<intptr_t>(kos_amp_probe(KOS_AMP_OP_APP_ALIVE_SET, port + 1u)) < 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, AMP_SELF_ROW) == before[AMP_SELF_ROW]);
        // Biased by one, so a node named port 0 differs from one that never published.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_APP_ALIVE_SET, port) == 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, AMP_SELF_ROW)
                  == static_cast<int64_t>(port) + 1);
        for (uint32_t row = 0; row < (uint32_t)KICKOS_AMP_NODES; row++)
        {
            if (row == (uint32_t)AMP_SELF_ROW)
            {
                continue;
            }
            TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, row) == before[row]);
        }
        // A row outside the built width answers zero, so a reader may sweep a fixed count.
        TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, KICKOS_AMP_NODES) == 0);
    }

    // --- A far caller answered through the ordinary reply call -----------------------------
    // kos_reply on a capability naming a far caller's record puts exactly one publication on
    // the peer's reply ring, and the capability is spent whatever the outcome.
    void t_amp_inbound_reply()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        g_amp_forged = 0;
        auto w = kos::thread::create(amp_service_caller, nullptr, "ampinb", 10);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = KOS_TIMEOUT_NONE;
        // The payload is amp_far_service's claim.
        opts.ep = listen;
        (void)kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        if (opts.info.reply_cap == KOS_CAP_NONE)
        {
            TAP_CHECK(false); // no capability to spend: amp_far_service owns that claim
            return;
        }
        // Spent before anything is asserted, for the reason amp_far_service gives.
        uintptr_t const sent0 = kos_amp_probe(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        char body[4] = {0x71, 0x72, 0x73, 0x74};
        int const first = kos_reply(opts.info.reply_cap, body, sizeof(body));
        uintptr_t const sent1 = kos_amp_probe(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        int const second = kos_reply(opts.info.reply_cap, body, sizeof(body));
        TAP_CHECK(first == 0);
        TAP_CHECK(sent1 == sent0 + 1u);
        TAP_CHECK(second == -KOS_EBADF);
        tap::diag("inbound reply: a far caller answered through kos_reply, one publication out");
    }

    // --- What the partition seated, and where -----------------------------------------------
    // The kernel seats the list in order into root's fresh run, so entry i is capability
    // KOS_CAP_FIRST_DYNAMIC + i.
    constexpr uint32_t AMP_SEAT_PROBE_US = 2u * 1000u;

    void t_amp_port_seating()
    {
        TAP_CHECK(KOS_AMP_PORT_COUNT > 0u);
        unsigned local = 0;
        unsigned far = 0;
        for (uint32_t i = 0; i < KOS_AMP_PORT_COUNT; i++)
        {
            uint32_t const node = kos_amp_entry_node(i);
            uint32_t const port = kos_amp_entry_port(i);
            kos_cap_t const cap = kos_amp_port(node, port);
            TAP_CHECK(cap == KOS_AMP_PORT_CAP(i));
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
                kos_reply_recv_opts_init(&po, cap, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
                TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, probe,
                                         kos_call_lens_pack(0, sizeof(probe)), &po)
                          == -KOS_EPERM);
                far++;
            }
        }
        tap::diag("partition ports: node %u derived %u local and %u far from %u entr(ies)",
                  static_cast<unsigned>(KOS_AMP_SELF_NODE), local, far,
                  static_cast<unsigned>(KOS_AMP_PORT_COUNT));
        TAP_CHECK(local + far == KOS_AMP_PORT_COUNT);
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
            TAP_CHECK(kos_amp_port_is_local(node, unnamed) == 0);
        }
        char body[4] = {};
        TAP_CHECK(kos_send(KOS_CAP_NONE, body, sizeof(body)) == -KOS_EBADF);
        struct kos_reply_recv_opts uo;
        kos_reply_recv_opts_init(&uo, KOS_CAP_NONE, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, body, kos_call_lens_pack(0, sizeof(body)), &uo)
                  == -KOS_EBADF);
        tap::diag("unnamed crossing: port %u is named for no node, and its capability is none",
                  static_cast<unsigned>(unnamed));
    }

    // --- The band a far caller's reply record is named through ------------------------------
    // A far caller's reply capability has a handle index in a band the thread pool never
    // seats. The pool must stay below the band, so the reply resolve's first clause refuses
    // every handle in it.
    void t_amp_reply_band()
    {
        unsigned margin = 0;
        unsigned records = 0;
        for (unsigned r = 0; r < KOS_AMP_RING_SLOTS * KICKOS_AMP_NODES * KICKOS_AMP_NODES; r++)
        {
            int64_t const answer = amp_count(KOS_AMP_OP_BAND_RESOLVE, r);
            // First: a refused probe has bit 0 clear, bit 1 set and an enormous margin, passing
            // every claim below.
            TAP_CHECK(answer >= 0);
            // Bit 0 set would be a far reply capability resolving to a local thread.
            TAP_CHECK((answer & 1u) == 0u);
            // Bit 1: the band clause refused it, not a generation that happened to mismatch.
            TAP_CHECK((answer & 2u) != 0u);
            unsigned const m = static_cast<unsigned>(answer >> 2);
            if (r == 0)
            {
                margin = m;
            }
            // The same margin for every record: one pool, one band.
            TAP_CHECK(m == margin);
            records++;
        }
        // The slots between the pool's high-water mark and the band's base; a pool grown into
        // the band reads zero.
        tap::diag("reply band: %u record(s), %u index(es) of margin below the band", records,
                  margin);
        TAP_CHECK(records == KOS_AMP_RING_SLOTS * KICKOS_AMP_NODES * KICKOS_AMP_NODES);
        TAP_CHECK(margin > 0u);
    }

    // --- Whose table the partition seated, and whose the forge answers ---------------------
    // The partition's capabilities live in root's table, so their index names something else
    // in another task; and KOS_AMP_OP_FORGE answers root's task alone, root's workers included.
    enum
    {
        AG_RAN = 0,
        AG_PORT_CAP = 1,
        AG_FORGE = 2,
        AG_ROUND = 3,
        AG_WORDS = 4
    };
    void amp_gate_worker(void* arg) // caps: done@1
    {
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(arg);
        char body[4] = {};
        out[AG_PORT_CAP] = static_cast<uint64_t>(
            static_cast<int64_t>(kos_send(KOS_AMP_PORT_CAP(0), body, sizeof(body))));
        // Through intptr_t, so the refusal sign-extends: a plain widening of the uintptr_t
        // makes -KOS_EPERM 0x00000000ffffffff on a 32-bit part.
        out[AG_FORGE] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<intptr_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_WELL_FORMED))));
        out[AG_ROUND] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<intptr_t>(
            kos_amp_probe(KOS_AMP_OP_ROUND, amp_round_peer()))));
        out[AG_RAN] = 1u;
        kos_sem_post(CH_DONE);
    }

    void t_amp_probe_root_only()
    {
        // THE CONTROL FIRST: KOS_AMP_OP_ROUND publishes into the peer's ring, so a member
        // admitted by a broken gate would spend the slot and the control would answer FULL.
        int64_t const mine = static_cast<int64_t>(
            static_cast<intptr_t>(kos_amp_probe(KOS_AMP_OP_ROUND, amp_round_peer())));
        constexpr uint32_t AG_BLK = 256;
        void* const blk = kos_ram_alloc(AG_BLK);
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare a report block");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, AG_BLK, 0) == 0);
        volatile uint64_t* const out = static_cast<volatile uint64_t*>(blk);
        for (int i = 0; i < AG_WORDS; i++)
        {
            out[i] = 0;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(blk, AG_BLK, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        if (not kos::thread::create_caps(amp_gate_worker, blk, "ampgat", 10, caps, 1,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t)
                    .valid())
        {
            (void)kos_task_kill(t);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        (void)kos_task_kill(t);
        tap::diag("amp from a non-root task: partition slot %ld, forge %ld, round %ld; "
                  "root's own round %ld",
                  static_cast<long>(static_cast<int64_t>(out[AG_PORT_CAP])),
                  static_cast<long>(static_cast<int64_t>(out[AG_FORGE])),
                  static_cast<long>(static_cast<int64_t>(out[AG_ROUND])),
                  static_cast<long>(mine));
        TAP_CHECK(out[AG_RAN] == 1u);
        // The first dynamic index in another task's table is that task's own, naming no
        // endpoint.
        TAP_CHECK(static_cast<int64_t>(out[AG_PORT_CAP]) == -KOS_EBADF);
        TAP_CHECK(static_cast<int64_t>(out[AG_FORGE]) == -KOS_EPERM);
        // KOS_AMP_OP_ROUND spends a peer's doorbell budget, so it is gated by the same task as
        // the forge; root's own answer is the other half.
        TAP_CHECK(static_cast<int64_t>(out[AG_ROUND]) == -KOS_EPERM);
        TAP_CHECK(mine == 0);
        // The gate's refusal: root's answer is amp::send's, and no send produces this code.
        TAP_CHECK(static_cast<int64_t>(out[AG_ROUND]) != mine);
    }

    // KICKOS_MAX_ENDPOINTS' Kconfig ceiling: the pool cannot be wider than this, so the fill
    // below either reaches it or the arm skips.
    constexpr int AMP_REUSE_SLOTS = 32;
    constexpr uint32_t AMP_REUSE_US = 100u * 1000u;

    // FILLING THE POOL TAKES TWO TASKS: KICKOS_TASK_ENDPOINT_BUDGET sits strictly below the
    // pool's width. The two arms below need the pool full and bump-allocated to its last index,
    // or the create after their close lands on a fresh slot instead of the freed one. Root's
    // own AMP port capabilities count against its ceiling.
    constexpr int CH_HOLD = 2; // the release semaphore, delegated second

    void amp_pool_filler(void*)
    {
        kos_cap_t mine[AMP_REUSE_SLOTS];
        int n = 0;
        while (n < AMP_REUSE_SLOTS)
        {
            if (kos_endpoint_create(&mine[n]) != 0)
            {
                break;
            }
            n++;
        }
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_HOLD); // hold the slots while root runs its close-and-create
        for (int i = 0; i < n; i++)
        {
            kos_handle_close(mine[i]);
        }
    }

    // A second group holding the pool's remaining slots. amp_helper_fill answers false and
    // leaves nothing behind when the staging does not take.
    struct PoolHelper
    {
        kos_task_t group = KOS_TASK_NONE;
        kos_cap_t hold = KOS_CAP_NONE;
        bool live = false;
    };

    bool amp_helper_fill(PoolHelper* h)
    {
        if (kos_sem_create(0, &h->hold) != 0)
        {
            return false;
        }
        if (kos_task_create(nullptr, 0, 0, &h->group) != 0)
        {
            kos_sem_destroy(h->hold);
            h->hold = KOS_CAP_NONE;
            return false;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {h->hold, CH_FULL}};
        auto const w = kos::thread::create_caps(amp_pool_filler, nullptr, "ampfill", 10, caps, 2,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                KOS_AUTH_MEMORY, nullptr, h->group);
        if (not w.valid())
        {
            (void)kos_task_kill(h->group);
            kos_sem_destroy(h->hold);
            h->group = KOS_TASK_NONE;
            h->hold = KOS_CAP_NONE;
            return false;
        }
        wait_n(1); // the filler has taken everything it could
        h->live = true;
        return true;
    }

    void amp_helper_release(PoolHelper* h)
    {
        if (h->live)
        {
            kos_sem_post(h->hold);
            h->live = false;
        }
        if (h->group != KOS_TASK_NONE)
        {
            (void)kos_task_kill(h->group);
            h->group = KOS_TASK_NONE;
        }
        if (h->hold != KOS_CAP_NONE)
        {
            kos_sem_destroy(h->hold);
            h->hold = KOS_CAP_NONE;
        }
    }

    // A local port capability names a slot amp::port_bind also names, by index with no
    // generation, so closing the capability must not free it: freed, the next endpoint_create
    // from any task lands there and answers a peer's callers.
    void t_amp_local_port_slot_held()
    {
        // Closing this SPENDS the partition's local port for the life of the image, so no arm
        // that receives on it may follow.
        kos_cap_t const local = amp_local_cap();
        if (local == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no local port");
            return;
        }
        kos_cap_t held[AMP_REUSE_SLOTS];
        int n = 0;
        while (n < AMP_REUSE_SLOTS)
        {
            if (kos_endpoint_create(&held[n]) != 0)
            {
                break; // this task's own ceiling, or the pool: either way it takes no more
            }
            n++;
        }
        // One back, so root keeps a unit of ceiling to PROBE the pool with below.
        if (n > 0)
        {
            n--;
            kos_handle_close(held[n]);
        }
        PoolHelper helper;
        if (not amp_helper_fill(&helper))
        {
            for (int i = 0; i < n; i++)
            {
                kos_handle_close(held[i]);
            }
            tap::skip("no second group to hold the reserved slot");
            return;
        }
        // THE PROBE IS THE PRECONDITION: -KOS_ENOMEM proves the pool full, forcing the create
        // after the close onto the freed slot.
        kos_cap_t probe = KOS_CAP_NONE;
        int const full = kos_endpoint_create(&probe);
        if (full != -KOS_ENOMEM)
        {
            if (full == 0)
            {
                kos_handle_close(probe);
            }
            amp_helper_release(&helper);
            for (int i = 0; i < n; i++)
            {
                kos_handle_close(held[i]);
            }
            tap::skip("the endpoint pool did not fill");
            return;
        }
        int const closed = kos_handle_close(local);
        kos_cap_t reused = KOS_CAP_NONE;
        int const created = kos_endpoint_create(&reused);
        // Released BEFORE the first check that can return: a failing check would leave the
        // pool full under every later arm.
        amp_helper_release(&helper);
        for (int i = 0; i < n; i++)
        {
            kos_handle_close(held[i]);
        }
        if (created == 0)
        {
            kos_handle_close(reused);
        }
        tap::diag("local port slot: root held %d, close %d, create after it %d", n,
                  closed, created);
        TAP_CHECK(closed == 0);
        // The bind holds its own reference, so the close freed no slot.
        TAP_CHECK(created == -KOS_ENOMEM);
    }

    // A far slot handed back as a local endpoint. The pool leaves a freed slot's fields
    // standing, so a create seating only its own keeps the far route the mint wrote. With the
    // pool full, the freed far slot is the only free one under any allocation policy.
    void t_amp_far_slot_reuse()
    {
        // Closing the partition's far endpoint SPENDS it for the life of the image, so this arm
        // is last of the block.
        kos_cap_t const far_ep = amp_far_any();
        if (far_ep == KOS_CAP_NONE)
        {
            tap::skip("the partition names no far entry");
            return;
        }
        kos_cap_t held[AMP_REUSE_SLOTS];
        int n = 0;
        while (n < AMP_REUSE_SLOTS)
        {
            if (kos_endpoint_create(&held[n]) != 0)
            {
                break; // this task's own ceiling, or the pool: either way it takes no more
            }
            n++;
        }
        // One back, so root keeps a unit of ceiling to PROBE the pool with below.
        if (n > 0)
        {
            n--;
            kos_handle_close(held[n]);
        }
        PoolHelper helper;
        if (not amp_helper_fill(&helper))
        {
            for (int i = 0; i < n; i++)
            {
                kos_handle_close(held[i]);
            }
            kos_handle_close(far_ep);
            tap::skip("no second group to hold the reserved slot");
            return;
        }
        // THE PROBE IS THE PRECONDITION: -KOS_ENOMEM proves the pool full, forcing the create
        // after the close onto the freed slot.
        kos_cap_t probe = KOS_CAP_NONE;
        int const full = kos_endpoint_create(&probe);
        if (full != -KOS_ENOMEM)
        {
            if (full == 0)
            {
                kos_handle_close(probe);
            }
            amp_helper_release(&helper);
            for (int i = 0; i < n; i++)
            {
                kos_handle_close(held[i]);
            }
            kos_handle_close(far_ep);
            tap::skip("the endpoint pool did not fill");
            return;
        }
        kos_handle_close(far_ep);
        kos_cap_t reused = KOS_CAP_NONE;
        int const created = kos_endpoint_create(&reused);
        // Released BEFORE the first check that can return: a failing check would leave the
        // pool full under every later arm.
        amp_helper_release(&helper);
        for (int i = 0; i < n; i++)
        {
            kos_handle_close(held[i]);
        }
        TAP_CHECK(created == 0);

        // This node's own row: any other answers a peer's traffic.
        int64_t const sent0 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        char buf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<char>(0x60u + i);
        }
        int32_t const r = kos_send_timed(reused, buf, sizeof(buf), AMP_REUSE_US);
        int64_t const sent1 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        kos_handle_close(reused);
        // A local endpoint with no receiver parks until its deadline; a slot still carrying
        // the far route publishes to the peer and answers the byte count instead.
        tap::diag("far slot reuse: send returned %ld, node sent %ld->%ld",
                  static_cast<long>(r), static_cast<long>(sent0), static_cast<long>(sent1));
        TAP_CHECK(r == -KOS_ETIMEDOUT);
        // Before the equality: two refusals are equal to each other.
        TAP_CHECK(sent0 >= 0);
        TAP_CHECK(sent1 >= 0);
        TAP_CHECK(sent1 == sent0);
    }
#endif
}
