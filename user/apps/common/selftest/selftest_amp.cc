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
    // How long one echo round is given, and the step it waits in. Both far over what the
    // crossing costs: what is being waited on is the HOST scheduling another vCPU thread.
    constexpr uint64_t AMP_REPLY_NS = 2ull * 1000ull * 1000ull * 1000ull;
    constexpr uint64_t AMP_REPLY_TICK_NS = 1ull * 1000ull * 1000ull;

    // Which counts row is this node's. Under one image the kernel runs on core 0 alone, its
    // peers parked in the doorbell, so the thread running this is node 0's.
#if KICKOS_AMP_OWN_IMAGE
    constexpr unsigned AMP_SELF_ROW = KICKOS_AMP_NODE_ID;
#else
    constexpr unsigned AMP_SELF_ROW = 0u;
#endif

    // One counter, or -1 for anything the probe REFUSED. The op answers through a uintptr_t, so
    // a negative errno arrives as a count with its top bit set and every `> 0` on it holds.
    int64_t amp_count(uint32_t op, uint32_t a1)
    {
        uintptr_t const raw = kos_amp_probe(op, a1);
        if (static_cast<intptr_t>(raw) < 0)
        {
            return -1;
        }
        return static_cast<int64_t>(raw);
    }

    // Whether a peer is running a kernel of its own, which is what the two arms below key on:
    // liveness and never which build this is. A kernel that has run drained its inboxes at
    // least once, at window_init, so its serviced count is nonzero. Zero is a peer that never
    // reached its own kernel init, which is a node booted alone.
    bool amp_peer_kernel_live(uint32_t node)
    {
        if (node == KOS_AMP_SELF_NODE)
        {
            return false;
        }
        return amp_count(KOS_AMP_OP_SERVICED, node) > 0;
    }

    // A record across a ring resynchronisation: the reset advances indices past a slot a record
    // still names, so one of the two owns that record's death and the other must be refused.
    // The scaffold drives the whole interleaving inside one masked call, needing a record
    // seated on a HELD slot, the reset under it, a fresh call taken at the same masked slot and
    // the first holder's token spent afterwards, in that order with nothing between.
    //
    // As t_amp_window does, this STOMPS the peer's call ring, so a real publication in flight
    // from that peer is lost.
    void t_amp_reset_record()
    {
        uintptr_t const bits = kos_amp_probe(KOS_AMP_OP_RESET_RECORD, 0u);
        // Bit 16 first: a forge that could not run answers zero, which would otherwise read as
        // all four claims failing at once.
        TAP_CHECK((bits & 16u) != 0u);
        // The reset freed the record whose slot it abandoned. Left live, that record refuses a
        // seat to every later call landing on its masked slot.
        TAP_CHECK((bits & 1u) != 0u);
        // So the next call at that slot is granted a record, which is the reply capability its
        // far caller waits on.
        TAP_CHECK((bits & 2u) != 0u);
        // And it is a DIFFERENT token: same table slot, a generation on.
        TAP_CHECK((bits & 4u) != 0u);
        // Spending the abandoned token released nothing. Accepted, it would hand the peer back
        // the slot the call above is still being served on, one wrap later at the same masked
        // index.
        TAP_CHECK((bits & 8u) != 0u);
    }

    // Test replies after route validation fails. The first two cases modify a
    // peer ring and require that peer to be offline; skip if it runs a kernel.
    // The third uses this node's unused self-reply ring and works in either mode.
    // Each test resets its reply-ring capacity before injecting a call.

    void t_amp_far_reset_answers()
    {
        int64_t const unsent0 = amp_count(KOS_AMP_OP_REPLY_UNSENT, AMP_SELF_ROW);
        // THE READING INSTRUMENT, before anything keys on it: the op exists, and a row outside
        // the built range answers zero rather than node 0's.
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
        // Bit 16 first: a forge that could not run answers zero, which would otherwise read as
        // every claim failing at once.
        TAP_CHECK((bits & 16u) != 0u);
        TAP_CHECK((bits & 1u) != 0u);
        TAP_CHECK((bits & 2u) != 0u);
        // THE CLAIM THIS ARM EXISTS FOR: the caller that dead record named was answered, with
        // the wire's only refusal shape and its tag verbatim. A far caller under
        // KOS_TIMEOUT_NONE has no deadline, so a record dropped in silence is a thread parked
        // for the life of the image.
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
        // What makes the loss the bytes' alone: the slot is still this node's, so the caller
        // it names is still owed an answer.
        TAP_CHECK((defer & 2u) != 0u);
        TAP_CHECK((defer & 4u) != 0u);
        TAP_CHECK(unsent1 == unsent0 + 1);
        TAP_CHECK((done & 8u) != 0u);
        TAP_CHECK((done & 1u) != 0u);
        TAP_CHECK((done & 2u) != 0u);
        TAP_CHECK((done & 4u) != 0u);
        // The discharge counts no second loss: the bytes were counted lost once.
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
        // Refused first, so the bound is a bound and not an absence of one.
        TAP_CHECK((bits & 1u) != 0u);
        // And taken at it: the answer that reached the bound is the one the recovery delivers.
        TAP_CHECK((bits & 2u) != 0u);
        // AT the far tail this node adopted. A recovery that kept its own head would publish
        // into a slot the consumer reads as already past.
        TAP_CHECK((bits & 4u) != 0u);
        TAP_CHECK((bits & 8u) != 0u);
        TAP_CHECK(reset1 == reset0 + 1);
    }

    void t_amp_window()
    {
        // The reading instrument on a known value, before any arm keys on it: the set's own
        // width is an op no dispatch carries, so amp_count must report -1. Keyed on the
        // SENTINEL and never on the last op by name, which an op added above would make real.
        TAP_CHECK(amp_count(KOS_AMP_OP_MAX, 0) == -1);
        // The order matters at the end and not the start: the last inbox forge must leave the
        // ring well formed, a refused depth deliberately not advancing the tail.
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
        // The one malformation neither field clause can see, both fields being well formed:
        // a REPLY-class port published into the CALL ring. No counter op reaches wrong_class,
        // so the verdict carries the whole claim, and it must be its OWN code: folded into
        // KOS_AMP_V_EMPTY it would read as a ring that held nothing.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_CLASS)
                  == KOS_AMP_V_CLASS);
        // The SEND side's own untrusted index, which no receive-side arm reaches. Forged on
        // the self-ring, so a live peer's consumer index is never written to run it.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_TAIL_DEPTH)
                  == KOS_AMP_V_SEND_DEPTH);
        // The local node, refused by name: node_service skips its own self-ring, so four
        // accepted self-sends would fill it permanently.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_SELF_SEND)
                  == KOS_AMP_V_SEND_NODE);
        // A refused depth does not advance the tail, so the refusal is bounded: after
        // DEPTH_STRIKES the ring is resynchronised and the next well-formed publication is
        // taken. A ring left dead answers DEPTH here instead.
        int64_t const resets = amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW);
        // A refusal is equal to itself, so `resets + 1` and `> 0` below both hold on one.
        TAP_CHECK(resets >= 0);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_DEPTH_RESET)
                  == KOS_AMP_V_TOOK);
        // Which mechanism recovered it: without this the arm passes on a build that simply
        // stopped refusing the depth.
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW) == resets + 1);

        // The REPLY ring's own bound, which the assertion above does not cover. This selector
        // runs the doorbell's real service body between its strikes, so the call ring of the
        // same pair is taken four times while the reply ring accumulates: a strike count keyed
        // by the ordered pair alone is cleared by each of those takes, the reply ring never
        // reaches DEPTH_STRIKES, and it stays dead for the life of the image. N6f states that
        // bound as the whole of that ring's recovery.
        int64_t const reply_resets = amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW);
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_DEPTH_SERVICE)
                  == KOS_AMP_V_TOOK);
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW) == reply_resets + 1);

        // A row the partition does not hold answers zero, and the RESET counter is what makes
        // this non-vacuous: the assertion just above proves this node's own reset row moved, so
        // an accessor answering a real node's row instead would answer nonzero here. The
        // publication counters cannot serve, the forges writing their slots directly rather
        // than through amp::send, so `sent` is still zero at this point on every posture.
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, AMP_SELF_ROW) > 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, KICKOS_AMP_NODES) == 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_DEPTH_RESET, KICKOS_AMP_NODES + 7u) == 0);

        // The half that needs a peer that is RUNNING, decided at runtime and not by posture:
        // under one image the peers are this image's own cores, and under one image per node
        // the deployment starts them, which a node booted alone does not.
        //
        // Any live peer, and NOT node 1. Node 1 is a peer of node 0 and is node 1's own row, so
        // keying on it would probe this node's own row wherever this image IS node 1, which
        // amp::send refuses by name.
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
            // Skip self rather than start at one: a sweep from 1 is node 0's peer set and no
            // other node's, and this node's own row is the ring no service drains.
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
            // The reply is taken by THIS node's own doorbell service, so this thread has to
            // leave the CPU with interrupts open: a syscall runs with them masked.
            //
            // A time bound and not a spin count. A yield with nothing else runnable returns at
            // once, so a count of them is microseconds of guest time; the far side is a vCPU
            // thread the HOST schedules, and under emulation without icount it can go
            // unscheduled for milliseconds while this core spends any count at all.
            uint64_t const deadline = kos_clock_now() + AMP_REPLY_NS;
            while (kos_clock_now() < deadline)
            {
                if (amp_count(KOS_AMP_OP_TOOK, AMP_SELF_ROW) != mine_took0)
                {
                    break;
                }
                kos_sleep_ns(AMP_REPLY_TICK_NS);
            }
            int64_t const mine_took1 = amp_count(KOS_AMP_OP_TOOK, AMP_SELF_ROW);
            int64_t const peer_took1 = amp_count(KOS_AMP_OP_TOOK, n);
            int64_t const peer_sent1 = amp_count(KOS_AMP_OP_SENT, n);
            int64_t const peer_svc1 = amp_count(KOS_AMP_OP_SERVICED, n);
            // SERVICED is what separates the two ways this can go wrong: a peer that never
            // entered its handler, and one that entered and took nothing.
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
            // Both: the peer read the payload it was handed rather than answering an empty ring.
            TAP_CHECK(peer_took1 > peer_took0);
            TAP_CHECK(peer_sent1 > peer_sent0);
        }
        // Both figures, the gating and the claim not being the same set: one live peer admits
        // this half and EVERY peer is then required to answer, a deployment that starts one
        // peer starting all of them (N6h).
        tap::diag("amp window: %u of %u peer node(s) answered over the doorbell, %u live "
                  "before the sweep", answered,
                  static_cast<unsigned>(KICKOS_AMP_NODES) - 1u, live);
        TAP_CHECK(answered == static_cast<unsigned>(KICKOS_AMP_NODES) - 1u);
        // Not vacuous: a node that never serviced a doorbell answered nothing.
        TAP_CHECK(amp_count(KOS_AMP_OP_SERVICED, AMP_SELF_ROW) > 0);
        }
    }

    // --- The partition's port capabilities -------------------------------------------------
    // Every capability the arms use was seated into root by the kernel out of
    // CONFIG_KICKOS_AMP_PORTS before root's first instruction; nothing below mints or binds.
    //
    // Under one image a peer core runs a SERVICE BODY and not a kernel, which is why the two
    // kinds of far entry below are not interchangeable: the window layer answers the echo port
    // with no thread involved, and a service port needs a thread pool on the far side to park
    // a receiver in.
    constexpr uint32_t AMP_FAR_US = 2u * 1000u * 1000u; // as AMP_REPLY_NS: a host-scheduled vCPU
    constexpr size_t AMP_FAR_LEN = 16;

    // The port this image serves, bound by the partition to a local endpoint, and that
    // endpoint's capability. KOS_AMP_NO_ENTRY / KOS_CAP_NONE where the partition names none.
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

    // Any far entry: a crossing this image holds a capability to CALL. KOS_CAP_NONE where the
    // partition names this node none.
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

    // The first far entry of the partition, and whether the node it names runs a kernel.
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

    // Which far entry a peer can answer is the PARTITION'S OWN LIST and not a runtime guess.
    // A partition that names an ECHO crossing names it because its peers run no kernel of their
    // own: the window layer answers that port with no thread involved. A partition that names
    // none expects its peers to bind what they are named, so the first service crossing is the
    // answered one.
    bool amp_partition_has_echo(void)
    {
        uint32_t port = 0;
        uint32_t node = 0;
        return amp_far_first(&node, &port, true, 0) != KOS_CAP_NONE;
    }

    // A far entry whose call COMES BACK. Liveness decides only whether to try and never which
    // entry, so a peer that is not running makes the arm skip by name rather than wait out a
    // deadline.
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

    // A far entry whose call is NOT answered, so a caller parks on it for as long as an arm
    // needs it parked. Where the partition names an echo crossing that is any service port, a
    // bodiless peer binding nothing and dropping the call; where it names none, the first
    // service port is the answered one, so this is the SECOND, bound by the peer's kernel to an
    // endpoint whose receive nobody holds.
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

    // Spin until `node` has stopped servicing and this node has stopped taking replies, both
    // unchanged across two ticks. For an arm about to read a DELTA of a counter the whole
    // partition feeds: the way to own the window is to leave nothing outstanding.
    //
    // FALSE WHERE THE DEADLINE CAME FIRST, and the caller owes that answer an exit. A partition
    // still moving cannot be told a delta of its own from a neighbour's, and widening the
    // tolerance instead is what docs/design-multicore.md N6f refuses.
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

    // The peer KOS_AMP_OP_ROUND is driven at. Build constants alone, so a member of ANOTHER
    // task derives the same node out of its own copy of this image's data.
    uint32_t amp_round_peer(void)
    {
        if (KOS_AMP_SELF_NODE == 0u)
        {
            return 1u;
        }
        return 0u;
    }


    // A signed probe answer, whole: amp_count collapses every refusal to -1, and this arm's
    // claim is WHICH errno the mint answered.
    int64_t amp_rc(uint32_t op, uint32_t a1)
    {
        return static_cast<int64_t>(static_cast<intptr_t>(kos_amp_probe(op, a1)));
    }

    // Use the test mint operation to target PORT_REPLY and exercise class rejection.
    // Then fill reply capacity: taking a call must leave it unread when no reply
    // slot can be reserved. Malformed peer fields instead consume and drop a slot.
    // Each scenario restores the rings it modifies.
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
        // THE VERDICT and not "some refusal": the code is the whole of what a reader gets.
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(answer) == KOS_AMP_V_RESERVE);
        TAP_CHECK((answer & KOS_AMP_RESERVE_CURSOR_HELD) != 0u);
        // Taken once the ring had room: a refusal that LOST the call would pass the two above.
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
        // The one port and not every reserved one: PORT_ECHO is minted on every row and IS a
        // service, answered by the window layer with no thread involved.
        TAP_CHECK(amp_rc(KOS_AMP_OP_MINT, KOS_AMP_PORT_ECHO) == 0);
        // And the crossing the partition names, which is what says the mint still works.
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
        // Root is unprivileged from its first instruction, so no user thread on this board
        // reaches the far-endpoint mint through KOS_SYS_AMP_ENDPOINT_CREATE at all. The
        // capability above came from the partition and never from this call.
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

        // The call FIRST, and the order is load-bearing where the peer is a THREAD. A far call
        // that finds nothing parked on the port is refused ON THE SPOT rather than held
        // (docs/design-multicore.md N6f), so a one-thread service is unreachable for as long as
        // it is answering something else: with the send ahead of it, this arm would call the
        // peer while it was still replying to that send.
        char cbuf[AMP_FAR_LEN];
        for (size_t i = 0; i < sizeof(cbuf); i++)
        {
            cbuf[i] = static_cast<char>(0x50u + i);
        }
        int32_t const n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), AMP_FAR_US);
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
        // The reply is the request's own bytes, so a wake carrying nothing would pass the
        // count check above and fail here.
        TAP_CHECK(same);

        // A far send must succeed without a local receiver. Use a peer port with
        // no responding thread so a later dropped-reply check sees no delayed response.
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

    // The guard arm's two halves. MAIN is the one that parks, because a far endpoint's cap
    // carries no TRANSFER and so cannot be delegated to a worker; the worker is the forger.
    kos_cap_t g_amp_guard_done = KOS_CAP_NONE;
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_parked{0};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_unparked{99};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_wrong_ring{99};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_stale_seq{99};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_alias_seq{99};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_good{99};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_drops{0};
    Atomic<uint32_t, Order::RELAXED> g_amp_guard_still_parked{0};

    void amp_guard_forger(void*) // caps: g_amp_guard_done@1 (CH_DONE)
    {
        uint64_t const deadline = kos_clock_now() + AMP_REPLY_NS;
        while (kos_clock_now() < deadline)
        {
            if (kos_amp_probe(KOS_AMP_OP_FAR_PARKED, AMP_SELF_ROW) != 0u)
            {
                g_amp_guard_parked = 1;
                break;
            }
            kos_sleep_ns(AMP_REPLY_TICK_NS);
        }
        if (g_amp_guard_parked == 0)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        uintptr_t const drops0 = kos_amp_probe(KOS_AMP_OP_REPLY_DROP, AMP_SELF_ROW);
        g_amp_guard_unparked = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_UNPARKED));
        g_amp_guard_wrong_ring = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_WRONG_RING));
        g_amp_guard_stale_seq = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_STALE_SEQ));
        g_amp_guard_alias_seq = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_ALIAS_SEQ));
        g_amp_guard_drops = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_REPLY_DROP, AMP_SELF_ROW) - drops0);
        // Read BEFORE the control below: after it the caller is awake either way.
        g_amp_guard_still_parked =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_FAR_PARKED, AMP_SELF_ROW));
        g_amp_guard_good = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_GOOD));
        kos_sem_post(CH_DONE);
    }

    void t_amp_far_reply_guard()
    {
        // The partition's spare service port, which a peer's kernel BINDS and its app never
        // receives on. That is not enough on its own: a peer that can be poked answers every
        // call it takes, an empty reply included, and a caller it answered is off its park
        // before the first forge. So the peer's doorbell SEAT is withheld for the length of
        // this arm, and the publication sits in its ring with no raise behind it.
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
        // AND THE PEER MUST BE QUIESCENT BEFORE THE PUBLICATION, which withholding the seat
        // does not by itself give: a raise LATCHED before the seat came off still fires, and the
        // pass it buys drains this arm's own call and answers it inside the drop delta below.
        // reply_drop is fed by every node (docs/design-multicore.md N6f), so the delta owes an
        // empty partition rather than a widened tolerance. A peer that is not running never
        // moves, so this falls through at once there.
        if (not amp_wait_quiet(quiet_node))
        {
            // RELEASED FIRST: a seat left withheld makes the peer deaf for the life of the
            // image, and nothing below runs to release it.
            (void)kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 0u);
            tap::skip("the partition never went quiet, so no delta of a counter it all feeds "
                      "can be attributed");
            return;
        }
        // Every control and every result, before the caller parks: a repeated in-process run
        // would otherwise read the last run's answers and pass on them.
        g_amp_guard_parked = 0;
        g_amp_guard_unparked = 99;
        g_amp_guard_wrong_ring = 99;
        g_amp_guard_stale_seq = 99;
        g_amp_guard_alias_seq = 99;
        g_amp_guard_good = 99;
        g_amp_guard_drops = 0;
        g_amp_guard_still_parked = 0;
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
        char cbuf[AMP_FAR_LEN] = {};
        int32_t const n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), AMP_FAR_US);
        kos_sem_wait(g_amp_guard_done);
        kos_sem_destroy(g_amp_guard_done);
        // BEFORE the first check that can return. The peer then services the call it was
        // never poked for and publishes its own empty answer for a caller the control below
        // already completed, which lands as one more dropped reply AFTER the count was read.
        uint32_t const released = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_PEER_HOLD, 0u));
        if (g_amp_guard_parked == 0)
        {
            tap::skip("no caller reached the far park");
            return;
        }
        TAP_CHECK(released == 1u);
        // All three refused: a tag for a thread that is not parked, a live caller's own tag
        // published on a ring it is not parked on, and a sequence one call out of date.
        TAP_CHECK(g_amp_guard_unparked == KOS_AMP_V_EMPTY);
        TAP_CHECK(g_amp_guard_wrong_ring == KOS_AMP_V_EMPTY);
        TAP_CHECK(g_amp_guard_stale_seq == KOS_AMP_V_EMPTY);
        // The FOURTH, and the one an 8-bit comparison takes: the low byte is the parked
        // caller's own and the whole 16-bit sequence is not, which is the alias 256 short
        // calls bring round while a caller still holds its tag.
        TAP_CHECK(g_amp_guard_alias_seq == KOS_AMP_V_EMPTY);
        // Dropped AND counted, which is what separates a refusal from a reply that never
        // arrived at all. How many of the four can be played is the partition's WIDTH: the
        // wrong-ring forge needs a ring the caller is NOT parked on, and a partition of two
        // holds no third ring for it (docs/design-multicore.md N6c).
        unsigned expect_drops = 4u;
        if (KICKOS_AMP_NODES < 3)
        {
            expect_drops = 3u;
        }
        TAP_CHECK(g_amp_guard_drops == expect_drops);
        // And the caller none of them named is still parked.
        TAP_CHECK(g_amp_guard_still_parked == 1u);
        // The control on that same caller: the right tag on the right ring completes it.
        TAP_CHECK(g_amp_guard_good == KOS_AMP_V_TOOK);
        tap::diag("far reply guard: %u hostile reply(ies) dropped of 4 forged, control "
                  "returned %ld", static_cast<unsigned>(g_amp_guard_drops.load()),
                  static_cast<long>(n));
        TAP_CHECK(n > 0);
        TAP_CHECK(cbuf[0] == static_cast<char>(0xC0u));
    }


    // --- A refused far call still answers its caller ---------------------------------------
    // Every take that did not become a record publishes a ZERO-LENGTH reply carrying the
    // call's tag, so a delivery refused past the take wakes its caller instead of leaving it
    // parked. Under KOS_TIMEOUT_NONE nothing else ever would.
    Atomic<uint32_t, Order::RELAXED> g_amp_empty_forge{99};

    void amp_empty_forger(void*) // caps: g_amp_guard_done@1 (CH_DONE)
    {
        uint64_t const deadline = kos_clock_now() + AMP_REPLY_NS;
        while (kos_clock_now() < deadline)
        {
            if (kos_amp_probe(KOS_AMP_OP_FAR_PARKED, AMP_SELF_ROW) != 0u)
            {
                g_amp_guard_parked = 1;
                break;
            }
            kos_sleep_ns(AMP_REPLY_TICK_NS);
        }
        if (g_amp_guard_parked == 0)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        g_amp_empty_forge = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_EMPTY));
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
        // TWO POSTURES, ONE CLAIM. Where that node runs a kernel the answer is ITS OWN, the
        // port being bound there with no receiver on it, and no forge is involved; where it
        // runs none the call parks and the forge stands in for the answer.
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
            // EXACTLY zero: a wake carrying no payload, and never -KOS_ETIMEDOUT, which is
            // what a caller nobody answered reads.
            TAP_CHECK(n == 0);
            return;
        }
        g_amp_guard_parked = 0;
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
        int32_t const n = kos_call_timed(ep, cbuf, sizeof(cbuf), sizeof(cbuf), AMP_FAR_US);
        kos_sem_wait(g_amp_guard_done);
        kos_sem_destroy(g_amp_guard_done);
        if (g_amp_guard_parked == 0)
        {
            tap::skip("no caller reached the far park");
            return;
        }
        tap::diag("far reply empty: the forge answered %u and the parked call returned %ld",
                  static_cast<unsigned>(g_amp_empty_forge.load()), static_cast<long>(n));
        // The reply reached THAT caller, so the wake below is this publication's and not a
        // deadline's.
        TAP_CHECK(g_amp_empty_forge == KOS_AMP_V_TOOK);
        TAP_CHECK(n == 0);
    }

    // Deliver a far call to a normal receiving thread and reply with its capability.
    // Use the partition-created endpoint. On a standalone image, the test hook
    // supplies the peer publication.
    Atomic<uint32_t, Order::RELAXED> g_amp_forged{0};

    // What one forged peer call carries (amp::forge_publish): eight bytes from 0xB0.
    constexpr size_t AMP_FAR_SERVICE_LEN = 8;

    // THE PRECONDITION EVERY FORGED PEER CALL OWES, made a fact at each forge rather than left
    // to the registration order. take_call reserves a slot in the reply ring this node answers
    // that sender into and answers RESERVE where it cannot, every refusal now publishes its
    // answer there, and on a node booted alone nothing ever moves that tail: without this an
    // earlier arm's answers refuse every later take, and the forge then reports its slot UNREAD
    // through the same unmoved tail that means "a receiver holds it". Answers the free count, so
    // a caller may assert the precondition instead of hoping for it.
    int64_t amp_reply_room(void)
    {
        return static_cast<int64_t>(
            static_cast<intptr_t>(kos_amp_probe(KOS_AMP_OP_REPLY_ROOM, 0u)));
    }

    void amp_service_caller(void*) // caps: none
    {
        // The receiver must be parked before the call arrives, or the delivery finds no thread
        // and refuses on the spot. A clock and not a spin count: the far side is a vCPU the
        // host schedules.
        kos_sleep_ns(AMP_REPLY_TICK_NS * 4u);
        (void)amp_reply_room();
        g_amp_forged = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
    }

    void amp_blind_caller(void*) // caps: none
    {
        kos_sleep_ns(AMP_REPLY_TICK_NS * 4u);
        (void)amp_reply_room();
        g_amp_forged = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL_BLIND));
    }

    // Set once the info-less receiver is off its park, so the loop below stops publishing at
    // a thread that has moved on.
    Atomic<uint32_t, Order::RELAXED> g_amp_infoless_served{0};
    constexpr int AMP_INFOLESS_TRIES = 64;

    // THE ONE FORGE DRIVER THAT RETRIES: a publication landing before that thread parked
    // finds no waiter and wakes nobody, and another one is what recovers it. Publishing on at
    // a receiver already served costs a refused delivery and nothing else.
    //
    // ACCUMULATED rather than overwritten, so which publication did the delivering need not be
    // known: a HELD bit anywhere in the run is one publication that left a slot standing, and
    // this branch of the forge answers KOS_AMP_V_EMPTY or KOS_AMP_V_TOOK alone, so their union
    // is still a verdict.
    void amp_infoless_caller(void*) // caps: none
    {
        int tries = 0;
        while (tries < AMP_INFOLESS_TRIES and g_amp_infoless_served.load() == 0u)
        {
            kos_sleep_ns(AMP_REPLY_TICK_NS * 4u);
            // EVERY iteration: each refused delivery of the retries above publishes an answer
            // of its own, so this loop is the one driver that can fill the ring by itself.
            (void)amp_reply_room();
            uint32_t const answer = static_cast<uint32_t>(
                kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
            g_amp_forged = g_amp_forged.load() | answer;
            tries++;
        }
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

        // The ordinary receive. This thread does not know its caller is in another kernel.
        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = AMP_FAR_US;
        opts.ep = listen;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        // Answered before anything is asserted, and that order is load-bearing: a failing check
        // returns from the arm, so an arm that asserted first would abandon a live reply
        // capability and the next arm's caller would be refused one against
        // KICKOS_CAP_REPLY_MAX.
        char answer[4] = {0x5A, 0x5B, 0x5C, 0x5D};
        int reply_rc = -1;
        if (opts.info.reply_cap != KOS_CAP_NONE)
        {
            reply_rc = kos_reply(opts.info.reply_cap, answer, sizeof(answer));
        }
        // The caller's own bytes and not merely a wake: a delivery that woke this receiver
        // carrying nothing would satisfy a count check and fail here.
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
        // And the reply capability is an ordinary one.
        TAP_CHECK(opts.info.reply_cap != KOS_CAP_NONE);
        TAP_CHECK(reply_rc == 0);
        // The forge reports what the DRAIN made of it, so a publication nothing dispatched
        // would read empty here even with the receive above satisfied by something else.
        uint32_t const forged = g_amp_forged.load();
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(forged) == KOS_AMP_V_TOOK);
        // AND THE SLOT IS STILL THE CALLER'S RECORD. A delivery that released it here would
        // answer this receiver's reply into a slot the ring has already handed on.
        TAP_CHECK((forged & KOS_AMP_PEER_CALL_HELD) != 0u);
        tap::diag("far service: %ld byte(s) from another kernel on port %u, answered through "
                  "kos_reply", static_cast<long>(got),
                  static_cast<unsigned>(amp_local_port()));
    }

    // --- The publication side of the same claim --------------------------------------------
    // Both refusals a peer's call can meet past the take, and what this node OWES for each:
    // one publication out. The counter is the witness the caller cannot be here, the peer's
    // caller living in another kernel; KOS_AMP_PEER_CALL_HELD clear is what says the slot
    // never became a record, so the answer had to come from the taker.
    void t_amp_far_refusal_answered()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        // NOTHING PARKED on that port, so the delivery is refused where it looks for a
        // receiver. No blind arm is set here, so a refusal this early leaves nothing standing.
        int64_t const sent0 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        uint32_t const plain = static_cast<uint32_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
        int64_t const sent1 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);

        // The other refusal, with a receiver parked so the forged blind arm is spent rather
        // than left to poison the next delivery: the capability is installed and then undone,
        // which is a refusal past the point where the receiver came off recv_waiters.
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
        opts.timeout_us = AMP_FAR_US;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = listen;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        (void)b.join(AMP_FAR_US);
        uint32_t const blind = g_amp_forged.load();
        int64_t const sent2 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);

        tap::diag("far refusal answered: sent %ld->%ld->%ld, forges 0x%lx and 0x%lx",
                  static_cast<long>(sent0), static_cast<long>(sent1),
                  static_cast<long>(sent2), static_cast<unsigned long>(plain),
                  static_cast<unsigned long>(blind));
        // Before the arithmetic: two refusals are equal to each other.
        TAP_CHECK(sent0 >= 0);
        TAP_CHECK(sent1 >= 0);
        TAP_CHECK(sent2 >= 0);
        // The drain ran and the slot never became a record, so what follows is about a
        // refusal and not about a call that was served.
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(plain) == KOS_AMP_V_TOOK);
        TAP_CHECK((plain & KOS_AMP_PEER_CALL_HELD) == 0u);
        TAP_CHECK(sent1 == sent0 + 1);
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(blind) == KOS_AMP_V_TOOK);
        TAP_CHECK((blind & KOS_AMP_PEER_CALL_HELD) == 0u);
        TAP_CHECK(opts.info.reply_cap == KOS_CAP_NONE);
        // -KOS_EFAULT AND NEVER 0: the receiver's own out-pointer is what refused, and 0 is a
        // valid zero-length arrival, so it would describe a payload that DID land as an empty
        // one. The count below is the row that names this node's own buffer fault, held apart
        // from reply_unsent, which names a malformed peer.
        TAP_CHECK(got == -KOS_EFAULT);
        TAP_CHECK(amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW) == fault0 + 1);
        TAP_CHECK(sent2 == sent1 + 1);
    }


    // Unmap a local buffer after its thread parks, then deliver a far message.
    // Both call and reply paths must return EFAULT through wait_result.
    // Check a deliver_fault increment of one as well as the error, distinguishing
    // a delivery failure from rejection during syscall validation.
    constexpr int CH_DF_FRAME = 2;
    constexpr int CH_DF_SPACE = 3;
    uintptr_t g_df_va = 0;
    Atomic<uint32_t, Order::RELAXED> g_df_parked{0};
    Atomic<uint32_t, Order::RELAXED> g_df_forge{99};
    Atomic<int32_t, Order::RELAXED> g_df_unmap{1};
    Atomic<int32_t, Order::RELAXED> g_df_delta{-1};
    // Set once root's call is over, however it ended. A publication the ring had no room for
    // never parks anyone, and without this the forger would poll out its whole deadline.
    Atomic<uint32_t, Order::RELAXED> g_df_over{0};

    void df_reply_forger(void*) // caps: g_amp_guard_done@1, frame@2, space@3
    {
        uint64_t const deadline = kos_clock_now() + AMP_REPLY_NS;
        while (kos_clock_now() < deadline and g_df_over.load() == 0u)
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
        // The request copy is behind us by now, so the page goes AFTER the park and the reply
        // is the only copy left to meet it.
        g_df_unmap = kos_frame_unmap(CH_DF_FRAME, CH_DF_SPACE, g_df_va);
        int64_t const before = amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW);
        g_df_forge =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_REPLY_GOOD));
        g_df_delta = static_cast<int32_t>(
            amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW) - before);
        kos_sem_post(CH_DONE);
    }

    void df_call_forger(void*) // caps: g_amp_guard_done@1, frame@2, space@3
    {
        // No probe reads "a receiver is parked on the local port", so this sleeps as every
        // other peer-call driver here does; the HELD bit and the counter below are what say the
        // publication reached a receiver rather than a boundary check.
        kos_sleep_ns(AMP_REPLY_TICK_NS * 4u);
        g_df_unmap = kos_frame_unmap(CH_DF_FRAME, CH_DF_SPACE, g_df_va);
        int64_t const before = amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW);
        g_df_forge =
            static_cast<uint32_t>(kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_PEER_CALL));
        g_df_delta = static_cast<int32_t>(
            amp_count(KOS_AMP_OP_DELIVER_FAULT, AMP_SELF_ROW) - before);
        kos_sem_post(CH_DONE);
    }

    void t_amp_far_deliver_fault()
    {
        // READ SIGNED, AND AT THE POINTER WIDTH. A board with no address-space seam compiles
        // the probe's whole dispatch arm out and the syscall is REFUSED, so the answer is a
        // negative errno and never 0. The width is the second half of it, uintptr_t being
        // the return type: a 32-bit refusal widened to 64 bits first reads POSITIVE.
        // Such a board's access_copy is an unconditional kmemcpy, leaving the overlap refusal
        // as its only reachable one, so there is nothing here to witness.
        // A seeded run's own answer is never negative: both fields are small handle indices.
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
        // A far port whose call nobody answers, with the peer's doorbell seat withheld for the
        // length of the phase: a peer that can be poked answers every call it takes, and a
        // caller it answered is off its park before the forge. RELEASED ON EVERY EXIT.
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
                g_df_delta = -1;
                g_df_over = 0;
                kos_sem_create(0, &g_amp_guard_done);
                caps[0].source_cap = g_amp_guard_done;
                // Whatever the other phase left: the map below is this phase's precondition
                // and must not inherit a page that is already mapped.
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
                    r_delta = g_df_delta.load();
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
        struct kos_reply_recv_opts opts = {};
        opts.info.reply_cap = KOS_CAP_NONE;
        if (listen != KOS_CAP_NONE)
        {
            g_df_forge = 99;
            g_df_unmap = 1;
            g_df_delta = -1;
            kos_sem_create(0, &g_amp_guard_done);
            caps[0].source_cap = g_amp_guard_done;
            (void)kos_frame_unmap(fcap, acap, va); // as above: this phase's own precondition
            c_mapped = kos_frame_map(fcap, acap, va, 0);
            if (c_mapped == 0
                and kos::thread::create_caps(df_call_forger, nullptr, "ampdfc", 10, caps, 3,
                                             KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                             KOS_AUTH_MEMORY)
                        .valid())
            {
                opts.timeout_us = AMP_FAR_US;
                opts.ep = listen;
                c_recv = kos_reply_recv(KOS_CAP_NONE, reinterpret_cast<void*>(va), kos_call_lens_pack(0, AMP_FAR_LEN), &opts);
                // ANSWERED FIRST WHATEVER THE BYTE COUNT WAS, which is the shape a server
                // loop owes: a reply capability this arm dropped would hold the far caller's
                // ring slot. NONE is what this delivery must leave, and the check below is
                // where that is asserted; this stays so that a regression to disclosing one
                // does not also strand the caller inside the suite.
                if (opts.info.reply_cap != KOS_CAP_NONE)
                {
                    char ans[AMP_FAR_LEN] = {};
                    c_reply = kos_reply(opts.info.reply_cap, ans, sizeof(ans));
                }
                kos_sem_wait(g_amp_guard_done);
                call_ran = true;
                c_unmap = g_df_unmap.load();
            }
            kos_sem_destroy(g_amp_guard_done);
            g_amp_guard_done = KOS_CAP_NONE;
        }
        uint32_t const c_forge = g_df_forge.load();
        int32_t const c_delta = g_df_delta.load();

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
            // The reply resolved THIS caller and woke it, so the code is the delivery's and
            // not a deadline's, and 0 would have been a valid empty answer instead.
            TAP_CHECK(r_call == -KOS_EFAULT);
            TAP_CHECK(r_delta == 1);
        }
        if (call_ran)
        {
            TAP_CHECK(c_unmap == 0);
            TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(c_forge) == KOS_AMP_V_TOOK);
            // A RECEIVER WAS REACHED AND ITS BUFFER REFUSED THE PAYLOAD, which is what
            // separates this from a publication no receiver ever saw: nothing but a delivery
            // that resolved a parked thread moves this count.
            TAP_CHECK(c_delta == 1);
            TAP_CHECK(c_recv == -KOS_EFAULT);
            // AND NO CAPABILITY WAS DISCLOSED FOR BYTES THAT NEVER ARRIVED. A receiver
            // answered -KOS_EFAULT saw no request, so it is handed no obligation and no
            // handle, and the far caller is answered by dispatch_call's single publish site
            // instead: the slot is CLEAR of the record it would otherwise still hold. The
            // same two claims hold for the info write amp_far_undisclosed drives, which is
            // the whole point of them being the same two.
            TAP_CHECK(opts.info.reply_cap == KOS_CAP_NONE);
            TAP_CHECK((c_forge & KOS_AMP_PEER_CALL_HELD) == 0u);
            TAP_CHECK(c_reply == -1); // never attempted, there being nothing to attempt it with
        }
    }

    // If reply-cap write-back fails, undo the capability and call record so
    // the ring slot can be released. Repeat KICKOS_CAP_REPLY_MAX times before
    // a successful control to check recovery of both table slots and reply budget.
    void t_amp_far_undisclosed()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        // ITS OWN PRECONDITION, and the reserve counter beside it: a take refused for want of a
        // reply slot leaves the call UNREAD, which reads through the forge's unmoved tail as a
        // receiver still holding it. Asserted so that wedge names itself here.
        TAP_CHECK(amp_reply_room() > 0);
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
            opts.timeout_us = AMP_FAR_US;
            // The kernel reads the deadline out of this struct and writes nothing else into
            // it, so a handle still reading NONE afterwards is one that was never disclosed.
            opts.info.reply_cap = KOS_CAP_NONE;
            opts.ep = listen;
            int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
            (void)b.join(AMP_FAR_US);
            uint32_t const forged = g_amp_forged.load();
            if (KOS_AMP_PEER_CALL_VERDICT(forged) != KOS_AMP_V_TOOK)
            {
                blind_took = false;
            }
            if ((forged & KOS_AMP_PEER_CALL_HELD) != 0u)
            {
                blind_released = false;
            }
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                blind_silent = false;
            }
            // The receiver is woken and told the fault, never left parked on nothing and
            // never told a payload it did not get was zero bytes long.
            if (got != -KOS_EFAULT)
            {
                blind_empty = false;
            }
        }
        // The drain ran, so the claims below are about a delivery and not about a publication
        // that never reached one.
        TAP_CHECK(amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW) == reserve0);
        TAP_CHECK(blind_took);
        TAP_CHECK(blind_released);
        TAP_CHECK(blind_silent);
        TAP_CHECK(blind_empty);

        // THE CONTROL, and the whole point of the loop above: an ordinary call, whose mint
        // runs against the bound every refusal above had to give back.
        g_amp_forged = 0;
        auto w = kos::thread::create(amp_service_caller, nullptr, "ampctl", 10);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = AMP_FAR_US;
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
        (void)w.join(AMP_FAR_US);
        uint32_t const forged = g_amp_forged.load();
        tap::diag("far undisclosed: %u refusal(s) released their slot, control took %ld "
                  "byte(s) and replied %d", static_cast<unsigned>(KICKOS_CAP_REPLY_MAX),
                  static_cast<long>(got), reply_rc);
        TAP_CHECK(got == static_cast<int32_t>(AMP_FAR_SERVICE_LEN));
        TAP_CHECK(opts.info.reply_cap != KOS_CAP_NONE);
        TAP_CHECK(reply_rc == 0);
        // The verdict FIRST, as every other reader of a forge does. The probe answers a refusal
        // through the same unsigned word as the packed verdict, and -KOS_EPERM truncated to
        // uint32_t carries KOS_AMP_PEER_CALL_HELD set, so the held check alone holds on a probe
        // that dispatched nothing.
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(forged) == KOS_AMP_V_TOOK);
        TAP_CHECK((forged & KOS_AMP_PEER_CALL_HELD) != 0u);
    }

    // An info-less far receive cannot return a reply cap. It accepts a datagram
    // and leaves slot cleanup to the taker; the wire carries no local errno.
    // Use KOS_RECV_NO_INFO with a deadline so missing delivery cannot hang the test.
    void t_amp_far_infoless()
    {
        kos_cap_t const listen = amp_local_cap();
        if (listen == KOS_CAP_NONE)
        {
            tap::skip("the partition names this node no port");
            return;
        }
        // As amp_far_undisclosed: the arm owns its own reply-ring room, and the reserve counter
        // beside it so a take refused for want of a slot names itself rather than presenting as
        // a receiver that never woke.
        TAP_CHECK(amp_reply_room() > 0);
        int64_t const reserve0 = amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW);
        g_amp_forged = 0;
        g_amp_infoless_served = 0;
        auto w = kos::thread::create(amp_infoless_caller, nullptr, "ampifl", 10);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        char buf[16] = {};
        struct kos_reply_recv_opts opts = {};
        opts.timeout_us = AMP_FAR_US;
        // The delivery reads an out-ptr of zero, which is the whole of this arm.
        opts.flags = KOS_RECV_NO_INFO;
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = listen;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        g_amp_infoless_served = 1;
        (void)w.join(AMP_FAR_US);
        uint32_t const forged = g_amp_forged.load();
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
        // The datagram still lands: a receiver that cannot host a call is not one that
        // cannot be sent to.
        TAP_CHECK(amp_count(KOS_AMP_OP_REPLY_RESERVE, AMP_SELF_ROW) == reserve0);
        TAP_CHECK(got == static_cast<int32_t>(AMP_FAR_SERVICE_LEN));
        TAP_CHECK(same);
        TAP_CHECK(KOS_AMP_PEER_CALL_VERDICT(forged) == KOS_AMP_V_TOOK);
        // Nothing was written where an out-ptr of zero says there is nowhere to write.
        TAP_CHECK(opts.info.reply_cap == KOS_CAP_NONE);
        // And no record was seated for it, so the taker released the slot.
        TAP_CHECK((forged & KOS_AMP_PEER_CALL_HELD) == 0u);
    }

    // --- A publication outlives the doorbell that would have announced it -------------------
    // The ring is the authority and the raise is a hint: a peer that cannot yet be poked is
    // published to anyway and reads its rings once it can be, so a skipped raise costs latency
    // and never a message. This reopens that window on a partition that has long since closed
    // it.
    void t_amp_deferred_doorbell()
    {
        int64_t const sent0 = amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW);
        int64_t const answer = amp_count(KOS_AMP_OP_DEFER, 0);
        // A doorbell whose raise names its peer with one register write keeps no seat to
        // clear, so the probe refuses this scenario ahead of the publication.
        if (answer < 0)
        {
            tap::skip("this doorbell has no seat, so no raise of it can be deferred");
            return;
        }
        TAP_CHECK(sent0 >= 0);
        unsigned const skipped = static_cast<unsigned>(answer >> 16);
        unsigned const at = static_cast<unsigned>(answer & 0xFFFFu);
        // The node is the kernel's own choice and not this arm's.
        tap::diag("deferred doorbell: %u raise(s) skipped at unseated node %u", skipped, at);
        // The kernel never publishes at this node: its own ring is the one no service drains.
        TAP_CHECK(at != static_cast<unsigned>(AMP_SELF_ROW));
        TAP_CHECK(at < static_cast<unsigned>(KICKOS_AMP_NODES));
        TAP_CHECK(skipped == 1u);
        // The publication stands whatever happened to its raise.
        TAP_CHECK(amp_count(KOS_AMP_OP_SENT, AMP_SELF_ROW) == sent0 + 1);
        // The delivery itself is not witnessed here: the seated flag only ever goes unseated to
        // seated, so the window this reopens is one a running partition cannot re-enter.
    }

    // --- A node's own app declaring itself into the record every node shares ----------------
    // The one field of a node's window row that moves without traffic, and therefore the only
    // witness a node no crossing ever reaches has: every counter beside it needs a message.
    //
    // This is the WRITE side's contract, which is what makes userspace asking for it safe. The
    // row is derived in the kernel from the core it is running on and is never a parameter, so
    // no caller can speak for a peer; and the value stored is the kernel's own derivation from
    // the partition list, so a claim naming any other port is refused rather than recorded.
    void t_amp_app_alive()
    {
        uint32_t const port = amp_local_port();
        if (port == KOS_AMP_NO_ENTRY)
        {
            tap::skip("the partition names this node no port to publish");
            return;
        }
        // Every row ahead of the publication, so the claim that none but this node's moves is
        // read across the write rather than asserted from one sample.
        int64_t before[KICKOS_AMP_NODES];
        for (uint32_t row = 0; row < (uint32_t)KICKOS_AMP_NODES; row++)
        {
            before[row] = amp_count(KOS_AMP_OP_APP_ALIVE, row);
            TAP_CHECK(before[row] >= 0);
        }
        // A claim the partition's list does not bear this node is refused, and refused BEFORE
        // the row moves: what lands in the shared region is never a word an app supplied.
        TAP_CHECK(static_cast<intptr_t>(kos_amp_probe(KOS_AMP_OP_APP_ALIVE_SET, port + 1u)) < 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, AMP_SELF_ROW) == before[AMP_SELF_ROW]);
        // The accepted claim records the port the partition names this node, biased by one so
        // that a node named port 0 is still distinguishable from one that never published.
        TAP_CHECK(kos_amp_probe(KOS_AMP_OP_APP_ALIVE_SET, port) == 0);
        TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, AMP_SELF_ROW)
                  == static_cast<int64_t>(port) + 1);
        // And no other node's row moved under it.
        for (uint32_t row = 0; row < (uint32_t)KICKOS_AMP_NODES; row++)
        {
            if (row == (uint32_t)AMP_SELF_ROW)
            {
                continue;
            }
            TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, row) == before[row]);
        }
        // A row outside the built width answers zero rather than a real peer's, so a reader
        // may sweep a fixed count.
        TAP_CHECK(amp_count(KOS_AMP_OP_APP_ALIVE, KICKOS_AMP_NODES) == 0);
    }

    // --- A far caller answered through the ordinary reply call -----------------------------
    // The reply capability rather than the receive: the record seated for the held call is
    // named through the band, kos_reply on it puts exactly ONE publication on the peer's REPLY
    // ring carrying the token verbatim, and the capability is spent whatever the outcome. The
    // service side is the ordinary one-shot reply and knows none of it.
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
        opts.timeout_us = AMP_FAR_US;
        // The payload is amp_far_service's claim: this arm asserts the capability alone.
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
        // The ordinary reply call, on a capability naming a caller in another kernel.
        TAP_CHECK(first == 0);
        // One publication left this node: the reply.
        TAP_CHECK(sent1 == sent0 + 1u);
        // And the capability is one-shot, consumed whatever the outcome.
        TAP_CHECK(second == -KOS_EBADF);
        tap::diag("inbound reply: a far caller answered through kos_reply, one publication out");
    }

    // --- What the partition seated, and where -----------------------------------------------
    // One list states every crossing, and the kernel walks it in order into root's freshly
    // attached run, so entry i IS capability index KOS_CAP_FIRST_DYNAMIC + i. An app spells
    // that constant and asks the kernel nothing.
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
            // Positional, which is what makes a build-time constant sufficient.
            TAP_CHECK(cap == KOS_AMP_PORT_CAP(i));
            // The role is the node's own reading of the same entry: a local entry carries
            // WAIT, a far one CAP_SIGNAL alone.
            char probe[1] = {};
            if (node == KOS_AMP_SELF_NODE)
            {
                TAP_CHECK(kos_amp_port_is_local(node, port) == 1);
                // The capability resolves WITH the wait right, so the receive genuinely parks
                // and comes back on its own deadline.
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
    // A node handed no capability for a crossing cannot reach it, and it learns so where it
    // ASKS rather than by an answer that never comes.
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
        // And every call on that answer is refused by name.
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
    // A reply capability for a peer's caller is an ordinary one whose handle index lies where
    // the thread pool never seats. What makes that safe is the pool staying BELOW the band, so
    // the reply resolve's first clause refuses every handle in it.
    void t_amp_reply_band()
    {
        unsigned margin = 0;
        unsigned records = 0;
        for (unsigned r = 0; r < KOS_AMP_RING_SLOTS * KICKOS_AMP_NODES * KICKOS_AMP_NODES; r++)
        {
            int64_t const answer = amp_count(KOS_AMP_OP_BAND_RESOLVE, r);
            // FIRST: a refusal carries a clear bit 0, a set bit 1 and an enormous margin, so
            // every claim below holds on a probe that ran nothing.
            TAP_CHECK(answer >= 0);
            // Bit 0 set would be a far reply capability resolving to a local thread.
            TAP_CHECK((answer & 1u) == 0u);
            // Bit 1 is WHICH clause refused it. Without it the arm passes on a draw: a handle
            // below the pool's mark is refused by a generation that happens not to match.
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
        // The slots between the pool's high-water mark and the band's base are what the
        // refusal rests on: a pool grown into the band reads zero here.
        tap::diag("reply band: %u record(s), %u index(es) of margin below the band", records,
                  margin);
        TAP_CHECK(records == KOS_AMP_RING_SLOTS * KICKOS_AMP_NODES * KICKOS_AMP_NODES);
        TAP_CHECK(margin > 0u);
    }

    // --- Whose table the partition seated, and whose the forge answers ---------------------
    // Two things that are root's and not ambient. The partition's capabilities live in ROOT's
    // table, so the index an app spells for one names something else entirely in a task of its
    // own; and KOS_AMP_OP_FORGE answers root's TASK and no other.
    //
    // Root's TASK is the forge's gate and not root's thread: the guard arm above runs it from a
    // worker of root's, and this arm holds the other side.
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
        // Through intptr_t, so the refusal is SIGN-extended into the report word: a plain
        // widening of the uintptr_t puts -KOS_EPERM in as 0x00000000ffffffff on a 32-bit part.
        out[AG_FORGE] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<intptr_t>(
            kos_amp_probe(KOS_AMP_OP_FORGE, KOS_AMP_FORGE_WELL_FORMED))));
        // The node comes from build constants, this member holding its own copy of every
        // datum root computed.
        out[AG_ROUND] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<intptr_t>(
            kos_amp_probe(KOS_AMP_OP_ROUND, amp_round_peer()))));
        out[AG_RAN] = 1u;
        kos_sem_post(CH_DONE);
    }

    void t_amp_probe_root_only()
    {
        // THE CONTROL FIRST, and the order is load-bearing: KOS_AMP_OP_ROUND publishes into
        // the peer's ring, so a member admitted by a broken gate would have spent the slot
        // this reads and the control would answer FULL for the wrong reason.
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
        // The partition seated ROOT and not the system: the first dynamic index in another
        // task's table is that task's own, so the constant an app spells for a crossing names
        // no endpoint there.
        TAP_CHECK(static_cast<int64_t>(out[AG_PORT_CAP]) == -KOS_EBADF);
        TAP_CHECK(static_cast<int64_t>(out[AG_FORGE]) == -KOS_EPERM);
        // KOS_AMP_OP_ROUND spends a PEER's doorbell budget, so it is gated where the forge is
        // and by the same task. Both halves: an ungated one answers the member as it answers
        // root, and a gate that closed on root would answer neither.
        TAP_CHECK(static_cast<int64_t>(out[AG_ROUND]) == -KOS_EPERM);
        TAP_CHECK(mine == 0);
        // The gate and not the window: root's own answer above is amp::send's, so a member
        // refused by the gate reads a code no send ever produces.
        TAP_CHECK(static_cast<int64_t>(out[AG_ROUND]) != mine);
    }

    // KICKOS_MAX_ENDPOINTS' Kconfig ceiling: the pool cannot be wider than this, so the fill
    // below either reaches it or the arm skips.
    constexpr int AMP_REUSE_SLOTS = 32;
    constexpr uint32_t AMP_REUSE_US = 100u * 1000u;

    // A far slot handed back as a local endpoint. The pool leaves a freed slot's fields
    // standing, so a create that seats only its own would keep the far route the mint wrote.
    // FILLING THE POOL TAKES TWO TASKS. KICKOS_TASK_ENDPOINT_BUDGET sits strictly below the
    // pool's width, so a task at its ceiling always leaves slots free and NO ONE task can
    // fill a pool by creating. The two arms below need the pool full AND bump-allocated to
    // its last index, or the create after their close lands on a fresh slot instead of the
    // freed one and proves nothing. So a second group takes what this one's budget keeps out
    // of its reach. Root's own AMP port capabilities count against its ceiling too.
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

    // A second group holding the pool's remaining slots. Answers false when the staging did
    // not take, and leaves nothing behind either way.
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

    // The pool is FILLED first, which is what forces the slot: the far one is then the only
    // free slot and any allocation policy returns it.
    // The bind's OWN reference, which the far arm below has no half for: a local port
    // capability names a slot amp::port_bind also names, by INDEX and with no generation
    // beside it, so closing the capability may not free that slot. Freed, the next
    // endpoint_create from any task lands there and answers a peer's callers.
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
        // THE PROBE IS THE PRECONDITION: -KOS_ENOMEM here is the pool actually being full,
        // which is what forces the create after the close onto the slot that close freed.
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
        // Released BEFORE the first check that can return, as the far arm does.
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
        // No slot came free, so the pool is still full: the bind holds a reference of its own
        // and the capability's was only one of two.
        TAP_CHECK(created == -KOS_ENOMEM);
    }

    void t_amp_far_slot_reuse()
    {
        // Closing the partition's own far endpoint SPENDS it for the life of the image, the
        // slot it frees being what this arm needs, so this arm is last of the block.
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
        // THE PROBE IS THE PRECONDITION: -KOS_ENOMEM here is the pool actually being full,
        // which is what forces the create after the close onto the slot that close freed.
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
        // Released BEFORE the first check that can return: the slot is claimed, so the pool
        // has nothing left to hold, and a failing check would otherwise leave it full under
        // every arm that follows.
        amp_helper_release(&helper);
        for (int i = 0; i < n; i++)
        {
            kos_handle_close(held[i]);
        }
        TAP_CHECK(created == 0);

        // This node's own row: the claim is that THIS image published nothing, and any other
        // row answers a peer's traffic.
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
        // And the payload never crossed.
        TAP_CHECK(sent1 == sent0);
    }
#endif
}
