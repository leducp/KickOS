// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Echo calls on the crossing this node's composition serves, the first partition port named
// for it; a marked call is held first, answered before a hold, or sets the wall clock and is held.
// Port 4 is served by a task that never receives, for amp_far_share, and port 7 by main_mortal.c.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "book.h"

#include <kickos/amp.h>
#include <kickos/sys.h>

// Written before the first receive, so a caller this node has answered reads it. The selftest's
// amp_share_crossing states the same line and word.
#define AMP_SHARE_LINE 64u
#define AMP_SHARE_MARK(node) (0x53480000u | (uint32_t)(node))
// The calls this node has received, at this word of its own line. amp_far_call_held reads it.
#define AMP_SEEN_WORD 4u
// The sends this node has received, at this word of its own line. amp_far_send_held reads it.
#define AMP_SENT_WORD 12u
// A call whose first byte is AMP_HOLD_MARK is answered once node 0 stores its second byte at
// AMP_HOLD_WORD of its own line, or after AMP_HOLD_NS. amp_far_reply_unmapped states the same.
#define AMP_HOLD_MARK 0xD0u
// A call whose first byte is AMP_BUSY_MARK is answered at once, and this node then stays off its
// receive as for an AMP_HOLD_MARK call. amp_far_call_held states the same.
#define AMP_BUSY_MARK 0xD2u
#define AMP_HOLD_WORD 8u
#define AMP_HOLD_NS (2000ull * 1000ull * 1000ull)
#define AMP_HOLD_POLL_NS (1000ull * 1000ull)
// A call whose first byte is AMP_WALL_MARK sets the wall clock a day ahead, then is held as an
// AMP_HOLD_MARK call is, and answered with the set's code in its third byte and whether node 0's
// store came in its fourth. amp_far_wall states the same.
#define AMP_WALL_MARK 0xD1u
#define AMP_WALL_DAY_NS (86400ull * 1000000000ull)

// True once node 0 stored `seq`, false after AMP_HOLD_NS of the monotonic clock without it. The
// clock and not a count of polls: on a loaded host each poll's sleep overruns, and a hold counted
// in polls outlasts the caller's own deadline.
static bool ampecho_hold(unsigned char const* view, unsigned char seq)
{
    int64_t const until = kos_monotonic_now().ns + (int64_t)AMP_HOLD_NS;
    while (view != NULL and kos_monotonic_now().ns < until)
    {
        if (__atomic_load_n((uint32_t const*)(view + AMP_HOLD_WORD), __ATOMIC_ACQUIRE) == seq)
        {
            return true;
        }
        kos_sleep_for(kos_ns(AMP_HOLD_POLL_NS));
    }
    return false;
}

int main(void)
{
    kos_cap_t ep = KOS_CAP_NONE;
    uint32_t const port = ampping_crossing(KOS_CAP_WAIT, KOS_AMP_NO_ENTRY, &ep);
    if (port == KOS_AMP_NO_ENTRY or ep == KOS_CAP_NONE)
    {
        printf("ampecho: node %u serves no port\n", (unsigned)KOS_AMP_SELF_NODE);
        exit(1);
    }
    kos_window_t const share = kos_grant_mem("/shm/share");
    unsigned char* const view = (unsigned char*)kos_window_addr(share);
    if (view != NULL and kos_window_size(share) >= (KOS_AMP_SELF_NODE + 1u) * AMP_SHARE_LINE)
    {
        __atomic_store_n((uint32_t*)(view + KOS_AMP_SELF_NODE * AMP_SHARE_LINE),
                         AMP_SHARE_MARK(KOS_AMP_SELF_NODE), __ATOMIC_RELAXED);
    }
    printf("ampecho: node %u echoing on port %u\n", (unsigned)KOS_AMP_SELF_NODE,
           (unsigned)port);

    while (true)
    {
        unsigned char msg[KOS_EP_MSG_MAX];
        struct kos_recv_info info = {0};
        int32_t got;
        // NOT the zero the initialiser leaves: KOS_CAP_NONE is all-ones and zero is a real
        // capability index, KOS_CAP_STDOUT. A path where the kernel never reaches the info
        // write leaves exactly what is here.
        info.reply_cap = KOS_CAP_NONE;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, KOS_NEVER);
        got = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
        info = opts.info;
        if (got < 0)
        {
            // A REPLY CAPABILITY CAN OUTLIVE A REFUSED ARRIVAL, and <kickos/sys.h> asks this
            // loop to answer or close on EVERY path: a far caller under KOS_NEVER has
            // no deadline to fall back on. Tested against KOS_CAP_NONE and never for a sign,
            // per struct kos_recv_info.
            if (info.reply_cap != KOS_CAP_NONE)
            {
                (void)kos_reply(info.reply_cap, msg, 0);
            }
            continue;
        }
        uint32_t word = AMP_SEEN_WORD;
        if (info.reply_cap == KOS_CAP_NONE)
        {
            word = AMP_SENT_WORD;
        }
        if (view != NULL and kos_window_size(share) >= (KOS_AMP_SELF_NODE + 1u) * AMP_SHARE_LINE)
        {
            uint32_t* const seen = (uint32_t*)(view + KOS_AMP_SELF_NODE * AMP_SHARE_LINE + word);
            __atomic_store_n(seen, __atomic_load_n(seen, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELEASE);
        }
        if (info.reply_cap == KOS_CAP_NONE)
        {
            continue; // a send and not a call: there is nothing to answer
        }
        if (got >= 2 and msg[0] == AMP_HOLD_MARK)
        {
            (void)ampecho_hold(view, msg[1]);
        }
        if (got >= 4 and msg[0] == AMP_WALL_MARK)
        {
            kos_realtime_t later = kos_realtime_now();
            later.ns += (int64_t)AMP_WALL_DAY_NS;
            msg[2] = (unsigned char)kos_realtime_set(later);
            msg[3] = (unsigned char)ampecho_hold(view, msg[1]);
        }
        // The caller's own bytes, unchanged: the far-call arms check the payload.
        (void)kos_reply(info.reply_cap, msg, (size_t)got);
        if (got >= 2 and msg[0] == AMP_BUSY_MARK)
        {
            (void)ampecho_hold(view, msg[1]);
        }
    }
}
