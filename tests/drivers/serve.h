// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The request the witness's drivers serve: a call carrying nothing, answered with how many
// requests this instance has answered, 1 to SERVED.

#ifndef KICKOS_TESTS_DRIVERS_SERVE_H
#define KICKOS_TESTS_DRIVERS_SERVE_H

#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <stdint.h>

namespace testdrivers
{
    constexpr uint32_t SERVED = 5u;

    // Answers SERVED requests on `ep`, then returns 0, or the failing call's answer. A reply the
    // caller is no longer there to take is not counted.
    inline int serve(kos_cap_t ep)
    {
        for (uint32_t served = 1; served <= SERVED; served++)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, ep, 0u, KOS_TIMEOUT_NONE);
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n = kos_reply_recv(KOS_CAP_NONE, nullptr, kos_call_lens_pack(0u, 0u), &opts);
            if (n < 0)
            {
                return n;
            }
            if (opts.info.reply_cap == KOS_CAP_NONE)
            {
                served--;
                continue;
            }
            int const rc = kos_reply(opts.info.reply_cap, &served, sizeof(served));
            if (rc == -KOS_ESRCH)
            {
                // The caller's timed call expired after this request was taken: it calls again.
                served--;
                continue;
            }
            if (rc < 0)
            {
                return rc;
            }
        }
        return 0;
    }
}

#endif
