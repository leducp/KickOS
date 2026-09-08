// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test scaffolding for the shared window and the doorbell that drives it (KOS_SYS_AMP_PROBE).
// Each op runs a whole scenario and answers a number.
//
// Gated on KICKOS_AMP_NODE and never on KICKOS_HAVE_ASPACE: a node's kernel need not translate.

#include <kickos/arch/arch.h>

#include "syscall_internal.h"

#if KICKOS_AMP_NODE && defined(KICKOS_ENABLE_SELFTEST)

#include <kickos/ampwindow.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        constexpr uint32_t ROUND_LEN = 16u;

        constexpr amp::ReplyTag PROBE_TAG = amp::REPLY_TAG_NONE;

        uint64_t amp_round(uint32_t node)
        {
            uint8_t payload[ROUND_LEN];
            for (uint32_t i = 0; i < ROUND_LEN; i++)
            {
                payload[i] = static_cast<uint8_t>(0xA0u + i);
            }
            return static_cast<uint64_t>(static_cast<uint32_t>(
                amp::send(node, amp::PORT_ECHO, PROBE_TAG, payload, ROUND_LEN)));
        }

        // NO `default`, deliberately: -Wswitch under -Werror is what makes this mapping
        // exhaustive, so a Verdict added without a code here fails the build instead of
        // reaching userspace as EMPTY. The trailing return exists for flow analysis only.
        uint64_t verdict_code(amp::Verdict v)
        {
            switch (v)
            {
                case amp::Verdict::EMPTY:
                {
                    return KOS_AMP_V_EMPTY;
                }
                case amp::Verdict::TOOK:
                {
                    return KOS_AMP_V_TOOK;
                }
                case amp::Verdict::DEPTH:
                {
                    return KOS_AMP_V_DEPTH;
                }
                case amp::Verdict::LENGTH:
                {
                    return KOS_AMP_V_LENGTH;
                }
                case amp::Verdict::PORT:
                {
                    return KOS_AMP_V_PORT;
                }
                case amp::Verdict::CLASS:
                {
                    return KOS_AMP_V_CLASS;
                }
                case amp::Verdict::RESERVE:
                {
                    return KOS_AMP_V_RESERVE;
                }
            }
            return KOS_AMP_V_EMPTY;
        }

        // The peer every forge and every far endpoint below names.
        uint32_t amp_peer_node(void)
        {
            if (amp::self() == 0u)
            {
                return 1u;
            }
            return 0u;
        }

        // The first port the partition names `node`, or PORT_MAX where it names none.
        uint32_t amp_first_port_of(uint32_t node)
        {
            for (uint32_t i = 0; i < amp::PORT_COUNT; i++)
            {
                if (amp::PORT_NODE[i] == node)
                {
                    return amp::PORT_PORT[i];
                }
            }
            return amp::PORT_MAX;
        }

        // The first port the partition names this node, or PORT_MAX where it names none.
        uint32_t amp_self_port(void)
        {
            return amp_first_port_of(amp::SELF_NODE);
        }

        // A port inside the mint's width that this node has not minted, or PORT_MAX where it
        // has minted every one of them.
        uint32_t amp_unminted_port(void)
        {
            uint32_t p = amp::PORT_MAX;
            while (p > 0u)
            {
                p--;
                if (not amp::port_minted(amp::self(), p))
                {
                    return p;
                }
            }
            return amp::PORT_MAX;
        }

        // True for any thread of root's TASK, not root's thread alone. A null root task
        // refuses rather than matching: a caller mid-exit has a null task of its own, and
        // equality alone would open the gate exactly then.
        //
        // Caller holds IrqLock: the field is another thread's and thread exit writes it there.
        bool amp_probe_caller_ok(Thread const* c)
        {
            if (c == nullptr)
            {
                return false;
            }
            Task const* const root = kernel().threads.slots[ThreadPool::ROOT_INDEX].task;
            if (root == nullptr)
            {
                return false;
            }
            return c->task == root;
        }

        // What a HOLD took off the peer. Put back rather than cleared: an unpublished affinity
        // reads as affinity zero, which is a real core.
        uint32_t g_peer_seat_was = ARCH_IPI_SEAT_NONE;

        uint64_t send_code(amp::Sent rc)
        {
            if (rc == amp::Sent::OK)
            {
                return KOS_AMP_V_SEND_OK;
            }
            if (rc == amp::Sent::DEPTH)
            {
                return KOS_AMP_V_SEND_DEPTH;
            }
            if (rc == amp::Sent::NODE)
            {
                return KOS_AMP_V_SEND_NODE;
            }
            return KOS_AMP_V_SEND_REFUSED;
        }

        // The reply forges, which mean nothing without a caller parked on a far call.
        uint64_t amp_forge_reply(uint32_t selector)
        {
            amp::ReplyTag tag = {};
            uint32_t node = 0;
            if (selector == KOS_AMP_FORGE_REPLY_UNPARKED)
            {
                // The caller's own route, which is running rather than parked.
                Thread const* const c = sched::current();
                if (c == nullptr)
                {
                    return KOS_AMP_V_EMPTY;
                }
                int const idx = kernel().threads.index_of(c);
                if (idx < 0)
                {
                    return KOS_AMP_V_EMPTY;
                }
                tag.thread = kernel().threads.handle_for(idx);
                tag.seq = amp::reply_seq(c->call_seq);
                node = amp_peer_node();
            }
            else
            {
                if (not endpoint_far_reply_route(&tag, &node))
                {
                    return KOS_AMP_V_EMPTY;
                }
                if (selector == KOS_AMP_FORGE_REPLY_WRONG_RING)
                {
                    // Any peer ring the caller is NOT parked on.
                    uint32_t const parked = node;
                    node = amp::NODE_MAX;
                    for (uint32_t n = 0; n < amp::NODE_MAX; n++)
                    {
                        if (n == parked or n == amp::self())
                        {
                            continue;
                        }
                        node = n;
                        break;
                    }
                    if (node >= amp::NODE_MAX)
                    {
                        return KOS_AMP_V_EMPTY; // a two-node partition has no third ring
                    }
                }
                else if (selector == KOS_AMP_FORGE_REPLY_STALE_SEQ)
                {
                    tag.seq = amp::reply_seq(tag.seq + 1u);
                }
                else if (selector == KOS_AMP_FORGE_REPLY_ALIAS_SEQ)
                {
                    // The low byte left standing, so an arm validating that byte alone takes
                    // this as the caller's own tag.
                    tag.seq = amp::reply_seq(tag.seq + 0x100u);
                }
            }
            uint32_t len = 8u;
            if (selector == KOS_AMP_FORGE_REPLY_EMPTY)
            {
                len = 0u;
            }
            if (amp::forge_reply(node, tag, len))
            {
                return KOS_AMP_V_TOOK;
            }
            return KOS_AMP_V_EMPTY;
        }

        uint64_t amp_forge(uint32_t selector)
        {
            if ((selector >= KOS_AMP_FORGE_REPLY_UNPARKED
                 and selector <= KOS_AMP_FORGE_REPLY_GOOD)
                or selector == KOS_AMP_FORGE_REPLY_EMPTY
                or selector == KOS_AMP_FORGE_REPLY_ALIAS_SEQ)
            {
                return amp_forge_reply(selector);
            }
            if (selector == KOS_AMP_FORGE_CLASS)
            {
                return verdict_code(amp::forge_class_take(amp_peer_node()));
            }
            if (selector == KOS_AMP_FORGE_RESERVE)
            {
                uint32_t bits = 0u;
                // Through verdict_code as every other forge is: the ABI code is what the arm
                // asserts, so it may not be spelled a second time here.
                uint64_t const code =
                    verdict_code(amp::forge_reserve_take(amp_peer_node(), &bits));
                static_assert(amp::FORGE_RESERVE_RAN == KOS_AMP_RESERVE_RAN
                                  and amp::FORGE_RESERVE_CURSOR_HELD == KOS_AMP_RESERVE_CURSOR_HELD
                                  and amp::FORGE_RESERVE_THEN_TOOK == KOS_AMP_RESERVE_THEN_TOOK,
                              "the reserve forge's claim bits are one encoding, not two");
                return code | static_cast<uint64_t>(bits);
            }
            if (selector == KOS_AMP_FORGE_TAIL_DEPTH)
            {
                return send_code(amp::forge_tail_and_send(amp_peer_node(), amp::RING_SLOTS + 1u));
            }
            if (selector == KOS_AMP_FORGE_SELF_SEND)
            {
                uint8_t byte = 0;
                return send_code(
                    amp::send(amp::self(), amp::PORT_ECHO, PROBE_TAG, &byte, sizeof(byte)));
            }
            if (selector == KOS_AMP_FORGE_DEPTH_RESET)
            {
                return verdict_code(amp::forge_depth_recovery(amp_peer_node()));
            }
            if (selector == KOS_AMP_FORGE_REPLY_DEPTH_SERVICE)
            {
                return verdict_code(amp::forge_reply_depth_recovery(amp_peer_node()));
            }
            if (selector == KOS_AMP_FORGE_PEER_CALL
                or selector == KOS_AMP_FORGE_PEER_CALL_BLIND)
            {
                uint32_t const port = amp_self_port();
                if (port >= amp::PORT_MAX)
                {
                    return KOS_AMP_V_EMPTY;
                }
                amp::ReplyTag const tag = {KOS_THREAD_NONE, 0u};
                if (not amp::forge_publish(amp_peer_node(), port, tag))
                {
                    return KOS_AMP_V_EMPTY;
                }
                // After the publication: a forge that could not publish must leave nothing armed.
                if (selector == KOS_AMP_FORGE_PEER_CALL_BLIND)
                {
                    endpoint_far_blind_arm();
                }
                uintptr_t answer = KOS_AMP_V_TOOK;
                if (amp::forge_drain_held(amp_peer_node()))
                {
                    answer = answer | KOS_AMP_PEER_CALL_HELD;
                }
                return answer;
            }

            uint32_t port = amp::PORT_ECHO;
            uint32_t len = ROUND_LEN;
            uint32_t head_jump = 0;
            if (selector == KOS_AMP_FORGE_HEAD_DEPTH)
            {
                head_jump = amp::RING_SLOTS + 1u;
            }
            else if (selector == KOS_AMP_FORGE_LENGTH)
            {
                len = amp::SLOT_BYTES + 1u;
            }
            else if (selector == KOS_AMP_FORGE_PORT)
            {
                // Inside the mint's width and never minted, which a width check alone passes.
                port = amp_unminted_port();
                if (port >= amp::PORT_MAX)
                {
                    return KOS_AMP_V_EMPTY; // a node that minted every port of its width
                }
            }
            else if (selector == KOS_AMP_FORGE_PORT_WIDE)
            {
                port = amp::PORT_MAX;
            }
            else if (selector == KOS_AMP_FORGE_ZERO_LEN)
            {
                len = 0;
            }
            else if (selector != KOS_AMP_FORGE_WELL_FORMED)
            {
                return KOS_AMP_V_EMPTY;
            }
            return verdict_code(
                amp::forge_and_take(amp_peer_node(), port, PROBE_TAG, len, head_jump));
        }
    }

    // Every op holds the lock across the window call it makes, because that is what the real
    // paths run under: amp::send is reached from syscall_ipc under the caller's IrqLock, and
    // take_call from the doorbell service under that core's own mask, which is the whole of an
    // AMP node's exclusion. Driven with interrupts open, a forge drives a state the mechanism
    // never presents.
    uint64_t amp_probe(uintptr_t op, uintptr_t a1)
    {
        switch (op)
        {
            case KOS_AMP_OP_ROUND:
            {
                IrqLock lock;
                // GATED LIKE FORGE: this publishes into a peer's ring and rings its doorbell,
                // so an ungated one lets any task spend a peer's doorbell budget.
                if (not amp_probe_caller_ok(sched::current()))
                {
                    return static_cast<uint64_t>(-KOS_EPERM);
                }
                return amp_round(static_cast<uint32_t>(a1));
            }
            case KOS_AMP_OP_FORGE:
            {
                IrqLock lock;
                if (not amp_probe_caller_ok(sched::current()))
                {
                    return static_cast<uint64_t>(-KOS_EPERM);
                }
                return amp_forge(static_cast<uint32_t>(a1));
            }
            case KOS_AMP_OP_TOOK:
            {
                return amp::counts(static_cast<uint32_t>(a1)).took.load();
            }
            case KOS_AMP_OP_DEPTH:
            {
                return amp::counts(static_cast<uint32_t>(a1)).depth.load();
            }
            case KOS_AMP_OP_DEPTH_RESET:
            {
                return amp::counts(static_cast<uint32_t>(a1)).depth_reset.load();
            }
            case KOS_AMP_OP_LENGTH:
            {
                return amp::counts(static_cast<uint32_t>(a1)).length.load();
            }
            case KOS_AMP_OP_PORT:
            {
                return amp::counts(static_cast<uint32_t>(a1)).port.load();
            }
            case KOS_AMP_OP_SENT:
            {
                return amp::counts(static_cast<uint32_t>(a1)).sent.load();
            }
            case KOS_AMP_OP_SEND_REFUSED:
            {
                return amp::counts(static_cast<uint32_t>(a1)).send_refused.load();
            }
            case KOS_AMP_OP_SERVICED:
            {
                return amp::counts(static_cast<uint32_t>(a1)).serviced.load();
            }
            case KOS_AMP_OP_REPLY_RESERVE:
            {
                return amp::counts(static_cast<uint32_t>(a1)).reply_reserve.load();
            }
            case KOS_AMP_OP_REPLY_ROOM:
            {
                IrqLock lock;
                if (not amp_probe_caller_ok(sched::current()))
                {
                    return static_cast<uint64_t>(-KOS_EPERM);
                }
                return amp::forge_reply_room(amp_peer_node());
            }
            case KOS_AMP_OP_REPLY_DROP:
            {
                return amp::counts(static_cast<uint32_t>(a1)).reply_drop.load();
            }
            case KOS_AMP_OP_DEFER:
            {
                IrqLock lock;
                Thread* const c = sched::current();
                if (not amp_probe_caller_ok(c))
                {
                    return 0;
                }
                uint32_t const peer = amp_peer_node();
                // The doorbell's cells are indexed by CORE and this probe names a NODE; the
                // two coincide for node 0 alone.
                uint32_t const peer_core = amp::core_of(peer);
                uint32_t const before = arch_ipi_deferred(peer_core);
                // Unseat the peer, so the publication below raises no notice.
                uint32_t const was = arch_ipi_seat_set(peer_core, 0u);
                // Refused before the send, so the publication counter is untouched: a backend
                // with no seat cannot skip a raise.
                if (was == ARCH_IPI_SEAT_NONE)
                {
                    return static_cast<uint64_t>(-KOS_ENOSYS);
                }
                uint8_t const body[4] = {0xD0u, 0xD1u, 0xD2u, 0xD3u};
                amp::ReplyTag const tag = {KOS_THREAD_NONE, 0u};
                (void)amp::send(peer, KOS_AMP_PORT_ECHO, tag, body, sizeof(body));
                uint32_t const skipped = arch_ipi_deferred(peer_core) - before;
                // Put back what was there: an unpublished affinity reads as affinity zero,
                // which is a real core.
                (void)arch_ipi_seat_set(peer_core, was);
                // The node travels with the count: which peer this went to is this function's
                // choice, not the caller's.
                static_assert(amp::NODE_MAX <= 0xFFFFu, "the node field is 16 bits wide");
                return (static_cast<uintptr_t>(skipped) << 16) | static_cast<uintptr_t>(peer);
            }
            case KOS_AMP_OP_BAND_RESOLVE:
            {
                IrqLock lock;
                if (a1 >= ThreadPool::FAR_REPLY_RECORDS)
                {
                    return 0;
                }
                uint32_t const handle =
                    ThreadPool::far_reply_handle(static_cast<uint32_t>(a1));
                // A band handle must resolve to no thread, whatever generation or sequence
                // accompanies it: the zero mask drops the sequence clause, so only an earlier
                // one can be what refuses.
                uint32_t resolved = 0u;
                if (cap_reply_thread(handle, 0u, 0u) != nullptr)
                {
                    resolved = 1u;
                }
                // Which clause refused it: "did not resolve" is satisfied by four of them and
                // only the first is the claim.
                uint32_t const index = handle & ((1u << ThreadPool::INDEX_BITS) - 1u);
                uint32_t above = 0u;
                if (index >= static_cast<uint32_t>(kernel().threads.next))
                {
                    above = 1u;
                }
                // The margin the refusal rests on: the pool's high-water mark below the
                // band's base.
                uint32_t const margin =
                    ThreadPool::FAR_REPLY_BASE - static_cast<uint32_t>(kernel().threads.next);
                return (static_cast<uintptr_t>(margin) << 2) | (above << 1) | resolved;
            }
            case KOS_AMP_OP_RESET_RECORD:
            {
                IrqLock lock;
                Thread* const c = sched::current();
                if (not amp_probe_caller_ok(c))
                {
                    return 0;
                }
                // Walks the doorbell's own take path over a ring it has stomped, losing a
                // live peer's traffic on it.
                return amp::forge_reset_record(amp_peer_node());
            }
            case KOS_AMP_OP_APP_ALIVE:
            {
                return amp::counts(static_cast<uint32_t>(a1)).app_alive.load();
            }
            case KOS_AMP_OP_APP_ALIVE_SET:
            {
                IrqLock lock;
                if (not amp_probe_caller_ok(sched::current()))
                {
                    return static_cast<uint64_t>(-KOS_EPERM);
                }
                // The row is this node's, derived here and never taken from the caller: one
                // writer per row is what lets a row sit where a peer reads it.
                uint32_t const port = amp_first_port_of(amp::self());
                if (port >= amp::PORT_MAX)
                {
                    return static_cast<uint64_t>(-KOS_EINVAL);
                }
                // The caller's argument is a CLAIM about the port it bound, checked against
                // the list rather than stored: what lands in the shared region below is this
                // derivation, so no word an app supplies ever crosses.
                if (port != static_cast<uint32_t>(a1))
                {
                    return static_cast<uint64_t>(-KOS_EINVAL);
                }
                amp::app_alive_set(port + 1u);
                return 0;
            }
            case KOS_AMP_OP_PEER_HOLD:
            {
                IrqLock lock;
                if (not amp_probe_caller_ok(sched::current()))
                {
                    return static_cast<uint64_t>(-KOS_EPERM);
                }
                // The doorbell's cells are indexed by CORE and this names a NODE; the two
                // coincide for node 0 alone.
                uint32_t const peer_core = amp::core_of(amp_peer_node());
                if (a1 != 0)
                {
                    uint32_t const was = arch_ipi_seat_set(peer_core, 0u);
                    if (was == ARCH_IPI_SEAT_NONE)
                    {
                        return 0; // a backend keeping no seat cannot withhold a raise
                    }
                    g_peer_seat_was = was;
                    return 1;
                }
                if (g_peer_seat_was == ARCH_IPI_SEAT_NONE)
                {
                    return 0; // nothing was ever held, so nothing may be put back
                }
                (void)arch_ipi_seat_set(peer_core, g_peer_seat_was);
                g_peer_seat_was = ARCH_IPI_SEAT_NONE;
                return 1;
            }
            case KOS_AMP_OP_MINT:
            {
                IrqLock lock;
                Thread* const c = sched::current();
                if (not amp_probe_caller_ok(c))
                {
                    return static_cast<uint64_t>(-KOS_EPERM);
                }
                uint32_t cap = KCAP_INVALID;
                int const rc =
                    amp_endpoint_mint(c, amp_peer_node(), static_cast<uint32_t>(a1), &cap);
                if (rc == 0)
                {
                    // The table is as it was whatever the answer: this reads the mint's own
                    // refusal and is not a way to acquire a crossing the partition withheld.
                    (void)handle_close(c, cap);
                }
                return static_cast<uint64_t>(static_cast<int64_t>(rc));
            }
            case KOS_AMP_OP_FAR_PARKED:
            {
                IrqLock lock;
                amp::ReplyTag tag = {};
                uint32_t node = 0;
                if (endpoint_far_reply_route(&tag, &node))
                {
                    return 1;
                }
                return 0;
            }
            default:
            {
                break;
            }
        }
        return static_cast<uint64_t>(-KOS_EINVAL);
    }
}

#endif
