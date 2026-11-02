// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Creating an on_request peer program and waiting for its end.

#ifndef KICKOS_APPS_PEER_RUN_H
#define KICKOS_APPS_PEER_RUN_H

#include <stdint.h>

#include <kickos/kos.h>
#include <kickos/sys.h>

namespace kickos::apps
{
    // kos_task_create, asked again for up to a second while the instance's last incarnation is
    // still dying.
    inline int create_peer(kos_program_t prog, kos_task_params const& params, kos_task_t* out)
    {
        int rc = -KOS_EAGAIN;
        for (int tries = 0; rc == -KOS_EAGAIN and tries < 10000; tries++)
        {
            rc = kos_task_create(prog, &params, out);
            if (rc == -KOS_EAGAIN)
            {
                kos::sleep_ns(100000ull);
            }
        }
        return rc;
    }

    // The value of a hex argument as `%p` prints it, with or without the 0x.
    inline uintptr_t hex_arg(char const* text)
    {
        if (text[0] == '0' and (text[1] == 'x' or text[1] == 'X'))
        {
            text = text + 2;
        }
        uintptr_t value = 0;
        for (; *text != '\0'; text++)
        {
            unsigned digit = 0;
            if (*text >= '0' and *text <= '9')
            {
                digit = static_cast<unsigned>(*text - '0');
            }
            else if (*text >= 'a' and *text <= 'f')
            {
                digit = static_cast<unsigned>(*text - 'a') + 10u;
            }
            else if (*text >= 'A' and *text <= 'F')
            {
                digit = static_cast<unsigned>(*text - 'A') + 10u;
            }
            value = (value << 4) | digit;
        }
        return value;
    }

    // Waits until `task` is dead and answers its status, or a negative kos_task_state error, or
    // -KOS_ETIMEDOUT.
    inline int wait_dead(kos_task_t task, int* status, uint64_t timeout_ms)
    {
        for (uint64_t ms = 0; ms < timeout_ms; ms++)
        {
            int const state = kos_task_state(task, status);
            if (state < 0)
            {
                return state;
            }
            if ((state & KOS_TASK_DEAD) != 0)
            {
                return 0;
            }
            kos::sleep_ns(1000000ull);
        }
        return -KOS_ETIMEDOUT;
    }
}

#endif
