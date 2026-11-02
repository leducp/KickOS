// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The budgets as the syscall ABI answers them, over the budgets system.yaml states: a task at
// its THREADS budget beside a sibling that still takes its whole one, a member sweeping its
// capabilities still counted, the RAM budgets, a declaration's lifetime RAM across its starts,
// the init restarting a server beside a task at its ENDPOINTS budget, capability tables of mixed
// widths within a declaration's CAP_SLOTS across a restart, and where the backend translates, the
// PAGES budget. Emulator images only.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <kickos/apps/arm_count.h>
#include <kickos/apps/peer_run.h>

#include "budgetwit.h"

namespace
{
    kickos::apps::ArmCount harness("budgetwit");

    constexpr uint64_t DEAD_MS = 5000u;

    kos_program_t g_peer = KOS_CAP_NONE;

    // The server's call: its report, and the reply capability that ends that start.
    bool server_call(kos_cap_t svc, Report* out, kos_cap_t* reply)
    {
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, svc, 0u, CALL_US);
        bool const heard = kos_reply_recv(KOS_CAP_NONE, out, kos_call_lens_pack(0, sizeof(*out)),
                                          &opts)
                           == static_cast<int32_t>(sizeof(*out));
        *reply = opts.info.reply_cap;
        return heard and *reply != KOS_CAP_NONE;
    }

    template <class R>
    bool receive(kos_cap_t ep, R* out)
    {
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, CALL_US);
        return kos_reply_recv(KOS_CAP_NONE, out, kos_call_lens_pack(0, sizeof(R)), &opts)
               == static_cast<int32_t>(sizeof(R));
    }

    // Creates instance `instance` running `role`, its report endpoint `ep` at the first
    // delegated index and `extra` beside it where it is not KOS_CAP_NONE.
    int start(char const* instance, char const* role, kos_cap_t ep, kos_cap_t extra,
              kos_task_t* out)
    {
        char const* argv[] = {"peer", role, nullptr};
        kos_cap_grant caps[2] = {{ep, KOS_CAP_SIGNAL}, {extra, KOS_CAP_TRANSFER}};
        kos_task_params p = {};
        p.argv = argv;
        p.instance = instance;
        p.caps = caps;
        p.cap_count = 1;
        if (extra != KOS_CAP_NONE)
        {
            p.cap_count = 2;
        }
        return kickos::apps::create_peer(g_peer, p, out);
    }

    // Runs `role` in `instance` to its end and answers its report.
    template <class R>
    bool run(char const* instance, char const* role, kos_cap_t ep, kos_cap_t extra, R* out)
    {
        kos_task_t t = KOS_TASK_NONE;
        if (start(instance, role, ep, extra, &t) != 0)
        {
            return false;
        }
        bool const heard = receive(ep, out);
        int status = -1;
        return heard and kickos::apps::wait_dead(t, &status, DEAD_MS) == 0;
    }

    // Starts `relaunch` until a start's block is refused: the start that was, or -1.
    int relaunch_until_refused(kos_cap_t ep, int cap)
    {
        char const* argv[] = {"peer", "relaunch", nullptr};
        kos_task_params p = {};
        p.argv = argv;
        p.instance = "relaunch";
        kos_cap_grant caps[1] = {{ep, KOS_CAP_SIGNAL}};
        p.caps = caps;
        p.cap_count = 1;
        for (int i = 0; i < cap; i++)
        {
            kos_task_t t = KOS_TASK_NONE;
            int status = -1;
            if (kickos::apps::create_peer(g_peer, p, &t) != 0
                or kickos::apps::wait_dead(t, &status, DEAD_MS) != 0)
            {
                return -2;
            }
            if (status != 0)
            {
                return i;
            }
        }
        return -1;
    }
}

int main(int, char**)
{
    // The server's first start, which the init makes once this receive makes main ready.
    kos_cap_t const svc = kos_grant_endpoint("/svc/report");
    Report first{-1, -1, 0, 0, 0};
    kos_cap_t first_reply = KOS_CAP_NONE;
    bool const server_up = server_call(svc, &first, &first_reply);

    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0 or kos_program_find("budgetwit_peer", &g_peer) != 0)
    {
        harness.check(false, "the report endpoint and the peer program");
        return harness.verdict();
    }

    // --- THREADS: a task at its budget, and a sibling taking all of its own, twice ----------
    kos_task_t hog = KOS_TASK_NONE;
    Report h{-1, 0, -1, 0, 0};
    bool const hog_up = start("hog", "hog", ep, KOS_CAP_NONE, &hog) == 0 and receive(ep, &h);
    harness.note_rc("the hog's refused spawn", h.rc);
    harness.check(hog_up and h.rc == -KOS_EAGAIN and h.count == HOG_THREADS - 1,
                  "a task at its THREADS budget is refused with the budget's code");
    bool sibling_ok = true;
    for (int i = 0; i < 2; i++)
    {
        Report s{-1, -1, 0, 0, 0};
        sibling_ok = run("sibling", "all", ep, KOS_CAP_NONE, &s) and s.rc == 0
                     and s.count == SIBLING_SPAWNS and sibling_ok;
    }
    harness.check(sibling_ok, "a sibling started again beside it spawns every thread of its budget");

    // --- ENDPOINTS, and the init restarting a server beside the task at its budget ----------
    harness.check(hog_up and h.rc2 == -KOS_EAGAIN and h.extra == HOG_ENDPOINTS - 1,
                  "a task at its ENDPOINTS budget is refused with the budget's code");
    // The reply ends the server's first start, and the init starts it again.
    Report second{-1, -1, 0, 0, 0};
    kos_cap_t second_reply = KOS_CAP_NONE;
    bool const restarted = server_up and kos_reply(first_reply, &first, sizeof(first)) == 0
                           and server_call(svc, &second, &second_reply);
    harness.note_rc("the restarted server's endpoint_create", second.rc);
    harness.check(restarted and first.rc == 0 and second.rc == 0,
                  "the init restarts a server, which creates its endpoint");
    (void)kos_reply(second_reply, &second, sizeof(second));

    // --- THREADS through a member's sweep ---------------------------------------------------
    kos_task_t sweeper = KOS_TASK_NONE;
    Report w{-1, 0, 0, -1, 0};
    bool const swept = start("sweeper", "sweep", ep, KOS_CAP_NONE, &sweeper) == 0
                       and receive(ep, &w);
    harness.note_rc("the sweeper's refused spawn", w.rc);
    harness.note_rc("the sweeper's last wake", w.extra);
    harness.note_rc("the sweeper's members still mid-sweep", w.paused);
    // Every member pauses in its sweep, so the spawn after the budget's last is refused with
    // each earlier member still unreclaimed.
    bool const counted = w.rc == -KOS_EAGAIN and w.count == SWEEPER_THREADS - 1
                         and w.paused == SWEEPER_THREADS - 1;
    harness.check(swept and w.rc2 == 0 and counted,
                  "a member sweeping its capabilities still counts against its THREADS budget");
    // While those members are still mid-sweep: they run below every task here.
    Report taker{-1, -1, 0, 0, 0};
    bool const took = run("sibling", "all", ep, KOS_CAP_NONE, &taker);
    harness.check(took and taker.rc == 0 and taker.count == SIBLING_SPAWNS,
                  "another task still obtains its whole THREADS budget");
    int status = -1;
    (void)kickos::apps::wait_dead(sweeper, &status, DEAD_MS);

    // --- CAP_SLOTS: tables per declaration, twice, the second a restart --------------------
    WidthsReport tw[2] = {};
    bool widths_ran = true;
    for (int i = 0; i < 2; i++)
    {
        widths_ran = run("widths", "widths", ep, KOS_CAP_NONE, &tw[i]) and widths_ran;
    }
    bool seated = widths_ran;
    bool kept = widths_ran;
    for (int k = 0; k < 2; k++)
    {
        for (int i = 0; i < 3; i++)
        {
            seated = seated and tw[k].seated[i] == WIDTHS_ASK[i] - KOS_CAP_FIRST_DYNAMIC;
            kept = kept and tw[k].kept[i] == tw[k].seated[i];
        }
    }
    harness.note_rc("the widths' first worker room", tw[0].seated[0]);
    harness.note_rc("the widths' replacement spawn", tw[0].wide);
    harness.note_rc("the restarted widths' replacement spawn", tw[1].wide);
    harness.check(seated, "each thread seats the width it asks, wider and narrower than its task's");
    harness.check(kept and tw[0].over == -KOS_EAGAIN and tw[1].over == -KOS_EAGAIN,
                  "a thread asking a width past its task's slots is refused while the task's other "
                  "threads keep theirs");
    harness.check(tw[0].floor == -KOS_EINVAL and tw[1].floor == -KOS_EINVAL,
                  "a width below the spawn's grant list is refused");
    bool wide = true;
    bool small = true;
    for (int k = 0; k < 2; k++)
    {
        wide = wide and tw[k].wide == 0 and tw[k].wide_seated == WIDTHS_WIDE - KOS_CAP_FIRST_DYNAMIC;
        small = small and tw[k].small == WIDTHS_SMALL and tw[k].small_rc == -KOS_EAGAIN;
    }
    harness.check(widths_ran and wide,
                  "a replacement wider than any exited worker is seated once their sweeps end, "
                  "in every start of the declaration");
    harness.check(widths_ran and small,
                  "the declared width fills every slot of the budget again, in every start");

    // --- RAM ---------------------------------------------------------------------------------
    Report r{-1, 0, 0, 0, 0};
    bool const ram_ran = run("ram", "ram", ep, KOS_CAP_NONE, &r);
    harness.check(ram_ran and r.rc == 1,
                  "a kos_ram_alloc past the task's RAM_BYTES budget answers NULL");
    harness.check(ram_ran and r.count == RAM_BLOCKS_BUDGET,
                  "the block past the task's RAM_BLOCKS budget answers NULL");
#if KICKOS_HAVE_ASPACE
    int const refused = relaunch_until_refused(ep, RELAUNCH_BLOCKS + 2);
    harness.note_rc("the relaunch refused at start", refused);
    harness.check(refused == -1, "each incarnation of a translating board's task has its whole RAM "
                                 "budget");
#else
    int const refused = relaunch_until_refused(ep, RELAUNCH_CAP);
    harness.note_rc("the relaunch refused at start", refused);
    harness.check(refused == RELAUNCH_BLOCKS,
                  "a declaration that allocates at every start is refused past its lifetime budget");
#endif
    Report one{-1, -1, 0, 0, 0};
    harness.check(run("sibling", "one", ep, KOS_CAP_NONE, &one) and one.rc == 0 and one.count == 1,
                  "and a sibling's spawn still succeeds");

    // --- PAGES --------------------------------------------------------------------------------
#if KICKOS_HAVE_ASPACE
    kos_cap_t space = KOS_CAP_NONE;
    PagesReport p{};
    bool const paged = kos_aspace_self(&space) == 0 and run("pager", "pager", ep, space, &p);
    harness.note_rc("the pager's largest run", p.largest);
    harness.check(paged and p.largest > 0 and p.over == -KOS_EAGAIN,
                  "a run past the task's PAGES budget is refused");
    harness.check(paged and p.mapped == 0 and p.after_map + p.largest / 2u <= p.largest,
                  "a run mapped and closed still charges its minter");
    PagesReport q{};
    bool const after = run("pager", "pager-after", ep, KOS_CAP_NONE, &q);
    harness.note_rc("the next incarnation's largest run", q.largest);
    harness.check(after and p.elsewhere == 0 and q.largest + ELSEWHERE_PAGES <= p.largest,
                  "a run a dead incarnation mapped elsewhere still charges its declaration");
    PagesReport race{};
    bool const raced = run("pager", "race", ep, KOS_CAP_NONE, &race);
    harness.note_rc("the racing reservations that fit", race.won);
    harness.check(raced and race.won == 1u,
                  "of two reservations racing a presync, one fits the PAGES budget");
#else
    harness.skip("a run past the task's PAGES budget is refused: this board does not translate");
    harness.skip("a run mapped and closed still charges its minter: this board does not translate");
    harness.skip("a run a dead incarnation mapped elsewhere still charges its declaration: this "
                 "board does not translate");
    harness.skip("of two reservations racing a presync, one fits the PAGES budget: this board "
                 "does not translate");
#endif

    (void)kos_task_slay(hog, CALL_US);
    return harness.verdict();
}
