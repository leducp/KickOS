// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What an app on a node of an AMP partition holds, and how it names it.
//
// The partition states its crossings ONCE, in CONFIG_KICKOS_AMP_PORTS, as node:port pairs.
// Every node's image reads that same list and derives BOTH of its own sets from its own
// index: an entry naming this node is a port bound to a local endpoint this app receives on,
// an entry naming another node is a far endpoint this app calls. Node 0 and node 1 read the
// same string and get opposite roles.
//
// The kernel seats those capabilities into root before its first instruction, in list order,
// so an entry's POSITION in the list is its capability handle. Nothing below is a syscall:
// every value here is decided at build time.
//
// A crossing the partition does not name has no capability: kos_amp_port answers KOS_CAP_NONE,
// which every send, call and receive refuses with -KOS_EBADF.
//
// A NODE THAT SERVES MUST NOT RETURN FROM main. Returning ends the system, and the nodes of a
// partition share one machine, so a serving node's main taking the exit would take its callers'
// machine with it. <kickos/sys/init.h> states the same rule for any init that has to persist.
// A node that CALLS may return as any app does.

#ifndef KICKOS_AMP_H
#define KICKOS_AMP_H

#include <stdint.h>

#include <kickos/config/amp_ports.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/cap_index.h>

// Crossings the partition names, over the whole partition and not just this node's share.
#define KOS_AMP_PORT_COUNT KICKOS_AMP_PORT_COUNT

// This image's own node index.
#define KOS_AMP_SELF_NODE KICKOS_AMP_SELF_NODE

#if KICKOS_AMP_PORT_COUNT > 0

#ifdef __cplusplus
extern "C"
{
#endif

// Where entry `i` of the list was seated. The kernel's seating is the first dynamic install
// into root's run and it runs in list order; it panics at boot rather than let either stop
// being true.
#define KOS_AMP_PORT_CAP(i) ((kos_cap_t)(KOS_CAP_FIRST_DYNAMIC + (i)))

// Entry `i` of the list. Out of range answers KOS_AMP_NO_ENTRY.
#define KOS_AMP_NO_ENTRY 0xFFFFFFFFu

static inline uint32_t kos_amp_entry_node(uint32_t i)
{
    static const uint8_t k_node[KOS_AMP_PORT_COUNT] = {KICKOS_AMP_PORT_NODE_LIST};
    if (i >= KOS_AMP_PORT_COUNT)
    {
        return KOS_AMP_NO_ENTRY;
    }
    return k_node[i];
}

static inline uint32_t kos_amp_entry_port(uint32_t i)
{
    static const uint8_t k_port[KOS_AMP_PORT_COUNT] = {KICKOS_AMP_PORT_PORT_LIST};
    if (i >= KOS_AMP_PORT_COUNT)
    {
        return KOS_AMP_NO_ENTRY;
    }
    return k_port[i];
}

// The capability this image was handed for `port` of `node`, or KOS_CAP_NONE where the
// partition names no such crossing.
//
// A LOCAL entry answers an endpoint to RECEIVE on (kos_recv / kos_reply); a far entry answers
// one to CALL (kos_send / kos_call). kos_amp_port_is_local says which, and a caller that does
// not care need not ask.
static inline kos_cap_t kos_amp_port(uint32_t node, uint32_t port)
{
    static const uint8_t k_node[KOS_AMP_PORT_COUNT] = {KICKOS_AMP_PORT_NODE_LIST};
    static const uint8_t k_port[KOS_AMP_PORT_COUNT] = {KICKOS_AMP_PORT_PORT_LIST};
    uint32_t i;
    for (i = 0; i < KOS_AMP_PORT_COUNT; i++)
    {
        if (k_node[i] == node)
        {
            if (k_port[i] == port)
            {
                return KOS_AMP_PORT_CAP(i);
            }
        }
    }
    return KOS_CAP_NONE;
}

// 1 where `port` of `node` is served BY THIS IMAGE, so its capability is a local endpoint to
// receive on; 0 where it is a far endpoint to call, and 0 for a crossing the partition does
// not name at all. Ask kos_amp_port first: KOS_CAP_NONE is the only answer that means absent.
static inline int kos_amp_port_is_local(uint32_t node, uint32_t port)
{
    if (node != KOS_AMP_SELF_NODE)
    {
        return 0;
    }
    if (kos_amp_port(node, port) == KOS_CAP_NONE)
    {
        return 0;
    }
    return 1;
}

#ifdef __cplusplus
}
#endif

#endif

#endif
