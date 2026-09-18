// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Test object-budget errors through real syscalls, including endpoint creation.
// The endpoint budget is reached before capability-table capacity; semaphore
// allocation tests the reverse. A second task verifies that the shared pool
// retains capacity beyond one task's budget. Self-test images only.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/errno.h>

using kickos::emit;

namespace
{
    int failures = 0;
    int arms = 0;

    void check(bool ok, char const* what)
    {
        char msg[128];
        if (ok)
        {
            arms = arms + 1;
            ksnprintf(msg, sizeof(msg), "[objbudget] ok - %s\n", what);
            emit(msg);
            return;
        }
        failures = failures + 1;
        ksnprintf(msg, sizeof(msg), "[objbudget] ERROR: %s\n", what);
        emit(msg);
    }

    void report_rc(char const* what, int rc)
    {
        char msg[128];
        ksnprintf(msg, sizeof(msg), "[objbudget]   %s rc=%d\n", what, rc);
        emit(msg);
    }

    // More endpoints than any pool in the fleet, so the loop below is bounded by the kernel's
    // refusal and never by this array.
    constexpr int HELD_MAX = 40;
    kos_cap_t held[HELD_MAX];
    int held_n = 0;

    // THE CHILD ANSWERS DOWN AN ENDPOINT AND NOT THROUGH A GLOBAL. Its task holds an address
    // space of its own on a translating board, so a global it writes is its own copy of the
    // page and root reads the value it started with, which reads as a child that never ran.
    // The endpoint is one ROOT already created and delegated SIGNAL-only, so the reporting
    // channel costs the pool nothing and leaves root at its ceiling while the child creates.
    constexpr kos_cap_t REPLY_EP = 1; // grants land at child indices 1..cap_count

    void child_creates_an_endpoint(void*)
    {
        kos_cap_t ep = KOS_CAP_NONE;
        int rc = kos_endpoint_create(&ep);
        if (rc == 0)
        {
            (void)kos_handle_close(ep);
        }
        (void)kos_send(REPLY_EP, &rc, sizeof(rc));
        kos_exit(0);
    }

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
    report_rc("endpoint_create at the ceiling", refused);
    check(refused == -KOS_EOVERFLOW,
          "a task at its endpoint ceiling is refused with the budget's own code, not ENOMEM");
    check(held_n >= 1, "and the ceiling admitted at least one endpoint first");

    // --- 3: the budget comes back at the release ------------------------------------
    // Without this a long-running task dies of its own churn, and no create-only arm can
    // tell: the refusal above is identical either way.
    int const closed = kos_handle_close(held[held_n - 1]);
    held_n = held_n - 1;
    kos_cap_t again = KOS_CAP_NONE;
    int const after = kos_endpoint_create(&again);
    report_rc("endpoint_create after closing one", after);
    if (after == 0)
    {
        held[held_n] = again;
        held_n = held_n + 1;
    }
    check(closed == 0 and after == 0,
          "releasing an endpoint gives its budget back to the task that created it");

    // --- 4: THE ITEM'S CLAIM. Another TASK's creator still lands --------------------
    // A task of its own and not a plain spawn: a plain spawn is a thread of THIS task and
    // would share this ceiling, so it could not tell a per-task bound from a global one.
    kos_task_t group = KOS_TASK_NONE;
    int const tk = kos_task_create(nullptr, 0, 0, &group);
    report_rc("task_create", tk);
    bool child_ok = false;
    int child_rc = 1;
    if (tk == 0)
    {
        kos_cap_grant caps[] = {
            { held[0], KOS_CAP_SIGNAL },
        };
        auto const c = kos::thread::create_caps(child_creates_an_endpoint, nullptr, "objb", 10,
                                                caps, 1, KOS_POLICY_FIFO, 0,
                                                /*privileged=*/false, nullptr, 0,
                                                KOS_AUTH_MEMORY, nullptr, group);
        if (c.valid())
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, held[0], KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            int32_t const got = kos_reply_recv(KOS_CAP_NONE, &child_rc,
                                               kos_call_lens_pack(0, sizeof(child_rc)), &opts);
            (void)c.join(KOS_TIMEOUT_NONE);
            child_ok = got == static_cast<int32_t>(sizeof(child_rc)) and child_rc == 0;
        }
        report_rc("the child task's endpoint_create", child_rc);
    }
    check(child_ok,
          "a task at its ceiling does not deny another task's endpoint_create");

    // --- 5: the ceiling is PER POOL and not one budget across the kinds -------------
    // Still at the endpoint ceiling here. One shared count would refuse this too, and a
    // supervisor that cannot take a semaphore because a driver took the endpoints is the same
    // denial one pool away.
    kos_cap_t s = KOS_CAP_NONE;
    int const other_kind = kos_sem_create(0, &s);
    report_rc("sem_create at the endpoint ceiling", other_kind);
    check(other_kind == 0,
          "the endpoint ceiling does not refuse a semaphore: the count is per pool");
    if (other_kind == 0)
    {
        (void)kos_handle_close(s);
    }

    release_all();
    if (tk == 0)
    {
        (void)kos_task_kill(group);
    }

    char msg[64];
    if (failures != 0)
    {
        ksnprintf(msg, sizeof(msg), "[objbudget] FAIL (%d)\n", failures);
        emit(msg);
        return 1;
    }
    // The count comes from a counter and the `ok -` lines from one emit per arm, so the gate
    // cross-checks the two and output lost between them cannot read as a clean run.
    ksnprintf(msg, sizeof(msg), "[objbudget] PASS (%d arms)\n", arms);
    emit(msg);
    return 0;
}
