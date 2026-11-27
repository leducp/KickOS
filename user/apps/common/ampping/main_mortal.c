// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Echoes calls on crossing AMP_MORTAL_PORT, a call's third byte replaced by this instance's
// life, the count of its starts stored at AMP_LIVES_WORD of this node's line. A call marked
// AMP_DIE_MARK is answered; this instance then waits for its kernel to take one more message,
// stores how many it took at AMP_TAKEN_WORD, and ends AMP_LINGER_NS later. The selftest's
// amp_far_server_gone states the same port, mark, words and linger.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <kickos/amp.h>
#include <kickos/sys.h>

#define AMP_MORTAL_PORT "/amp/7"
#define AMP_SHARE_LINE 64u
#define AMP_LIVES_WORD 16u
#define AMP_DIE_MARK 0xD4u
#define AMP_TAKEN_WORD 20u
#define AMP_DIE_NS (2000ull * 1000ull * 1000ull)
#define AMP_DIE_POLL_NS (1000ull * 1000ull)
#define AMP_LINGER_NS (200ull * 1000ull * 1000ull)

static uint32_t mortal_took(void)
{
    uint32_t took = 0;
    (void)kos_amp_count(KOS_AMP_SELF_NODE, KOS_AMP_COUNT_TOOK, &took);
    return took;
}

static uint32_t mortal_hold(uint32_t took)
{
    int64_t const until = kos_monotonic_now().ns + (int64_t)AMP_DIE_NS;
    uint32_t taken = mortal_took() - took;
    while (taken == 0u and kos_monotonic_now().ns < until)
    {
        kos_sleep_for(kos_ns(AMP_DIE_POLL_NS));
        taken = mortal_took() - took;
    }
    return taken;
}

int main(void)
{
    kos_cap_t const ep = kos_grant_endpoint(AMP_MORTAL_PORT);
    kos_window_t const share = kos_grant_mem("/shm/share");
    unsigned char* const view = (unsigned char*)kos_window_addr(share);
    if (ep == KOS_CAP_NONE or view == NULL or kos_window_size(share) < (KOS_AMP_SELF_NODE + 1u) * AMP_SHARE_LINE)
    {
        exit(1);
    }
    unsigned char* const line = view + KOS_AMP_SELF_NODE * AMP_SHARE_LINE;
    uint32_t* const lives = (uint32_t*)(line + AMP_LIVES_WORD);
    uint32_t const life = __atomic_load_n(lives, __ATOMIC_RELAXED) + 1u;
    __atomic_store_n(lives, life, __ATOMIC_RELEASE);

    while (true)
    {
        unsigned char msg[KOS_EP_MSG_MAX];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, KOS_NEVER);
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
        kos_cap_t const reply = opts.info.reply_cap;
        if (reply == KOS_CAP_NONE)
        {
            continue;
        }
        if (got < 3)
        {
            (void)kos_reply(reply, msg, 0);
            continue;
        }
        msg[2] = (unsigned char)life;
        // Read ahead of the reply: the next call may be taken before the reply returns.
        uint32_t const took = mortal_took();
        (void)kos_reply(reply, msg, (size_t)got);
        if (msg[0] == AMP_DIE_MARK)
        {
            __atomic_store_n((uint32_t*)(line + AMP_TAKEN_WORD), mortal_hold(took), __ATOMIC_RELEASE);
            kos_sleep_for(kos_ns(AMP_LINGER_NS));
            exit(0);
        }
    }
}
