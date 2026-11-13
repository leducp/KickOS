// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Creating an on_request peer program, waiting out its last incarnation, and waiting for its end.

#ifndef KICKOS_APPS_PEER_RUN_H
#define KICKOS_APPS_PEER_RUN_H

#include <stdint.h>

#include <kickos/kos.h>
#include <kickos/sys.h>

namespace kickos::apps
{
    // kos_task_create, and while it answers -KOS_EAGAIN for a named instance still in the way, a
    // KOS_WATCH_INSTANCE watch raising `notify_cap`'s badge bit `bit` on the notification the
    // calling thread is bound to, waited on until `until`, then the create again. -KOS_ETIMEDOUT,
    // or -KOS_ECANCELED, where the wait ends first; the bit is left pending where a raise races
    // that end.
    inline int create_when_startable(kos_program_t prog, kos_task_params const& params,
                                     kos_monotonic_t until, kos_cap_t notify_cap, uint32_t bit,
                                     kos_task_t* out)
    {
        while (true)
        {
            int const rc = kos_task_create(prog, &params, out);
            if (rc != -KOS_EAGAIN or params.instance == nullptr)
            {
                return rc;
            }
            kos_cap_t watch = KOS_CAP_NONE;
            int const armed =
                kos_watch(KOS_WATCH_INSTANCE, KOS_INSTANCE_STARTABLE, prog, notify_cap,
                          reinterpret_cast<uintptr_t>(params.instance), &watch);
            if (armed == -KOS_EALREADY)
            {
                return kos_task_create(prog, &params, out);
            }
            if (armed != 0)
            {
                return rc;
            }
            uint32_t bits = 0;
            int const waited = kos_notify_wait(1u << bit, until, &bits);
            (void)kos_handle_close(watch);
            if (waited != 0)
            {
                return waited;
            }
        }
    }

    // The same through a notification of its own, bound for the wait, for a calling thread bound to
    // nothing: where it cannot make or bind one, the create's -KOS_EAGAIN.
    inline int create_when_startable(kos_program_t prog, kos_task_params const& params,
                                     kos_monotonic_t until, kos_task_t* out)
    {
        int const rc = kos_task_create(prog, &params, out);
        if (rc != -KOS_EAGAIN or params.instance == nullptr)
        {
            return rc;
        }
        kos_cap_t note = KOS_CAP_NONE;
        if (kos_notify_create(&note) != 0)
        {
            return rc;
        }
        if (kos_notify_bind(note) != 0)
        {
            (void)kos_handle_close(note);
            return rc;
        }
        int const started = create_when_startable(prog, params, until, note, 0u, out);
        (void)kos_notify_unbind(note);
        (void)kos_handle_close(note);
        return started;
    }

    // create_when_startable, a named instance waited for up to a second through a notification of
    // its own; with none named, the create alone.
    inline int create_peer(kos_program_t prog, kos_task_params const& params, kos_task_t* out)
    {
        return create_when_startable(prog, params, kos::after(std::chrono::seconds{1}), out);
    }

    // create_when_startable, a named instance waited for up to a second on bit `bit` of the
    // notification the calling thread is bound to, through `notify_cap`; with none named, the
    // create alone.
    inline int create_peer(kos_program_t prog, kos_task_params const& params, kos_cap_t notify_cap,
                           uint32_t bit, kos_task_t* out)
    {
        return create_when_startable(prog, params, kos::after(std::chrono::seconds{1}), notify_cap,
                                     bit, out);
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
    // -KOS_ETIMEDOUT once `bound` has passed.
    template <class Rep, class Period>
    inline int wait_dead(kos_task_t task, int* status, std::chrono::duration<Rep, Period> bound)
    {
        kos_monotonic_t const until = kos::after(bound);
        while (true)
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
            if (kos_monotonic_now().ns >= until.ns)
            {
                return -KOS_ETIMEDOUT;
            }
            kos::sleep_for(std::chrono::milliseconds{1});
        }
    }
}

#endif
