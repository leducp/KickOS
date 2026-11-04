// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test object-budget errors through real syscalls, including endpoint creation.
// The endpoint budget is reached before capability-table capacity; notification
// allocation tests the reverse. A second task verifies that the shared pool
// retains capacity beyond one task's budget. Self-test images only.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>

#include <kickos/apps/arm_count.h>
#include <kickos/apps/peer_run.h>

namespace
{
    kickos::apps::ArmCount harness("objbudget");

    // More endpoints than any pool in the fleet, so the loop below is bounded by the kernel's
    // refusal and never by this array.
    constexpr int HELD_MAX = 40;
    kos_cap_t held[HELD_MAX];
    int held_n = 0;

    // Create endpoints until the kernel refuses, and answer the refusal.
    int fill_endpoints()
    {
        while (held_n < HELD_MAX)
        {
            kos_cap_t ep = KOS_CAP_NONE;
            int const rc = kos_endpoint_create(&ep);
            if (rc != 0)
            {
                return rc;
            }
            held[held_n] = ep;
            held_n = held_n + 1;
        }
        return 0;
    }

    void release_all()
    {
        for (int i = 0; i < held_n; i++)
        {
            (void)kos_handle_close(held[i]);
        }
        held_n = 0;
    }
}

int main(int, char**)
{
    // --- 1 + 2: the refusal is the BUDGET's and not the POOL's -----------------------
    // -KOS_ENOMEM here would mean the pool ran dry, which is the denial this budget exists
    // to prevent: a slot must be left over for somebody else.
    int const refused = fill_endpoints();
    harness.note_rc("endpoint_create at the ceiling", refused);
    harness.check(refused == -KOS_EAGAIN,
                  "a task at its endpoint ceiling is refused with the budget's own code, "
                  "not ENOMEM");
    harness.check(held_n >= 1, "and the ceiling admitted at least one endpoint first");

    // --- 3: the budget comes back at the release ------------------------------------
    // Without this a long-running task dies of its own churn, and no create-only arm can
    // tell: the refusal above is identical either way.
    int const closed = kos_handle_close(held[held_n - 1]);
    held_n = held_n - 1;
    kos_cap_t again = KOS_CAP_NONE;
    int const after = kos_endpoint_create(&again);
    harness.note_rc("endpoint_create after closing one", after);
    if (after == 0)
    {
        held[held_n] = again;
        held_n = held_n + 1;
    }
    harness.check(closed == 0 and after == 0,
                  "releasing an endpoint gives its budget back to the task that created it");

    // --- 4: THE ITEM'S CLAIM. Another TASK's creator still lands --------------------
    // A task of its own and not a plain spawn: a plain spawn is a thread of THIS task and
    // would share this ceiling, so it could not tell a per-task bound from a global one.
    bool child_ok = false;
    int child_rc = 1;
    kos_program_t prog = KOS_CAP_NONE;
    int const found = kos_program_find("objbudget_peer", &prog);
    harness.note_rc("program_find", found);
    if (found == 0)
    {
        kos_cap_grant caps[] = {
            { held[0], KOS_CAP_SIGNAL },
        };
        kos_task_params p = {};
        p.caps = caps;
        p.cap_count = 1;
        kos_task_t t = KOS_TASK_NONE;
        int const tk = kickos::apps::create_peer(prog, p, &t);
        harness.note_rc("task_create", tk);
        if (tk == 0)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, held[0], KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            int32_t const got = kos_reply_recv(KOS_CAP_NONE, &child_rc,
                                               kos_call_lens_pack(0, sizeof(child_rc)), &opts);
            int status = -1;
            (void)kickos::apps::wait_dead(t, &status, 5000u);
            child_ok = got == static_cast<int32_t>(sizeof(child_rc)) and child_rc == 0;
        }
        (void)kos_handle_close(prog);
        harness.note_rc("the child task's endpoint_create", child_rc);
    }
    harness.check(child_ok,
                  "a task at its ceiling does not deny another task's endpoint_create");

    // --- 6: a program's task is its own ------------------------------------------
    {
        kos_program_t peer_prog = KOS_CAP_NONE;
        bool statics_ok = false;
        if (kos_program_find("objbudget_peer", &peer_prog) == 0)
        {
            // Two creates of one instance: each starts from the image, so each counts one.
            char const* const count_argv[] = {"peer", "count", nullptr};
            kos_task_params cp = {};
            cp.argv = count_argv;
            int runs_ok = 0;
            for (int i = 0; i < 2; i++)
            {
                kos_task_t t = KOS_TASK_NONE;
                int status = -1;
                if (kickos::apps::create_peer(peer_prog, cp, &t) == 0
                    and kickos::apps::wait_dead(t, &status, 5000u) == 0 and status == 0)
                {
                    runs_ok = runs_ok + 1;
                }
            }
            statics_ok = runs_ok == 2;
            (void)kos_handle_close(peer_prog);
        }
#if KICKOS_ARCH_SIM
        (void)statics_ok;
        harness.skip("a peer's statics start from its image at every create: the sim starts a "
                     "program's statics once");
#else
        harness.check(statics_ok, "a peer's statics start from its image at every create");
#endif
    }

    // --- 5: the ceiling is PER POOL and not one budget across the kinds -------------
    // Still at the endpoint ceiling here. One shared count would refuse this too, and a
    // supervisor that cannot take a notification because a driver took the endpoints is the
    // same denial one pool away.
    kos_cap_t s = KOS_CAP_NONE;
    int const other_kind = kos_notify_create(&s);
    harness.note_rc("notify_create at the endpoint ceiling", other_kind);
    harness.check(other_kind == 0,
                  "the endpoint ceiling does not refuse a notification: the count is per pool");
    if (other_kind == 0)
    {
        (void)kos_handle_close(s);
    }

    release_all();
    return harness.verdict();
}
